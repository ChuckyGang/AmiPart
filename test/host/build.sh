#!/bin/sh
# Build the host (Linux) endian regression test: compiles src/rdb.c natively
# for a little-endian machine and round-trips an Amiga-written RDB image.
cd "$(dirname "$0")"
exec gcc -O2 -Wall -I . -I inc -I ../../src -o hostrdb \
    host_main.c amiga_shim.c ../../src/rdb.c
