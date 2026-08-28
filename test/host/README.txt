Host-side endian regression test for src/rdb.c
==============================================

rdb.c is endian-explicit (src/rdbbe.h): on the big-endian Amiga the
BE32R/BE32W accessors compile to direct access, on a little-endian host
they byte-swap.  This harness compiles rdb.c natively on Linux and
proves the little-endian path:

  ./build.sh
  ./hostrdb <amiga-written-image.hdf> [blank-same-size.hdf]

With one argument it parses and prints the RDB (partitions,
filesystems, code sizes).  With a second argument (a zero-filled image
of the same size) it also calls RDB_Write there; the output must be
BYTE-IDENTICAL to the Amiga-written original:

  md5sum original.hdf written.hdf     # must match

Descended from /home/john/Documents/Code/rdb-host-poc (which needed a
block-swap hack and documented why that can't work in general - the
refactor this harness tests is the fix).  amiga_compat.h/amiga_shim.c
stub just enough AmigaOS API for rdb.c's BD_FILE backend; the exec
device path compiles but is stubbed to fail.
