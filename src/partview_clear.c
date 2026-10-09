/*
 * partview_clear.c - Advanced > Clear RDB and MBR...
 *
 * Zeroes the partition-table area of the open disk (RDB scan area, MBR,
 * GPT headers) after two confirmations.  Returns TRUE when something was
 * written so the caller re-reads the disk.
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include "clib.h"
#include "rdb.h"
#include "mbr.h"
#include "clearrdb.h"
#include "partview_internal.h"
#include "locale_support.h"

extern struct IntuitionBase *IntuitionBase;

BOOL offer_clear_rdb(struct Window *win, struct BlockDev *bd,
                     const struct RDBInfo *rdb, const struct MBRInfo *mbr)
{
    struct EasyStruct es;
    struct ClearPlan  plan;
    char   body[400], err[96];
    ULONG  zeroed = 0;
    BOOL   ok;

    es.es_StructSize = sizeof(es);
    es.es_Flags      = 0;
    es.es_Title      = (UBYTE *)GS(MSG_CLR_TITLE);

    if (!bd) {
        es.es_TextFormat   = (UBYTE *)GS(MSG_IMG_DEV_NOT_ACCESSIBLE);
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
        return FALSE;
    }

    ClearRDB_Plan(bd, rdb, mbr, &plan);

    DP_SNPRINTF(body, GS(MSG_CLR_WARN_BODY_FMT), bd->devname,
                (unsigned long)bd->unit, (unsigned long)(plan.head_blocks - 1),
                plan.gpt ? GS(MSG_CLR_GPT_NOTE) : "");
    es.es_TextFormat   = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)GS(MSG_IMG_CONTINUE_CANCEL);
    if (EasyRequest(win, &es, NULL) != 1) return FALSE;

    DP_SNPRINTF(body, GS(MSG_CLR_CONFIRM_BODY_FMT), bd->devname,
                (unsigned long)bd->unit);
    es.es_TextFormat   = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)GS(MSG_CLR_CONFIRM_GADGETS);
    if (EasyRequest(win, &es, NULL) != 1) return FALSE;

    ok = ClearRDB_Run(bd, &plan, &zeroed, err);

    if (ok)
        DP_SNPRINTF(body, GS(MSG_CLR_DONE_FMT), (unsigned long)zeroed);
    else
        DP_SNPRINTF(body, GS(MSG_CLR_FAIL_FMT), err);
    es.es_TextFormat   = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
    EasyRequest(win, &es, NULL);

    return (BOOL)(zeroed > 0);   /* partial writes also need a re-read */
}
