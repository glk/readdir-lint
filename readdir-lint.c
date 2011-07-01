/*-
 * Copyright (c) 2011 Gleb Kurtsou
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <inttypes.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __FreeBSD__
#define HAVE_DIRENT_NAMLEN
#define HAVE_GETPROGNAME
#endif

#define	DIRSIZE_MAX		(4*1024*1024)
#define	DIRSIZE_ENTRY		(sizeof(struct dirent))
#define	DIRSIZE_MIN		((sizeof (struct dirent) - (MAXNAMLEN+1)) + 4)
#define	DIRSIZE_BLOCK		512
#define	DIRSIZE_PAGE		4096

#define DIRENT_MAX		512

#define DP_NEXT(a)		((struct dirent *)(((char *)(a)) + a->d_reclen))

#define WARNX(var, fmt, ...)					\
	do {							\
		if ((var) == 0) {				\
			printf(fmt "\n", ## __VA_ARGS__ );	\
			var = 1;				\
		}						\
	} while (0)

static int opt_verbose;
static int opt_skip;
static char *opt_path;

#ifdef HAVE_DIRENT_NAMLEN
static int warn_nameterm;
#endif
static int warn_noerr;
static int warn_seekoff;
static int warn_zeroino;
static int warn_zerooff;
static int warn_reclen;

struct dirbuf {
	struct dirent *dp, *begin, *end;
	long base;
	int fd;
	int eof;
	size_t bufsize;
};

static void
dir_init(struct dirbuf *dir, const char *path, int bufsize)
{
	dir->fd = open(path, O_RDONLY);
	if (dir->fd == -1)
		err(1, "open %s", path);

	dir->begin = malloc(bufsize);
	if (dir->begin == NULL)
		err(1, "malloc failed");
	dir->dp = NULL;
	dir->end = NULL;
	dir->base = 0;
	dir->eof = 0;
	dir->bufsize = bufsize;
}

static void
dir_destroy(struct dirbuf *dir)
{
	free(dir->begin);
}

static off_t
dir_offset(struct dirbuf *dir)
{
	return lseek(dir->fd, 0, SEEK_CUR);
}

static int
dir_readx(struct dirbuf *dir)
{
	struct dirent *di;
	off_t seekoff;
	int rv;

	rv = getdirentries(dir->fd, (char *)dir->begin, dir->bufsize, &dir->base);
	if (opt_verbose >= 3)
		printf("dir_read %d: len=%d base=%ld\n", dir->fd, rv, dir->base);
	if (rv == -1)
		return (rv);
	if (rv == 0) {
		dir->eof = 1;
		dir->dp = NULL;
		dir->end = NULL;
	} else {
		dir->dp = dir->begin;
		dir->end = (struct dirent *)((char *)dir->begin + rv);
		seekoff = dir_offset(dir);
		for (di = dir->dp; di < dir->end; di = DP_NEXT(di)) {
			if (di->d_reclen <= 0 ||
			    di->d_reclen > (char *)dir->end - (char *)di) {
				WARNX(warn_reclen, "Invalid entry size: %d", di->d_reclen);
				dir->end = di;
				break;
			}
#ifdef HAVE_DIRENT_NAMLEN
			if (di->d_namlen > MAXNAMLEN)
				errx(1, "Ivalid name lenghth: %d", di->d_namlen);
			if (di->d_name[di->d_namlen] != '\0') {
				di->d_name[di->d_namlen] = '\0';
				WARNX(warn_nameterm, "Entry names are not NUL-terminated");
			}
#else
#endif
			if (di->d_fileno == 0) {
				WARNX(warn_zeroino, "Zero d_fileno: %08jx %s",
				    (uintmax_t)di->d_off, di->d_name);
			}
			if (di->d_off == 0)
				WARNX(warn_zerooff, "Zero d_off: %ju %s",
				    (uintmax_t)di->d_fileno, di->d_name);
			if (DP_NEXT(di) >= dir->end && di->d_off != seekoff) {
				WARNX(warn_seekoff, "Directory(%zd) and last entry offsets mismatch: %ju -- %ju",
				    dir->bufsize, (uintmax_t)seekoff, (uintmax_t)di->d_off);
			}
		}
	}

	return (rv);
}

static int
dir_read(struct dirbuf *dir)
{
	int rv;

	rv = dir_readx(dir);
	if (rv == -1)
		err(1, "Directory read");
	return (rv);
}


static struct dirent *
dir_next(struct dirbuf *dir)
{
	if (dir->eof)
		return NULL;

	if (dir->dp == NULL || DP_NEXT(dir->dp) >= dir->end) {
		dir_read(dir);
		if (dir->eof)
			return NULL;
	} else {
		dir->dp = DP_NEXT(dir->dp);
	}

	return dir->dp;
}

static void
dir_seek(struct dirbuf *dir, off_t off)
{
	int rv;

	rv = lseek(dir->fd, off, SEEK_SET);
	if (rv == -1)
		err(3, "seek(%jd, SEEK_SET)", (uintmax_t)off);
	dir->dp = NULL;
	dir->end = NULL;
	dir->base = 0;
	dir->eof = 0;
}

static int
dir_cmpent(struct dirbuf *dir1, struct dirbuf *dir2)
{
	struct dirent *dp1, *dp2;

	dp1 = dir1->dp;
	dp2 = dir2->dp;

	if (dir1->eof != dir2->eof)
		errx(3, "Invalid EOF: %d %ld -- %d %ld",
		    dir1->eof, dir1->base, dir2->eof, dir2->base);
	else if (dir1->eof)
		return (1);
	if (opt_verbose >= 2)
		printf("   %08jx (%d bytes) %-12s -- %08jx (%d bytes) %-12s\n",
		    (uintmax_t)dp1->d_off, dp1->d_reclen, dp1->d_name,
		    (uintmax_t)dp2->d_off, dp2->d_reclen, dp2->d_name);
	if (strcmp(dp1->d_name, dp2->d_name) != 0 ||
	    dp1->d_off != dp2->d_off)
		printf("Entries mismatch: %08jx (%d bytes) %-12s -- %08jx (%d bytes) %-12s\n",
		    (uintmax_t)dp1->d_off, dp1->d_reclen, dp1->d_name,
		    (uintmax_t)dp2->d_off, dp2->d_reclen, dp2->d_name);

	return (0);
}

static void
dir_lint(struct dirbuf *dir1, struct dirbuf *dir2)
{
	dir_read(dir1);
	dir_read(dir2);

	while (dir_cmpent(dir1, dir2) == 0) {
		dir_next(dir1);
		dir_next(dir2);
	}
}

#ifndef HAVE_GETPROGNAME
#define getprogname()	"readdir-lint"
#endif

static void
test_bufsize(struct dirbuf *dir_expect, struct dirbuf *dir)
{
	int tests_bufsize[] = { DIRSIZE_PAGE, DIRSIZE_BLOCK, DIRSIZE_ENTRY, 0 };
	int *ip;

	for (ip = tests_bufsize; *ip != 0; ip++) {
		if (opt_skip > 0) {
			opt_skip--;
			continue;
		}
		printf("Test buffer sizes: %d -- %d\n", DIRSIZE_MAX, *ip);
		dir_init(dir, opt_path, *ip);
		dir_seek(dir_expect, 0);
		dir_lint(dir_expect, dir);
		dir_destroy(dir);
	}
}

static void
test_minbufsize(struct dirbuf *dir_expect, struct dirbuf *dir)
{
	int len;

	if (opt_skip > 0) {
		opt_skip--;
		return;
	}

	printf("Test minimal buffer size\n");
	dir_init(dir, opt_path, DIRSIZE_ENTRY);
	dir_seek(dir_expect, 0);
	dir_read(dir_expect);
	while(!dir_expect->eof) {
		off_t prevoff = dir_offset(dir);

		for (dir->bufsize = DIRSIZE_MIN; dir->bufsize <= DIRSIZE_ENTRY;
		    dir->bufsize += 4) {
			len = dir_readx(dir);
			if (len <= 0) {
				if (prevoff != dir_offset(dir))
					errx(2, "Directory offset changed but no data read: %jd %jd",
					    (uintmax_t)prevoff, (uintmax_t)dir_offset(dir));
				if (len == 0) {
					WARNX(warn_noerr, "EINVAL expected for small buffer read, 0 byte result");
					continue;
				}
				if (errno == EINVAL)
					continue;
				err(1, "Directory read");
			}
			if (opt_verbose >= 1)
				printf("   min size %08jx (%d of %zd bytes) %s\n",
				    (uintmax_t)dir->dp->d_off, dir->dp->d_reclen, dir->bufsize,
				    dir->dp->d_name);
			break;
		}
		if (dir->bufsize > DIRSIZE_ENTRY) {
			errx(2, "Couldn't read entry at offset %jd",
			    (uintmax_t)dir_offset(dir));
		}
		dir->eof = 0;
		if (dir_cmpent(dir_expect, dir) != 0)
			break;
		dir_next(dir_expect);
		dir_seek(dir, dir->dp->d_off);
	}
	dir_destroy(dir);
}

static void
usage(int exitcode)
{
	fprintf(stderr, "usage: %s directory\n", getprogname());
	exit(exitcode);
}

int
main(int argc, char **argv)
{
	struct dirbuf dir_max;
	struct dirbuf dir_i;
	int len, opt;
	long prevbase;

	while ((opt = getopt(argc, argv, "hs:v")) != -1) {
		switch (opt) {
		case 's':
			opt_skip = atoi(optarg);
			break;
		case 'v':
			opt_verbose++;
			break;
		case 'h':
			usage(0);
			break;
		case '?':
		default:
			usage(-1);
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 0)
		usage(1);
	opt_path = argv[0];

	dir_init(&dir_max, opt_path, DIRSIZE_MAX);
	dir_read(&dir_max);
	prevbase = dir_max.base;
	len = dir_read(&dir_max);
	if (!dir_max.eof || len != 0)
		errx(1, "Directory is too large");

	test_bufsize(&dir_max, &dir_i);
	test_minbufsize(&dir_max, &dir_i);

	return (0);
}
