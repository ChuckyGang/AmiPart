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
#include "pfsresize.h"
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
/* Byte-offset big-endian helpers (PFS3 / SFS blocks mix word and     */
/* longword fields, so longword indexing does not fit them).           */
/* ------------------------------------------------------------------ */

static void putl(UBYTE *b, ULONG off, ULONG v)
{
    b += off;
    b[0] = (UBYTE)(v >> 24); b[1] = (UBYTE)(v >> 16);
    b[2] = (UBYTE)(v >>  8); b[3] = (UBYTE)(v);
}

static void putw(UBYTE *b, ULONG off, UWORD v)
{
    b += off;
    b[0] = (UBYTE)(v >> 8); b[1] = (UBYTE)(v);
}

static ULONG getl(const UBYTE *b, ULONG off)
{
    b += off;
    return ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) |
           ((ULONG)b[2] <<  8) |  (ULONG)b[3];
}

/* Write `count` consecutive 512-byte device sectors from buf. */
static BOOL write_sectors(struct BlockDev *bd, ULONG part_abs, ULONG sector,
                          ULONG count, const UBYTE *buf)
{
    ULONG i;
    for (i = 0; i < count; i++)
        if (!BlockDev_WriteBlock(bd, part_abs + sector + i, buf + i * 512))
            return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* PFS3 - Professional File System 3, exactly as pfs3aio 19.2 does it */
/* ------------------------------------------------------------------ */
/*
 * Ported from pfs3aio's format.c / allocation.c / anodes.c / update.c
 * (the 19.2 revision, i.e. the L:pfs3aio everybody runs), and verified
 * byte-for-byte against the real handler formatting the same partition
 * under AmiFUSE (test/fmttest.py).  What the handler does:
 *
 *   - Two boot sectors: 'PFS\1' in the first, the rest zero.
 *   - A reserved area of `numreserved` reserved blocks (1 KB each; 2/4 KB
 *     on >104 GB / >411 GB partitions) starting at sector 2.  Its size is
 *     a piecewise-linear function of the partition size (schijf[] table),
 *     rounded up to a multiple of 32.
 *   - The rootblock cluster at sector 2: 512-byte rootblock + the
 *     reserved-area bitmap ('BM' block) directly behind it.
 *   - Reserved blocks are handed out by a roving first-fit allocator over
 *     that bitmap, in this order: rootblock extension ('EX'), then per
 *     253 bitmap blocks one bitmap-index block ('MI') followed by its
 *     bitmap blocks ('BM', every bit 1 = free), then [superindex block
 *     'SB' on >5 GB partitions] anode-index block 'IB', anode block 'AB'
 *     (anodes 0-4 reserved, anode 5 = the root directory), the root
 *     directory block 'DB', then the deldir blocks 'DD'.
 *   - The handler commits three times during a format (rootblock
 *     datestamp 1, 2, 3) and its rootblock extension is copy-on-write:
 *     every commit moves it to a fresh reserved block and frees the old
 *     one.  We reproduce that - including the two stale 'EX' copies left
 *     in freed reserved blocks - so the result is identical.
 *   - pfs3aio 19.2 formats with 512-byte logical blocks regardless of
 *     DE_SECSPERBLK, so BLOCKSIZE > 512 is refused here (use SAFE if a
 *     newer handler with real large-block support is installed).
 *
 * AmiPart's own additions on top (the same ones the OS path applies with
 * the setfnsize/setdeldir packets after a Format()): maximum filename
 * length 107 instead of 32, and DELDIR=<n> deldir blocks instead of the
 * handler's default 2.
 */

#define PFS_ID_DISK        0x50465301UL     /* 'PFS\1' */
#define PFS_ID2_DISK       0x50465302UL     /* 'PFS\2' */
#define PFS_DBLKID         0x4442           /* 'DB' */
#define PFS_ABLKID         0x4142           /* 'AB' */
#define PFS_IBLKID         0x4942           /* 'IB' */
#define PFS_BMBLKID        0x424D           /* 'BM' */
#define PFS_BMIBLKID       0x4D49           /* 'MI' */
#define PFS_DELDIRID       0x4444           /* 'DD' */
#define PFS_EXTENSIONID    0x4558           /* 'EX' */
#define PFS_SBLKID         0x5342           /* 'SB' */
#define PFS_VERSION_192    0x00130002UL     /* pfs2version: 19.2 */

#define PFS_MAXSMALLDISK   (5UL * 253UL * 253UL * 32UL)          /* 10241440   */
#define PFS_MAXDISKSIZE1K  (104UL * 253UL * 253UL * 32UL)        /* 213021952  */
#define PFS_MAXDISKSIZE2K  (104UL * 509UL * 509UL * 32UL)        /* 862221568  */
#define PFS_MAXDISKSIZE4K  (104UL * 1021UL * 1021UL * 32UL)      /* 3469243648 */
#define PFS_MAXNUMRESERVED (4096UL + 255UL * 1024UL * 8UL)
#define PFS_MAXBITMAPINDEX 104              /* MI blocks: large mode */
#define PFS_MAXSMALLBMI    5                /* MI blocks: small mode */
#define PFS_MAXDELDIR      32
#define PFS_FNSIZE_AMIPART 107
#define PFS_DELENTRY_PROT  0x0005UL

/* MODE_* rootblock options */
#define PFS_MODE_HARDDISK        1
#define PFS_MODE_SPLITTED_ANODES 2
#define PFS_MODE_DIR_EXTENSION   4
#define PFS_MODE_DELDIR          8
#define PFS_MODE_SIZEFIELD       16
#define PFS_MODE_EXTENSION       32
#define PFS_MODE_DATESTAMP       64
#define PFS_MODE_SUPERINDEX      128
#define PFS_MODE_SUPERDELDIR     256
#define PFS_MODE_EXTROVING       512
#define PFS_MODE_LONGFN          1024

/* Reserved-area allocator state (AllocReservedBlock / FreeReservedBlock). */
struct pfs_res {
    UBYTE *bm;            /* bitmap, MSB-first, bit set = free */
    ULONG  numreserved;
    ULONG  roving;        /* alloc_data.res_roving */
    ULONG  free;          /* rootblock->reserved_free */
    ULONG  firstres;      /* 2 */
    ULONG  lastres;
    ULONG  rescluster;    /* sectors per reserved block */
};

static ULONG pfs_res_alloc(struct pfs_res *r)
{
    ULONG i, nlongs = (r->numreserved + 31) / 32;
    LONG  j;
    if (r->free == 0) return 0;
    j = 31 - (LONG)(r->roving % 32);
    for (i = r->roving / 32; i < nlongs; i++, j = 31) {
        ULONG field = get32(r->bm, i);
        if (!field) continue;
        for (; j >= 0; j--) {
            if (field & (1UL << j)) {
                ULONG idx = i * 32 + (31 - (ULONG)j);
                ULONG blk = r->firstres + idx * r->rescluster;
                if (blk <= r->lastres) {
                    put32(r->bm, i, field & ~(1UL << j));
                    r->free--;
                    r->roving = idx;
                    return blk;
                }
            }
        }
    }
    if (r->roving) { r->roving = 0; return pfs_res_alloc(r); }
    return 0;
}

static void pfs_res_free(struct pfs_res *r, ULONG blk)
{
    ULONG t = (blk - r->firstres) / r->rescluster;
    put32(r->bm, t / 32, get32(r->bm, t / 32) | (0x80000000UL >> (t % 32)));
    r->free++;
}

/* CalcNumReserved() of pfs3aio 19.2 (schijf[] table). */
static ULONG pfs_calc_num_reserved(ULONG sectors, ULONG resblksize)
{
    static const ULONG schijf[6][2] = {
        { 20480UL, 20 }, { 51200UL, 30 }, { 512000UL, 40 },
        { 1048567UL, 50 }, { 10000000UL, 70 }, { 0xFFFFFFFFUL, 80 }
    };
    ULONG temp = sectors / (resblksize / 512);
    ULONG taken = 0, i;
    for (i = 0; temp > schijf[i][0]; i++) {
        taken += schijf[i][0] / schijf[i][1];
        temp  -= schijf[i][0];
    }
    taken += temp / schijf[i][1];
    taken += 10;
    if (taken > PFS_MAXNUMRESERVED) taken = PFS_MAXNUMRESERVED;
    return (taken + 31) & ~31UL;
}

/* Header shared by all reserved-area blocks: id, not_used, datestamp, seqnr. */
static void pfs_hdr(UBYTE *b, ULONG bsz, UWORD id, ULONG datestamp, ULONG seqnr)
{
    memset(b, 0, bsz);
    putw(b, 0, id);
    putl(b, 4, datestamp);
    putl(b, 8, seqnr);
}

struct pfs_rext_args {
    ULONG datestamp, res_roving, deldirsize, superblk;
    UWORD fnsize;
    UWORD cday, cmin, ctick;          /* creation (root_date) */
    UWORD vday, vmin, vtick;          /* volume_date */
    const ULONG *deldir;              /* deldirsize entries */
};

static void pfs_compose_rext(UBYTE *b, ULONG bsz, const struct pfs_rext_args *a)
{
    ULONG i;
    memset(b, 0, bsz);
    putw(b, 0, PFS_EXTENSIONID);
    putl(b, 8, a->datestamp);
    putl(b, 12, PFS_VERSION_192);
    putw(b, 16, a->cday); putw(b, 18, a->cmin); putw(b, 20, a->ctick);
    putw(b, 22, a->vday); putw(b, 24, a->vmin); putw(b, 26, a->vtick);
    putl(b, 44, a->res_roving);
    putw(b, 54, (UWORD)a->deldirsize);
    putw(b, 56, a->fnsize);
    if (a->superblk) putl(b, 64, a->superblk);      /* superindex[0] */
    for (i = 0; i < a->deldirsize; i++)
        putl(b, 144 + 4 * i, a->deldir[i]);
}

static BOOL pfs_format(struct BlockDev *bd, const struct RDBInfo *rdb,
                       const struct PartInfo *pi, char *errbuf, ULONG errlen)
{
    ULONG heads   = pi->heads   > 0 ? pi->heads   : (rdb ? rdb->heads   : 0);
    ULONG sectors = pi->sectors > 0 ? pi->sectors : (rdb ? rdb->sectors : 0);
    ULONG spb     = pi->sectors_per_block > 0 ? pi->sectors_per_block : 1;
    ULONG part_abs, total, resblksize, rescluster, numreserved, nb1k, numblocks;
    ULONG rblkcluster, firstres, lastres, blocksfree, alwaysfree, options;
    ULONG longsperbmb, ipb, bitmapstart, t, no_bmb, no_mi, seq, mi, i, k;
    ULONG disktype, ext0, ext1, ext2, sb = 0, ib, ab, db, ndel;
    ULONG mi_blk[PFS_MAXBITMAPINDEX], dd_blk[PFS_MAXDELDIR];
    ULONG days, mins, ticks;
    UWORD name_len = 0;
    BOOL  supermode = FALSE, ok = FALSE;
    UBYTE *buf = NULL, *ibuf = NULL, *rootbuf = NULL;
    struct pfs_res res;
    struct pfs_rext_args ra;
    char  msg[160];

    memset(&res, 0, sizeof(res));
    memset(&ra, 0, sizeof(ra));

    if (heads == 0 || sectors == 0 || pi->high_cyl < pi->low_cyl) {
        snprintf(msg, sizeof(msg), GS(MSG_NF_BAD_GEOMETRY_FMT),
                 (unsigned long)heads, (unsigned long)sectors);
        set_err(errbuf, errlen, msg); return FALSE;
    }
    if ((pi->block_size > 0 && pi->block_size != 512) || bd->block_size != 512) {
        set_err(errbuf, errlen, GS(MSG_NF_ONLY_512_SECTORS)); return FALSE;
    }
    if (spb != 1) { set_err(errbuf, errlen, GS(MSG_NF_PFS_512_ONLY)); return FALSE; }
    {
        UWORD n = 0;
        const char *nm = pi->volume_name;
        while (nm[n]) { if (nm[n] == ':' || nm[n] == '/') break; n++; }
        if (n == 0 || nm[n]) { set_err(errbuf, errlen, GS(MSG_NF_BAD_NAME)); return FALSE; }
        name_len = n > 31 ? 31 : n;
    }

    part_abs = pi->low_cyl * heads * sectors;
    total    = (pi->high_cyl - pi->low_cyl + 1) * heads * sectors;  /* dg_TotalSectors */
    if (total > PFS_MAXDISKSIZE4K) {
        set_err(errbuf, errlen, GS(MSG_NF_PFS_TOO_LARGE)); return FALSE;
    }

    /* MakeRootBlock: reserved block size + mode */
    resblksize = 1024;
    if (total > PFS_MAXSMALLDISK) {
        supermode = TRUE;
        if (total > PFS_MAXDISKSIZE1K) {
            resblksize = 2048;
            if (total > PFS_MAXDISKSIZE2K) resblksize = 4096;
        }
    }
    disktype    = resblksize > 1024 ? PFS_ID2_DISK : PFS_ID_DISK;
    rescluster  = resblksize / 512;
    longsperbmb = resblksize / 4 - 3;
    ipb         = (resblksize - 12) / 4;
    options     = PFS_MODE_HARDDISK | PFS_MODE_SPLITTED_ANODES | PFS_MODE_DIR_EXTENSION |
                  PFS_MODE_SIZEFIELD | PFS_MODE_DATESTAMP | PFS_MODE_EXTROVING |
                  PFS_MODE_LONGFN | PFS_MODE_EXTENSION |
                  PFS_MODE_DELDIR | PFS_MODE_SUPERDELDIR;
    if (supermode) options |= PFS_MODE_SUPERINDEX;

    numreserved = pfs_calc_num_reserved(total, resblksize);
    firstres    = 2;
    lastres     = rescluster * numreserved + firstres - 1;
    if (lastres + 64 >= total) {
        snprintf(msg, sizeof(msg), GS(MSG_NF_TOO_SMALL_FMT), (unsigned long)total);
        set_err(errbuf, errlen, msg); return FALSE;
    }

    /* MakeReservedBitmap: rootblock cluster size */
    nb1k = 1;
    for (i = 125; i < numreserved / 32; i += 256) nb1k++;
    numblocks   = (1024 * nb1k + resblksize - 1) / resblksize;
    rblkcluster = rescluster * numblocks;
    blocksfree  = total - rescluster * numreserved - firstres;
    alwaysfree  = blocksfree / 20;
    ext0        = firstres + rblkcluster;

    buf     = (UBYTE *)AllocVec(resblksize, MEMF_PUBLIC | MEMF_CLEAR);
    ibuf    = (UBYTE *)AllocVec(resblksize, MEMF_PUBLIC | MEMF_CLEAR);
    rootbuf = (UBYTE *)AllocVec(rblkcluster * 512, MEMF_PUBLIC | MEMF_CLEAR);
    res.bm  = (UBYTE *)AllocVec(((numreserved + 31) / 32 + 1) * 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf || !ibuf || !rootbuf || !res.bm) {
        set_err(errbuf, errlen, GS(MSG_NF_OUT_OF_MEMORY)); goto done;
    }
    res.numreserved = numreserved;
    res.firstres    = firstres;
    res.lastres     = lastres;
    res.rescluster  = rescluster;
    res.free        = numreserved - numblocks - 1;   /* cluster + extension */
    res.roving      = 0;
    for (i = 0; i < numreserved / 32; i++) put32(res.bm, i, 0xFFFFFFFFUL);
    for (i = 0; i < numblocks + 1; i++)
        put32(res.bm, i / 32, get32(res.bm, i / 32) ^ (0x80000000UL >> (i % 32)));

    /* InitAllocation: main bitmap geometry */
    bitmapstart = lastres + 1;
    t      = (total - bitmapstart + 31) / 32;
    no_bmb = (t + longsperbmb - 1) / longsperbmb;
    no_mi  = (no_bmb + ipb - 1) / ipb;
    if (no_mi > (supermode ? PFS_MAXBITMAPINDEX : PFS_MAXSMALLBMI)) {
        set_err(errbuf, errlen, GS(MSG_NF_PFS_TOO_LARGE)); goto done;
    }

    now_datestamp(&days, &mins, &ticks);
    ra.cday = (UWORD)days; ra.cmin = (UWORD)mins; ra.ctick = (UWORD)ticks;
    ra.vday = ra.cday;     ra.vmin = ra.cmin;     ra.vtick = ra.ctick;
    ra.fnsize = PFS_FNSIZE_AMIPART;

    /* Wipe the boot sectors first (no PFS signature on a half-written disk). */
    memset(buf, 0, resblksize);
    if (!write_sectors(bd, part_abs, 0, 2, buf)) goto write_fail;

    /* ---- commit 1 (datestamp 1): bitmap index + bitmap blocks ---- */
    seq = 0;
    for (mi = 0; mi < no_mi; mi++) {
        ULONG s;
        mi_blk[mi] = pfs_res_alloc(&res);
        pfs_hdr(ibuf, resblksize, PFS_BMIBLKID, 1, mi);
        for (s = 0; s < ipb && seq < no_bmb; s++, seq++) {
            ULONG bm = pfs_res_alloc(&res);
            putl(ibuf, 12 + 4 * s, bm);
            pfs_hdr(buf, resblksize, PFS_BMBLKID, 1, seq);
            for (k = 0; k < longsperbmb; k++) putl(buf, 12 + 4 * k, 0xFFFFFFFFUL);
            if (!write_sectors(bd, part_abs, bm, rescluster, buf)) goto write_fail;
        }
        if (!write_sectors(bd, part_abs, mi_blk[mi], rescluster, ibuf)) goto write_fail;
    }

    /* anodes 0..4 (reserved) + 5 (root dir): super/index/anode blocks */
    if (supermode) sb = pfs_res_alloc(&res);
    ib = pfs_res_alloc(&res);
    ab = pfs_res_alloc(&res);
    db = pfs_res_alloc(&res);            /* MakeRootDir */
    if (!ib || !ab || !db || (supermode && !sb)) {
        set_err(errbuf, errlen, GS(MSG_NF_PFS_TOO_LARGE)); goto done;
    }
    if (supermode) {
        pfs_hdr(buf, resblksize, PFS_SBLKID, 1, 0);
        putl(buf, 12, ib);
        if (!write_sectors(bd, part_abs, sb, rescluster, buf)) goto write_fail;
    }
    pfs_hdr(buf, resblksize, PFS_IBLKID, 1, 0);
    putl(buf, 12, ab);
    if (!write_sectors(bd, part_abs, ib, rescluster, buf)) goto write_fail;

    pfs_hdr(buf, resblksize, PFS_ABLKID, 1, 0);
    for (k = 0; k < 5; k++) putl(buf, 16 + 12 * k + 4, 0xFFFFFFFFUL);  /* blocknr */
    putl(buf, 16 + 12 * 5 + 0, 1);       /* anode 5: clustersize 1 */
    putl(buf, 16 + 12 * 5 + 4, db);      /*          blocknr = root dir block */
    if (!write_sectors(bd, part_abs, ab, rescluster, buf)) goto write_fail;

    pfs_hdr(buf, resblksize, PFS_DBLKID, 1, 0);
    putl(buf, 8, 0);                     /* not_used_2, no seqnr here */
    putl(buf, 12, 5);                    /* anodenr = ANODE_ROOTDIR */
    putl(buf, 16, 0);                    /* parent  */
    if (!write_sectors(bd, part_abs, db, rescluster, buf)) goto write_fail;

    /* rootblock extension, first (and later stale) copy */
    ra.datestamp  = 1;
    ra.res_roving = res.roving;
    ra.deldirsize = 0;
    ra.superblk   = sb;
    pfs_compose_rext(buf, resblksize, &ra);
    if (!write_sectors(bd, part_abs, ext0, rescluster, buf)) goto write_fail;

    /* ---- SetDeldir(n) -> commit 2 (datestamp 2) ---- */
    ndel = pi->deldir_blocks ? pi->deldir_blocks : 2;
    if (ndel > PFS_MAXDELDIR) ndel = PFS_MAXDELDIR;
    for (i = 0; i < ndel; i++) {
        dd_blk[i] = pfs_res_alloc(&res);
        if (!dd_blk[i]) { set_err(errbuf, errlen, GS(MSG_NF_PFS_TOO_LARGE)); goto done; }
    }
    ext1 = pfs_res_alloc(&res);          /* MakeBlockDirty(rext): copy-on-write */
    for (i = 0; i < ndel; i++) {
        pfs_hdr(buf, resblksize, PFS_DELDIRID, 2, i);
        putl(buf, 22, PFS_DELENTRY_PROT);
        putw(buf, 26, ra.cday); putw(buf, 28, ra.cmin); putw(buf, 30, ra.ctick);
        if (!write_sectors(bd, part_abs, dd_blk[i], rescluster, buf)) goto write_fail;
    }
    ra.datestamp  = 2;
    ra.res_roving = res.roving;
    ra.deldirsize = ndel;
    ra.deldir     = dd_blk;
    pfs_compose_rext(buf, resblksize, &ra);
    if (!write_sectors(bd, part_abs, ext1, rescluster, buf)) goto write_fail;
    pfs_res_free(&res, ext0);

    /* ---- final commit 3 (datestamp 3): extension moves once more ---- */
    ext2 = pfs_res_alloc(&res);
    pfs_res_free(&res, ext1);
    ra.datestamp  = 3;
    ra.res_roving = res.roving;
    pfs_compose_rext(buf, resblksize, &ra);
    if (!write_sectors(bd, part_abs, ext2, rescluster, buf)) goto write_fail;

    /* rootblock cluster: rootblock + reserved bitmap block */
    memset(rootbuf, 0, rblkcluster * 512);
    putl(rootbuf, 0, disktype);
    putl(rootbuf, 4, options);
    putl(rootbuf, 8, 3);                            /* datestamp */
    putw(rootbuf, 12, ra.cday); putw(rootbuf, 14, ra.cmin); putw(rootbuf, 16, ra.ctick);
    putw(rootbuf, 18, 0xF0);                        /* protection */
    rootbuf[20] = (UBYTE)name_len;
    memcpy(rootbuf + 21, pi->volume_name, name_len);
    putl(rootbuf, 52, lastres);
    putl(rootbuf, 56, firstres);
    putl(rootbuf, 60, res.free);
    putw(rootbuf, 64, (UWORD)resblksize);
    putw(rootbuf, 66, (UWORD)rblkcluster);
    putl(rootbuf, 68, blocksfree);
    putl(rootbuf, 72, alwaysfree);
    putl(rootbuf, 76, 0);                           /* roving_ptr */
    putl(rootbuf, 80, 0);                           /* deldir (old style) */
    putl(rootbuf, 84, total);                       /* disksize */
    putl(rootbuf, 88, ext2);                        /* extension */
    if (supermode) {
        for (mi = 0; mi < no_mi; mi++) putl(rootbuf, 96 + 4 * mi, mi_blk[mi]);
    } else {
        for (mi = 0; mi < no_mi; mi++) putl(rootbuf, 96 + 4 * mi, mi_blk[mi]);
        putl(rootbuf, 96 + 4 * PFS_MAXSMALLBMI, ib);   /* small.indexblocks[0] */
    }
    /* reserved bitmap block directly behind the (512-byte) rootblock */
    putw(rootbuf, 512, PFS_BMBLKID);
    for (i = 0; i < numreserved / 32; i++)
        putl(rootbuf, 512 + 12 + 4 * i, get32(res.bm, i));
    if (!write_sectors(bd, part_abs, firstres, rblkcluster, rootbuf)) goto write_fail;

    /* boot sectors last */
    memset(buf, 0, 1024);
    putl(buf, 0, PFS_ID_DISK);
    if (!write_sectors(bd, part_abs, 0, 2, buf)) goto write_fail;

    ok = TRUE;
    goto done;

write_fail:
    set_err(errbuf, errlen, GS(MSG_NF_WRITE_FAIL_PFS));
done:
    if (buf)     FreeVec(buf);
    if (ibuf)    FreeVec(ibuf);
    if (rootbuf) FreeVec(rootbuf);
    if (res.bm)  FreeVec(res.bm);
    return ok;
}

/* ------------------------------------------------------------------ */
/* SFS - Smart File System 1.279 (SFS\0 and the 64-bit SFS\2 layout)   */
/* ------------------------------------------------------------------ */
/*
 * Ported from the handler's ACTION_FORMAT (AROS rom/filesys/SFS) and
 * verified byte-for-byte against SmartFilesystem 1.279 formatting the
 * same partition under AmiFUSE.  An empty SFS volume is:
 *
 *   block 0 and block total-1   root blocks (identical apart from ownblock)
 *   reserved_start (DE_RESERVEDBLKS, min 1) = admin space container 'ADMC'
 *   +1  root object container 'OBJC' (root dir object + fsRootInfo at the
 *       end of the block)
 *   +2  root hash table 'HTAB' (one entry: .recycled)
 *   +3  'TROK' transaction placeholder
 *   +4  extent B-tree root 'BNDC'
 *   +5  object node container 'NDC ' (node 1 = root, 2 = .recycled)
 *   +6  '.recycled' object container 'OBJC'
 *   admin space covers 32 blocks from the container; the bitmap blocks
 *   ('BTMP', bit set = free, MSB-first) follow the admin space, one per
 *   (blocksize-12)*8 blocks; one block is reserved at the very end
 *   (DE_PREALLOC, min 1) for the second root block.
 *
 * SFS\2 (SFS2, files > 4 GB) differs only in: root id/version 4, two
 * extra bytes in each object header before the modification date, a
 * 16-byte extent node, and the checksum being one less.
 */

#define SFS_ID_ADMC   0x41444D43UL   /* 'ADMC' */
#define SFS_ID_OBJC   0x4F424A43UL   /* 'OBJC' */
#define SFS_ID_HTAB   0x48544142UL   /* 'HTAB' */
#define SFS_ID_TROK   0x54524F4BUL   /* 'TROK' */
#define SFS_ID_BNDC   0x424E4443UL   /* 'BNDC' */
#define SFS_ID_NDC    0x4E444320UL   /* 'NDC ' */
#define SFS_ID_BTMP   0x42544D50UL   /* 'BTMP' */
#define SFS_ROOTNODE      1
#define SFS_RECYCLEDNODE  2
#define SFS_OTYPE_HIDDEN      1
#define SFS_OTYPE_UNDELETABLE 2
#define SFS_OTYPE_QUICKDIR    4
#define SFS_OTYPE_DIR         128
#define SFS_ROOTBITS_RECYCLED 64
#define SFS_ADMIN_BLOCKS      32

struct sfs_ctx { ULONG bsz; BOOL v2; };

/* checksum: the sum of all longwords must be 0xFFFFFFFF (SFS\0) or
   0xFFFFFFFE (SFS\2); the checksum field itself is 0 while summing. */
static void sfs_finish(const struct sfs_ctx *c, UBYTE *b, ULONG own)
{
    ULONG sum = 0, i;
    putl(b, 4, 0);
    putl(b, 8, own);
    for (i = 0; i < c->bsz; i += 4) sum += getl(b, i);
    putl(b, 4, (~sum) - (c->v2 ? 1UL : 0UL));
}

/* SFS name hash (case-insensitive: the volume is formatted that way). */
static UWORD sfs_hash(const char *name)
{
    UWORD h = 0;
    const char *p = name;
    while (*p) { h++; p++; }
    for (p = name; *p; p++) {
        UBYTE ch = (UBYTE)*p;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        else if (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) ch -= 32;
        h = (UWORD)(h * 13 + ch);
    }
    return h;
}

/* fsObject at byte offset off: returns the offset just past name+comment. */
static ULONG sfs_object(const struct sfs_ctx *c, UBYTE *b, ULONG off, ULONG node,
                        ULONG prot, ULONG hashtable, ULONG firstdirblock,
                        ULONG date, UBYTE bits, const char *name, UWORD nlen)
{
    ULONG x = c->v2 ? 2 : 0;
    putl(b, off + 4,  node);
    putl(b, off + 8,  prot);
    putl(b, off + 12, hashtable);
    putl(b, off + 16, firstdirblock);
    putl(b, off + 20 + x, date);
    b[off + 24 + x] = bits;
    memcpy(b + off + 25 + x, name, nlen);
    return off + 25 + x + nlen + 1 + 1;      /* NUL + empty comment NUL */
}

static BOOL sfs_format(struct BlockDev *bd, const struct RDBInfo *rdb,
                       const struct PartInfo *pi, char *errbuf, ULONG errlen)
{
    ULONG heads   = pi->heads   > 0 ? pi->heads   : (rdb ? rdb->heads   : 0);
    ULONG sectors = pi->sectors > 0 ? pi->sectors : (rdb ? rdb->sectors : 0);
    ULONG spb     = pi->sectors_per_block > 0 ? pi->sectors_per_block : 1;
    ULONG bsz     = 512 * spb;
    ULONG part_abs, dev_blocks, total, bits_per_bm, nbm, rs, re, adm, root, bmbase;
    ULONG date, days, mins, ticks, i, k, off;
    UQUAD byte_low, byte_high;
    UWORD name_len = 0, chain;
    UBYTE *buf = NULL;
    BOOL  ok = FALSE;
    struct sfs_ctx c;
    char  msg[160];

    c.bsz = bsz;
    c.v2  = (pi->dos_type == 0x53465302UL);
    if (pi->dos_type != 0x53465300UL && !c.v2) {
        set_err(errbuf, errlen, GS(MSG_NF_SFS_VARIANT)); return FALSE;
    }
    if (heads == 0 || sectors == 0 || pi->high_cyl < pi->low_cyl) {
        snprintf(msg, sizeof(msg), GS(MSG_NF_BAD_GEOMETRY_FMT),
                 (unsigned long)heads, (unsigned long)sectors);
        set_err(errbuf, errlen, msg); return FALSE;
    }
    if ((pi->block_size > 0 && pi->block_size != 512) || bd->block_size != 512) {
        set_err(errbuf, errlen, GS(MSG_NF_ONLY_512_SECTORS)); return FALSE;
    }
    if (bsz < 512 || bsz > 16384 || (bsz & (bsz - 1)) != 0) {
        snprintf(msg, sizeof(msg), GS(MSG_NF_BAD_BLOCKSIZE_FMT), (unsigned long)bsz);
        set_err(errbuf, errlen, msg); return FALSE;
    }
    {
        UWORD n = 0;
        const char *nm = pi->volume_name;
        while (nm[n]) { if (nm[n] == ':' || nm[n] == '/') break; n++; }
        if (n == 0 || nm[n]) { set_err(errbuf, errlen, GS(MSG_NF_BAD_NAME)); return FALSE; }
        name_len = n > 30 ? 30 : n;
    }

    part_abs   = pi->low_cyl * heads * sectors;
    dev_blocks = (pi->high_cyl - pi->low_cyl + 1) * heads * sectors;
    total      = dev_blocks / spb;
    byte_low   = (UQUAD)part_abs * 512ULL;
    byte_high  = (UQUAD)(part_abs + dev_blocks) * 512ULL;

    bits_per_bm = (bsz - 12) * 8;
    nbm    = (total + bits_per_bm - 1) / bits_per_bm;
    rs     = pi->reserved_blks > 0 ? pi->reserved_blks : 1;    /* blocks_reserved_start */
    re     = 1;                                                  /* blocks_reserved_end (DE_PREALLOC 0 -> 1) */
    adm    = rs;                                                 /* admin space container */
    root   = adm + 1;
    bmbase = adm + SFS_ADMIN_BLOCKS;
    if (total < bmbase + nbm + re + 16) {
        snprintf(msg, sizeof(msg), GS(MSG_NF_TOO_SMALL_FMT), (unsigned long)total);
        set_err(errbuf, errlen, msg); return FALSE;
    }

    buf = (UBYTE *)AllocVec(bsz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { set_err(errbuf, errlen, GS(MSG_NF_OUT_OF_MEMORY)); return FALSE; }

    now_datestamp(&days, &mins, &ticks);
    date = days * 86400UL + mins * 60UL + ticks / 50UL;   /* seconds since 1978 */

    /* Kill any old root signature first. */
    memset(buf, 0, bsz);
    if (!write_fs_block(bd, part_abs, 0, spb, buf)) goto write_fail;

    /* admin space container */
    memset(buf, 0, bsz);
    putl(buf, 0, SFS_ID_ADMC);
    buf[20] = SFS_ADMIN_BLOCKS;                     /* ac->bits = blocks_admin */
    putl(buf, 24, adm);                             /* adminspace[0].space */
    putl(buf, 28, 0xFE000000UL);                    /* admin+root+hash+trans+2 nodes+recycled */
    sfs_finish(&c, buf, adm);
    if (!write_fs_block(bd, part_abs, adm, spb, buf)) goto write_fail;

    /* root object container + fsRootInfo */
    memset(buf, 0, bsz);
    putl(buf, 0, SFS_ID_OBJC);
    sfs_object(&c, buf, 24, SFS_ROOTNODE, 0xF, root + 1, root + 5, date,
               SFS_OTYPE_DIR, pi->volume_name, name_len);
    putl(buf, bsz - 36 + 8,  total - SFS_ADMIN_BLOCKS - rs - re - nbm);   /* freeblocks */
    putl(buf, bsz - 36 + 12, date);                                        /* datecreated */
    sfs_finish(&c, buf, root);
    if (!write_fs_block(bd, part_abs, root, spb, buf)) goto write_fail;

    /* root hash table: .recycled hashed in */
    memset(buf, 0, bsz);
    putl(buf, 0, SFS_ID_HTAB);
    putl(buf, 12, SFS_ROOTNODE);
    chain = (UWORD)(sfs_hash(".recycled") % (UWORD)((bsz - 16) >> 2));
    putl(buf, 16 + 4 * chain, SFS_RECYCLEDNODE);
    sfs_finish(&c, buf, root + 1);
    if (!write_fs_block(bd, part_abs, root + 1, spb, buf)) goto write_fail;

    /* transaction placeholder */
    memset(buf, 0, bsz);
    putl(buf, 0, SFS_ID_TROK);
    sfs_finish(&c, buf, root + 2);
    if (!write_fs_block(bd, part_abs, root + 2, spb, buf)) goto write_fail;

    /* extent B-tree root: empty leaf */
    memset(buf, 0, bsz);
    putl(buf, 0, SFS_ID_BNDC);
    buf[14] = 1;                                    /* isleaf   */
    buf[15] = c.v2 ? 16 : 14;                       /* nodesize */
    sfs_finish(&c, buf, root + 3);
    if (!write_fs_block(bd, part_abs, root + 3, spb, buf)) goto write_fail;

    /* object node container: node 1 = root, 2 = .recycled, 3-6 reserved */
    memset(buf, 0, bsz);
    putl(buf, 0, SFS_ID_NDC);
    putl(buf, 12, 1);                               /* nodenumber */
    putl(buf, 16, 1);                               /* nodes (leaf) */
    off = 20;
    putl(buf, off, root);                       off += 10;
    putl(buf, off, root + 5); putw(buf, off + 8, sfs_hash(".recycled")); off += 10;
    for (k = 0; k < 4; k++) { putl(buf, off, 0xFFFFFFFFUL); off += 10; }
    sfs_finish(&c, buf, root + 4);
    if (!write_fs_block(bd, part_abs, root + 4, spb, buf)) goto write_fail;

    /* .recycled object container */
    memset(buf, 0, bsz);
    putl(buf, 0, SFS_ID_OBJC);
    putl(buf, 12, SFS_ROOTNODE);                    /* parent */
    sfs_object(&c, buf, 24, SFS_RECYCLEDNODE, 0xC, 0, 0, date,
               SFS_OTYPE_DIR | SFS_OTYPE_UNDELETABLE | SFS_OTYPE_QUICKDIR | SFS_OTYPE_HIDDEN,
               ".recycled", 9);
    sfs_finish(&c, buf, root + 5);
    if (!write_fs_block(bd, part_abs, root + 5, spb, buf)) goto write_fail;

    /* bitmap blocks */
    {
        LONG startfree = (LONG)(SFS_ADMIN_BLOCKS + nbm + rs);
        LONG sizefree  = (LONG)total - startfree - (LONG)re;
        for (i = 0; i < nbm; i++) {
            memset(buf, 0, bsz);
            putl(buf, 0, SFS_ID_BTMP);
            for (k = 0; k < (bits_per_bm >> 5); k++) {
                if (startfree > 0) {
                    startfree -= 32;
                    if (startfree < 0) {
                        putl(buf, 12 + 4 * k, (1UL << (-startfree)) - 1);
                        sizefree += startfree;
                    }
                } else if (sizefree > 0) {
                    sizefree -= 32;
                    if (sizefree < 0)
                        putl(buf, 12 + 4 * k, ~((1UL << (-sizefree)) - 1));
                    else
                        putl(buf, 12 + 4 * k, 0xFFFFFFFFUL);
                } else {
                    break;
                }
            }
            sfs_finish(&c, buf, bmbase + i);
            if (!write_fs_block(bd, part_abs, bmbase + i, spb, buf)) goto write_fail;
        }
    }

    /* root blocks: last block first, block 0 last */
    memset(buf, 0, bsz);
    putl(buf, 0, pi->dos_type);
    putw(buf, 12, c.v2 ? 4 : 3);                    /* structure version */
    putl(buf, 16, date);
    buf[20] = SFS_ROOTBITS_RECYCLED;
    putl(buf, 32, (ULONG)(byte_low  >> 32));
    putl(buf, 36, (ULONG)(byte_low));
    putl(buf, 40, (ULONG)(byte_high >> 32));
    putl(buf, 44, (ULONG)(byte_high));
    putl(buf, 48, total);
    putl(buf, 52, bsz);
    putl(buf, 96,  bmbase);
    putl(buf, 100, adm);
    putl(buf, 104, root);
    putl(buf, 108, root + 3);
    putl(buf, 112, root + 4);
    sfs_finish(&c, buf, total - 1);
    if (!write_fs_block(bd, part_abs, total - 1, spb, buf)) goto write_fail;
    sfs_finish(&c, buf, 0);
    if (!write_fs_block(bd, part_abs, 0, spb, buf)) goto write_fail;

    ok = TRUE;
    goto done;

write_fail:
    set_err(errbuf, errlen, GS(MSG_NF_WRITE_FAIL_SFS));
done:
    if (buf) FreeVec(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Public engine entry points                                           */
/* ------------------------------------------------------------------ */

BOOL NativeFormat_Supported(ULONG dostype)
{
    return ffs_is_type(dostype) || PFS_IsSupportedType(dostype) ||
           dostype == 0x53465300UL || dostype == 0x53465302UL;
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
    if (PFS_IsSupportedType(pi->dos_type))
        return pfs_format(bd, rdb, pi, errbuf, errlen);
    if (pi->dos_type == 0x53465300UL || pi->dos_type == 0x53465302UL)
        return sfs_format(bd, rdb, pi, errbuf, errlen);

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
