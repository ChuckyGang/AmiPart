# AmiPart

A native AmigaOS 2.x hard disk partition editor with full RDB (Rigid Disk Block) support.

> **Note:** AmiPart is the software formerly known as **DiskPart** — renamed to avoid confusion with the unrelated Microsoft Windows tool of the same name. Same program, same author, new name.

## Download

**[Latest build (1.0 release candidate)](https://github.com/ChuckyGang/AmiPart/releases/tag/rc)** — including the executable, an autoboot ADF, a native Linux CLI binary, and the experimental Linux GUI binary. Click **Assets** on that page to see the downloadable files.

## Linux CLI

AmiPart also builds as a **native Linux command-line tool** from the same
source — the full CLI and script engine, operating on disk image files
(`.hdf`), with backups made on the Amiga verifiable on Linux and vice
versa:

```
make linux                          # builds host/amipart (plain gcc, no deps)
./host/amipart IMAGE=disk.hdf INFO
./host/amipart SCRIPT prep.script FORCE
./host/amipart ?
```

Same commands and syntax as the Amiga version (see `cmdline.txt`).
Raw devices work too — `sudo ./host/amipart DEV=/dev/sdb INFO` — with
the disk opened exclusively (mounted disks are refused by the kernel).
Quick-format works here too: AmiPart's internal formatter writes FFS/OFS,
PFS3 and SFS volumes itself, byte-identical to the real handlers.
See `host/README.txt` for details.

## Linux GUI (experimental)

**This is an experimental wrapper and a fun side project.** It runs the
Amiga GUI code on Linux through a small compatibility layer, so yes, it
looks very Amiga-like, and that is by choice. The main goal of AmiPart is
still the Amiga program; this is not a native Linux application and gets
attention only after the Amiga side.

The same GUI as on the Amiga, in a window on Linux — the Amiga GUI source
compiled against a small Intuition/GadTools layer on SDL2. Open an image
file or a raw disk, edit, rescue, write:

A prebuilt `amipart-gui-linux-x86_64` is attached to each release; it
needs only the SDL2 runtime (`sudo apt install libsdl2-2.0-0`) and a
`chmod +x`. To build it yourself:

```
sudo apt install libsdl2-dev        # build dependency
make linux-gui                      # builds host/amipart-gui
./host/amipart-gui                  # GUI;  with arguments it is the CLI
```

It looks and works like AmiPart on an Amiga, not like a native Linux
program. Menus are in the bar at the top (left click, or Ctrl+key for the
shortcuts). Mounting, unmounting, the OS formatter and reboot prompts do
not exist here; SMART and the raw SCSI debug tools need an Amiga driver.
Raw disks need the same permissions as the CLI. See `host/README.txt`.

---

## About

AmiPart is a partition management tool for the Amiga, built as a clean GadTools application that runs directly on Kickstart 2.x with no external library dependencies beyond the ROM. It was created out of a simple conviction: the Amiga deserves a good, modern partition editor — and it is better to have one now than never.

> *"Vibecoded software might be argued with, but this is an experiment in what AI-assisted development can produce when given a clear goal and a demanding user."*

**Director:** John Hertell — john(at)hertell.nu  
**Code:** Claude Code (Anthropic)

---


## Requirements

- AmigaOS 2.x (Kickstart 2.04 or later)
- Intuition, GadTools, DOS libraries (all standard ROM)
- ASL library optional (enables the Browse file requester in the filesystem driver dialog)

---

## Usage

Run `AmiPart` from the Shell or double-click from Workbench.

1. A device selector appears listing all detected disk controllers.
   Use **Filter / Show All** to toggle between storage-only and full device lists.
2. Select a device and click **Select** — a progress window shows each unit being probed.
3. Select a unit and click **Select** to open the partition editor.
4. Use the buttons along the bottom to manage partitions and filesystem drivers.
5. Click **Write** when satisfied to commit changes to disk.

### Workbench icon tooltypes

When launched by double-clicking its icon, AmiPart reads these tooltypes from the icon (set them via the icon's **Information** window, or `NOWARNING` on the CLI):

- `NOWARNING` — suppresses the startup disclaimer.
- `LOWCHIP` — keep windows SIMPLE_REFRESH (no chip-RAM backing bitmap); uncovered window areas are then repainted only after the dialog on top closes. Default is SMART_REFRESH.
- `WINDOW=left/top/width/height` — restores the partition editor window to this position and size on open, e.g. `WINDOW=50/30/640/400`. Ignored (falls back to the default centered size) if the saved geometry no longer fits the current screen — for example after switching to a smaller resolution. Not read when AmiPart is run from the Shell.

---

## Translations

AmiPart uses the AmigaOS `locale.library` catalog system (AmigaOS 2.1 and
later; on 2.04 it stays in English), so anyone can translate it without the
source code or a compiler. The release archive has a `Catalogs` drawer with
everything needed:

- `AmiPart.cd` – every text of the program, in English
- `AmiPart.ct` – a ready-made template to translate
- `README.txt` – step-by-step instructions
- `support/gencat.py` – a small Python 3 tool that checks a translation
  (placeholders, button separators), builds the `AmiPart.catalog`, and brings
  an older translation up to date when a new version adds texts

FlexCat and CatComp read the same files if you prefer to work on the Amiga.
Translations are welcome: send the `.ct` file as an issue or pull request.

## Building

Supports two m68k toolchains, auto-detected by `make`:

- **Bebbo** (`m68k-amigaos-gcc`, default `/opt/amiga`)
- **Bartman/Abyss** (`m68k-amiga-elf-gcc`)

```sh
make                       # auto-detect (prefers Bartman when its VS Code extension is installed, else Bebbo)
make TOOLCHAIN=bebbo       # force Bebbo
make TOOLCHAIN=bartman     # force Bartman
```

Or build with the Bebbo toolchain via Docker (no host install required):

```sh
./docker.sh
```

`make adf` builds `out/AmiPart.adf`, an autoboot floppy image that boots
straight into AmiPart (no Workbench needed). Requires amitools
(`pip install amitools`) for its xdftool.

Output: `out/AmiPart`

---

## A Note on Vibecoding

This project was developed through AI-assisted ("vibecoded") collaboration — the architecture, decisions, and direction came from a human; the implementation was written by an AI. Whether that makes the software more or less trustworthy is a fair question. The answer offered here is: judge it by what it does, read the source if you want, and always keep a backup before touching partition tables.

---


## License

MIT License

Copyright (c) 2026 John Hertell

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Third-party software

AmiPart contains an unmodified copy of **pfs3aio** (PFS3 All-In-One, version
3.1 / 19.2) by Toni Wilen, based on Professional File System III. It is the
default filesystem driver offered by *Filesystems > Add*.
Sources: <https://github.com/tonioni/pfs3aio>

This product includes software developed by Michiel Pelt.

```
Copyright (c) 2011, Michiel Pelt
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. All advertising materials mentioning features or use of this software
   must display the following acknowledgement:
   This product includes software developed by Michiel Pelt.
4. Neither the name of the author nor the
   names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY MICHIEL PELT ''AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL MICHIEL PELT BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
