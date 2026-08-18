// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * pc98-timesync: Lightweight, non-blocking SNTP client for PC-98 Linux.
 *
 * Synchronizes system time against NTP pools over UDP port 123, updates the
 * kernel realtime clock, and syncs to hardware RTC. Runs safely in the
 * background without blocking boot when networking is absent or offline.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>

#define NTP_PORT 123
#define NTP_EPOCH_DELTA 2208988800ULL /* Seconds between 1900 and 1970 */

static const char *default_servers[] = {
	"pool.ntp.org",
	"time.google.com",
	"time.cloudflare.com",
	"time.apple.com",
	NULL
};

/* 48-byte standard NTP packet structure */
struct ntp_packet {
	uint8_t li_vn_mode;      /* Leap indicator (2 bits), Version (3 bits), Mode (3 bits) */
	uint8_t stratum;
	uint8_t poll;
	uint8_t precision;
	uint32_t root_delay;
	uint32_t root_dispersion;
	uint32_t ref_id;
	uint32_t ref_ts_sec;
	uint32_t ref_ts_frac;
	uint32_t orig_ts_sec;
	uint32_t orig_ts_frac;
	uint32_t recv_ts_sec;
	uint32_t recv_ts_frac;
	uint32_t tx_ts_sec;
	uint32_t tx_ts_frac;
};

static int sync_with_server(const char *hostname)
{
	struct addrinfo hints, *res = NULL, *rp;
	int fd = -1, rc = -1;
	struct ntp_packet pkt;
	struct timeval tv;
	ssize_t n;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%d", NTP_PORT);

	if (getaddrinfo(hostname, port_str, &hints, &res) != 0 || !res)
		return -1;

	for (rp = res; rp != NULL; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0)
			continue;

		tv.tv_sec = 3;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;

		close(fd);
		fd = -1;
	}

	freeaddrinfo(res);
	if (fd < 0)
		return -1;

	memset(&pkt, 0, sizeof(pkt));
	pkt.li_vn_mode = (0 << 6) | (4 << 3) | 3; /* LI=0, VN=4, Mode=3 (client) */

	n = send(fd, &pkt, sizeof(pkt), 0);
	if (n == (ssize_t)sizeof(pkt)) {
		n = recv(fd, &pkt, sizeof(pkt), 0);
		if (n >= (ssize_t)sizeof(pkt)) {
			uint32_t sec = ntohl(pkt.tx_ts_sec);
			uint32_t frac = ntohl(pkt.tx_ts_frac);

			if (sec > NTP_EPOCH_DELTA) {
				struct timespec ts;
				ts.tv_sec = (time_t)(sec - NTP_EPOCH_DELTA);
				ts.tv_nsec = (long)(((uint64_t)frac * 1000000000ULL) >> 32);

				if (clock_settime(CLOCK_REALTIME, &ts) == 0) {
					char buf[64];
					struct tm tm_info;
					gmtime_r(&ts.tv_sec, &tm_info);
					strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm_info);
					printf("pc98-timesync: set system clock to %s (via %s)\n", buf, hostname);
					fflush(stdout);

					/* Update hardware RTC if hwclock is available */
					int ret = system("hwclock --systohc 2>/dev/null || hwclock -w 2>/dev/null");
					(void)ret;

					rc = 0;
				}
			}
		}
	}

	close(fd);
	return rc;
}

static int try_sync(const char **servers)
{
	for (int i = 0; servers[i] != NULL; i++) {
		if (sync_with_server(servers[i]) == 0)
			return 0;
	}
	return -1;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-d|--daemon] [-1|--once] [server ...]\n"
		"Synchronizes system clock from NTP servers in background without blocking.\n",
		prog);
}

int main(int argc, char **argv)
{
	int daemon_mode = 0;
	int once_mode = 0;
	const char *custom_servers[16];
	int num_custom = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
			daemon_mode = 1;
		} else if (strcmp(argv[i], "-1") == 0 || strcmp(argv[i], "--once") == 0) {
			once_mode = 1;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			if (num_custom < 15)
				custom_servers[num_custom++] = argv[i];
		}
	}
	custom_servers[num_custom] = NULL;

	const char **servers = (num_custom > 0) ? custom_servers : default_servers;

	if (daemon_mode) {
		if (daemon(0, 0) != 0) {
			perror("daemon");
			return 1;
		}

		/* Loop indefinitely: sync immediately when network is ready, then resync periodically */
		unsigned int sleep_sec = 5;
		for (;;) {
			if (try_sync(servers) == 0) {
				/* Success: resync every 1 hour */
				sleep_sec = 3600;
			} else {
				/* Failure / offline: retry with backoff (5s, 10s, 30s, 60s, up to 300s) */
				if (sleep_sec < 10)
					sleep_sec = 10;
				else if (sleep_sec < 30)
					sleep_sec = 30;
				else if (sleep_sec < 60)
					sleep_sec = 60;
				else if (sleep_sec < 300)
					sleep_sec = 300;
			}
			sleep(sleep_sec);
		}
	}

	if (once_mode) {
		return try_sync(servers) == 0 ? 0 : 1;
	}

	/* Default interactive invocation: try once */
	return try_sync(servers) == 0 ? 0 : 1;
}
