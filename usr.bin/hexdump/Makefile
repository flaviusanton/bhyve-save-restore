#	@(#)Makefile	8.1 (Berkeley) 6/6/93
# $FreeBSD: head/usr.bin/hexdump/Makefile 201386 2010-01-02 10:27:05Z ed $

PROG=	hexdump
SRCS=	conv.c display.c hexdump.c hexsyntax.c odsyntax.c parse.c
MAN=	hexdump.1 od.1
MLINKS=	hexdump.1 hd.1
LINKS=	${BINDIR}/hexdump ${BINDIR}/od
LINKS+=	${BINDIR}/hexdump ${BINDIR}/hd

.include <bsd.prog.mk>
