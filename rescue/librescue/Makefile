#
# $FreeBSD: head/rescue/librescue/Makefile 298107 2016-04-16 07:45:30Z gjb $
#

.include <src.opts.mk>
PACKAGE=	rescue
MK_SSP=		no

# Certain library entries have hard-coded references to
# /bin, /sbin, etc, that require those entries to be
# recompiled for use in /rescue.  This Makefile
# accomplishes that.  Note that this is pure build hackery.
# This library should never be installed, and isn't even linked
# with in the normal way. (See ../rescue/Makefile for details.)

.PATH: ${.CURDIR}/../../lib/libc/gen \
       ${.CURDIR}/../../lib/libc/net \
       ${.CURDIR}/../../lib/libc/stdlib \
       ${.CURDIR}/../../lib/libutil 

LIB=		rescue
INTERNALLIB=	# Don't install this library
SRCS=		exec.c getusershell.c login_class.c popen.c rcmdsh.c \
		sysctl.c system.c

CFLAGS+=	-DRESCUE
# Flags copied from src/lib/libc and src/lib/libutil
# libc/db/Makefile.inc
CFLAGS+=	-D__DBINTERFACE_PRIVATE
# libc/net/Makefile.inc & libutil/Makefile
.if ${MK_INET6_SUPPORT} != "no"
CFLAGS+=	-DINET6
.endif
# libc/regex/Makefile.inc & libc/regex/grot/Makefile
CFLAGS+=	-DPOSIX_MISTAKE
# libc/rpc/Makefile.inc
CFLAGS+=	-DBROKEN_DES -DPORTMAP -DDES_BUILTIN
# libc/Makefile
.if ${MK_NIS} != "no"
CFLAGS+=       -DYP
.endif
.if ${MK_HESIOD} != "no"
CFLAGS+=       -DHESIOD
.endif
CFLAGS+=	-I${.CURDIR}/../../lib/libc/include

.include <bsd.lib.mk>
