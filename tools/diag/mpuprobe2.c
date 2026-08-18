/*
 * mpuprobe2 - detailed probe of the PC-98 MPU-401 register map around the
 * detected base (0xE0D0).  PC-98 MIDI cards can place the status/command
 * register at base+1 or base+2, so try the reset and enter-UART-mode
 * commands at several offsets and report the raw register reads.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

static int fd;

static uint8_t r(unsigned p)
{
	uint8_t v = 0xff;
	(void)pread(fd, &v, 1, (off_t)p);
	return v;
}

static void w(unsigned p, uint8_t v)
{
	(void)pwrite(fd, &v, 1, (off_t)p);
}

int main(void)
{
	int p, off;

	fd = open("/dev/port", O_RDWR);
	if (fd < 0) { perror("/dev/port"); return 1; }

	printf("raw reads 0xE0D0..0xE0DF:\n  ");
	for (p = 0xe0d0; p <= 0xe0df; p++)
		printf("0x%04x=%02x ", p, r(p));
	printf("\n");

	for (off = 0; off <= 3; off++) {
		w(0xe0d0 + off, 0xff);		/* RESET */
		usleep(50000);
		printf("RESET@0x%04x -> data(0xE0D0)=0x%02x st(+1)=0x%02x st(+2)=0x%02x st(+3)=0x%02x\n",
		       0xe0d0 + off, r(0xe0d0), r(0xe0d1), r(0xe0d2), r(0xe0d3));
	}
	for (off = 0; off <= 3; off++) {
		w(0xe0d0 + off, 0x3f);		/* UART mode */
		usleep(50000);
		printf("UART@0x%04x  -> data(0xE0D0)=0x%02x\n", 0xe0d0 + off, r(0xe0d0));
	}
	return 0;
}
