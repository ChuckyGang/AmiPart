/*
 * rescue.c - RDB rescue scan.  See rescue.h.
 *
 * READ-ONLY: nothing in this file writes to the disk.
 *
 * How each filesystem gives its partition away
 * --------------------------------------------
 *   PFS3  rootblock at partition block 2 (id PFS\1 / PFS\2 / AFS\1).  It
 *         stores the partition size (disksize, MODE_SIZEFIELD), so start
 *         and length are exact.  Cross-checked against the reserved bitmap
 *         block ('BM') that follows it.
 *   SFS   root block at partition block 0 (id SFS\0 / SFS\2), checksummed,
 *         stores block size and block count; a second copy sits in the last
 *         block and confirms the length.
 *   FFS   root block (T_SHORT / ST_ROOT, checksummed) in the MIDDLE of the
 *         partition.  It does not store its own block number, so the start
 *         has to be proven: a candidate start S is right when a block the
 *         root's hash table points to sits where S says it must AND names
 *         itself and the root correctly (own key + parent).  Candidate
 *         starts come from DOS\x boot blocks seen earlier in the scan, and
 *         from the standard layout (first bitmap block right behind the
 *         root).  The size is roughly twice the root position; the exact
 *         value (+-1 block) is settled by the neighbouring partition or the
 *         cylinder size.
 *
 * Disk images stored as files (ADF/HDF) look exactly like partitions.
 * They are told apart by position: a find whose start lies inside another
 * find is subordinate (an image file, or a leftover of an older layout) and
 * is left out of the proposal.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "clib.h"
#include "locale_support.h"
#include "rdb.h"
#include "rescue.h"

#define RS_CHUNK      128            /* blocks per scan read (64 KB)       */
#define RS_MAXFSB     32768UL        /* largest filesystem block handled   */
#define RS_BOOT_FIXED 256            /* first boot blocks seen: kept       */
#define RS_BOOT_RING  256            /* plus the most recent ones          */

#define ID_DOS_MASK   0xFFFFFF00UL
#define ID_DOS        0x444F5300UL   /* 'DOS\0' */
#define ID_PFS1       0x50465301UL
#define ID_PFS2       0x50465302UL
#define ID_AFS1       0x41465301UL
#define ID_SFS0       0x53465300UL
#define ID_SFS2       0x53465302UL
#define ID_PFS3_TYPE  0x50465303UL   /* PFS\3: the usual partition DosType */
#define ID_OBJC       0x4F424A43UL

#define PFS_MODE_SIZEFIELD 16

struct RsBoot { ULONG blk; ULONG dostype; };

struct RsCtx {
    struct BlockDev     *bd;
    struct RescueResult *res;
    UBYTE *chunk;                    /* current scan window                */
    UBYTE *aux;                      /* reads outside the window           */
    UBYTE *root;                     /* FFS root block under test          */
    ULONG  chunk_blk, chunk_n;
    ULONG  total;
    ULONG  skip_to;                  /* != 0: resume the scan here         */
    BOOL   deep;
    struct RsBoot *boots;            /* RS_BOOT_FIXED + RS_BOOT_RING       */
    ULONG  nboot;                    /* boot blocks seen so far            */
    ULONG  stamp[RESCUE_MAX_FS];     /* FFS root change time, for ties     */
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static ULONG g32(const UBYTE *b, ULONG off)
{
    b += off;
    return ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) |
           ((ULONG)b[2] <<  8) |  (ULONG)b[3];
}

static ULONG g16(const UBYTE *b, ULONG off)
{
    return ((ULONG)b[off] << 8) | (ULONG)b[off + 1];
}

static ULONG sum_be(const UBYTE *b, ULONG nlongs)
{
    ULONG sum = 0, i;
    for (i = 0; i < nlongs; i++) sum += g32(b, i * 4);
    return sum;
}

static BOOL all_zero(const UBYTE *b, ULONG len)
{
    ULONG i;
    for (i = 0; i < len; i++) if (b[i]) return FALSE;
    return TRUE;
}

static ULONG gcd_ul(ULONG a, ULONG b)
{
    while (b) { ULONG t = a % b; a = b; b = t; }
    return a;
}

/* Copy a length-prefixed (BSTR) or bounded name, replacing anything that
   would garble the report. */
static void copy_name(char *dst, const UBYTE *src, ULONG len)
{
    ULONG i;
    if (len > 31) len = 31;
    for (i = 0; i < len; i++) {
        UBYTE ch = src[i];
        if (ch == 0) break;
        dst[i] = (ch < 32 || ch == 127 || ch == '"') ? '?' : (char)ch;
    }
    dst[i] = '\0';
}

/* n consecutive blocks starting at blk: straight out of the scan window
   when they are in it, otherwise read into c->aux.  The returned pointer
   is only good until the next rs_get(). */
static const UBYTE *rs_get(struct RsCtx *c, ULONG blk, ULONG n)
{
    if (n == 0 || n > RS_MAXFSB / 512) return NULL;
    if (blk >= c->total || n > c->total - blk) return NULL;
    if (blk >= c->chunk_blk && blk + n <= c->chunk_blk + c->chunk_n)
        return c->chunk + (blk - c->chunk_blk) * 512UL;
    if (!BlockDev_ReadBlocks(c->bd, blk, n, c->aux)) return NULL;
    return c->aux;
}

/* TRUE when [start, start+len) touches a find we already trust.  Used to
   decide whether jumping over a new find is safe: only when nothing else
   claims the same ground. */
static BOOL rs_overlaps_known(const struct RsCtx *c, ULONG start, ULONG len)
{
    const struct RescueResult *r = c->res;
    UWORD i;
    for (i = 0; i < r->num_fs; i++) {
        const struct RescueFS *f = &r->fs[i];
        if ((f->flags & RSF_NOSTART) || f->conf < RESCUE_CONF_MED) continue;
        if (start < f->start + f->len && f->start < start + len) return TRUE;
    }
    return FALSE;
}

/* Add a find.  When the table is full the smallest entry makes room: the
   small ones are the disk images stored as files. */
static void rs_add_fs(struct RsCtx *c, const struct RescueFS *fs, ULONG stamp)
{
    struct RescueResult *r = c->res;
    UWORD slot, i;

    if (r->num_fs < RESCUE_MAX_FS) {
        slot = r->num_fs++;
    } else {
        slot = 0;
        for (i = 1; i < RESCUE_MAX_FS; i++)
            if (r->fs[i].len < r->fs[slot].len) slot = i;
        r->nested_dropped++;
        if (r->fs[slot].len >= fs->len) return;
    }
    r->fs[slot] = *fs;
    r->fs[slot].part_idx = -1;
    c->stamp[slot] = stamp;
}

/* ------------------------------------------------------------------ */
/* RDB blocks                                                           */
/* ------------------------------------------------------------------ */

static BOOL rdb_blk_ok(const UBYTE *p)
{
    ULONG sl = g32(p, 4);
    return (BOOL)(sl >= 2 && sl <= 128 && sum_be(p, sl) == 0);
}

static void rs_check_rdsk(struct RsCtx *c, ULONG blk, const UBYTE *p)
{
    struct RescueResult *r = c->res;
    if (r->rdsk_found || !rdb_blk_ok(p)) return;
    r->rdsk_found   = TRUE;
    r->rdsk_block   = blk;
    r->rdsk_cyls    = g32(p, 64);
    r->rdsk_sectors = g32(p, 68);
    r->rdsk_heads   = g32(p, 72);
}

static void rs_check_part(struct RsCtx *c, ULONG blk, const UBYTE *p)
{
    struct RescueResult *r = c->res;
    struct RescuePart *rp;
    struct PartInfo   *pi;
    const UBYTE *e = p + 128;                 /* pb_Environment */
    ULONG ts;
    UQUAD cylblk, s64, e64;

    if (!rdb_blk_ok(p)) return;
    if (r->num_parts >= RESCUE_MAX_PART) { r->part_dropped++; return; }

    rp = &r->parts[r->num_parts++];
    memset(rp, 0, sizeof(*rp));
    rp->block  = blk;
    rp->fs_idx = -1;
    pi = &rp->pi;
    pi->block_num = blk;
    pi->next_part = g32(p, 16);
    pi->flags     = g32(p, 20);
    pi->dev_flags = g32(p, 32);
    copy_name(pi->drive_name, p + 37, p[36]);

    ts = g32(e, 0);
    pi->block_size        = g32(e, 1 * 4) * 4;
    pi->heads             = g32(e, 3 * 4);
    pi->sectors_per_block = (ts >= 4 && g32(e, 4 * 4) > 0) ? g32(e, 4 * 4) : 1;
    pi->sectors           = g32(e, 5 * 4);
    pi->reserved_blks     = g32(e, 6 * 4);
    pi->interleave        = g32(e, 8 * 4);
    pi->low_cyl           = g32(e, 9 * 4);
    pi->high_cyl          = g32(e, 10 * 4);
    pi->num_buffer        = g32(e, 11 * 4);
    pi->buf_mem_type      = g32(e, 12 * 4);
    pi->max_transfer      = g32(e, 13 * 4);
    pi->mask              = g32(e, 14 * 4);
    pi->boot_pri          = (LONG)g32(e, 15 * 4);
    pi->dos_type          = g32(e, 16 * 4);
    pi->baud              = (ts >= 17) ? g32(e, 17 * 4) : 0;
    pi->control           = (ts >= 18) ? g32(e, 18 * 4) : 0;
    pi->boot_blocks       = (ts >= 19) ? g32(e, 19 * 4) : 0;

    if (pi->heads == 0 || pi->sectors == 0 || pi->high_cyl < pi->low_cyl ||
        (pi->block_size != 0 && pi->block_size != 512)) {
        rp->flags |= RPF_BADGEO;
        return;
    }
    cylblk = (UQUAD)pi->heads * pi->sectors;
    s64 = cylblk * pi->low_cyl;
    e64 = cylblk * ((UQUAD)pi->high_cyl + 1);
    if (s64 >= c->total) { rp->flags |= RPF_BADGEO; return; }
    rp->start = (ULONG)s64;
    if (e64 > c->total) {
        rp->flags |= RPF_BEYOND;
        e64 = c->total;
    }
    rp->len = (ULONG)(e64 - s64);
}

static void rs_check_fshd(struct RsCtx *c, ULONG blk, const UBYTE *p)
{
    struct RescueResult *r = c->res;
    if (!rdb_blk_ok(p) || r->num_fshd >= RESCUE_MAX_FSHD) return;
    r->fshd[r->num_fshd].block    = blk;
    r->fshd[r->num_fshd].dos_type = g32(p, 32);
    r->fshd[r->num_fshd].version  = g32(p, 36);
    r->num_fshd++;
}

/* ------------------------------------------------------------------ */
/* Boot-block candidates (possible FFS partition starts)                */
/* ------------------------------------------------------------------ */

static void rs_note_boot(struct RsCtx *c, ULONG blk, ULONG dostype)
{
    ULONG slot;
    if (c->nboot < RS_BOOT_FIXED) slot = c->nboot;
    else slot = RS_BOOT_FIXED + (c->nboot - RS_BOOT_FIXED) % RS_BOOT_RING;
    c->boots[slot].blk     = blk;
    c->boots[slot].dostype = dostype;
    c->nboot++;
}

/* ------------------------------------------------------------------ */
/* PFS3                                                                 */
/* ------------------------------------------------------------------ */

static void rs_check_pfs(struct RsCtx *c, ULONG blk, const UBYTE *p)
{
    struct RescueFS fs;
    ULONG options  = g32(p, 4);
    ULONG lastres  = g32(p, 52);
    ULONG firstres = g32(p, 56);
    ULONG resblk   = g16(p, 64);
    ULONG cluster  = g16(p, 66);
    ULONG disksize = g32(p, 84);
    const UBYTE *q;
    BOOL  bm_ok, boot_ok;

    /* The boot block carries the same id; only a real rootblock passes. */
    if (blk < 2 || firstres != 2 || lastres <= 2) return;
    if (resblk < 1024 || resblk > 8192 || (resblk & 511)) return;
    if (cluster == 0 || cluster > 1024 || p[20] > 31) return;

    memset(&fs, 0, sizeof(fs));
    fs.kind      = RESCUE_FS_PFS;
    fs.sig_block = blk;
    fs.start     = blk - 2;
    fs.fs_bsz    = 512;
    fs.dos_type  = ID_PFS3_TYPE;
    fs.flags     = RSF_TYPEGUESS;
    copy_name(fs.volname, p + 21, p[20]);

    q = rs_get(c, blk + 1, 1);
    bm_ok = (BOOL)(q && g16(q, 0) == 0x424D);          /* 'BM' */
    q = rs_get(c, blk - 2, 1);
    boot_ok = (BOOL)(q && (g32(q, 0) == ID_PFS1 || g32(q, 0) == ID_PFS2 ||
                           g32(q, 0) == ID_AFS1));
    if (!bm_ok && !boot_ok) return;

    if (!(options & PFS_MODE_SIZEFIELD) || disksize <= lastres) {
        /* Pre-size-field volume: start known, length not. */
        fs.flags |= RSF_NOSIZE;
        fs.len = fs.len_min = fs.len_max = lastres + 1;
        fs.conf = RESCUE_CONF_LOW;
        rs_add_fs(c, &fs, 0);
        return;
    }
    fs.len = fs.len_min = fs.len_max = disksize;
    fs.conf = (bm_ok && boot_ok) ? RESCUE_CONF_HIGH : RESCUE_CONF_MED;
    if (disksize > c->total - fs.start) {
        fs.flags |= RSF_BEYOND;
        fs.conf = RESCUE_CONF_LOW;
    }
    if (!c->deep && fs.conf == RESCUE_CONF_HIGH &&
        !rs_overlaps_known(c, fs.start, fs.len))
        c->skip_to = fs.start + fs.len;
    rs_add_fs(c, &fs, 0);
}

/* ------------------------------------------------------------------ */
/* SFS                                                                  */
/* ------------------------------------------------------------------ */

static BOOL sfs_sum_ok(const UBYTE *b, ULONG bsz, ULONG id)
{
    return (BOOL)(sum_be(b, bsz / 4) ==
                  (id == ID_SFS2 ? 0xFFFFFFFEUL : 0xFFFFFFFFUL));
}

static void rs_check_sfs(struct RsCtx *c, ULONG blk, const UBYTE *p)
{
    struct RescueFS fs;
    ULONG id    = g32(p, 0);
    ULONG total = g32(p, 48);
    ULONG bsz   = g32(p, 52);
    ULONG spb, rootobj;
    UQUAD len64;
    const UBYTE *q;

    if (g32(p, 8) != 0) return;                         /* own block = 0 */
    if (bsz < 512 || bsz > RS_MAXFSB || (bsz & (bsz - 1))) return;
    if (total < 64) return;
    spb = bsz / 512;
    q = rs_get(c, blk, spb);
    if (!q || !sfs_sum_ok(q, bsz, id)) return;
    rootobj = g32(q, 104);

    memset(&fs, 0, sizeof(fs));
    fs.kind      = RESCUE_FS_SFS;
    fs.sig_block = blk;
    fs.start     = blk;
    fs.fs_bsz    = bsz;
    fs.dos_type  = id;
    fs.conf      = RESCUE_CONF_MED;

    len64 = (UQUAD)total * spb;
    if (len64 > (UQUAD)(c->total - blk)) {
        fs.flags |= RSF_BEYOND;
        fs.conf = RESCUE_CONF_LOW;
        len64 = c->total - blk;
    } else {
        /* The second root block in the last block confirms the length. */
        q = rs_get(c, blk + (total - 1) * spb, spb);
        if (q && g32(q, 0) == id && g32(q, 8) == total - 1 &&
            sfs_sum_ok(q, bsz, id))
            fs.conf = RESCUE_CONF_HIGH;
    }
    fs.len = fs.len_min = fs.len_max = (ULONG)len64;

    if (rootobj > 0 && rootobj < total) {
        q = rs_get(c, blk + rootobj * spb, spb);
        if (q && g32(q, 0) == ID_OBJC)
            copy_name(fs.volname, q + (id == ID_SFS2 ? 51 : 49), 31);
    }
    if (!c->deep && fs.conf == RESCUE_CONF_HIGH &&
        !rs_overlaps_known(c, fs.start, fs.len))
        c->skip_to = fs.start + fs.len;
    rs_add_fs(c, &fs, 0);
}

/* ------------------------------------------------------------------ */
/* FFS / OFS                                                            */
/* ------------------------------------------------------------------ */

/* Is S the start of the partition whose root block (in c->root) sits rr
   filesystem blocks behind it?
     2 = proven: a block the root's hash table points to names itself and
         the root correctly
     1 = plausible: the root is empty, but its first bitmap block is where
         S says it must be
     0 = no */
static UBYTE ffs_verify(struct RsCtx *c, ULONG S, ULONG rr,
                        ULONG nlongs, ULONG spb)
{
    const UBYTE *root = c->root;
    const UBYTE *q;
    ULONG ht = nlongs - 56, i, h, tried = 0, bm0;
    UQUAD lim = (UQUAD)rr * 2 + 1;           /* highest possible block no. */

    if (rr < 2) return 0;
    for (i = 0; i < ht && tried < 3; i++) {
        h = g32(root, (6 + i) * 4);
        if (h == 0) continue;
        tried++;
        if (h < 2 || h > lim || h == rr) return 0;
        q = rs_get(c, S + h * spb, spb);
        if (q && g32(q, 0) == 2 && g32(q, 4) == h &&
            g32(q, (nlongs - 3) * 4) == rr && sum_be(q, nlongs) == 0)
            return 2;
    }
    if (tried) return 0;

    bm0 = g32(root, (nlongs - 49) * 4);
    if (bm0 < 2 || bm0 > lim || bm0 == rr) return 0;
    q = rs_get(c, S + bm0 * spb, spb);
    if (q && sum_be(q, nlongs) == 0 && !all_zero(q, nlongs * 4)) return 1;
    return 0;
}

static void rs_check_ffs_root(struct RsCtx *c, ULONG blk, const UBYTE *p)
{
    struct RescueFS fs;
    ULONG ht = g32(p, 12), nlongs, bsz, spb;
    ULONG best_s = 0, best_rr = 0, stamp, d1, d2, seq, k, guess[2];
    UBYTE best = 0, str;
    BOOL  best_boot = FALSE;
    ULONG best_dt = 0;
    const UBYTE *q;

    /* Cheap pre-filter on the 512 bytes in hand: T_SHORT, no own key, no
       sequence number, a hash-table size that fits a block size. */
    if (g32(p, 4) != 0 || g32(p, 8) != 0 || g32(p, 16) != 0) return;
    nlongs = ht + 56;
    bsz = nlongs * 4;
    if (bsz < 512 || bsz > 16384 || (bsz & (bsz - 1))) return;
    spb = bsz / 512;
    q = rs_get(c, blk, spb);
    if (!q) return;
    if (g32(q, (nlongs - 1) * 4) != 1) return;          /* ST_ROOT        */
    if (g32(q, (nlongs - 3) * 4) != 0) return;          /* no parent      */
    if (sum_be(q, nlongs) != 0) return;
    memcpy(c->root, q, bsz);

    /* 1. Boot blocks seen so far, most recent first: the newest
          RS_BOOT_RING of them, then the first RS_BOOT_FIXED. */
    seq = c->nboot;
    while (seq > 0 && best < 2) {
        ULONG slot, S;
        seq--;
        if (seq >= RS_BOOT_FIXED) {
            if (c->nboot - seq > RS_BOOT_RING) { seq = RS_BOOT_FIXED; continue; }
            slot = RS_BOOT_FIXED + (seq - RS_BOOT_FIXED) % RS_BOOT_RING;
        } else {
            slot = seq;
        }
        S = c->boots[slot].blk;
        if (S >= blk || (blk - S) % spb) continue;
        str = ffs_verify(c, S, (blk - S) / spb, nlongs, spb);
        if (str > best) {
            best = str; best_s = S; best_rr = (blk - S) / spb;
            best_boot = TRUE; best_dt = c->boots[slot].dostype;
        }
    }

    /* 2. Standard layout: the bitmap extension block, or else the first
          bitmap block, directly follows the root. */
    guess[0] = g32(c->root, (nlongs - 24) * 4);          /* bm_ext        */
    guess[1] = g32(c->root, (nlongs - 49) * 4);          /* bm_pages[0]   */
    for (k = 0; k < 2 && best < 2; k++) {
        ULONG rr, S;
        if (guess[k] < 3) continue;
        rr = guess[k] - 1;
        if ((UQUAD)rr * spb > blk) continue;
        S = blk - rr * spb;
        str = ffs_verify(c, S, rr, nlongs, spb);
        if (str > best) {
            best = str; best_s = S; best_rr = rr;
            q = rs_get(c, S, 1);
            best_boot = (BOOL)(q && (g32(q, 0) & ID_DOS_MASK) == ID_DOS &&
                               (g32(q, 0) & 0xFF) <= 7);
            best_dt = best_boot ? g32(q, 0) : 0;
        }
    }

    memset(&fs, 0, sizeof(fs));
    fs.kind      = RESCUE_FS_FFS;
    fs.sig_block = blk;
    fs.fs_bsz    = bsz;
    copy_name(fs.volname, c->root + (nlongs - 20) * 4 + 1,
              c->root[(nlongs - 20) * 4]);
    /* Last change of the root / of the volume, in seconds: a leftover root
       of an older layout stopped changing when the disk was repartitioned. */
    d1 = g32(c->root, (nlongs - 23) * 4) * 86400UL +
         g32(c->root, (nlongs - 22) * 4) * 60UL + g32(c->root, (nlongs - 21) * 4) / 50UL;
    d2 = g32(c->root, (nlongs - 10) * 4) * 86400UL +
         g32(c->root, (nlongs - 9) * 4) * 60UL + g32(c->root, (nlongs - 8) * 4) / 50UL;
    stamp = d1 > d2 ? d1 : d2;

    if (best == 0) {
        fs.flags = RSF_NOSTART;
        fs.conf  = RESCUE_CONF_LOW;
        rs_add_fs(c, &fs, stamp);
        return;
    }

    fs.start   = best_s;
    fs.len     = best_rr * 2 * spb;
    fs.len_min = (best_rr * 2 - 1) * spb;
    fs.len_max = (best_rr * 2 + 2) * spb - 1;
    if (best_boot) {
        fs.dos_type = best_dt;
    } else {
        /* Newer formatters (and every DOS\6 / DOS\7 volume) keep a copy
           of the DosType in the root block. */
        ULONG dt = g32(c->root, (nlongs - 4) * 4);
        fs.flags |= RSF_NOBOOT;
        if ((dt & ID_DOS_MASK) == ID_DOS && (dt & 0xFF) <= 7) fs.dos_type = dt;
    }
    fs.conf = (best == 2) ? RESCUE_CONF_HIGH
            : (best_boot ? RESCUE_CONF_MED : RESCUE_CONF_LOW);
    if (fs.len_min > c->total - fs.start) {
        fs.flags |= RSF_BEYOND;
        fs.conf = RESCUE_CONF_LOW;
        fs.len = fs.len_min = fs.len_max = c->total - fs.start;
    } else {
        if (fs.len_max > c->total - fs.start) fs.len_max = c->total - fs.start;
        if (fs.len > fs.len_max) fs.len = fs.len_max;
    }
    /* Jump to the earliest block the next partition could start at. */
    if (!c->deep && fs.conf == RESCUE_CONF_HIGH && fs.start != 0 &&
        !rs_overlaps_known(c, fs.start, fs.len_max))
        c->skip_to = fs.start + fs.len_min;
    rs_add_fs(c, &fs, stamp);
}

/* ------------------------------------------------------------------ */
/* Per-block dispatch                                                   */
/* ------------------------------------------------------------------ */

static void rs_classify(struct RsCtx *c, ULONG blk, const UBYTE *p)
{
    ULONG id = g32(p, 0);

    switch (id) {
    case 0:                return;
    case IDNAME_RIGIDDISK: rs_check_rdsk(c, blk, p); return;
    case IDNAME_PARTITION: rs_check_part(c, blk, p); return;
    case IDNAME_FSHEADER:  rs_check_fshd(c, blk, p); return;
    case IDNAME_LOADSEG:   if (rdb_blk_ok(p)) c->res->num_lseg++; return;
    case 2:                rs_check_ffs_root(c, blk, p); return;
    case ID_PFS1:
    case ID_PFS2:
    case ID_AFS1:          rs_check_pfs(c, blk, p); return;
    case ID_SFS0:
    case ID_SFS2:          rs_check_sfs(c, blk, p); return;
    default:
        if ((id & ID_DOS_MASK) == ID_DOS && (id & 0xFF) <= 7)
            rs_note_boot(c, blk, id);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Resolution: which finds are partitions, which are images or stale    */
/* ------------------------------------------------------------------ */

static BOOL fs_anchored(const struct RescueResult *r, const struct RescueFS *f,
                        const UBYTE *part_ok)
{
    UWORD i;
    if (f->flags & RSF_NOSTART) return FALSE;
    for (i = 0; i < r->num_parts; i++)
        if (part_ok[i] && r->parts[i].start == f->start) return TRUE;
    return FALSE;
}

/* Sort order: by start; on a tie the find a PART block agrees with first,
   then the one with its boot block, then the more recently changed, then
   the larger.  Finds without a start go last. */
static BOOL fs_before(const struct RescueFS *a, ULONG sa, BOOL aa,
                      const struct RescueFS *b, ULONG sb, BOOL ab)
{
    BOOL na = (BOOL)((a->flags & RSF_NOSTART) != 0);
    BOOL nb = (BOOL)((b->flags & RSF_NOSTART) != 0);
    if (na != nb) return nb;
    if (na) return (BOOL)(a->sig_block < b->sig_block);
    if (a->start != b->start) return (BOOL)(a->start < b->start);
    if (aa != ab) return aa;
    if ((a->flags & RSF_NOBOOT) != (b->flags & RSF_NOBOOT))
        return (BOOL)!(a->flags & RSF_NOBOOT);
    if (sa != sb) return (BOOL)(sa > sb);
    return (BOOL)(a->len > b->len);
}

/* Largest divisor of n that is <= cap. */
static ULONG largest_div(ULONG n, ULONG cap)
{
    ULONG d;
    for (d = cap; d > 1; d--) if (n % d == 0) return d;
    return 1;
}

static void rs_resolve(struct RsCtx *c)
{
    struct RescueResult *r = c->res;
    UBYTE part_ok[RESCUE_MAX_PART];
    UBYTE anch[RESCUE_MAX_FS];
    ULONG m = 0xFFFFFFFFUL, cylblk, g;
    UWORD i, j, n;
    WORD  top;

    /* --- PART blocks: a real one sits in front of every partition.  One
           that sits at or behind the lowest partition start is part of a
           disk image stored as a file. */
    for (i = 0; i < r->num_parts; i++) {
        struct RescuePart *p = &r->parts[i];
        if (p->flags & RPF_BADGEO) continue;
        if (p->block < p->start && p->start < m) m = p->start;
    }
    for (i = 0; i < r->num_parts; i++) {
        struct RescuePart *p = &r->parts[i];
        if (!(p->flags & RPF_BADGEO) && p->block >= m) p->flags |= RPF_NESTED;
        part_ok[i] = (UBYTE)!(p->flags & (RPF_BADGEO | RPF_NESTED));
    }

    /* --- Sort the filesystem finds. */
    for (i = 0; i < r->num_fs; i++) anch[i] = (UBYTE)fs_anchored(r, &r->fs[i], part_ok);
    for (i = 1; i < r->num_fs; i++) {
        struct RescueFS t = r->fs[i];
        ULONG ts = c->stamp[i];
        UBYTE ta = anch[i];
        for (j = i; j > 0 &&
             fs_before(&t, ts, ta, &r->fs[j - 1], c->stamp[j - 1], anch[j - 1]); j--) {
            r->fs[j] = r->fs[j - 1]; c->stamp[j] = c->stamp[j - 1]; anch[j] = anch[j - 1];
        }
        r->fs[j] = t; c->stamp[j] = ts; anch[j] = ta;
    }

    /* --- Sweep front to back.  A find whose start lies inside the current
           top-level find is subordinate: fully inside = an image file
           (dropped), otherwise a leftover of an older layout (flagged). */
    top = -1;
    for (i = 0; i < r->num_fs; i++) {
        struct RescueFS *f = &r->fs[i];
        if (f->flags & RSF_NOSTART) {
            for (j = 0; j < i; j++) {
                struct RescueFS *t = &r->fs[j];
                if (t->kind && !(t->flags & (RSF_NOSTART | RSF_OVERLAP)) &&
                    t->start != 0 && f->sig_block >= t->start &&
                    f->sig_block - t->start < t->len_min) { f->kind = 0; break; }
            }
            continue;
        }
        if (f->start == 0) continue;            /* whole-disk volume: never a parent */
        if (f->conf < RESCUE_CONF_MED) {
            if (top >= 0 && f->start - r->fs[top].start < r->fs[top].len_min)
                f->flags |= RSF_OVERLAP;
            continue;
        }
        if (top >= 0 && f->start - r->fs[top].start < r->fs[top].len_min) {
            struct RescueFS *t = &r->fs[top];
            if (anch[i] && !anch[top]) {
                t->flags |= RSF_OVERLAP;
                top = (WORD)i;
            } else if (f->start != t->start &&
                       f->start - t->start + f->len <= t->len_max) {
                f->kind = 0;
            } else {
                f->flags |= RSF_OVERLAP;
            }
        } else {
            top = (WORD)i;
        }
    }
    for (i = 0, n = 0; i < r->num_fs; i++) {
        if (r->fs[i].kind == 0) { r->nested_dropped++; continue; }
        if (n != i) r->fs[n] = r->fs[i];
        n++;
    }
    r->num_fs = n;

    /* --- A PART block inside a top-level find is part of an image too. */
    for (i = 0; i < r->num_parts; i++) {
        struct RescuePart *p = &r->parts[i];
        if (!part_ok[i]) continue;
        for (j = 0; j < r->num_fs; j++) {
            struct RescueFS *f = &r->fs[j];
            if ((f->flags & (RSF_NOSTART | RSF_OVERLAP)) || f->start == 0 ||
                f->conf < RESCUE_CONF_MED) continue;
            if (p->block >= f->start && p->block - f->start < f->len) {
                p->flags |= RPF_NESTED; part_ok[i] = 0; break;
            }
        }
    }

    /* --- Link PART blocks and filesystems; PART-vs-PART overlaps. */
    for (i = 0; i < r->num_parts; i++) {
        struct RescuePart *p = &r->parts[i];
        if (!part_ok[i]) continue;
        for (j = 0; j < r->num_fs; j++) {
            struct RescueFS *f = &r->fs[j];
            if ((f->flags & RSF_NOSTART) || f->start != p->start || f->part_idx >= 0)
                continue;
            f->part_idx = (WORD)i;
            p->fs_idx   = (WORD)j;
            if (p->len >= f->len_min && p->len <= f->len_max) {
                f->len = f->len_min = f->len_max = p->len;      /* settled */
            } else if (!(f->flags & RSF_NOSIZE)) {
                f->flags |= RSF_SIZEDIFF;
            }
            break;
        }
        for (j = 0; j < i; j++) {
            struct RescuePart *o = &r->parts[j];
            if (!part_ok[j]) continue;
            if (p->start < o->start + o->len && o->start < p->start + p->len) {
                p->flags |= RPF_OVERLAP; o->flags |= RPF_OVERLAP;
            }
        }
    }
    /* A filesystem no PART block names, on ground a PART block claims. */
    for (j = 0; j < r->num_fs; j++) {
        struct RescueFS *f = &r->fs[j];
        if (f->part_idx >= 0 || (f->flags & RSF_NOSTART)) continue;
        for (i = 0; i < r->num_parts; i++) {
            struct RescuePart *p = &r->parts[i];
            if (!part_ok[i]) continue;
            if (f->start < p->start + p->len && p->start < f->start + f->len_min)
                f->flags |= RSF_OVERLAP;
        }
    }

    /* --- FFS length: the next find or the end of the disk settles it. */
    for (i = 0; i < r->num_fs; i++) {
        struct RescueFS *f = &r->fs[i];
        ULONG bound = c->total;
        if (f->kind != RESCUE_FS_FFS || f->part_idx >= 0 ||
            (f->flags & (RSF_NOSTART | RSF_BEYOND))) continue;
        for (j = 0; j < r->num_fs; j++) {
            struct RescueFS *o = &r->fs[j];
            if (j == i || (o->flags & (RSF_NOSTART | RSF_OVERLAP))) continue;
            if (o->start > f->start && o->start < bound) bound = o->start;
        }
        for (j = 0; j < r->num_parts; j++)
            if (part_ok[j] && r->parts[j].start > f->start &&
                r->parts[j].start < bound) bound = r->parts[j].start;
        if (bound - f->start >= f->len_min && bound - f->start <= f->len_max) {
            f->len = bound - f->start;
            f->len_min = f->len_max = f->len;       /* settled */
        }
    }

    /* --- Geometry for the proposal. */
    r->geo_src = RESCUE_GEO_NONE;
    if (r->rdsk_found && r->rdsk_heads && r->rdsk_sectors) {
        r->geo_src = RESCUE_GEO_RDSK;
        r->heads = r->rdsk_heads; r->sectors = r->rdsk_sectors;
    } else {
        for (i = 0; i < r->num_parts; i++)
            if (part_ok[i]) {
                r->geo_src = RESCUE_GEO_PART;
                r->heads = r->parts[i].pi.heads; r->sectors = r->parts[i].pi.sectors;
                break;
            }
    }
    if (r->geo_src == RESCUE_GEO_NONE) {
        /* Every start and every exactly-known length is a whole number of
           cylinders, so the cylinder size divides their gcd. */
        g = 0;
        for (i = 0; i < r->num_fs; i++) {
            struct RescueFS *f = &r->fs[i];
            if ((f->flags & (RSF_NOSTART | RSF_OVERLAP | RSF_BEYOND | RSF_NOSIZE)) ||
                f->start == 0 || f->conf < RESCUE_CONF_MED) continue;
            g = gcd_ul(g, f->start);
            if (f->len_min == f->len_max) g = gcd_ul(g, f->len);
        }
        if (g) {
            for (i = 0; i < r->num_fs; i++) {
                struct RescueFS *f = &r->fs[i];
                ULONG e;
                if ((f->flags & (RSF_NOSTART | RSF_OVERLAP | RSF_BEYOND | RSF_NOSIZE)) ||
                    f->start == 0 || f->conf < RESCUE_CONF_MED ||
                    f->len_min == f->len_max) continue;
                e = ((f->start + f->len_max) / g) * g;      /* aligned end */
                if (e >= f->start + f->len_min) f->len = e - f->start;
                else g = gcd_ul(g, f->start + f->len);
            }
            if (g % 1008 == 0) { r->heads = 16; r->sectors = 63; }
            else {
                r->sectors = largest_div(g, 255);
                r->heads   = largest_div(g / r->sectors, 255);
            }
            r->geo_src = RESCUE_GEO_DERIVED;
        }
    }
    if (r->geo_src == RESCUE_GEO_NONE) return;
    cylblk = r->heads * r->sectors;
    r->cylinders = (r->geo_src == RESCUE_GEO_RDSK && r->rdsk_cyls)
                 ? r->rdsk_cyls : c->total / cylblk;

    /* --- Fit the filesystem-only finds to that geometry. */
    for (i = 0; i < r->num_fs; i++) {
        struct RescueFS *f = &r->fs[i];
        if (f->part_idx >= 0 || (f->flags & (RSF_NOSTART | RSF_NOSIZE))) continue;
        if (f->len_min != f->len_max) {
            ULONG e = ((f->start + f->len_max) / cylblk) * cylblk;
            if (e >= f->start + f->len_min)
                f->len = f->len_min = f->len_max = e - f->start;   /* settled */
        }
        if (f->start % cylblk || f->len % cylblk) f->flags |= RSF_MISALIGNED;
    }
}

/* ------------------------------------------------------------------ */
/* Rescue_Scan                                                          */
/* ------------------------------------------------------------------ */

BOOL Rescue_Scan(struct BlockDev *bd, BOOL deep,
                 RescueProgFn prog, void *ud,
                 struct RescueResult *res, char *err)
{
    struct RsCtx *c;
    ULONG blk, total;
    BOOL  ok = FALSE;

    memset(res, 0, sizeof(*res));
    err[0] = '\0';
    if (!bd || bd->block_size != 512) {
        strncpy(err, GS(MSG_RS_ERR_512), 95); err[95] = '\0';
        return FALSE;
    }
    total = (bd->total_bytes / 512 > 0xFFFFFFFFULL)
          ? 0xFFFFFFFFUL : (ULONG)(bd->total_bytes / 512);
    if (total == 0) {
        strncpy(err, GS(MSG_CLI_BBSCAN_NO_SIZE), 95); err[95] = '\0';
        return FALSE;
    }
    res->total_blocks = total;

    c = (struct RsCtx *)AllocVec(sizeof(*c), MEMF_PUBLIC | MEMF_CLEAR);
    if (c) {
        c->chunk = (UBYTE *)AllocVec(RS_CHUNK * 512UL, MEMF_PUBLIC);
        c->aux   = (UBYTE *)AllocVec(RS_MAXFSB, MEMF_PUBLIC);
        c->root  = (UBYTE *)AllocVec(16384, MEMF_PUBLIC);
        c->boots = (struct RsBoot *)AllocVec(
            (RS_BOOT_FIXED + RS_BOOT_RING) * sizeof(struct RsBoot), MEMF_PUBLIC);
    }
    if (!c || !c->chunk || !c->aux || !c->root || !c->boots) {
        strncpy(err, GS(MSG_CLI_OUT_OF_MEMORY), 95); err[95] = '\0';
        goto done;
    }
    c->bd = bd; c->res = res; c->total = total; c->deep = deep;

    blk = 0;
    while (blk < total) {
        ULONG n = total - blk, i, next;
        if (n > RS_CHUNK) n = RS_CHUNK;
        if (prog && !prog(ud, blk, total)) { res->cancelled = TRUE; break; }

        if (!BlockDev_ReadBlocks(bd, blk, n, c->chunk)) {
            for (i = 0; i < n; i++)
                if (!BlockDev_ReadBlock(bd, blk + i, c->chunk + i * 512UL)) {
                    memset(c->chunk + i * 512UL, 0, 512);
                    res->read_errors++;
                }
        }
        c->chunk_blk = blk; c->chunk_n = n;
        res->scanned += n;

        next = blk + n;
        for (i = 0; i < n; i++) {
            c->skip_to = 0;
            rs_classify(c, blk + i, c->chunk + i * 512UL);
            if (c->skip_to > blk + i + 1) {
                if (c->skip_to >= blk + n) { next = c->skip_to; break; }
                i = c->skip_to - blk - 1;       /* still inside this window */
            }
        }
        if (next > blk + n) res->skipped += next - (blk + n);
        blk = next;
    }
    if (!res->cancelled && prog) prog(ud, total, total);

    rs_resolve(c);
    ok = TRUE;

done:
    if (c) {
        if (c->chunk) FreeVec(c->chunk);
        if (c->aux)   FreeVec(c->aux);
        if (c->root)  FreeVec(c->root);
        if (c->boots) FreeVec(c->boots);
        FreeVec(c);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Rescue_Report                                                        */
/* ------------------------------------------------------------------ */

#define RS_LINE(...) do { snprintf(line, sizeof(line), __VA_ARGS__); \
                          emit(ud, line); } while (0)

static const char *kind_name(UBYTE kind)
{
    return kind == RESCUE_FS_FFS ? "FFS/OFS"
         : kind == RESCUE_FS_PFS ? "PFS3" : "SFS";
}

/* May this PART-described partition go into the proposal? */
static BOOL part_usable(const struct RescuePart *p)
{
    if (p->flags & (RPF_NESTED | RPF_BADGEO | RPF_BEYOND)) return FALSE;
    if ((p->flags & RPF_OVERLAP) && p->fs_idx < 0) return FALSE;
    return TRUE;
}

/* May this filesystem-only find go into the proposal? */
static BOOL fs_usable(const struct RescueFS *f)
{
    if (f->part_idx >= 0) return FALSE;         /* listed via its PART block */
    if (f->conf < RESCUE_CONF_MED || f->start == 0) return FALSE;
    if (f->flags & (RSF_NOSTART | RSF_OVERLAP | RSF_BEYOND |
                    RSF_MISALIGNED | RSF_NOSIZE)) return FALSE;
    return TRUE;
}

void Rescue_Report(const struct RescueResult *r, RescueEmitFn emit, void *ud)
{
    char  line[256], dt[16], sz[16];
    UWORD i, listed = 0, left_out = 0;
    ULONG cylblk, last;
    BOOL  any;

    emit(ud, GS(MSG_RS_HDR));
    RS_LINE(GS(MSG_RS_SCANNED_FMT), (unsigned long)r->scanned,
            (unsigned long)r->skipped, (unsigned long)r->read_errors);
    if (r->cancelled) emit(ud, GS(MSG_RS_CANCELLED));

    /* ---- RDB blocks ---- */
    emit(ud, GS(MSG_RS_HDR_RDB));
    if (r->rdsk_found)
        RS_LINE(GS(MSG_RS_RDSK_FMT), (unsigned long)r->rdsk_block,
                (unsigned long)r->rdsk_cyls, (unsigned long)r->rdsk_heads,
                (unsigned long)r->rdsk_sectors);
    else
        emit(ud, GS(MSG_RS_NO_RDSK));
    if (r->num_parts == 0) emit(ud, GS(MSG_RS_NO_PART));
    for (i = 0; i < r->num_parts; i++) {
        const struct RescuePart *p = &r->parts[i];
        FormatDosType(p->pi.dos_type, dt);
        FormatSize((UQUAD)p->len * 512, sz);
        RS_LINE(GS(MSG_RS_PART_FMT), (unsigned long)p->block, p->pi.drive_name,
                (unsigned long)p->pi.low_cyl, (unsigned long)p->pi.high_cyl, dt, sz);
        if (p->flags & RPF_BADGEO)  { emit(ud, GS(MSG_RS_PART_BADGEO)); continue; }
        if (p->flags & RPF_NESTED)  { emit(ud, GS(MSG_RS_PART_NESTED)); continue; }
        if (p->flags & RPF_BEYOND)  emit(ud, GS(MSG_RS_PART_BEYOND));
        if (p->flags & RPF_OVERLAP) emit(ud, GS(MSG_RS_PART_OVERLAP));
        if (p->fs_idx >= 0)
            RS_LINE(GS(MSG_RS_PART_FSOK_FMT), kind_name(r->fs[p->fs_idx].kind),
                    (unsigned long)p->start);
        else
            RS_LINE(GS(MSG_RS_PART_NOFS_FMT), (unsigned long)p->start);
    }
    if (r->part_dropped)
        RS_LINE(GS(MSG_RS_PART_DROPPED_FMT), (unsigned long)r->part_dropped);
    for (i = 0; i < r->num_fshd; i++) {
        FormatDosType(r->fshd[i].dos_type, dt);
        RS_LINE(GS(MSG_RS_FSHD_FMT), (unsigned long)r->fshd[i].block, dt,
                (unsigned long)(r->fshd[i].version >> 16),
                (unsigned long)(r->fshd[i].version & 0xFFFF));
    }
    if (r->num_lseg)
        RS_LINE(GS(MSG_RS_LSEG_FMT), (unsigned long)r->num_lseg);

    /* ---- Filesystems ---- */
    emit(ud, GS(MSG_RS_HDR_FS));
    if (r->num_fs == 0) emit(ud, GS(MSG_RS_NO_FS));
    for (i = 0; i < r->num_fs; i++) {
        const struct RescueFS *f = &r->fs[i];
        const char *conf = f->conf == RESCUE_CONF_HIGH ? GS(MSG_RS_CONF_HIGH)
                         : f->conf == RESCUE_CONF_MED  ? GS(MSG_RS_CONF_MED)
                         : GS(MSG_RS_CONF_LOW);
        if (f->flags & RSF_NOSTART) {
            RS_LINE(GS(MSG_RS_FS_NOSTART_FMT), kind_name(f->kind),
                    (unsigned long)f->sig_block, f->volname);
            continue;
        }
        FormatSize((UQUAD)f->len * 512, sz);
        RS_LINE(GS(MSG_RS_FS_FMT), kind_name(f->kind), (unsigned long)f->start,
                (unsigned long)f->len, sz, f->volname, conf);
        if (f->dos_type) {
            FormatDosType(f->dos_type, dt);
            RS_LINE(GS((f->flags & RSF_TYPEGUESS) ? MSG_RS_FS_TYPEGUESS_FMT
                                                  : MSG_RS_FS_TYPE_FMT),
                    dt, (unsigned long)f->fs_bsz);
        } else {
            RS_LINE(GS(MSG_RS_FS_TYPEUNK_FMT), (unsigned long)f->fs_bsz);
        }
        if (f->start == 0)             emit(ud, GS(MSG_RS_FS_WHOLEDISK));
        if (f->flags & RSF_NOBOOT)     emit(ud, GS(MSG_RS_FS_NOBOOT));
        if (f->flags & RSF_NOSIZE)     emit(ud, GS(MSG_RS_FS_NOSIZE));
        if (f->len_min != f->len_max && !(f->flags & RSF_NOSIZE))
            RS_LINE(GS(MSG_RS_FS_SIZERANGE_FMT), (unsigned long)f->len_min,
                    (unsigned long)f->len_max);
        if (f->flags & RSF_BEYOND)     emit(ud, GS(MSG_RS_FS_BEYOND));
        if (f->flags & RSF_OVERLAP)    emit(ud, GS(MSG_RS_FS_OVERLAP));
        if ((f->flags & RSF_SIZEDIFF) && f->part_idx >= 0)
            RS_LINE(GS(MSG_RS_FS_SIZEDIFF_FMT),
                    (unsigned long)r->parts[f->part_idx].len, (unsigned long)f->len);
        if ((f->flags & RSF_MISALIGNED) && f->part_idx < 0)
            emit(ud, GS(MSG_RS_FS_MISALIGNED));
    }
    if (r->nested_dropped)
        RS_LINE(GS(MSG_RS_NESTED_FMT), (unsigned long)r->nested_dropped);

    /* ---- Proposal ---- */
    any = FALSE;
    for (i = 0; i < r->num_parts; i++) if (part_usable(&r->parts[i])) any = TRUE;
    for (i = 0; i < r->num_fs; i++)    if (fs_usable(&r->fs[i]))      any = TRUE;
    if (!any || r->geo_src == RESCUE_GEO_NONE) {
        emit(ud, GS(MSG_RS_PROP_NONE));
        return;
    }
    cylblk = r->heads * r->sectors;
    RS_LINE(GS(MSG_RS_HDR_PROP_FMT), (unsigned long)r->cylinders,
            (unsigned long)r->heads, (unsigned long)r->sectors,
            r->geo_src == RESCUE_GEO_RDSK ? GS(MSG_RS_GEO_RDSK)
          : r->geo_src == RESCUE_GEO_PART ? GS(MSG_RS_GEO_PART)
          : GS(MSG_RS_GEO_DERIVED));

    /* Lowest start first: walk both tables, taking the next start above
       the last one printed. */
    last = 0;
    for (;;) {
        const struct RescuePart *bp = NULL;
        const struct RescueFS   *bf = NULL;
        ULONG best = 0xFFFFFFFFUL;
        for (i = 0; i < r->num_parts; i++) {
            const struct RescuePart *p = &r->parts[i];
            if (part_usable(p) && p->start >= last && p->start < best)
                { best = p->start; bp = p; bf = NULL; }
        }
        for (i = 0; i < r->num_fs; i++) {
            const struct RescueFS *f = &r->fs[i];
            if (fs_usable(f) && f->start >= last && f->start < best)
                { best = f->start; bf = f; bp = NULL; }
        }
        if (!bp && !bf) break;
        if (bp) {
            FormatDosType(bp->pi.dos_type, dt);
            FormatSize((UQUAD)bp->len * 512, sz);
            RS_LINE(GS(MSG_RS_PROP_FMT), bp->pi.drive_name,
                    (unsigned long)bp->pi.low_cyl, (unsigned long)bp->pi.high_cyl,
                    (unsigned long)bp->start, (unsigned long)(bp->start + bp->len - 1),
                    dt, sz,
                    bp->fs_idx >= 0 ? GS(MSG_RS_SRC_PART_FS) : GS(MSG_RS_SRC_PART));
        } else {
            if (bf->dos_type) FormatDosType(bf->dos_type, dt);
            else { dt[0] = '?'; dt[1] = '\0'; }
            FormatSize((UQUAD)bf->len * 512, sz);
            RS_LINE(GS(MSG_RS_PROP_FMT), "-",
                    (unsigned long)(bf->start / cylblk),
                    (unsigned long)((bf->start + bf->len) / cylblk - 1),
                    (unsigned long)bf->start, (unsigned long)(bf->start + bf->len - 1),
                    dt, sz, GS(MSG_RS_SRC_FS));
        }
        listed++;
        last = best + 1;
    }
    for (i = 0; i < r->num_parts; i++)
        if (!part_usable(&r->parts[i]) &&
            !(r->parts[i].flags & RPF_NESTED)) left_out++;
    for (i = 0; i < r->num_fs; i++)
        if (r->fs[i].part_idx < 0 && !fs_usable(&r->fs[i])) left_out++;
    if (left_out)
        RS_LINE(GS(MSG_RS_PROP_LEFTOUT_FMT), (unsigned long)left_out);
    (void)listed;
}

/* ------------------------------------------------------------------ */
/* Rescue_Apply                                                         */
/* ------------------------------------------------------------------ */

static BOOL name_eq_ci(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return FALSE;
        if (!x) return TRUE;
    }
}

static BOOL name_in_use(const struct RDBInfo *rdb, const char *name)
{
    UWORD i;
    for (i = 0; i < rdb->num_parts; i++)
        if (name_eq_ci(rdb->parts[i].drive_name, name)) return TRUE;
    return FALSE;
}

/* Lowest free DHn. */
static void free_name(const struct RDBInfo *rdb, char *out)
{
    ULONG n;
    for (n = 0; n < 1000; n++) {
        sprintf(out, "DH%lu", (unsigned long)n);
        if (!name_in_use(rdb, out)) return;
    }
}

/* Device-block range of a partition already in the table. */
static void part_range(const struct RDBInfo *rdb, const struct PartInfo *pi,
                       UQUAD *lo, UQUAD *hi)
{
    UQUAD cb = (UQUAD)(pi->heads   ? pi->heads   : rdb->heads) *
                      (pi->sectors ? pi->sectors : rdb->sectors);
    *lo = cb * pi->low_cyl;
    *hi = cb * ((UQUAD)pi->high_cyl + 1);
}

/* Load the driver code behind a surviving FSHD block.  Strict: every LSEG
   block must be readable and checksum-clean, or the driver is refused. */
static BOOL load_fshd(struct BlockDev *bd, ULONG blk, struct FSInfo *fi)
{
    UBYTE *buf, *code;
    ULONG  max_blocks = MAX_FS_CODE_SIZE / 492UL + 4UL;
    ULONG  n = 0, off = 0, lseg, pass;
    BOOL   ok = FALSE;

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC);
    if (!buf) return FALSE;
    if (!BlockDev_ReadBlock(bd, blk, buf) ||
        g32(buf, 0) != IDNAME_FSHEADER || !rdb_blk_ok(buf)) goto out;

    memset(fi, 0, sizeof(*fi));
    fi->flags        = g32(buf, 20);
    fi->dos_type     = g32(buf, 32);
    fi->version      = g32(buf, 36);
    fi->patch_flags  = g32(buf, 40);
    fi->stack_size   = g32(buf, 60);
    fi->priority     = (LONG)g32(buf, 64);
    fi->global_vec   = (LONG)g32(buf, 76);
    fi->seg_list_blk = g32(buf, 72);
    fi->seg_last_blk = RDB_END_MARK;
    memcpy(fi->fs_name, buf + 172, 84);
    fi->fs_name[83] = '\0';

    /* Pass 0 counts the chain, pass 1 copies it. */
    code = NULL;
    for (pass = 0; pass < 2; pass++) {
        ULONG cnt = 0;
        lseg = fi->seg_list_blk;
        while (lseg != RDB_END_MARK) {
            ULONG sl, bytes;
            if (lseg == 0 || cnt >= max_blocks) goto out_code;
            if (!BlockDev_ReadBlock(bd, lseg, buf) ||
                g32(buf, 0) != IDNAME_LOADSEG || !rdb_blk_ok(buf)) goto out_code;
            sl = g32(buf, 4);
            bytes = (sl > 5) ? (sl - 5) * 4UL : 0;
            if (bytes > 492) bytes = 492;
            if (pass == 1) { memcpy(code + off, buf + 20, bytes); off += bytes; }
            cnt++;
            lseg = g32(buf, 16);
        }
        if (pass == 0) {
            if (cnt == 0) goto out;              /* header without code */
            n = cnt;
            code = (UBYTE *)AllocVec(n * 492UL, MEMF_PUBLIC | MEMF_CLEAR);
            if (!code) goto out;
        } else if (cnt != n) {
            goto out_code;
        }
    }
    fi->code      = code;
    fi->code_size = off;
    ok = TRUE;
    goto out;

out_code:
    if (code) FreeVec(code);
out:
    FreeVec(buf);
    return ok;
}

UWORD Rescue_Apply(struct BlockDev *bd, const struct RescueResult *r,
                   struct RDBInfo *rdb, RescueEmitFn emit, void *ud)
{
    char  line[256], dt[16], sz[16], name[32];
    ULONG cylblk, last, first_part = 0xFFFFFFFFUL;
    UWORD i, added = 0, fs_only = 0;
    BOOL  fresh = (BOOL)!rdb->valid;

    emit(ud, GS(MSG_RS_WR_HDR));

    if (fresh) {
        if (r->geo_src == RESCUE_GEO_NONE || r->heads == 0 || r->sectors == 0) {
            emit(ud, GS(MSG_RS_WR_NOGEO));
            return 0;
        }
        RDB_InitFresh(rdb, r->total_blocks / (r->heads * r->sectors),
                      r->heads, r->sectors);
        RS_LINE(GS(MSG_RS_WR_NEWRDB_FMT), (unsigned long)rdb->cylinders,
                (unsigned long)rdb->heads, (unsigned long)rdb->sectors);
    } else {
        if (rdb->heads == 0 || rdb->sectors == 0) {
            emit(ud, GS(MSG_RS_WR_NOGEO));
            return 0;
        }
        RS_LINE(GS(MSG_RS_WR_KEEPRDB_FMT), (unsigned long)rdb->num_parts,
                (unsigned long)rdb->num_fs);
        if (rdb->chain_truncated) emit(ud, GS(MSG_RS_WR_TRUNC));
    }
    cylblk = rdb->heads * rdb->sectors;

    /* ---- Partitions, lowest start first ---- */
    last = 0;
    for (;;) {
        const struct RescuePart *bp = NULL;
        const struct RescueFS   *bf = NULL;
        struct PartInfo *pi;
        ULONG best = 0xFFFFFFFFUL, start, len, low, high;
        const char *label;
        BOOL  clash = FALSE, same = FALSE;

        for (i = 0; i < r->num_parts; i++) {
            const struct RescuePart *p = &r->parts[i];
            if (part_usable(p) && p->start >= last && p->start < best)
                { best = p->start; bp = p; bf = NULL; }
        }
        for (i = 0; i < r->num_fs; i++) {
            const struct RescueFS *f = &r->fs[i];
            if (fs_usable(f) && f->start >= last && f->start < best)
                { best = f->start; bf = f; bp = NULL; }
        }
        if (!bp && !bf) break;
        last  = best + 1;
        start = bp ? bp->start : bf->start;
        len   = bp ? bp->len   : bf->len;
        label = bp ? bp->pi.drive_name : bf->volname;

        /* Already in the table, or in the way of something that is? */
        for (i = 0; i < rdb->num_parts; i++) {
            UQUAD lo, hi;
            part_range(rdb, &rdb->parts[i], &lo, &hi);
            if ((UQUAD)start < hi && lo < (UQUAD)start + len) {
                if (lo == start) same = TRUE;
                else {
                    RS_LINE(GS(MSG_RS_WR_SKIP_OVERLAP_FMT), (unsigned long)start,
                            label, rdb->parts[i].drive_name);
                    clash = TRUE;
                }
                break;
            }
        }
        if (same || clash) continue;

        if (start % cylblk || len % cylblk || len == 0) {
            RS_LINE(GS(MSG_RS_WR_SKIP_ALIGN_FMT), (unsigned long)start, label);
            continue;
        }
        low  = start / cylblk;
        high = (start + len) / cylblk - 1;
        if (low == 0 || (UQUAD)(high + 1) * cylblk > r->total_blocks) {
            RS_LINE(GS(MSG_RS_WR_SKIP_RANGE_FMT), (unsigned long)start, label);
            continue;
        }
        if (bf && bf->dos_type == 0) {
            RS_LINE(GS(MSG_RS_WR_SKIP_TYPE_FMT), (unsigned long)start, label,
                    (unsigned long)low, (unsigned long)high);
            continue;
        }
        if (rdb->num_parts >= MAX_PARTITIONS) {
            emit(ud, GS(MSG_RS_WR_SKIP_FULL));
            break;
        }

        pi = &rdb->parts[rdb->num_parts];
        if (bp) {
            *pi = bp->pi;
            pi->block_num = 0;
            pi->next_part = 0;
            if (pi->drive_name[0] == '\0' || name_in_use(rdb, pi->drive_name))
                free_name(rdb, pi->drive_name);
        } else {
            memset(pi, 0, sizeof(*pi));
            free_name(rdb, name);
            strcpy(pi->drive_name, name);
            pi->dos_type          = bf->dos_type;
            pi->sectors_per_block = bf->fs_bsz / 512;
            if (pi->sectors_per_block == 0) pi->sectors_per_block = 1;
            pi->reserved_blks     = 2;
            pi->max_transfer      = 0x1FE00UL;      /* the ADDPART defaults */
            pi->mask              = 0x7FFFFFFEUL;
            pi->num_buffer        = 30;
            fs_only++;
        }
        /* One geometry for the whole table; the partition's data is NOT
           new, so nothing may be formatted or wiped on the next write. */
        pi->block_size  = 512;
        pi->heads       = rdb->heads;
        pi->sectors     = rdb->sectors;
        pi->low_cyl     = low;
        pi->high_cyl    = high;
        pi->is_new      = 0;
        pi->want_format = 0;
        pi->volume_name[0] = '\0';
        rdb->num_parts++;
        added++;

        FormatDosType(pi->dos_type, dt);
        FormatSize((UQUAD)len * 512, sz);
        RS_LINE(GS(MSG_RS_WR_ADD_FMT), pi->drive_name, (unsigned long)low,
                (unsigned long)high, dt, sz);
    }

    /* The table's cylinder range must cover what it now holds. */
    for (i = 0; i < rdb->num_parts; i++) {
        struct PartInfo *pi = &rdb->parts[i];
        UQUAD lo, hi;
        if (pi->low_cyl >= 1 && pi->low_cyl < rdb->lo_cyl) rdb->lo_cyl = pi->low_cyl;
        if (pi->high_cyl > rdb->hi_cyl) {
            rdb->hi_cyl = pi->high_cyl;
            if (rdb->cylinders < pi->high_cyl + 1) rdb->cylinders = pi->high_cyl + 1;
        }
        part_range(rdb, pi, &lo, &hi);
        if (lo < first_part) first_part = (ULONG)lo;
    }
    /* RDB_Read keeps the list sorted by start; so do we. */
    for (i = 1; i < rdb->num_parts; i++) {
        struct PartInfo t = rdb->parts[i];
        UWORD j;
        for (j = i; j > 0 && rdb->parts[j - 1].low_cyl > t.low_cyl; j--)
            rdb->parts[j] = rdb->parts[j - 1];
        rdb->parts[j] = t;
    }

    /* ---- Filesystem drivers that survived in the RDB area ---- */
    for (i = 0; i < r->num_fshd; i++) {
        const struct RescueFSHD *h = &r->fshd[i];
        struct FSInfo *fi;
        UWORD k;
        BOOL  have = FALSE;

        if (h->block >= first_part) continue;      /* inside a partition: an image */
        for (k = 0; k < rdb->num_fs; k++)
            if (rdb->filesystems[k].dos_type == h->dos_type &&
                rdb->filesystems[k].version >= h->version) have = TRUE;
        if (have || rdb->num_fs >= MAX_FILESYSTEMS) continue;

        fi = &rdb->filesystems[rdb->num_fs];
        FormatDosType(h->dos_type, dt);
        if (!load_fshd(bd, h->block, fi)) {
            memset(fi, 0, sizeof(*fi));
            RS_LINE(GS(MSG_RS_WR_FS_BAD_FMT), dt, (unsigned long)h->block);
            continue;
        }
        rdb->num_fs++;
        added++;
        RS_LINE(GS(MSG_RS_WR_FS_FMT), dt, (unsigned long)(fi->version >> 16),
                (unsigned long)(fi->version & 0xFFFF), (unsigned long)fi->code_size);
    }

    if (added == 0) {
        emit(ud, GS(MSG_RS_WR_NOTHING));
        return 0;
    }

    /* ---- What the user still has to do ---- */
    for (i = 0; i < rdb->num_parts; i++) {
        const struct PartInfo *pi = &rdb->parts[i];
        UWORD k;
        BOOL  have = FALSE;
        if ((pi->dos_type & ID_DOS_MASK) == ID_DOS) continue;   /* FFS is in ROM */
        for (k = 0; k < rdb->num_fs; k++)
            if (rdb->filesystems[k].dos_type == pi->dos_type) have = TRUE;
        if (have) continue;
        FormatDosType(pi->dos_type, dt);
        RS_LINE(GS(MSG_RS_WR_NEEDFS_FMT), pi->drive_name, dt);
    }
    if (fs_only) emit(ud, GS(MSG_RS_WR_DEFAULTS));

    /* The scan has collected every block that can still be read; what the
       broken chain no longer reaches is gone either way. */
    if (rdb->chain_truncated) rdb->allow_truncated_write = TRUE;
    return added;
}
