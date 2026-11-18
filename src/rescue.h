/*
 * rescue.h - RDB rescue scan (RESCUE command).
 *
 * READ-ONLY.  Reads the disk front to back looking for two kinds of
 * evidence of a lost or damaged partition table:
 *
 *   1. RDB metadata blocks (RDSK / PART / FSHD / LSEG) with a valid
 *      checksum, wherever they sit - the chain pointers are ignored, so a
 *      PART block survives a destroyed RDSK block or a broken chain.
 *   2. Filesystem signatures (FFS/OFS root block, PFS3 rootblock, SFS root
 *      block).  Each of those tells where its partition starts and how big
 *      it is, which is enough to rebuild a partition entry when the whole
 *      RDB area is gone.
 *
 * The result is a list of what was found plus a proposed partition table.
 * Nothing here writes to the disk.
 */

#ifndef RESCUE_H
#define RESCUE_H

#include <exec/types.h>
#include "rdb.h"

#define RESCUE_MAX_FS    96
#define RESCUE_MAX_PART  64
#define RESCUE_MAX_FSHD  16

/* RescueFS.kind */
#define RESCUE_FS_FFS  1
#define RESCUE_FS_PFS  2
#define RESCUE_FS_SFS  3

/* RescueFS.conf */
#define RESCUE_CONF_LOW   1
#define RESCUE_CONF_MED   2
#define RESCUE_CONF_HIGH  3

/* RescueFS.flags */
#define RSF_NOBOOT      0x0001  /* FFS: no DOS\x boot block at the start    */
#define RSF_NOSTART     0x0002  /* FFS root found, partition start unknown  */
#define RSF_OVERLAP     0x0008  /* overlaps another find                    */
#define RSF_TYPEGUESS   0x0010  /* dos_type is a guess, not read from disk  */
#define RSF_BEYOND      0x0020  /* claims blocks past the end of the disk   */
#define RSF_MISALIGNED  0x0040  /* does not fit the proposed geometry       */
#define RSF_SIZEDIFF    0x0080  /* PART block and filesystem disagree on size */
#define RSF_NOSIZE      0x0100  /* filesystem does not record its size      */

/* RescuePart.flags */
#define RPF_NESTED      0x01    /* PART block sits inside a found partition */
#define RPF_BADGEO      0x02    /* unusable geometry in the DosEnvec        */
#define RPF_BEYOND      0x04    /* partition runs past the end of the disk  */
#define RPF_OVERLAP     0x08    /* overlaps another PART-described partition */

struct RescueFS {
    ULONG start;          /* first device block of the partition           */
    ULONG len;            /* device blocks - best estimate                 */
    ULONG len_min;        /* FFS records no size: possible range           */
    ULONG len_max;
    ULONG sig_block;      /* device block the signature was found at       */
    ULONG dos_type;       /* 0 = unknown                                   */
    ULONG fs_bsz;         /* filesystem block size in bytes                */
    UWORD flags;          /* RSF_*                                         */
    UBYTE kind;           /* RESCUE_FS_*                                   */
    UBYTE conf;           /* RESCUE_CONF_*                                 */
    WORD  part_idx;       /* matching parts[] entry, or -1                 */
    char  volname[32];
};

struct RescuePart {
    ULONG block;          /* device block the PART block sits at           */
    ULONG start;          /* first device block of the partition           */
    ULONG len;            /* device blocks                                 */
    UBYTE flags;          /* RPF_*                                         */
    WORD  fs_idx;         /* matching fs[] entry, or -1                    */
    struct PartInfo pi;
};

struct RescueFSHD {
    ULONG block;
    ULONG dos_type;
    ULONG version;
};

/* Where the proposed geometry came from (RescueResult.geo_src). */
#define RESCUE_GEO_NONE     0
#define RESCUE_GEO_RDSK     1
#define RESCUE_GEO_PART     2
#define RESCUE_GEO_DERIVED  3

struct RescueResult {
    ULONG total_blocks;
    ULONG scanned;        /* blocks actually read                          */
    ULONG skipped;        /* blocks jumped over (inside found partitions)  */
    ULONG read_errors;
    BOOL  cancelled;

    BOOL  rdsk_found;
    ULONG rdsk_block;
    ULONG rdsk_cyls, rdsk_heads, rdsk_sectors;

    ULONG num_lseg;
    ULONG nested_dropped; /* nested signatures dropped to keep the table small */
    ULONG part_dropped;   /* PART blocks beyond RESCUE_MAX_PART            */

    UWORD num_fs, num_parts, num_fshd;

    UBYTE geo_src;
    ULONG heads, sectors, cylinders;

    struct RescueFS   fs[RESCUE_MAX_FS];
    struct RescuePart parts[RESCUE_MAX_PART];
    struct RescueFSHD fshd[RESCUE_MAX_FSHD];
};

/* Progress + cancel callback: return FALSE to stop the scan. */
typedef BOOL (*RescueProgFn)(void *ud, ULONG cur, ULONG total);
/* Line-output callback: receives fully formatted lines (with \n). */
typedef void (*RescueEmitFn)(void *ud, const char *line);

/*
 * Scan the disk.  deep = FALSE jumps over the inside of every partition
 * it has positively identified (fast, and what you normally want);
 * deep = TRUE reads every block.  Returns FALSE only when the scan could
 * not run at all (no memory, unknown disk size, non-512-byte sectors);
 * err (>= 96 bytes) then holds the reason.  A cancelled scan returns TRUE
 * with res->cancelled set and whatever was found so far.
 */
BOOL Rescue_Scan(struct BlockDev *bd, BOOL deep,
                 RescueProgFn prog, void *ud,
                 struct RescueResult *res, char *err);

/* Print the findings and the proposed partition table.  The caller adds
   its own closing line (MSG_RS_FOOTER for a scan that wrote nothing). */
void Rescue_Report(const struct RescueResult *res,
                   RescueEmitFn emit, void *ud);

/*
 * Merge the findings into rdb, in memory only - the caller writes it with
 * RDB_Write once the user has agreed.
 *
 *   rdb->valid == FALSE  a fresh RDB is set up with the proposed geometry
 *   rdb->valid == TRUE   (as read by RDB_Read) the existing table is kept
 *                        and only what it lacks is added
 *
 * Added: every usable partition of the proposal that does not collide with
 * one already in the table, and every filesystem driver whose FSHD block
 * and code survived in the RDB area (bd is only read, for that code).
 * Each decision is reported through emit.  Returns the number of entries
 * added; 0 means there is nothing to write.
 */
UWORD Rescue_Apply(struct BlockDev *bd, const struct RescueResult *res,
                   struct RDBInfo *rdb, RescueEmitFn emit, void *ud);

#endif /* RESCUE_H */
