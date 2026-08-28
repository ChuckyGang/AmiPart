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
  sudo ./amipart DEV=/dev/sdb INFO          # raw device (CF card reader etc.)
  sudo ./amipart DEV=/dev/sdb SCRIPT prep.script FORCE
  ./amipart ?

Raw devices (DEV=/dev/...):
  * The device node is opened with O_EXCL, so the kernel REFUSES the
    open while the disk (or any of its partitions) is mounted or held
    by another program - the same guard mkfs and wipefs rely on.
    Unmount first (e.g. udisksctl unmount / umount).
  * Regular files are rejected - use IMAGE=<file> for those.
  * Needs write permission on the node: sudo, or membership in the
    'disk' group.  Without it the device opens read-only (INFO works,
    writes fail cleanly).
  * Reported geometry is the conventional 16 heads x 63 sectors at
    512 bytes/block, identical to image mode - the same disk prepped
    via DEV= or via an image dd'd onto it ends up byte-identical.
  * LISTDEV lists the machine's whole-disk block devices from sysfs
    (path, size, model, [IN USE] when mounted/swapped/held) - no disk
    I/O, so it never spins anything up.  Virtual devices (dm/md/ram/
    zram), CD/DVD drives and bare loop devices are skipped; a loop
    device WITH a backing file is listed (shown as "loop: <file>").
    LISTDEV UNITS additionally read-probes each disk (read-only,
    non-exclusive) and reports what is on it: RDB @ block N with the
    partition names, or GPT/MBR, or no signature.  The listed /dev
    path is exactly what DEV= takes.

Scope (KISS):
  * Quick-format (VOLNAME=) is Amiga-only: it needs the real filesystem
    handlers.  The host build reports it as unavailable.
  * REBOOT is ignored.

Correctness: the reference partitioning script produces a byte-identical
image from this binary and from the m68k binary under vamos, and
test/host/hostrdb round-trips an Amiga-written RDB byte-identically
through the same endian-explicit core (src/rdbbe.h).
