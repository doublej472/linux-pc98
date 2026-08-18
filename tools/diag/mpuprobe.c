/*
 * mpuprobe - detect a Roland MPU-401 (MPU-PC98) MIDI card by probing the
 * candidate PC-98 C-bus I/O addresses through /dev/port.
 *
 * The MPU-401 reset command (0xFF written to the command/status port) is
 * acknowledged with 0xFE read from the data port, so a real card reports
 * ack == 0xFE while an unpopulated port reads back 0xFF.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

static int fd;

static uint8_t rport(unsigned p)
{
	uint8_t v = 0;

	if (pread(fd, &v, 1, (off_t)p) != 1)
		return 0xff;
	return v;
}

static void wport(unsigned p, uint8_t v)
{
	(void)pwrite(fd, &v, 1, (off_t)p);
}

int main(void)
{
	static const unsigned bases[] = {
		0x80d0, 0x80d2, 0x80d4, 0x80d6, 0x80d8,
		0x90d0, 0xc0d0, 0xc8d0, 0xd0d0, 0xd8d0,
		0xe0d0, 0xe8d0, 0xf0d0, 0xf8d0,
		0x330, 0x332,
	};
	unsigned i;

	fd = open("/dev/port", O_RDWR);
	if (fd < 0) {
		perror("/dev/port");
		return 1;
	}

	printf("Roland MPU-401 (MPU-PC98) probe\n");
	for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
		unsigned b = bases[i];
		uint8_t before, ack, status;

		before = rport(b);
		wport(b + 1, 0xff);		/* MPU-401 RESET */
		usleep(50000);
		ack = rport(b);			/* data port = ACK (0xFE) */
		status = rport(b + 1);		/* status/command port */

		printf("0x%04x: data-before=0x%02x ack=0x%02x status=0x%02x %s\n",
		       b, before, ack, status,
		       (ack == 0xfe) ? "  << MPU-401 present!" : "");
	}
	return 0;
}
