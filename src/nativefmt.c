/*
 * nativefmt.c - Internal ("native") quick format + the format dispatcher.
 *
 * See nativefmt.h for the rationale.  Every block is composed as an array
 * of big-endian longwords through put32()/get32(), so the very same code
 * writes the same bytes on the m68k Amiga and on a little-endian host - no
 * swabbing anywhere.
 *
 * FFS/OFS (DOS\0..DOS\7) - what an empty volume consists of
 * -----------------------------------------------------------
 * Layout follows the AmigaDOS FFS on-disk format (ADF spec / ADFlib /
 * amitools; the same conventions ffsresize.c relies on):
 *
 *   FS block 0            boot block: dostype, checksum 0 (no boot code),
 *                         root block number.  The other reserved blocks
 *                         (usually just block 1) are zeroed.
 *   root                  = blocks / 2 (the FFS/ADFlib/amitools convention;
 *                         also what ffsresize.c assumes when it relocates
 *                         a root after a grow).
 *   root+1 ..             bitmap extension blocks (only when more than the
 *                         25 bitmap pointers the root block holds are
 *                         needed, i.e. partitions above ~50 GB at 512 B),
 *   then                  the bitmap blocks, one per (nlongs-1)*32 blocks.
 *   DOS\4/DOS\5 only:     one empty directory-cache block for the root,
 *                         hung off the root block's extension field, at the
 *                         first free block of the 32-block bitmap group the
 *                         root lives in (i.e. usually just before the root -
 *                         where the formatters' allocator, which starts its
 *                         search at the root's bitmap longword, puts it).
 *
 *   Bitmap: bit (b - reserved) of the bitmap = 1 free / 0 used, LSB-first
 *   inside each longword, long 0 of every bitmap block is its checksum.
 *   Everything is free except root, extension, bitmap and dircache blocks.
 *   Bits past the last block are left at 1 like the real formatters do
 *   (FFS never looks at them).
 *
 *   Root block: T_SHORT, hash table of nlongs-56 empty entries, bm_flag
 *   valid (-1), bitmap pointers, the three timestamps set to "now", the
 *   volume name as a BSTR (max 30 chars), ST_ROOT.  DOS\6/DOS\7 volumes
 *   additionally carry the used-block count and their own dostype in the
 *   fields older FFS versions leave at 0.
 *
 * Write order: the old boot signature is wiped first, the boot block goes
 * LAST, so an interrupted format never leaves a DOS signature on top of a
 * half-written skeleton.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/dos.h>

#ifdef AMIPART_HOST
#include <time.h>
#endif

#include "clib.h"
#include "rdb.h"
#include "locale_support.h"
#include "quickformat.h"
#include "nativefmt.h"

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static void set_err(char *errbuf, ULONG errlen, const char *msg)
{
    if (!errbuf || errlen == 0) return;
    strncpy(errbuf, msg, errlen - 1);
    errbuf[errlen - 1] = '\0';
}

/* Big-endian longword access at longword index idx of a byte buffer. */
static void put32(UBYTE *b, ULONG idx, ULONG v)
{
    b += idx * 4;
    b[0] = (UBYTE)(v >> 24); b[1] = (UBYTE)(v >> 16);
    b[2] = (UBYTE)(v >>  8); b[3] = (UBYTE)(v);
}

static ULONG get32(const UBYTE *b, ULONG idx)
{
    b += idx * 4;
    return ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) |
           ((ULONG)b[2] <<  8) |  (ULONG)b[3];
}

/* AmigaDOS block checksum: the sum of all longs must be 0.  The checksum
   slot itself must be 0 when this is called. */
static ULONG blk_checksum(const UBYTE *b, ULONG nlongs)
{
    ULONG sum = 0, i;
    for (i = 0; i < nlongs; i++) sum += get32(b, i);
    return (ULONG)(0UL - sum);
}

/* One FS block = spb device blocks of bd->block_size bytes. */
static BOOL write_fs_block(struct BlockDev *bd, ULONG part_abs, ULONG fs_blk,
                           ULONG spb, const UBYTE *buf)
{
    ULONG abs = part_abs + fs_blk * spb;
    ULONG bsz = bd->block_size > 0 ? bd->block_size : 512;
    ULONG i;
    for (i = 0; i < spb; i++)
        if (!BlockDev_WriteBlock(bd, abs + i, buf + i * bsz))
            return FALSE;
    return TRUE;
}

/* "Now" as an AmigaDOS DateStamp (days since 1978-01-01, minutes, ticks). */
static void now_datestamp(ULONG *days, ULONG *mins, ULONG *ticks)
{
#ifdef AMIPART_HOST
    /* AmigaDOS DateStamps are LOCAL time; 1978-01-01 is 2922 days after
       the Unix epoch. */
    time_t     t  = time(NULL);
    struct tm  lt = *localtime(&t);
    long       s  = (long)timegm(&lt) - 2922L * 86400L;
    if (s < 0) s = 0;
    *days  = (ULONG)(s / 86400L);
    *mins  = (ULONG)((s % 86400L) / 60L);
    *ticks = (ULONG)((s % 60L) * 50L);
#else
    struct DateStamp ds;
    DateStamp(&ds);
    *days  = (ULONG)ds.ds_Days;
    *mins  = (ULONG)ds.ds_Minute;
    *ticks = (ULONG)ds.ds_Tick;
#endif
}

/* ------------------------------------------------------------------ */
/* FFS / OFS                                                            */
/* ------------------------------------------------------------------ */

#define FFS_T_SHORT      2UL
#define FFS_T_DIRCACHE   33UL
#define FFS_ST_ROOT      1UL
#define FFS_BM_VALID     0xFFFFFFFFUL
#define FFS_ROOT_BM_MAX  25UL      /* bitmap pointers in the root block */
#define FFS_NAME_MAX     30        /* volume name length limit           */

static BOOL ffs_is_type(ULONG dostype)
{
    return (dostype & 0xFFFFFF00UL) == 0x444F5300UL && (dostype & 0xFFUL) <= 7;
}

/* Volume name rules FFS enforces: 1..30 chars, no ':' or '/'. */
static BOOL ffs_name_ok(const char *name, UWORD *len_out)
{
    UWORD n = 0;
    if (!name) return FALSE;
    while (name[n]) {
        if (name[n] == ':' || name[n] == '/') return FALSE;
        n++;
    }
    if (n == 0) return FALSE;
    if (n > FFS_NAME_MAX) n = FFS_NAME_MAX;
    *len_out = n;
    return TRUE;
}

static BOOL ffs_format(struct BlockDev *bd, const struct RDBInfo *rdb,
                       const struct PartInfo *pi, char *errbuf, ULONG errlen)
{
    ULONG heads   = pi->heads   > 0 ? pi->heads   : (rdb ? rdb->heads   : 0);
    ULONG sectors = pi->sectors > 0 ? pi->sectors : (rdb ? rdb->sectors : 0);
    ULONG dev_bsz = pi->block_size > 0 ? pi->block_size : 512;
    ULONG spb     = pi->sectors_per_block > 0 ? pi->sectors_per_block : 1;
    ULONG eff_bsz = dev_bsz * spb;
    ULONG nlongs  = eff_bsz / 4;
    ULONG dostype = pi->dos_type;
    ULONG variant = dostype & 0xFFUL;
    BOOL  dircache = (variant == 4 || variant == 5);
    BOOL  track_used = (variant == 6 || variant == 7);
    ULONG part_abs, dev_blocks, blocks, reserved, bpbm, bits;
    ULONG root, num_bm, num_ext, ext_slots, first_ext, first_bm, dc_blk;
    ULONG used_last, grp, i, k;
    ULONG days, mins, ticks;
    UWORD name_len = 0;
    UBYTE *buf = NULL;
    BOOL  ok = FALSE;
    char  msg[160];

    if (heads == 0 || sectors == 0 || pi->high_cyl < pi->low_cyl) {
        snprintf(msg, sizeof(msg), GS(MSG_NF_BAD_GEOMETRY_FMT),
                 (unsigned long)heads, (unsigned long)sectors);
        set_err(errbuf, errlen, msg);
        return FALSE;
    }
    if (dev_bsz != 512 || bd->block_size != 512) {
        set_err(errbuf, errlen, GS(MSG_NF_ONLY_512_SECTORS));
        return FALSE;
    }
    if (eff_bsz < 512 || eff_bsz > 16384 || (eff_bsz & (eff_bsz - 1)) != 0) {
        snprintf(msg, sizeof(msg), GS(MSG_NF_BAD_BLOCKSIZE_FMT),
                 (unsigned long)eff_bsz);
        set_err(errbuf, errlen, msg);
        return FALSE;
    }
    if (!ffs_name_ok(pi->volume_name, &name_len)) {
        set_err(errbuf, errlen, GS(MSG_NF_BAD_NAME));
        return FALSE;
    }

    part_abs   = pi->low_cyl * heads * sectors;
    dev_blocks = (pi->high_cyl - pi->low_cyl + 1) * heads * sectors;
    blocks     = dev_blocks / spb;                 /* FFS rounds down */
    reserved   = pi->reserved_blks > 0 ? pi->reserved_blks : 2UL;
    bpbm       = (nlongs - 1) * 32;                /* blocks per bitmap block */

    /* Layout (see the file header).  root .. used_last is one contiguous
       run of used blocks; the dircache block (if any) sits on its own. */
    if (blocks < reserved + 16) goto too_small;
    bits      = blocks - reserved;
    root      = blocks / 2;
    num_bm    = (bits + bpbm - 1) / bpbm;
    ext_slots = nlongs - 1;
    num_ext   = (num_bm > FFS_ROOT_BM_MAX)
                ? (num_bm - FFS_ROOT_BM_MAX + ext_slots - 1) / ext_slots : 0;
    first_ext = root + 1;
    first_bm  = first_ext + num_ext;
    used_last = first_bm + num_bm - 1;
    grp       = reserved + ((root - reserved) / 32) * 32;  /* root's bitmap group */
    dc_blk    = dircache ? (grp < root ? grp : used_last + 1) : 0;
    if (used_last >= blocks || dc_blk >= blocks) goto too_small;

    buf = (UBYTE *)AllocVec(eff_bsz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) {
        set_err(errbuf, errlen, GS(MSG_NF_OUT_OF_MEMORY));
        return FALSE;
    }

    /* 1. Wipe the reserved blocks (kills the old DOS signature first). */
    memset(buf, 0, eff_bsz);
    for (i = 0; i < reserved; i++)
        if (!write_fs_block(bd, part_abs, i, spb, buf)) goto write_fail;

    /* 2. Bitmap blocks.  Block k covers bits [k*bpbm, (k+1)*bpbm). */
    for (k = 0; k < num_bm; k++) {
        ULONG lo = k * bpbm;                       /* first bit in this block */
        ULONG b;
        memset(buf, 0xFF, eff_bsz);                /* all free (incl. tail) */
        for (b = root; b <= used_last + 1; b++) {
            ULONG off, rel, idx;
            if (b > used_last) {                   /* the dircache block */
                if (!dircache) break;
                b = dc_blk;
            }
            off = b - reserved;
            if (off >= lo && off < lo + bpbm) {
                rel = off - lo;
                idx = 1 + rel / 32;
                put32(buf, idx, get32(buf, idx) & ~(1UL << (rel % 32)));
            }
            if (b == dc_blk) break;
        }
        put32(buf, 0, 0);
        put32(buf, 0, blk_checksum(buf, nlongs));
        i = first_bm + k;
        if (!write_fs_block(bd, part_abs, i, spb, buf)) goto write_fail;
    }

    /* 3. Bitmap extension blocks (pointers to bitmap blocks 25.., chained
          through their last longword; no checksum). */
    for (k = 0; k < num_ext; k++) {
        ULONG first_ptr = FFS_ROOT_BM_MAX + k * ext_slots;
        ULONG s;
        memset(buf, 0, eff_bsz);
        for (s = 0; s < ext_slots && first_ptr + s < num_bm; s++)
            put32(buf, s, first_bm + first_ptr + s);
        put32(buf, nlongs - 1, (k + 1 < num_ext) ? first_ext + k + 1 : 0);
        i = first_ext + k;
        if (!write_fs_block(bd, part_abs, i, spb, buf)) goto write_fail;
    }

    /* 4. Empty directory-cache block for the root (DOS\4 / DOS\5). */
    if (dircache) {
        memset(buf, 0, eff_bsz);
        put32(buf, 0, FFS_T_DIRCACHE);
        put32(buf, 1, dc_blk);        /* own key    */
        put32(buf, 2, root);          /* parent     */
        put32(buf, 3, 0);             /* records    */
        put32(buf, 4, 0);             /* next cache */
        put32(buf, 5, blk_checksum(buf, nlongs));
        i = dc_blk;
        if (!write_fs_block(bd, part_abs, i, spb, buf)) goto write_fail;
    }

    /* 5. Root block. */
    now_datestamp(&days, &mins, &ticks);
    memset(buf, 0, eff_bsz);
    put32(buf, 0, FFS_T_SHORT);
    put32(buf, 3, nlongs - 56);                    /* hash table size */
    put32(buf, nlongs - 50, FFS_BM_VALID);         /* bm_flag         */
    for (k = 0; k < FFS_ROOT_BM_MAX && k < num_bm; k++)
        put32(buf, nlongs - 49 + k, first_bm + k); /* bm_pages[]      */
    put32(buf, nlongs - 24, num_ext ? first_ext : 0); /* bm_ext       */
    put32(buf, nlongs - 23, days);                 /* last root change */
    put32(buf, nlongs - 22, mins);
    put32(buf, nlongs - 21, ticks);
    buf[(nlongs - 20) * 4] = (UBYTE)name_len;      /* BSTR volume name */
    memcpy(buf + (nlongs - 20) * 4 + 1, pi->volume_name, name_len);
    if (track_used)
        put32(buf, nlongs - 11, used_last - root + 1);
    put32(buf, nlongs - 10, days);                 /* last disk change */
    put32(buf, nlongs -  9, mins);
    put32(buf, nlongs -  8, ticks);
    put32(buf, nlongs -  7, days);                 /* creation         */
    put32(buf, nlongs -  6, mins);
    put32(buf, nlongs -  5, ticks);
    if (track_used)
        put32(buf, nlongs - 4, dostype);
    put32(buf, nlongs - 2, dc_blk);                /* extension (dircache) */
    put32(buf, nlongs - 1, FFS_ST_ROOT);
    put32(buf, 5, blk_checksum(buf, nlongs));
    i = root;
    if (!write_fs_block(bd, part_abs, i, spb, buf)) goto write_fail;

    /* 6. Boot block last: dostype, no boot code (checksum 0), root pointer. */
    memset(buf, 0, eff_bsz);
    put32(buf, 0, dostype);
    put32(buf, 2, root);
    i = 0;
    if (!write_fs_block(bd, part_abs, i, spb, buf)) goto write_fail;

    ok = TRUE;
    goto done;

too_small:
    snprintf(msg, sizeof(msg), GS(MSG_NF_TOO_SMALL_FMT), (unsigned long)blocks);
    set_err(errbuf, errlen, msg);
    goto done;

write_fail:
    snprintf(msg, sizeof(msg), GS(MSG_NF_WRITE_FAIL_FMT),
             (unsigned long)i, (unsigned long)(part_abs + i * spb));
    set_err(errbuf, errlen, msg);

done:
    if (buf) FreeVec(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Public engine entry points                                           */
/* ------------------------------------------------------------------ */

BOOL NativeFormat_Supported(ULONG dostype)
{
    return ffs_is_type(dostype);
    /* PFS3 (PFS\x, PDS\x) and SFS (SFS\0, SFS\2): not yet. */
}

BOOL NativeFormat_Partition(struct BlockDev *bd, const struct RDBInfo *rdb,
                            const struct PartInfo *pi,
                            char *errbuf, ULONG errlen)
{
    char dt[16], msg[120];

    if (errbuf && errlen) errbuf[0] = '\0';
    if (!bd || !pi) { set_err(errbuf, errlen, "no device"); return FALSE; }

    if (ffs_is_type(pi->dos_type))
        return ffs_format(bd, rdb, pi, errbuf, errlen);

    FormatDosType(pi->dos_type, dt);
    snprintf(msg, sizeof(msg), GS(MSG_NF_UNSUPPORTED_FMT), dt);
    set_err(errbuf, errlen, msg);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                           */
/* ------------------------------------------------------------------ */

BOOL Format_Partition(struct BlockDev *bd, const struct RDBInfo *rdb,
                      const struct PartInfo *pi, BOOL safe,
                      char *mounted_name, char *errbuf, ULONG errlen,
                      char *notebuf, ULONG notelen)
{
    char dt[16];
    char mnt[40];
    char err2[80];

    if (mounted_name) mounted_name[0] = '\0';
    if (errbuf && errlen) errbuf[0] = '\0';
    if (notebuf && notelen) notebuf[0] = '\0';
    mnt[0] = '\0'; err2[0] = '\0';
    if (!bd || !pi) { set_err(errbuf, errlen, "no device"); return FALSE; }

    FormatDosType(pi->dos_type, dt);

    /* ---- internal formatter ---- */
    if (!safe && NativeFormat_Supported(pi->dos_type)) {
        if (!NativeFormat_Partition(bd, rdb, pi, errbuf, errlen))
            return FALSE;

        if (bd->backend == BD_FILE) {
            if (notebuf) snprintf(notebuf, notelen, GS(MSG_NF_NOTE_IMAGE_FMT), dt);
            return TRUE;
        }
#ifdef AMIPART_HOST
        if (notebuf) snprintf(notebuf, notelen, GS(MSG_NF_NOTE_DEVICE_FMT), dt);
#else
        /* Mount it live so the new volume is usable right away (best effort:
           the format itself is complete either way). */
        QuickFormat_EnsureHandler(rdb, pi->dos_type, err2, sizeof(err2));
        if (MountPartition(bd, pi, mnt, err2, sizeof(err2))) {
            const char *nm = mnt[0] ? mnt : pi->drive_name;
            MaterializeVolume(nm);
            if (mounted_name) { strncpy(mounted_name, nm, 39); mounted_name[39] = '\0'; }
            if (notebuf) snprintf(notebuf, notelen, GS(MSG_NF_NOTE_MOUNTED_FMT), dt, nm);
        } else {
            if (notebuf) snprintf(notebuf, notelen, GS(MSG_NF_NOTE_NOT_MOUNTED_FMT),
                                  dt, err2[0] ? err2 : "?");
        }
#endif
        return TRUE;
    }

    /* ---- OS formatter (SAFE, or no internal formatter for this type) ---- */
    if (bd->backend == BD_FILE) {
        char msg[160];
        if (safe) strncpy(msg, GS(MSG_NF_IMAGE_NO_OS), sizeof(msg) - 1);
        else      snprintf(msg, sizeof(msg), GS(MSG_NF_UNSUPPORTED_IMAGE_FMT), dt);
        msg[sizeof(msg) - 1] = '\0';
        set_err(errbuf, errlen, msg);
        return FALSE;
    }
    {
        char tnote[160];
        BOOL ok;
        tnote[0] = '\0';
        ok = QuickFormat_EnsureHandler(rdb, pi->dos_type, err2, sizeof(err2)) &&
             QuickFormat_Partition(bd, pi, mnt, err2, sizeof(err2));
        if (!ok) {
            if (safe) {
                set_err(errbuf, errlen, err2);
            } else {
                char msg[200];
                snprintf(msg, sizeof(msg), GS(MSG_NF_UNSUPPORTED_OS_FAIL_FMT), dt, err2);
                set_err(errbuf, errlen, msg);
            }
            return FALSE;
        }
        {
            const char *nm = mnt[0] ? mnt : pi->drive_name;
            if (mounted_name) { strncpy(mounted_name, nm, 39); mounted_name[39] = '\0'; }
            QuickFormat_PFS3Tune(nm, pi->dos_type, pi->deldir_blocks,
                                 tnote, sizeof(tnote));
        }
        if (notebuf) {
            ULONG n;
            if (safe) snprintf(notebuf, notelen, GS(MSG_NF_NOTE_OS_SAFE_FMT), dt);
            else      snprintf(notebuf, notelen, GS(MSG_NF_NOTE_OS_FALLBACK_FMT), dt);
            n = strlen(notebuf);
            if (tnote[0] && n + 2 < notelen)
                snprintf(notebuf + n, notelen - n, " %s", tnote);
        }
        return TRUE;
    }
}
