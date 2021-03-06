# $FreeBSD: head/sys/modules/rtwnfw/Makefile.inc 293011 2015-12-31 22:33:32Z adrian $
#
# Common rules for building firmware.  Note this gets auto-included
# by the subdir Makefile's as a consequence of included bsd.kmod.mk.

_FIRM=	${IMG}.fw

CLEANFILES+=	${_FIRM}

FIRMWS=	${_FIRM}:${KMOD}:111

FIRMWARE_LICENSE=	realtek

${_FIRM}: ${.CURDIR}/../../../contrib/dev/rtwn/${_FIRM}.uu
	uudecode -p $? > ${.TARGET}
