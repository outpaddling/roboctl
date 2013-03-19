#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sysexits.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "roboctl.h"

/* Keep <= PIC_ERASE_BLOCK_SIZE to ensure that all flash is erased before
   writing. */
#define VEX_WRITE_CLUSTER_SIZE    64

/****************************************************************************
 * Description: 
 *  Upload a .hex file to the VEX controller.
 *
 *  Line format of hex file is ": ll aaaa 00 data -checksum"
 *      ll = len (1 byte)
 *      aaaa = address (2 bytes)
 *      Note that the checksum is negated (2's comp)
 *      Ex. :02080600743349
 * Author: 
 ***************************************************************************/

rct_status_t    vex_upload_program(rct_pic_t *pic, char *hexfile_name)

{
    FILE            *fp;
    char            buff[PIC_HEX_LINE_MAX+1],
		    /* VEX User program memory = 0x0800 to 0x7ffd.
		       0x7ffe and 0x7fff are read-only. This gives
		       0x77fe bytes to work with, which we round
		       to 0x7800 to make an integral # of 64-byte
		       blocks. */
		    code[0x7800];
    unsigned long   c,
		    address,
		    start_address,
		    end_address,
		    cluster_offset,
		    len,
		    erase_blocks,
		    write_blocks,
		    blocks_per_write;
    int             val,
		    checksum;
    extern int      Debug; 
    struct timeval  tp_start,tp_stop;   /* Time the upload and report speed */
    double          write_time;

    fp = fopen(hexfile_name, "r");
    if ( fp == NULL )
    {
	fprintf(stderr, "vex_upload_program(): Cannot open %s.\n", hexfile_name);
	return -1;
    }

    /* 
     * Get starting and ending addresses. HEX file addresses need not be
     * in order with this read/rewind/read method.
     */
    start_address = 0x7fff;     /* Prime to highest possible address */
    end_address = 0;            /* Prime to lowest possible address */
    while ( fgets(buff, PIC_HEX_LINE_MAX, fp) != NULL )
    {
	address = hex_val(buff+3, 4);
	if ( address != 0 )
	{
	    len = hex_val(buff+1, 2);
	    if ( address < start_address )
		start_address = address;
	    if ( address+len > end_address )
		end_address = address+len;
	}
    }
    debug_printf("vex_upload_program(): Start address: %06lX  End address: %06lX\n", 
	    start_address, end_address);

    /* If the end address is within range, the end of the last block
       will be as well, as long as VEX_WRITE_CLUSTER_SIZE <=
       PIC_ERASE_BLOCK_SIZE. */
    if ( !vex_valid_program_range(start_address, end_address,
	    "vex_upload_program") )
	return -1;

    write_blocks = (end_address - start_address) / VEX_WRITE_CLUSTER_SIZE + 1;
    blocks_per_write = VEX_WRITE_CLUSTER_SIZE / PIC_WRITE_BLOCK_SIZE;

    /* Initialize code array so unused bytes contain 0xff. This will
       prevent them from being altered by write, and save a little 
       wear on the flash RAM. The erase command sets all bits to 1,
       and write only resets the bits that are 0 in the buffer. */
    memset(code, 0xff, write_blocks * VEX_WRITE_CLUSTER_SIZE);

    /* Load HEX file to buffer */
    rewind(fp);
    while ( fgets(buff, PIC_HEX_LINE_MAX, fp) != NULL )
    {
	/* Parse line.  See function comment for format. */
	address = hex_val(buff+3, 4);
	if ( address != 0 )
	{
	    len = hex_val(buff+1, 2);
	    debug_printf("%02X %04lX: ", len, address);
	    for (c=0; c < len; ++c)
	    {
		val = hex_val(buff+9+c*2, 2);
		checksum -= val;
		code[address - start_address + c] = val;
	    }
	    debug_hex_dump(code + address - start_address, len);
	}
    }
    fclose(fp);

    /* Erase code area */
    erase_blocks = (end_address - start_address) / PIC_ERASE_BLOCK_SIZE + 1;
    printf("Program size is %lu bytes.\n", end_address - start_address);
    printf("Erasing %lu blocks (64 bytes/block, %lu bytes total)...\n",
	    erase_blocks, erase_blocks * PIC_ERASE_BLOCK_SIZE);
    pic_erase_program_mem(pic, start_address, end_address);
    //pic_read_program_mem(pic, start_address, 32);
    
    /* Write code to PIC. */
    printf("Writing %lu clusters (8 bytes/block, 8 blocks/cluster, %lu bytes total)\n",
	    write_blocks, write_blocks * VEX_WRITE_CLUSTER_SIZE);
    gettimeofday(&tp_start,NULL);
    for (c=0; c < write_blocks; ++c)
    {
	cluster_offset = c * VEX_WRITE_CLUSTER_SIZE;
	address = start_address + cluster_offset;
	
	if ( Debug )
	    putchar('\n');
	pic_write_program_mem(pic, address, blocks_per_write, code + cluster_offset);

	printf("Bytes: %4lu\r",cluster_offset + VEX_WRITE_CLUSTER_SIZE);
	fflush(stdout);
    }
    gettimeofday(&tp_stop,NULL);
    write_time = (tp_stop.tv_sec - tp_start.tv_sec) + 
		(tp_stop.tv_usec - tp_start.tv_usec) / 1000000.0;
    cluster_offset = c * VEX_WRITE_CLUSTER_SIZE;
    printf("\n%lu bytes written in %f seconds (%0.2f bytes/sec)\n",
	    c * VEX_WRITE_CLUSTER_SIZE, write_time,
	    c * VEX_WRITE_CLUSTER_SIZE / write_time);
    putchar('\n');
    return RCT_OK;
}


int     vex_valid_program_range(unsigned long start_address,
				unsigned long end_address,
				char *caller)

{
    return pic_valid_program_range(start_address, end_address, 0x0800, 0x7ffd, caller);
}


void    deassert_pin(int fd,int pin_mask)

{
    int     state;
    
    ioctl(fd, TIOCMGET, &state);
    state &= ~pin_mask;
    ioctl(fd, TIOCMSET, &state);
}


void    assert_pin(int fd,int pin_mask)

{
    int     state;
    
    ioctl(fd, TIOCMGET, &state);
    state |= pin_mask;
    ioctl(fd, TIOCMSET, &state);
}


time_t  difftimeofday(
	struct timeval *t1, /* Newer time value */
	struct timeval *t2  /* Older time value */
       )
	    
{
    return 1000000 * (t1->tv_sec - t2->tv_sec) + (t1->tv_usec - t2->tv_usec);
}


void    vex_set_program_mode(rct_pic_t *pic)

{
    struct timeval  assert1,
		    deassert1,
		    assert2,
		    deassert2,
		    assert3,
		    deassert3;

    /*
    puts("Make sure the VEX controller is turned on.");
    puts("Press the button on the programming module until the PGRM STATUS button flashes.");
    puts("Then press return...");
    getchar();
    */

    /*
     *  Using usleep() like this runs a risk of drifting from the
     *  desired times, but it's probably close enough.  The alternative
     *  is to watch the timer using getitimer() or gettimeofday().
     *
     *  Note: RS232 uses negative logic: assert -> low, deassert -> high
     */

    /*
     *  New approach
     */
    
    puts("Dropping RTS and CTS...");
    /* Everything low except TD before beginning the prog init sequence */
    //assert_pin(pic->fd, TIOCM_LE);
    //assert_pin(pic->fd, TIOCM_DTR);
    //assert_pin(pic->fd, TIOCM_DSR);
    assert_pin(pic->fd, TIOCM_RTS);
    //deassert_pin(pic->fd, TIOCM_ST);    /* Is this the same as TD? */
    //assert_pin(pic->fd, TIOCM_SR);      /* Is this the same as TR? */
    assert_pin(pic->fd, TIOCM_CTS);
    //assert_pin(pic->fd, TIOCM_DCD);
    //assert_pin(pic->fd, TIOCM_RI);
    getchar();
    
    usleep(500000);
    
    puts("Raising RTS and CTS...");
    /* Begin sequence by raising RTS and CTS at the same time */
    deassert_pin(pic->fd, TIOCM_RTS|TIOCM_CTS);
    //deassert_pin(pic->fd, TIOCM_CTS);
    getchar();

#if 0
    /* Raise RD after 16.7ms for 1/2 ms */
    usleep(16700);
    deassert_pin(pic->fd, TIOCM_SR);    /* Is this the same as RD? */
    usleep(500);
    assert_pin(pic->fd, TIOCM_SR);    /* Is this the same as RD? */
#endif

    /* Drop RTS after 38.8ms */
    usleep(21600);
    assert_pin(pic->fd, TIOCM_RTS);
    
    /* Drop CTS after 64.4ms */
    usleep(25600);
    assert_pin(pic->fd, TIOCM_CTS);
    
    /* Drop TD after 143.5ms */
    assert_pin(pic->fd, TIOCM_ST);  /* Is this the same as TD? */
    
    usleep(1000000);
    
    return;
    
    /*
     *  Old attempts
     */
    
    /* 
     *  Start both RTS and DTR in a low state for an arbitrary period.
     *  0.1 seconds is not enough.  This caused FreeBSD 7.0 to fail
     *  sometimes.
     */
    assert_pin(pic->fd, TIOCM_DTR);
    assert_pin(pic->fd, TIOCM_RTS);
    getchar();
    
    usleep(250000);
    
    gettimeofday(&assert1,NULL);
    deassert_pin(pic->fd, TIOCM_DTR);
    //usleep(8000);
    usleep(7000);
    deassert_pin(pic->fd, TIOCM_RTS);
    //usleep(156000);
    usleep(150000);

    gettimeofday(&deassert1,NULL);
    assert_pin(pic->fd, TIOCM_DTR);
    //usleep(3000);
    usleep(2000);
    assert_pin(pic->fd, TIOCM_RTS);
    //usleep(507000);
    usleep(500000);
    
    gettimeofday(&assert2,NULL);
    deassert_pin(pic->fd, TIOCM_DTR);
    //usleep(10000);
    usleep(9000);
    deassert_pin(pic->fd, TIOCM_RTS);
    //usleep(262000);
    usleep(260000);
    
    gettimeofday(&deassert2,NULL);
    assert_pin(pic->fd, TIOCM_DTR);
    //usleep(3000);
    usleep(2000);
    assert_pin(pic->fd, TIOCM_RTS);
    //usleep(507000);
    usleep(500000);
    
    gettimeofday(&assert3,NULL);
    deassert_pin(pic->fd, TIOCM_DTR);
    //usleep(10000);
    usleep(9000);
    deassert_pin(pic->fd, TIOCM_RTS);
    //usleep(25000);
    usleep(24000);
    
    gettimeofday(&deassert3,NULL);
    assert_pin(pic->fd, TIOCM_RTS);
    //usleep(3000);
    usleep(2000);
    assert_pin(pic->fd, TIOCM_DTR);
    usleep(20000); 
    
    printf("%u %lu %lu %lu %lu %lu\n",
	0,
       (unsigned long)difftimeofday(&deassert1,&assert1),
       (unsigned long)difftimeofday(&assert2,&assert1),
       (unsigned long)difftimeofday(&deassert2,&assert1),
       (unsigned long)difftimeofday(&assert3,&assert1),
       (unsigned long)difftimeofday(&deassert3,&assert1));
}


rct_status_t    vex_open_controller(rct_pic_t *pic, char *device)

{
    int     status;

    if ( (status = pic_open_controller(pic,device)) != RCT_OK )
	return status;

    /* Vex uses 115200 bits/sec, no parity, 8 data bits, 1 stop bit */
    cfsetispeed(&pic->current_port_settings, PIC_BAUD_RATE);
    cfsetospeed(&pic->current_port_settings, PIC_BAUD_RATE);
    pic->current_port_settings.c_cflag |= CS8|CLOCAL|CREAD;
    pic->current_port_settings.c_iflag |= IGNPAR;
    if ( tcsetattr(pic->fd, TCSANOW, &pic->current_port_settings) != 0 )
    {
	fprintf(stderr, "%s: tcsetattr() failed: %s\n",
	    __func__, strerror(errno));
	exit(EX_OSERR);
    }

    // Not yet working reliably or at all on some platforms.
    vex_set_program_mode(pic);
    
    /* close() on Mac and Linux (but not FreeBSD) sends the VEX
       into programming mode, as if the dongle button had been pressed.
       Causes FreeBSD 8.0 to fail unless --debug is used.  OK on
       earlier versions.
     */

#ifndef __FreeBSD__
    //deassert_pin(pic->fd,TIOCM_RTS);
#endif
    //deassert_pin(pic->fd,TIOCM_DTR);
    
    /*
     *  Discard leftover characters buffered by the serial driver or
     *  hardware from previous vex output.  This is particularly
     *  a problem with OS X /dev/cu.usbserial.
     */
    
    /* Delay required on FreeBSD after opening port */
    usleep(10000);
    
    serial_eat_leftovers(pic->fd);
    
    return RCT_OK;
}


void    vex_close_controller(rct_pic_t *pic)

{
    /* 
     *  Empty serial input, or it might get picked up by the next
     *  vexctl command.  Should do this at startup as well.
     */
    serial_eat_leftovers(pic->fd);
    
    // There might be something else we need to do here.  Stay tuned...
    pic_close_controller(pic);
}


void    serial_eat_leftovers(int fd)

{
    int     original_flags,
	    nonblock_flags;
    char    ch;
    
    /* Set non-blocking mode */
    original_flags = fcntl(fd, F_GETFL, 0);
    nonblock_flags = original_flags | O_NONBLOCK;
    fcntl(fd, F_SETFL, nonblock_flags);
    
    /*
     *  Read characters until a time-out occurs waiting for input
     *  sleep value determined by trial-and-error.  300 was the
     *  lowest increment of 100 that seemed to work reliably.
     */
    
    //printf("Leftovers:\n");
    while ( read(fd, &ch, 1) != -1 )
    {
	usleep(1000);
	// putchar(ch);
    }

    /* Restore fd flags */
    fcntl(fd, F_SETFL, original_flags);
}

