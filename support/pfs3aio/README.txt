pfs3aio - PFS3 All-In-One filesystem handler, version 3.1 (19.2, 2.10.2018)

By Toni Wilen.  Sources: https://github.com/tonioni/pfs3aio
Unmodified binary, identical to the official Aminet release
(disk/misc/pfs3aio.lha).
  sha256: 185fae785d0f6e15a1936ecb44e4cb18a21b6f701094c57edbd331e5f9a16047

To update: copy another handler over support/pfs3aio/pfs3aio, run `make`,
and update the lines above.

AmiPart embeds this file (via support/bin2c.py -> src/pfs3aio_bin.c) as the
default handler offered by Filesystems > Add.  A handler file the user picks
from disk is always used instead, whatever its version.

This product includes software developed by Michiel Pelt.  See LICENSE.
