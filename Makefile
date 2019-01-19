# $Id: Makefile,v 1.3 2015/10/02 12:02:23 je Exp $

PREFIX	?= /usr/local

SRCS=	main.c

PROG=	lightplay
NOMAN=	lightplay

BINDIR=	${PREFIX}/bin

LDADD+=	-lsndio

.include <bsd.prog.mk>
