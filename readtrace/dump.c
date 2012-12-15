/* Userspace code for reading blk_account's trace
 *
 * Copyright (C) 2012 by Nadav Shemer <nadav.shemer@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139,
 * USA.
 *
 */
#include "readtrace.h"

struct timeval __cur_tv = { 0 };
unsigned long long __cur_timestamp = 0;

void exit_nicely(int ret) {
	fflush(NULL);
	close(0);
	if (ret != 0)
		abort();
	exit(ret);
}

void nice_abort(void) {
	exit_nicely(1);
}

struct bp_account_line bp_buffer[1024];

static void update_timestamp(void) {
	if (gettimeofday(&__cur_tv, NULL)) {
		perror("gettimeofday");
		exit(1);
	}
	__cur_timestamp = timestamp(__cur_tv.tv_sec, __cur_tv.tv_usec * 1000L);
}

static void process(int count) {
	int i;
	for (i = 0; i < count; i++) {
		struct bp_account_line *bp = &bp_buffer[i];
		fprintf(stderr, "Processing event time:%lu dev %s page:%x:%lu:%d block:%x:%lu:%d proc %d:%d:%s reason %s\n", bp->jiffies, bp->devname, bp->pgdevid, bp->i_ino, bp->pgindex, bp->devid, bp->block, bp->size, bp->pid, bp->tgid, bp->comm, reason_name(bp->reason));
	}
	i = fwrite(bp_buffer, sizeof(struct bp_account_line), count, stdout);
	if (i != count) {
		perror("write");
		exit_nicely(1);
	}
}

int was_idle = 1;
int idle = 1;
int signal_flag = 0;

static void handler(int sig, siginfo_t *si, void *unused) {
	signal_flag = 1;
}

int bp_file = -1;

static int read_data(void) {
	int nread = 0;
	while (nread < 1024) {
		int i = read(bp_file, &bp_buffer[nread], (1024-nread)*sizeof(struct bp_account_line));
		if (i < 0) {
			/* Error */
			perror("read()");
			exit_nicely(1);
		} else if (i == 0) {
			/* No data */
			break;
		}
		if (i % sizeof(struct bp_account_line)) {
			/* Panic */
			fprintf(stderr, "Data not structure-aligned\n");
			exit_nicely(1);
		}
		i = (i / sizeof(struct bp_account_line));
		fprintf(stderr, "Read %d lines\n", i);
		nread += i;
	}
	process(nread);
	update_timestamp();
	return (nread == 1024);
}

#define USEC_PER_SEC	(1000000L)
void set_time(struct timeval *tv, long sec, long usec)
{
	while (usec >= USEC_PER_SEC) {
		sec += 1;
		usec -= USEC_PER_SEC;
	}
	while (usec < 0) {
		sec -= 1;
		usec += USEC_PER_SEC;
	}
	tv->tv_sec = sec;
	tv->tv_usec = usec;
}

void main(int argc, char *argv[]) {
	static struct timeval sleep_tv = { .tv_sec = 5, .tv_usec = 0 };
	struct sigaction sa;
	int i;
	signal_flag = 0;
	bp_file = -1;

	bp_file = open("/proc/bp/kbpd0", O_RDONLY);
	while (bp_file == -1) {
		/* Panic */
		perror("open()");
		bp_file = open("/proc/bp/kbpd0", O_RDONLY);
	}

	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = handler;
	if (sigaction(SIGUSR1, &sa, NULL) == -1)
		perror("sigaction");

	while (1) {
		fd_set rd;
		struct timeval diff_tv;
		struct timeval old_tv;
		unsigned long avg = 0;
		unsigned long loops = 0;
		struct timeval tv;
		int times = 0;

		FD_ZERO(&rd);
		memcpy(&tv, &sleep_tv, sizeof(struct timeval));
		if (idle == 0)
			idle = 1;
		while ((tv.tv_usec != 0) || (tv.tv_sec != 0)) {
			FD_SET(bp_file, &rd);
			memcpy(&old_tv, &tv, sizeof(struct timeval));
			i = select(bp_file+1, &rd, NULL, NULL, &tv);
			set_time(&diff_tv, old_tv.tv_sec - tv.tv_sec, old_tv.tv_usec - tv.tv_usec);
			avg += diff_tv.tv_usec;
			avg += (diff_tv.tv_sec * 1000000UL);
			times++;
			if (signal_flag)
				exit_nicely(0);
			if (i == -1 && errno == EINTR) {
				continue;
			}
			if (i == -1) {
				perror("select()");
				exit_nicely(1);
			}
			if (i == 0) {
				break;
			}
			if (!FD_ISSET(bp_file, &rd)) {
				fprintf(stderr, "Select returned without files ready\n");
				exit_nicely(1);
			}
			if (was_idle) {
				fprintf(stderr, "No longer idle\n");
				was_idle = 0;
			}
			idle = 0;
			while (read_data()) {
				loops++;
			}
		}
		if (idle) {
			if (was_idle == 0) {
				was_idle = 1;
				fprintf(stderr, "Going idle\n");
			}
			fprintf(stderr, "%d Average sleep time %lu (woken %d times) %lu\n", idle, avg / times, times, loops);
			idle++;
		} else {
			fprintf(stderr, "Average sleep time %lu (woken %d times) not idle %lu\n", avg / times, times, loops);
		}
	}
}
