/*
 * partmove.c - Partition data move.
 *
 * Copies all physical blocks from the current cylinder range to a new
 * one using multi-block TD_READ64 / TD_WRITE64 I/O (128 blocks at a
 * time) to minimise Exec message-passing overhead.
 *
 * Direction safety:
 *   Moving to LOWER cylinders -> copy front-to-back (safe: destination
 *   precedes source so we never overwrite unread blocks).
 *   Moving to HIGHER cylinders -> copy back-to-front (safe: destination
 *   follows source for the same reason).
 *
 * SFS metadata:
 *   fsRootBlock.firstbyte / lastbyte are absolute byte offsets from the
 *   start of the disk.  They must be updated in both SFS root blocks
 *   (block 0 and block totalblocks-1) after the data copy.
 *   FFS and PFS use partition-relative block numbers internally, so they
 *   require no metadata changes.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <proto/exec.h>
#include <devices/trackdisk.h>
#ifndef TD_WRITE64
#define TD_WRITE64  (CMD_NONSTD + 16)
#endif
#ifndef TD_READ64
#define TD_READ64   (CMD_NONSTD + 15)
#endif

#include "clib.h"
#include "rdb.h"
#include "partmove.h"
#include "sfsresize.h"   /* SFS_IsSupportedType */
#include "sfs_util.h"
#include "locale_support.h"

/* ------------------------------------------------------------------ */
/* Multi-block I/O                                                      */
/* ------------------------------------------------------------------ */

/* Number of 512-byte sectors per read/write chunk.  64 KB per chunk. */
#define MOVE_CHUNK 128

/* Read/write 'count' consecutive 512-byte sectors starting at 'start'.
   Thin wrappers over rdb.c's multi-block API, which handles TD64 (high
   offset word in io_Actual), the CMD_READ/CMD_WRITE fallback for pre-TD64
   drivers, per-block retry, AND image files - the old private copies here
   issued DoIO() directly and so could not work on an IMAGE= target. */
static BOOL move_read_blocks(struct BlockDev *bd,
                              ULONG start, ULONG count, UBYTE *buf)
{
    return BlockDev_ReadBlocks(bd, start, count, buf);
}

static BOOL move_write_blocks(struct BlockDev *bd,
                               ULONG start, ULONG count, const UBYTE *buf)
{
    return BlockDev_WriteBlocks(bd, start, count, buf);
}

/* ------------------------------------------------------------------ */
/* SFS root block field offsets                                        */
/* ------------------------------------------------------------------ */
#define SFS_ROOT_ID     0x53465300UL
#define SFS_RB_ID           0
#define SFS_RB_CHECKSUM     4
#define SFS_RB_OWNBLOCK     8
#define SFS_RB_VERSION      12
#define SFS_RB_SEQNUM       14
#define SFS_RB_BLOCKSIZE    52
#define SFS_RB_FIRSTBYTEH   32
#define SFS_RB_FIRSTBYTE    36
#define SFS_RB_LASTBYTEH    40
#define SFS_RB_LASTBYTE     44
#define SFS_RB_TOTALBLOCKS  48

/* sfs_getl, sfs_getw, sfs_setl, sfs_set_checksum, sfs_verify_checksum
   are in sfs_util.c / sfs_util.h */

/* Read one multi-sector SFS block (sfs_phys physical sectors) at SFS
   block number sfs_blk relative to new_phys_base. */
static BOOL sfs_read_root(struct BlockDev *bd, ULONG new_phys_base,
                           ULONG sfs_blk, ULONG sfs_phys, UBYTE *buf)
{
    ULONG i;
    ULONG start = new_phys_base + sfs_blk * sfs_phys;
    for (i = 0; i < sfs_phys; i++)
        if (!BlockDev_ReadBlock(bd, start + i, buf + i * 512)) return FALSE;
    return TRUE;
}
static BOOL sfs_write_root(struct BlockDev *bd, ULONG new_phys_base,
                            ULONG sfs_blk, ULONG sfs_phys, const UBYTE *buf)
{
    ULONG i;
    ULONG start = new_phys_base + sfs_blk * sfs_phys;
    for (i = 0; i < sfs_phys; i++)
        if (!BlockDev_WriteBlock(bd, start + i, buf + i * 512)) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* PART_CanMove                                                         */
/* ------------------------------------------------------------------ */

BOOL PART_CanMove(const struct RDBInfo *rdb, const struct PartInfo *pi,
                  ULONG new_low_cyl, ULONG *new_high_cyl_out,
                  char *err_buf)
{
    ULONG cyl_count, new_high_cyl;
    UWORD i;

    if (pi->high_cyl < pi->low_cyl) {
        snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_INVALID_CYL_RANGE),
                (unsigned long)pi->low_cyl, (unsigned long)pi->high_cyl);
        return FALSE;
    }

    cyl_count   = pi->high_cyl - pi->low_cyl + 1;
    new_high_cyl = new_low_cyl + cyl_count - 1;

    if (new_high_cyl_out) *new_high_cyl_out = new_high_cyl;

    if (new_low_cyl == pi->low_cyl) {
        snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_ALREADY_AT_CYL),
                (unsigned long)new_low_cyl);
        return FALSE;
    }

    if (new_low_cyl < rdb->lo_cyl) {
        snprintf(err_buf, ENGINE_ERRBUF_SIZE,
                GS(MSG_PM_START_BELOW_LOWEST),
                (unsigned long)new_low_cyl, (unsigned long)rdb->lo_cyl);
        return FALSE;
    }

    if (new_high_cyl > rdb->hi_cyl) {
        snprintf(err_buf, ENGINE_ERRBUF_SIZE,
                GS(MSG_PM_END_EXCEEDS_DISK),
                (unsigned long)new_high_cyl, (unsigned long)rdb->hi_cyl);
        return FALSE;
    }

    /* Check for overlap with every other partition */
    for (i = 0; i < rdb->num_parts; i++) {
        const struct PartInfo *other = &rdb->parts[i];
        if (other == pi) continue;
        if (new_low_cyl <= other->high_cyl && new_high_cyl >= other->low_cyl) {
            snprintf(err_buf, ENGINE_ERRBUF_SIZE,
                    GS(MSG_PM_POSITION_OVERLAPS),
                    (unsigned long)new_low_cyl, (unsigned long)new_high_cyl,
                    other->drive_name,
                    (unsigned long)other->low_cyl,
                    (unsigned long)other->high_cyl);
            return FALSE;
        }
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* PART_Move                                                            */
/* ------------------------------------------------------------------ */

BOOL PART_Move(struct BlockDev *bd, const struct RDBInfo *rdb,
               struct PartInfo *pi, ULONG new_low_cyl,
               char *err_buf,
               MoveProgressFn progress_fn, void *progress_ud)
{
#define MOVE_PROGRESS(d,t,ph) \
    do { if (progress_fn) progress_fn(progress_ud,(d),(t),(ph)); } while(0)

    UBYTE  *buf          = NULL;
    UBYTE  *sfs_root_buf = NULL;
    BOOL    ok           = FALSE;

    ULONG heads, sectors, phys_per_lb;
    ULONG old_low, old_high, cyl_count, new_high_cyl;
    ULONG phys_base_old, phys_base_new, phys_count;
    ULONG done, chunk, i;

    err_buf[0] = '\0';   /* success path reads this to decide on the summary */

    old_low     = pi->low_cyl;
    old_high    = pi->high_cyl;
    cyl_count   = old_high - old_low + 1;
    new_high_cyl = new_low_cyl + cyl_count - 1;

    heads      = pi->heads   > 0 ? pi->heads   : rdb->heads;
    sectors    = pi->sectors > 0 ? pi->sectors : rdb->sectors;
    phys_per_lb = (pi->block_size >= 1024) ? (pi->block_size / 512) : 1;

    if (heads == 0 || sectors == 0) {
        snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_INVALID_GEOMETRY),
                (unsigned long)heads, (unsigned long)sectors);
        return FALSE;
    }

    phys_base_old = old_low     * heads * sectors * phys_per_lb;
    phys_base_new = new_low_cyl * heads * sectors * phys_per_lb;
    phys_count    = cyl_count   * heads * sectors * phys_per_lb;

    /* Re-validate (should have been checked by PART_CanMove already) */
    {
        char vbuf[128];
        if (!PART_CanMove(rdb, pi, new_low_cyl, NULL, vbuf)) {
            snprintf(err_buf, ENGINE_ERRBUF_SIZE, "%.127s", vbuf);
            return FALSE;
        }
    }

    /* Allocate copy buffer: MOVE_CHUNK sectors */
    buf = (UBYTE *)AllocVec((ULONG)MOVE_CHUNK * 512, MEMF_PUBLIC);
    if (!buf) {
        snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_OUT_OF_MEMORY_BYTES),
                (unsigned long)MOVE_CHUNK * 512);
        return FALSE;
    }

    /* SFS: the root blocks have to be patched AFTER the copy.  Make sure they
       are readable and sane BEFORE anything moves - a metadata problem found
       after an overlapping copy would leave the source half destroyed with
       the table still pointing at it. */
    if (SFS_IsSupportedType(pi->dos_type)) {
        UBYTE pre[512];
        ULONG bs;
        if (!BlockDev_ReadBlock(bd, phys_base_old, pre)) {
            snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_SFS_CANT_READ_ROOT0));
            goto done_label;
        }
        if (sfs_getl(pre, SFS_RB_ID) != SFS_ROOT_ID) {
            snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_SFS_ROOT_ID_MISMATCH), pi->drive_name);
            goto done_label;
        }
        bs = sfs_getl(pre, SFS_RB_BLOCKSIZE);
        if (bs < 512 || (bs & (bs - 1)) || (bs % 512) != 0) {
            snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_SFS_INVALID_BLOCKSIZE), (unsigned long)bs);
            goto done_label;
        }
    }

    /* ---------------------------------------------------------------- */
    /* Block copy - direction depends on relative positions              */
    /* ---------------------------------------------------------------- */
    done = 0;
    MOVE_PROGRESS(0, phys_count, GS(MSG_PM_COPYING_BLOCKS));

    if (phys_base_new < phys_base_old) {
        /* Moving to lower address: front-to-back */
        for (i = 0; i < phys_count; i += chunk) {
            chunk = phys_count - i;
            if (chunk > MOVE_CHUNK) chunk = MOVE_CHUNK;

            if (!move_read_blocks(bd, phys_base_old + i, chunk, buf)) {
                snprintf(err_buf, ENGINE_ERRBUF_SIZE,
                        GS(MSG_PM_READ_ERROR_AT_BLOCK),
                        (unsigned long)(phys_base_old + i),
                        (unsigned long)done);
                goto done_label;
            }
            if (!move_write_blocks(bd, phys_base_new + i, chunk, buf)) {
                snprintf(err_buf, ENGINE_ERRBUF_SIZE,
                        GS(MSG_PM_WRITE_ERROR_AT_BLOCK),
                        (unsigned long)(phys_base_new + i),
                        (unsigned long)done);
                goto done_label;
            }
            done += chunk;
            MOVE_PROGRESS(done, phys_count, GS(MSG_PM_COPYING_BLOCKS));
        }
    } else {
        /* Moving to higher address: back-to-front */
        i = phys_count;
        while (i > 0) {
            chunk = i < MOVE_CHUNK ? i : MOVE_CHUNK;
            i -= chunk;

            if (!move_read_blocks(bd, phys_base_old + i, chunk, buf)) {
                snprintf(err_buf, ENGINE_ERRBUF_SIZE,
                        GS(MSG_PM_READ_ERROR_AT_BLOCK),
                        (unsigned long)(phys_base_old + i),
                        (unsigned long)done);
                goto done_label;
            }
            if (!move_write_blocks(bd, phys_base_new + i, chunk, buf)) {
                snprintf(err_buf, ENGINE_ERRBUF_SIZE,
                        GS(MSG_PM_WRITE_ERROR_AT_BLOCK),
                        (unsigned long)(phys_base_new + i),
                        (unsigned long)done);
                goto done_label;
            }
            done += chunk;
            MOVE_PROGRESS(done, phys_count, GS(MSG_PM_COPYING_BLOCKS));
        }
    }

    MOVE_PROGRESS(phys_count, phys_count, GS(MSG_PM_COPYING_BLOCKS));

    /* ---------------------------------------------------------------- */
    /* SFS metadata update                                               */
    /* After the copy the SFS partition sits at the new location but its */
    /* root blocks still carry the old absolute firstbyte/lastbyte.      */
    /* Compute delta and patch both root copies.                         */
    /* ---------------------------------------------------------------- */
    /* The data is at its new place from here on.  Whatever happens to the
       SFS metadata below, the table MUST follow the data: failures set a
       warning in err_buf (shown in the success summary) and fall through to
       the PartInfo update instead of aborting. */
    if (SFS_IsSupportedType(pi->dos_type)) do {
        UBYTE  scratch[512];
        ULONG  sfs_blocksize, sfs_phys;
        ULONG  totalblocks;
        UQUAD  old_fb, new_fb, old_lb, new_lb;
        ULONG  root_blks[2];   /* SFS block numbers of both roots */
        UWORD  r;

        MOVE_PROGRESS(phys_count, phys_count, GS(MSG_PM_UPDATING_SFS));

        /* Read first physical sector of new location to get SFS blocksize */
        if (!BlockDev_ReadBlock(bd, phys_base_new, scratch)) {
            snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_SFS_CANT_READ_ROOT0));
            break;
        }
        if (sfs_getl(scratch, SFS_RB_ID) != SFS_ROOT_ID) {
            /* Not SFS - copy succeeded but metadata update skipped.
               This shouldn't happen; warn but don't fail the move. */
            snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_SFS_ROOT_ID_MISMATCH),
                    pi->drive_name);
            break;
        }

        sfs_blocksize = sfs_getl(scratch, SFS_RB_BLOCKSIZE);
        if (sfs_blocksize < 512 || (sfs_blocksize & (sfs_blocksize - 1)) ||
            (sfs_blocksize % 512) != 0) {
            snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_SFS_INVALID_BLOCKSIZE), (unsigned long)sfs_blocksize);
            break;
        }
        sfs_phys = sfs_blocksize / 512;

        sfs_root_buf = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
        if (!sfs_root_buf) {
            snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_OUT_OF_MEMORY_SFS));
            break;
        }

        /* Read full root block 0 */
        if (!sfs_read_root(bd, phys_base_new, 0, sfs_phys, sfs_root_buf) ||
            !sfs_verify_checksum(sfs_root_buf, sfs_blocksize)) {
            snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_SFS_ROOT0_CHECKSUM));
            break;
        }
        totalblocks = sfs_getl(sfs_root_buf, SFS_RB_TOTALBLOCKS);

        old_fb = ((UQUAD)sfs_getl(sfs_root_buf, SFS_RB_FIRSTBYTEH) << 32) |
                 (UQUAD)sfs_getl(sfs_root_buf, SFS_RB_FIRSTBYTE);
        old_lb = ((UQUAD)sfs_getl(sfs_root_buf, SFS_RB_LASTBYTEH)  << 32) |
                 (UQUAD)sfs_getl(sfs_root_buf, SFS_RB_LASTBYTE);

        /* New absolute byte offset of partition start */
        new_fb = (UQUAD)phys_base_new * (UQUAD)bd->block_size;
        new_lb = new_fb + (old_lb - old_fb);   /* size unchanged */

        root_blks[0] = 0;
        root_blks[1] = totalblocks - 1;

        for (r = 0; r < 2; r++) {
            if (!sfs_read_root(bd, phys_base_new, root_blks[r],
                                sfs_phys, sfs_root_buf))
                continue;  /* end root may be absent - skip silently */
            if (!sfs_verify_checksum(sfs_root_buf, sfs_blocksize))
                continue;
            if (sfs_getl(sfs_root_buf, SFS_RB_ID) != SFS_ROOT_ID)
                continue;

            sfs_setl(sfs_root_buf, SFS_RB_FIRSTBYTEH, (ULONG)(new_fb >> 32));
            sfs_setl(sfs_root_buf, SFS_RB_FIRSTBYTE,  (ULONG)(new_fb & 0xFFFFFFFFUL));
            sfs_setl(sfs_root_buf, SFS_RB_LASTBYTEH,  (ULONG)(new_lb >> 32));
            sfs_setl(sfs_root_buf, SFS_RB_LASTBYTE,   (ULONG)(new_lb & 0xFFFFFFFFUL));
            sfs_set_checksum(sfs_root_buf, sfs_blocksize);
            if (!sfs_write_root(bd, phys_base_new, root_blks[r],
                                sfs_phys, sfs_root_buf) &&
                !sfs_write_root(bd, phys_base_new, root_blks[r],
                                sfs_phys, sfs_root_buf)) {      /* one retry */
                if (r == 0) {
                    /* Primary root write failed - volume will be unmountable. */
                    snprintf(err_buf, ENGINE_ERRBUF_SIZE,
                            GS(MSG_PM_SFS_PRIMARY_WRITE_FAIL),
                            pi->drive_name);
                    break;
                } else {
                    /* Backup root write failed - primary is correct, warn only. */
                    snprintf(err_buf, ENGINE_ERRBUF_SIZE,
                            GS(MSG_PM_SFS_BACKUP_WRITE_FAIL),
                            (unsigned long)root_blks[1],
                            pi->drive_name);
                    /* Continue - ok will be set to TRUE below. */
                }
            }
        }
    } while (0);

    /* ---------------------------------------------------------------- */
    /* Update PartInfo - caller must then call RDB_Write                 */
    /* ---------------------------------------------------------------- */
    pi->low_cyl  = new_low_cyl;
    pi->high_cyl = new_high_cyl;

    if (err_buf[0] == '\0')
    snprintf(err_buf, ENGINE_ERRBUF_SIZE,
            GS(MSG_PM_MOVED_SUMMARY),
            (unsigned long)cyl_count,
            (unsigned long)old_low,  (unsigned long)old_high,
            (unsigned long)new_low_cyl, (unsigned long)new_high_cyl,
            (unsigned long)phys_base_old, (unsigned long)phys_base_new,
            (unsigned long)phys_count);

    ok = TRUE;

done_label:
    if (buf)          FreeVec(buf);
    if (sfs_root_buf) FreeVec(sfs_root_buf);
    return ok;

#undef MOVE_PROGRESS
}

/* ------------------------------------------------------------------ */
/* PART_Zero - overwrite every block in a partition with zeros.        */
/* ------------------------------------------------------------------ */
BOOL PART_Zero(struct BlockDev *bd, const struct RDBInfo *rdb,
               const struct PartInfo *pi,
               char *err_buf,
               MoveProgressFn progress_fn, void *progress_ud)
{
    ULONG heads   = pi->heads   > 0 ? pi->heads   : rdb->heads;
    ULONG sectors = pi->sectors > 0 ? pi->sectors : rdb->sectors;
    ULONG phys_base, phys_count, written;
    UBYTE *zbuf = NULL;

    err_buf[0] = '\0';

    if (heads == 0 || sectors == 0) {
        snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_INVALID_GEOMETRY),
                (unsigned long)heads, (unsigned long)sectors);
        return FALSE;
    }

    phys_base  = pi->low_cyl * heads * sectors;
    phys_count = (pi->high_cyl - pi->low_cyl + 1) * heads * sectors;

    zbuf = (UBYTE *)AllocVec((ULONG)MOVE_CHUNK * bd->block_size,
                              MEMF_PUBLIC | MEMF_CLEAR);
    if (!zbuf) {
        snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_OUT_OF_MEMORY_BYTES),
                (unsigned long)((ULONG)MOVE_CHUNK * bd->block_size));
        return FALSE;
    }

    if (progress_fn)
        progress_fn(progress_ud, 0, phys_count, GS(MSG_ZERO_WRITING));

    for (written = 0; written < phys_count; ) {
        ULONG chunk = phys_count - written;
        if (chunk > MOVE_CHUNK) chunk = MOVE_CHUNK;

        if (!move_write_blocks(bd, phys_base + written, chunk, zbuf)) {
            snprintf(err_buf, ENGINE_ERRBUF_SIZE, GS(MSG_PM_WRITE_ERROR_AT_BLOCK),
                    (unsigned long)(phys_base + written),
                    (unsigned long)written);
            FreeVec(zbuf);
            return FALSE;
        }
        written += chunk;
        if (progress_fn)
            progress_fn(progress_ud, written, phys_count, GS(MSG_ZERO_WRITING));
    }

    FreeVec(zbuf);
    return TRUE;
}
