# $FreeBSD: head/gnu/lib/libregex/Makefile 276562 2015-01-02 19:05:39Z bapt $

LIB=		gnuregex
SHLIB_MAJOR=	5

REGEXDIR=	${.CURDIR}/../../../contrib/libgnuregex
.PATH:		${REGEXDIR}

SRCS=		gnuregex.c
INCSGROUPS=	INCS WRINCS PXINCS
INCS=		regex.h.patched
INCSNAME=	regex.h
INCSDIR=	${INCLUDEDIR}/gnu
WRINCS=		gnuregex.h
PXINCS=		${REGEXDIR}/regex.h
PXINCSDIR=	${INCSDIR}/posix

CFLAGS+=	-D__attribute_warn_unused_result__=""
CFLAGS+=	-DHAVE_CONFIG_H -I${.CURDIR} -I${REGEXDIR}

CLEANFILES=	regex.h.patched gnuregex.c
regex.h.patched: regex.h
	sed 's=<posix/regex\.h>=<gnu/posix/regex.h>=g' \
		< ${.ALLSRC} > ${.TARGET}

gnuregex.c: regex.c
	sed 's=<regex\.h>=<gnu/regex.h>=g' \
		< ${.ALLSRC} > ${.TARGET}

.include <bsd.lib.mk>
