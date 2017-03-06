# $FreeBSD: head/cddl/usr.sbin/dtrace/tests/tools/genmakefiles.sh 299094 2016-05-04 23:20:53Z ngie $

usage()
{
    cat <<__EOF__ >&2
usage: $(basename $0)

This script regenerates the DTrace test suite makefiles. It should be run
whenever \$srcdir/cddl/contrib/opensolaris/cmd/dtrace/test/tst is modified.
__EOF__
    exit 1
}

# Format a file list for use in a make(1) variable assignment: take the
# basename of each input file and append " \" to it.
fmtflist()
{
    awk 'function bn(f) {
        sub(".*/", "", f)
        return f
    }
    {print "    ", bn($1), " \\"}'
}

genmakefile()
{
    local basedir=$1

    local tdir=${CONTRIB_TESTDIR}/${basedir}
    local tfiles=$(find $tdir -type f -a \
        \( -name \*.d -o -name \*.ksh -o -name \*.out \) | sort | fmtflist)
    local tcfiles=$(find $tdir -type f -a -name \*.c | sort | fmtflist)
    local texes=$(find $tdir -type f -a -name \*.exe | sort | fmtflist)

    # One-off variable definitions.
    local special
    if [ "$basedir" = proc ]; then
        special="
LIBADD.tst.sigwait.exe+= rt
"
    elif [ "$basedir" = uctf ]; then
        special="
WITH_CTF=YES
"
    fi

    local makefile=$(mktemp)
    cat <<__EOF__ > $makefile
# \$FreeBSD: head/cddl/usr.sbin/dtrace/tests/tools/genmakefiles.sh 299094 2016-05-04 23:20:53Z ngie $

#
# This Makefile was generated by \$srcdir${ORIGINDIR#${TOPDIR}}/genmakefiles.sh.
#

PACKAGE=	tests

\${PACKAGE}FILES= \\
$tfiles

TESTEXES= \\
$texes

CFILES= \\
$tcfiles

$special
.include "../../dtrace.test.mk"
__EOF__

    mv -f $makefile ${ORIGINDIR}/../common/${basedir}/Makefile
}

set -e

if [ $# -ne 0 ]; then
    usage
fi

readonly ORIGINDIR=$(realpath $(dirname $0))
readonly TOPDIR=$(realpath ${ORIGINDIR}/../../../../..)
readonly CONTRIB_TESTDIR=${TOPDIR}/cddl/contrib/opensolaris/cmd/dtrace/test/tst/common

# Generate a Makefile for each test group under common/.
for dir in $(find ${CONTRIB_TESTDIR} -mindepth 1 -maxdepth 1 -type d); do
    genmakefile $(basename $dir)
done
