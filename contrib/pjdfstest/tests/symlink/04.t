#!/bin/sh
# $FreeBSD: head/tools/regression/pjdfstest/tests/symlink/04.t 211352 2010-08-15 21:24:17Z pjd $

desc="symlink returns ENOENT if a component of the name2 path prefix does not exist"

dir=`dirname $0`
. ${dir}/../misc.sh

echo "1..3"

n0=`namegen`
n1=`namegen`

expect 0 mkdir ${n0} 0755
expect ENOENT symlink test ${n0}/${n1}/test
expect 0 rmdir ${n0}
