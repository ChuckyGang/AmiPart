/*
 * nativefmt.h - Internal ("native") quick format + the format dispatcher.
 *
 * The OS-assisted quick format (quickformat.c) mounts the new partition and
 * asks the real filesystem handler to Format() it.  That only works on an
 * Amiga with the handler resident and with a real device behind the
 * partition.  The internal formatter here writes the empty filesystem
 * skeleton itself, so a partition can be formatted
 *   - on the Linux/host build (image files AND raw devices),
 *   - into an image file on the Amiga,
 *   - without the filesystem handler being resident.
 *
 * Format_Partition() is the single entry point the CLI and the script engine
 * use: by default it runs the internal formatter for every dostype it knows
 * and falls back to the OS formatter for the rest; with safe=TRUE (the SAFE
 * keyword) it always uses the OS formatter.
 *
 * Filesystem coverage of the internal formatter (each verified
 * byte-for-byte against the real handler, see test/fmttest.py):
 *   FFS/OFS  DOS\0..DOS\7   any block size
 *   PFS3     PFS\x/PDS\x    pfs3aio 19.2 layout, 512-byte blocks only
 *   SFS      SFS\0/SFS\2    SmartFilesystem 1.279 layout, any block size
 * Anything else falls back to the OS formatter.
 */
#ifndef NATIVEFMT_H
#define NATIVEFMT_H

#include <exec/types.h>
#include "rdb.h"

/* TRUE if the internal formatter can write an empty volume of this dostype. */
BOOL NativeFormat_Supported(ULONG dostype);

/* Write an empty volume of pi->dos_type labelled pi->volume_name into the
 * partition described by pi (geometry from pi, falling back to rdb).  Works
 * on both backends (BD_DEVICE and BD_FILE).  Nothing is mounted.
 * Returns TRUE on success; errbuf (optional) gets a short reason otherwise. */
BOOL NativeFormat_Partition(struct BlockDev *bd, const struct RDBInfo *rdb,
                            const struct PartInfo *pi,
                            char *errbuf, ULONG errlen);

/* Format dispatcher for a freshly written partition (CLI + script).
 *
 *   safe = FALSE : internal formatter when NativeFormat_Supported(), then
 *                  (Amiga, real device) the partition is mounted live so the
 *                  volume shows up without a reboot.  Dostypes the internal
 *                  formatter does not cover yet use the OS formatter.
 *   safe = TRUE  : always the OS formatter (QuickFormat_Partition), i.e. the
 *                  real handler does the work.  Real devices on the Amiga
 *                  only - image files and the host build report an error.
 *
 *   mounted_name  - optional (>= 40 bytes): DOS device name the volume is
 *                   reachable under, or "" when it is not mounted.
 *   errbuf/errlen - optional: reason on failure.
 *   notebuf/notelen - optional: one informational line for the user on
 *                   success (which formatter ran, whether it is mounted,
 *                   PFS3 tuning outcome), "" if there is nothing to say.
 */
BOOL Format_Partition(struct BlockDev *bd, const struct RDBInfo *rdb,
                      const struct PartInfo *pi, BOOL safe,
                      char *mounted_name, char *errbuf, ULONG errlen,
                      char *notebuf, ULONG notelen);

#endif /* NATIVEFMT_H */
