#	@(#)Makefile	8.1 (Berkeley) 6/6/93
# $FreeBSD: head/usr.bin/uuencode/Makefile 265420 2014-05-06 04:22:01Z imp $

.include <src.opts.mk>

PROG=	uuencode
MAN=	uuencode.1 uuencode.format.5
LINKS=	${BINDIR}/uuencode ${BINDIR}/b64encode
MLINKS=	uuencode.1 uudecode.1 \
	uuencode.format.5 uuencode.5 \
	uuencode.1 b64encode.1 \
	b64encode.1 b64decode.1

.if ${MK_TESTS} != "no"
SUBDIR+= tests
.endif

.include <bsd.prog.mk>
