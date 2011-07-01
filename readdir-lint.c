#include <sys/types.h>
#include <dirent.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	DIRSIZE_MAX		(1024*1024)
#define	DIRSIZE_ENTRY		(sizeof(struct dirent))
#define	DIRSIZE_MIN		((sizeof (struct dirent) - (MAXNAMLEN+1)) + 4)
#define	DIRSIZE_BLOCK		512
#define	DIRSIZE_PAGE		4096

#define DP_NEXT(a) ((struct dirent *)(((char *)(a)) + _GENERIC_DIRSIZ((a))))

static int opt_verbose;

struct dirbuf {
	struct dirent *dp, *begin, *end;
	long base;
	int fd;
	int len;
	int eof;
	int bufsize;
};

static int
dir_init(struct dirbuf *dir, const char *path, int bufsize)
{
	dir->fd = open(path, O_RDONLY);
	if (dir->fd == -1)
		err(1, "open %s");

	dir->begin = malloc(bufsize);
	if (dir->begin == NULL)
		abort();
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
dir_seekoff(struct dirbuf *dir)
{
	return lseek(dir->fd, 0, SEEK_CUR);
}

static int
dir_read(struct dirbuf *dir)
{
	struct dirent *di;
	off_t seekoff;
	int rv;

	rv = getdirentries(dir->fd, (char *)dir->begin, dir->bufsize, &dir->base);
	if (rv == -1)
		err(3, "getdirentries");
	if (rv == 0) {
		dir->eof = 1;
		dir->dp = NULL;
		dir->end = NULL;
	} else {
		dir->dp = dir->begin;
		dir->end = (struct dirent *)((char *)dir->dp + rv);
		seekoff = dir_seekoff(dir);
		for (di = dir->dp; di < dir->end; di = DP_NEXT(di)) {
			if (di->d_off == 0)
				errx(2, "Zero d_off: %*s", di->d_namlen, di->d_name);
			if (di->d_fileno == 0)
				warnx("Zero d_fileno: %08jx %*s",
				    di->d_off, di->d_namlen, di->d_name);
			if (DP_NEXT(di) >= dir->end && di->d_off != seekoff) {
				errx(2, "Directory(%d) and last entry offsets mismatch: %jd -- %jd",
				    dir->bufsize, seekoff, di->d_off);
			}
		}
	}
	if (opt_verbose >= 3)
		printf("dir_read %d: len=%d base=%ld\n", dir->fd, rv, dir->base);

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
dir_restart(struct dirbuf *dir)
{
	int rv;

	rv = lseek(dir->fd, 0, SEEK_SET);
	if (rv == -1)
		err(3, "seek(0, SEEK_SET)");
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
		printf("   %08jx (%d bytes) %*s -- %08jx (%d bytes) %*s\n",
		    dp1->d_off, dp1->d_reclen, dp1->d_namlen, dp1->d_name,
		    dp2->d_off, dp2->d_reclen, dp2->d_namlen, dp2->d_name);
	if (dp1->d_namlen != dp2->d_namlen ||
	    strncmp(dp1->d_name, dp2->d_name, dp1->d_namlen) != 0 ||
	    dp1->d_off != dp2->d_off)
		errx(3, "Entries mismatch: %08jx (%d bytes) %*s -- %08jx (%d bytes) %*s",
		    dp1->d_off, dp1->d_reclen, dp1->d_namlen, dp1->d_name,
		    dp2->d_off, dp2->d_reclen, dp2->d_namlen, dp2->d_name);

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
	char *path;
	int len, opt;
	long prevbase;

	while ((opt = getopt(argc, argv, "vh")) != -1) {
		switch (opt) {
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
	path = argv[0];

	dir_init(&dir_max, path, DIRSIZE_MAX);
	dir_read(&dir_max);
	prevbase = dir_max.base;
	len = dir_read(&dir_max);
	if (!dir_max.eof || len != 0)
		errx(1, "Directory is too large");

	int tests_bufsize[] = { DIRSIZE_PAGE, DIRSIZE_BLOCK, DIRSIZE_ENTRY, 0 };
	for (int *ip = tests_bufsize; *ip != 0; ip++) {
		printf("Test buffer sizes: %d -- %d\n", DIRSIZE_MAX, *ip);
		dir_init(&dir_i, path, *ip);
		dir_restart(&dir_max);
		dir_lint(&dir_max, &dir_i);
		dir_destroy(&dir_i);
	}

	printf("Test minimal buffer size\n");
	dir_init(&dir_i, path, DIRSIZE_ENTRY);
	dir_restart(&dir_max);
	dir_read(&dir_max);
	while(!dir_max.eof) {
		off_t prevoff = dir_seekoff(&dir_i);
		for (dir_i.bufsize = DIRSIZE_MIN; dir_i.bufsize <= DIRSIZE_ENTRY;
		    dir_i.bufsize += 4) {
			dir_i.eof = 0;
			dir_i.dp = NULL;
			if (dir_read(&dir_i) > 0) {
				if (opt_verbose >= 1)
					printf("   min size %08jx (%d of %d bytes) %*s\n",
					    dir_i.dp->d_off, dir_i.dp->d_reclen, dir_i.bufsize,
					    dir_i.dp->d_namlen, dir_i.dp->d_name);
				break;
			}
			if (prevoff != dir_seekoff(&dir_i))
				errx(2, "Directory offset changed but no data read: %jd %jd",
				    prevoff, dir_seekoff(&dir_i));
		}
		if (dir_i.bufsize > DIRSIZE_ENTRY) {
			errx(2, "Couldn't read entry at offset %jd", dir_seekoff(&dir_i));
		}
		if (dir_cmpent(&dir_max, &dir_i) != 0)
			break;
		dir_next(&dir_max);
	}
	dir_destroy(&dir_i);

	return (0);
}
