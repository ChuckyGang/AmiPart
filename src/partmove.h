/*
 * partmove.h - Partition data move: copies all blocks from the current
 *              cylinder range to a new one, updating filesystem metadata
 *              where required (SFS firstbyte/lastbyte).
 *
 * WARNING: moving a partition physically copies every block on disk.
 *          Power loss or a crash during the copy will leave both the
 *          old and new locations partially written.  There is NO
 *          automatic rollback.  The caller must have warned the user.
 */

#ifndef PARTMOVE_H
#define PARTMOVE_H

#include <exec/types.h>
#include "rdb.h"

/*
 * Progress callback.  Called repeatedly during the block copy.
 *   done  : blocks copied so far (0 on first call before any I/O)
 *   total : total blocks to copy
 *   phase : short phase description, e.g. "Copying...", "Updating SFS..."
 */
typedef void (*MoveProgressFn)(void *ud,
                               ULONG done, ULONG total,
                               const char *phase);

/*
 * Take a partition offline before its blocks are overwritten or relocated
 * (move, zero, restore): a live handler would keep writing to the old
 * contents and corrupt the result.  Unmounts whatever DOS device is live on
 * this partition's blocks on bd's device+unit (quickformat.h
 * TakeRangeOffline).  TRUE when it is offline (or was not mounted / bd is an
 * image file); FALSE with err_buf set when it is in use - nothing must be
 * written then.  err_buf: at least ENGINE_ERRBUF_SIZE bytes.
 */
BOOL PART_TakeOffline(struct BlockDev *bd, const struct RDBInfo *rdb,
                      const struct PartInfo *pi, char *err_buf);

/*
 * Check whether moving partition pi to new_low_cyl is valid.
 *   - new range must fit inside rdb->lo_cyl .. rdb->hi_cyl
 *   - new range must not overlap any other partition
 *   - new_low_cyl must differ from pi->low_cyl
 * new_high_cyl_out (may be NULL) receives the computed new hi cylinder.
 * err_buf must be at least ENGINE_ERRBUF_SIZE bytes.
 * Returns TRUE if the move is valid.
 */
BOOL PART_CanMove(const struct RDBInfo *rdb, const struct PartInfo *pi,
                  ULONG new_low_cyl, ULONG *new_high_cyl_out,
                  char *err_buf);

/*
 * Move all partition data to new_low_cyl.
 *
 * On entry  : pi->low_cyl / pi->high_cyl are the CURRENT values.
 * On success: pi->low_cyl and pi->high_cyl are updated to the new values.
 *             Caller must call RDB_Write() afterwards.
 * On failure: the disk may be partially written; err_buf is filled.
 *             pi->low_cyl / pi->high_cyl are left unchanged on failure.
 *
 * The RDB PART block is NOT written by this function.
 */
BOOL PART_Move(struct BlockDev *bd, const struct RDBInfo *rdb,
               struct PartInfo *pi, ULONG new_low_cyl,
               char *err_buf,
               MoveProgressFn progress_fn, void *progress_ud);

/*
 * Zero out all blocks in a partition (overwrite with zeros).
 * Useful before disk imaging so unused space compresses well.
 * err_buf must be at least ENGINE_ERRBUF_SIZE bytes.
 */
BOOL PART_Zero(struct BlockDev *bd, const struct RDBInfo *rdb,
               const struct PartInfo *pi,
               char *err_buf,
               MoveProgressFn progress_fn, void *progress_ud);

#endif /* PARTMOVE_H */
