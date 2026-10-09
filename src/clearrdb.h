/*
 * clearrdb.h - "Clear RDB and MBR": zero the partition-table area of a disk
 * so it looks completely blank to AmigaOS (no RDB in blocks 0-15), PCs (no
 * MBR in block 0) and GPT-aware systems (no primary or backup GPT header).
 *
 * Partition DATA is never touched - only the head of the disk (and, for a
 * GPT disk, its last 33 blocks).  Shared by the GUI menu entry, the CLI
 * CLEARRDB switch and the script CLEARRDB command.
 */

#ifndef CLEARRDB_H
#define CLEARRDB_H

#include <exec/types.h>
#include "rdb.h"
#include "mbr.h"

struct ClearPlan {
    ULONG head_blocks;   /* zero blocks 0 .. head_blocks-1              */
    BOOL  gpt;           /* a protective-MBR GPT entry (0xEE) was seen  */
    ULONG tail_start;    /* GPT only: first block of the backup area    */
    ULONG tail_blocks;   /* GPT only: number of blocks there (33)       */
    BOOL  superfloppy;   /* block 0 is a FAT boot sector: FATs + root   */
                         /* dir sit inside the zeroed area (data loss)  */
    BOOL  tail_uncertain;/* GPT on a driver without READ CAPACITY: the  */
                         /* size is cylinder-rounded, backup may be missed */
};

/* Work out what to zero.  rdb/mbr may be NULL or invalid (then only the
   16-block RDB scan area is cleared).  With a valid RDB the range grows to
   cover every metadata block the chain references (PART/FSHD/LSEG/BADB),
   capped at CLEARRDB_MAX_HEAD blocks and at the disk size.  GPT is
   detected from the raw block-0 entry types (a protective 0xEE entry is
   usually sized 0xFFFFFFFF, which MBR_Read rejects as insane), so block 0
   is re-read here. */
void ClearRDB_Plan(struct BlockDev *bd, const struct RDBInfo *rdb,
                   const struct MBRInfo *mbr, struct ClearPlan *plan);

/* Zero every block the plan names.  err (>= 96 bytes) gets a localized
   reason on failure.  *zeroed receives the block count written so far. */
BOOL ClearRDB_Run(struct BlockDev *bd, const struct ClearPlan *plan,
                  ULONG *zeroed, char *err);

#define CLEARRDB_MAX_HEAD   65536UL   /* 32 MB at 512 bytes - sanity cap */
#define CLEARRDB_GPT_BLOCKS 34UL      /* protective MBR + header + 32 entry blocks */
#define CLEARRDB_GPT_TAIL   33UL      /* backup entries + backup header */

#endif /* CLEARRDB_H */
