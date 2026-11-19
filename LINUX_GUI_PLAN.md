# AmiPart Linux GUI - plan

Status 2026-11-19: stages 1-4 are built and pass the headless test
(`test/guitest.py`): the whole GUI compiles and runs on Linux, edits and
writes images and raw devices, and every menu entry opens. Not done yet:
a look at it on a real display by a person, packaging (stage 5 - the CI
artifact links SDL2 dynamically and is not attached to the release),
SMART / raw SCSI views on Linux, catalogs. See `host/README.txt`.

Original plan, written 2026-11-18 (numbers measured at commit f8f4e99):

## Goal

The same AmiPart GUI on Linux, working on image files and raw devices
(`/dev/sdX`), so disks can be prepared and rescued without an emulator.
Single binary shipped by CI next to `amipart-linux-x86_64`.

## Recommended route: run the existing GUI on a small Intuition/GadTools layer

Keep the Amiga GUI source as the only GUI source and give it, on Linux, a
host implementation of the calls it makes - the same idea as `host/` already
does for exec/dos, extended to Intuition, GadTools, graphics and ASL.

Why this and not a new GTK/Qt front end:

- The GUI is 17,300 lines in 21 files. A second front end means a second
  copy of every dialog and every safety check, kept in step by hand, forever.
  Every feature added since the CLI port (rescue, shrink, clone, MBR,
  SMART...) would have had to be written twice.
- The API surface the GUI actually uses is small: about 65 distinct calls,
  6 gadget kinds (button 45, string 16, checkbox 15, listview 6, cycle 4,
  text 2), one menu strip, 9 drawing primitives, EasyRequest (179 call
  sites), ASL file requester (12).
- New GUI features then appear on Linux for free.

What this route cannot give: a native-looking Linux application. Layout is
pixel-positioned and the partition map is hand-drawn, so it will look like
AmiPart on an Amiga, in a window. If a native look matters more than
feature parity, that is a different project (new front end on the same
engine) and should be decided before any work starts.

## The one real obstacle: pointers stored in ULONG

On the Amiga a pointer fits in a ULONG, and the GUI relies on it everywhere:
tag lists (`{ WA_Title, (ULONG)title }`, 133 tag arrays, roughly 190 pointer
casts), EasyRequest arguments (`EasyRequest(win, &es, NULL, (ULONG)buf)`)
and EasyRequestArgs argument arrays. On 64-bit Linux ULONG stays 32 bits
(the engines depend on that), so these casts cut pointers in half.

Fix, the way AROS did it: a pointer-width integer type `IPTR` (ULONG on
m68k, `uintptr_t` on the host), used for `TagItem.ti_Data` and for those
casts. It is a mechanical edit and the compiler finds every site
(`-Werror=pointer-to-int-cast`). On m68k `IPTR` is ULONG, so the Amiga binary
must come out byte-identical before and after - that comparison is the
acceptance test for the edit.

Rejected alternatives: a 32-bit build (needs 32-bit SDL and multilib on
every user's machine) and keeping all memory below 4 GB with mmap tricks
(works on x86-64 Linux only, fragile).

## Design choices

| Question | Recommendation | Reason |
|---|---|---|
| Backend | SDL2 | Wayland and X11 from one code base, leaves Windows/macOS possible later. Runtime library is already on a normal desktop. |
| Windows | One SDL window acting as the "screen"; AmiPart's windows and requesters are drawn inside it by the layer | Modal dialogs, stacking and focus behave the same everywhere; no Wayland multi-window problems; the whole GUI can be rendered headless and tested by screenshot in CI. |
| Font | One embedded 8x16 Latin-1 bitmap font | No font dependency; text widths are exact, which the pixel layouts need. Integer scaling (2x, 3x) for HiDPI. |
| Menus | Permanent menu bar at the top, left-click | Right-mouse-button menus are unfamiliar on Linux. Same menu definitions. |
| File requester | Built into the layer | Keeps a single binary; no zenity/kdialog dependency. |
| Event loop | `WaitPort`/`GT_GetIMsg` pump SDL events and hand back IntuiMessages | The GUI's blocking, nested modal loops then run unchanged. |
| Strings | Built-in English, as the host CLI does today | Catalog loading can come later. |

The window model is the one choice with a real alternative (one native
window per AmiPart window: feels more native, loses headless testing and
adds modal/focus work). It needs a yes or no before stage 2.

## Amiga-only behaviour on Linux

| Feature | On Linux |
|---|---|
| Device/unit selector | Backed by the existing sysfs scan (`host_devices.c`), plus "Open image file..." |
| Unmount / Inhibit / remount after resize | Not applicable. The device is opened O_EXCL, which already refuses a mounted disk. |
| OS formatter ("OS format" checkbox, SAFE) | Unavailable; internal formatter only, as in the host CLI |
| Reboot prompts | Replaced by "re-insert the disk / done" |
| Window position via icon tooltypes | Small config file, or dropped at first |
| SMART / ATA IDENTIFY | Later; needs an SG_IO pass-through in the shim |
| Raw device access | Needs root or the `disk` group; clear message when the device opens read-only |

## Stages

Each stage ends in something that can be checked.

1. **GUI compiles and links on Linux (no window yet).**
   IPTR edit; intuition/gadtools/graphics/asl/icon headers in `host/inc`;
   stub layer; no-op host versions of the DosList/Inhibit/ColdReboot/icon
   calls in the 5 files that use them. CI builds it on every push, so the
   GUI can no longer pick up a non-portable construct unnoticed.
   Check: Amiga binary byte-identical to before; host GUI target links.
   Size: about 190 cast sites, 6 new headers, a few hundred lines of stubs.

2. **Screen, windows, drawing, events.**
   SDL window, embedded font, the 9 RastPort calls, window frames with
   drag/depth/close/size, IDCMP delivery (close, refresh, newsize, mouse,
   keys). Check: the main window opens on an image and draws the partition
   map and static text correctly; compared against an Amiberry screenshot.

3. **Gadgets, menus, requesters.**
   The 6 gadget kinds with the tags the GUI uses, `GT_SetGadgetAttrs`,
   menu bar, EasyRequest with RawDoFmt-style formatting. Check: add, edit,
   delete and write a partition on an image entirely from the GUI; result
   byte-identical to the same steps done with the CLI.

4. **File requester, device selector, remaining dialogs.**
   ASL replacement, host device list, keyboard handling and paste in string
   gadgets, progress windows. Check: every menu entry opened once; rescue,
   image dump/restore and whole-disk copy run from the GUI.

5. **Packaging and tests.**
   CI artifact (SDL linked statically or an AppImage), headless screenshot
   smoke test in CI, README section, raw-device permission notes.

Rough size of the layer itself (stages 2-4): 3,000 to 5,000 lines of C.
Stage 1 is the smallest and gives value on its own. Stages 2 and 3 are the
bulk. These are estimates from the call-site counts, not from a prototype.

## Risks

- **Hidden 32-bit assumptions** beyond tag lists (pointers kept in ULONG
  variables, struct sizes). The compiler will show them in stage 1; until
  then the count is a guess.
- **GadTools behaviour the GUI leans on without saying so** - listview
  refresh after detach/reattach, string gadget activation, refresh
  ordering. The Amiga GUI already carries workarounds for real GadTools
  quirks; the layer has to behave closely enough that those do no harm.
- **Open pull request "Partition map: zoom and pan"** touches the map
  drawing and mouse handling. Whichever lands first, the other has to
  follow it.
- **Writing to raw devices from a GUI run as root.** Same engine and same
  O_EXCL guard as the CLI, but a GUI makes a wrong device easier to pick.
  The selector must show size, model and "in use" as LISTDEV does.

## Before starting

- `libsdl2-dev` is not installed on the build machine (the runtime library
  is). Needs `sudo apt install libsdl2-dev`.
- Decide: Amiga look in one window (recommended above), or a native-looking
  front end as a separate project.
- Decide: one "screen" window, or one native window per AmiPart window.
- Not in scope for the first version: Windows and macOS builds, catalogs,
  SMART.
