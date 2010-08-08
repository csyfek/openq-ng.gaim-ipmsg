#!/bin/sh
mkdir -p build
cd build
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig/ cmake .. || exit
exec /usr/bin/make "$@"
