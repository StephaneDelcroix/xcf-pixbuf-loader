#!/bin/sh

autoreconf -v --force --install

srcdir=`dirname $0`

$srcdir/configure "$@"
