#!/bin/sh
# Build the host endian regression test against the shared host layer
# (host/): compiles src/rdb.c natively and round-trips an Amiga-written
# RDB image.  See README.txt.
cd "$(dirname "$0")"
exec gcc -O2 -Wall -I ../../host -I ../../host/inc -I ../../src -o hostrdb \
    host_main.c ../../host/amiga_shim.c ../../host/host_locale.c ../../src/rdb.c ../../src/clib.c
