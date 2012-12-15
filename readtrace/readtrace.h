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
#ifndef READ_TRACE_H
#define READ_TRACE_H
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>

#define TASK_COMM_LEN 16
#define BDEVNAME_SIZE	32
struct bp_account_line {
	unsigned long jiffies;		/* 8  (8) */
	unsigned long i_ino;		/* 8  (16) */
	unsigned long block;		/* 8  (24) */
	unsigned int devid;		/* 4  (28) */
	unsigned int pgdevid;		/* 4  (32) */
	int pgindex;			/* 4  (36) */
	int size;			/* 4  (40) */
	int pid;			/* 4  (44) */
	int tgid;			/* 4  (48) */
	int reason;			/* 4  (52) */
	char comm[TASK_COMM_LEN];	/* 16 (68) */
	char devname[BDEVNAME_SIZE];	/* 32 (100) */
} __attribute__((packed));

#define BP_ACCOUNT_READ		0
#define BP_ACCOUNT_GET		1
#define BP_ACCOUNT_DIRTY	2
#define BP_ACCOUNT_ACCESSED	3
#define BP_ACCOUNT_INACTIVE	4
#define BP_ACCOUNT_ACTIVATED	5
#define BP_ACCOUNT_EVICTED	6

static inline char *reason_name(int reason) {
	switch (reason) {
		case BP_ACCOUNT_READ: return "Read";
		case BP_ACCOUNT_GET: return "Get";
		case BP_ACCOUNT_DIRTY: return "Dirty";
		case BP_ACCOUNT_ACCESSED: return "Access";
		case BP_ACCOUNT_INACTIVE: return "Deactivated";
		case BP_ACCOUNT_ACTIVATED: return "Activated";
		case BP_ACCOUNT_EVICTED: return "Evicted";
	}
	return "Invalid";
}

static inline unsigned long long timestamp(int time, int time2)
{
	return ((((unsigned long long)time) * 1000ULL) + (((unsigned long long)time2) / 1000000ULL));
}

#endif
