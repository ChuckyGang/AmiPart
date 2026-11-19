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
    (Linux only: on macOS O_EXCL does not block a mounted disk, so check
    with diskutil before writing to a raw device there.)
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
  * Quick-format (VOLNAME=) uses the internal formatter (src/nativefmt.c):
    FFS/OFS DOS\0..DOS\7, PFS3 (PFS\x/PDS\x, 512-byte blocks) and SFS\0/
    SFS\2 volumes are written directly, on image files and raw devices
    alike.  test/fmttest.py proves them byte-identical to xdftool's FFS
    and to the real pfs3aio / SmartFilesystem handlers formatting the same
    partition under AmiFUSE.  The OS formatter (the SAFE keyword) needs a
    real Amiga, so SAFE and other dostypes report an error here.
  * REBOOT is ignored.

Correctness: the reference partitioning script produces a byte-identical
image from this binary and from the m68k binary under vamos, and
test/host/hostrdb round-trips an Amiga-written RDB byte-identically
through the same endian-explicit core (src/rdbbe.h).


Linux GUI (experimental)
========================

  make gui            (in this directory; needs libsdl2-dev)
  ./amipart-gui       opens the GUI; with arguments it is the same CLI

This is an experimental wrapper and a fun side project.  It runs the
Amiga GUI code on Linux through a small compatibility layer, so yes, it
looks very Amiga-like, and that is by choice.  The main goal of AmiPart is
still the Amiga program; this is not a native Linux application.

The GUI is the Amiga GUI source (../src/partview*.c, diskselect.c, main.c
...) compiled unchanged against an implementation of the Intuition,
GadTools, graphics and ASL calls it makes:

  host_gui.h        the Amiga structs/constants/prototypes the GUI uses
  host_gui.c        screen, windows, drawing, input, event pump (SDL2)
  host_gadtools.c   button, checkbox, cycle, string, listview, text
                    gadgets and menus
  host_req.c        EasyRequest, the file requester, small exec/dos stubs
  gui_font.h        8x13 bitmap font (X11 misc-fixed, public domain;
                    regenerate with ../support/mkguifont.py)

One SDL window is the "screen"; AmiPart's windows are drawn inside it.
Input goes to the most recently opened window (all dialogs are modal).

Different from the Amiga:
  * Menus stay visible in the top bar; left click, or Ctrl+<key>.
  * A string field is committed when you click elsewhere, not only on
    Return.  Tab moves to the next field.  Ctrl+V pastes, Ctrl+X clears.
  * Esc answers a requester with its rightmost button (cancel).  Return
    only closes one-button requesters - a stray key never confirms a write.
  * The mouse wheel and the cursor / page keys scroll a list.
  * No unmount / mount / OS format / reboot: nothing from the disk is
    mounted here.  SMART and the raw SCSI debug views need an Amiga driver.
  * A disk that cannot be opened (mounted, no permission) is reported
    and you return to the device list.

Environment:
  AMIPART_GUI_SIZE=WxH     screen size in pixels before scaling (800x600)
  AMIPART_GUI_SCALE=n      integer scale 1-4 (default: the largest that fits
                           the desktop)
  AMIPART_GUI_HEADLESS=1   no SDL video at all - for tests
  AMIPART_GUI_SCRIPT=file  feed input and take screenshots (.ppm); see
                           hg_script_step() in host_gui.c and
                           ../test/guitest.py

Test:  python3 ../test/guitest.py     (headless, also run in CI)

