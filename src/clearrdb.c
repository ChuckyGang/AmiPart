/*
 * clearrdb.c - zero the RDB / MBR / GPT area of a disk.  See clearrdb.h.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "clib.h"
#include "locale_support.h"
#include "rdb.h"
#include "mbr.h"
#include "clearrdb.h"

#define CLR_CHUNK 64   /* blocks per write (32 KB at 512 bytes) */

void ClearRDB_Plan(const struct BlockDev *bd, const struct RDBInfo *rdb,
                   const struct MBRInfo *mbr, struct ClearPlan *plan)
{
    ULONG bs    = (bd && bd->block_size) ? bd->block_size : 512;
    ULONG total = (bd) ? (ULONG)(bd->total_bytes / bs) : 0;
    ULONG head  = RDB_SCAN_LIMIT;
    UWORD i;

    memset(plan, 0, sizeof(*plan));

    if (rdb && rdb->valid) {
        ULONG last = RDB_LastMetaBlock(rdb);
        if (last != RDB_END_MARK && last + 1 > head) head = last + 1;
    }

    if (mbr && mbr->valid) {
        for (i = 0; i < MBR_MAX_PARTS; i++)
            if (mbr->parts[i].present && mbr->parts[i].type == 0xEE)
                plan->gpt = TRUE;
    }
    if (plan->gpt) {
        if (head < CLEARRDB_GPT_BLOCKS) head = CLEARRDB_GPT_BLOCKS;
        if (total > CLEARRDB_GPT_TAIL + CLEARRDB_GPT_BLOCKS) {
            plan->tail_start  = total - CLEARRDB_GPT_TAIL;
            plan->tail_blocks = CLEARRDB_GPT_TAIL;
        }
    }

    if (head > CLEARRDB_MAX_HEAD) head = CLEARRDB_MAX_HEAD;
    if (total && head > total)    head = total;
    plan->head_blocks = head;
}

static BOOL zero_range(struct BlockDev *bd, ULONG start, ULONG count,
                       UBYTE *zbuf, ULONG *zeroed, char *err)
{
    while (count) {
        ULONG n = (count > CLR_CHUNK) ? CLR_CHUNK : count;
        if (!BlockDev_WriteBlocks(bd, start, n, zbuf)) {
            snprintf(err, 96, GS(MSG_CLR_ERR_WRITE_FMT),
                     (unsigned long)start, (long)bd->last_io_err);
            return FALSE;
        }
        start   += n;
        count   -= n;
        *zeroed += n;
    }
    return TRUE;
}

BOOL ClearRDB_Run(struct BlockDev *bd, const struct ClearPlan *plan,
                  ULONG *zeroed, char *err)
{
    ULONG  bs = (bd && bd->block_size) ? bd->block_size : 512;
    UBYTE *zbuf;
    BOOL   ok;

    *zeroed = 0;
    err[0]  = '\0';
    if (!bd) return FALSE;

    zbuf = (UBYTE *)AllocVec(CLR_CHUNK * bs, MEMF_PUBLIC | MEMF_CLEAR);
    if (!zbuf) {
        snprintf(err, 96, "%s", GS(MSG_CLR_ERR_NOMEM));
        return FALSE;
    }

    ok = zero_range(bd, 0, plan->head_blocks, zbuf, zeroed, err);
    if (ok && plan->gpt && plan->tail_blocks)
        ok = zero_range(bd, plan->tail_start, plan->tail_blocks, zbuf, zeroed, err);

    FreeVec(zbuf);
    return ok;
}
