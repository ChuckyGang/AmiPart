AmiPart host build (Linux, image-file mode)
===========================================

Native Linux build of AmiPart's CLI + script engine, sharing the exact
same source as the Amiga binary: src/cli.c, src/script.c, src/rdb.c and
every engine compile unmodified.  Only this directory is host-specific:

  amiga_compat.h  AmigaOS types/constants/struct stubs
  amiga_shim.c    POSIX bodies for the AmigaOS calls the core uses
                  (dos file I/O, console, memory) + host stubs for the
                  Amiga-only mount / quick-format / device-scan layer
  host_rdargs.c   ReadArgs over argv - same template, same syntax as
                  cmdline.txt documents ("amipart ?" works too)
  host_locale.c   GS() serves the built-in English strings (no
                  locale.library on the host)
  host_main.c     main(): no GUI - empty command line prints usage
  inc/            stub Amiga system headers

Build:  make          (produces ./amipart; plain gcc, no dependencies)

Usage examples:
  ./amipart IMAGE=disk.hdf INFO
  ./amipart IMAGE=disk.hdf SCRIPT prep.script FORCE
  ./amipart ?

Scope (KISS):
  * Image files only - no /dev/sdX access yet (that arrives as its own
    reviewed backend).  LISTDEV reports no devices by design.
  * Quick-format (VOLNAME=) is Amiga-only: it needs the real filesystem
    handlers.  The host build reports it as unavailable.
  * REBOOT is ignored.

Correctness: the reference partitioning script produces a byte-identical
image from this binary and from the m68k binary under vamos, and
test/host/hostrdb round-trips an Amiga-written RDB byte-identically
through the same endian-explicit core (src/rdbbe.h).
