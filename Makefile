# $FreeBSD$

WARNS?= 6

PROG=	readdir-lint
NO_MAN=
DEBUG_FLAGS= -O0 -g -I/usr/src/sys

.include <bsd.prog.mk>
