/*
 * atacmd.h - send one ATA command (IDENTIFY DEVICE, SMART) to a drive.
 *
 * Two ways in, tried in this order:
 *
 *  1. SCSI ATA PASS-THROUGH (16), then (12), down HD_SCSICMD.  Works with
 *     drivers that implement it (lide.device, UAE, ...).
 *
 *  2. Straight to the IDE task-file registers, for Commodore's scsi.device
 *     on the A600/A1200 (Gayle) and A4000/A4000T motherboard IDE ports.
 *     That driver translates a fixed list of SCSI opcodes and answers
 *     CHECK CONDITION to everything else, so pass-through can never work
 *     there.  See atacmd.c for the gates that guard this path.
 *
 * The path that worked is remembered in the BlockDev, so a session never
 * mixes the two.
 */

#ifndef ATACMD_H
#define ATACMD_H

#include <exec/types.h>
#include "rdb.h"

/* BlockDev.ata_via / AtaDiag.via */
#define ATA_VIA_NONE    0
#define ATA_VIA_PT16    1
#define ATA_VIA_PT12    2
#define ATA_VIA_DIRECT  3

/* AtaDiag.direct_rc */
#define ATAD_OK         0
#define ATAD_NOPORT   (-1)  /* no known IDE port behind this driver/unit   */
#define ATAD_BUSY     (-2)  /* drive busy right now - nothing was touched  */
#define ATAD_NODRIVE  (-3)  /* nothing (ready) answered on the port        */
#define ATAD_TIMEOUT  (-4)  /* drive stayed busy after the command         */
#define ATAD_ABORTED  (-5)  /* drive answered ERR - command not supported  */
#define ATAD_MISMATCH (-6)  /* the drive on the port is not the driver's   */
#define ATAD_NOMEM    (-7)

struct AtaDiag {
    UBYTE via;            /* ATA_VIA_* of the path that worked, 0 if none  */
    UBYTE pt_tried;       /* pass-through was attempted                    */
    UBYTE pt_nocmd;       /* ...and the driver has no HD_SCSICMD at all    */
    BYTE  pt_io_err;      /* io_Error of the last pass-through attempt     */
    UBYTE pt_status;      /* scsi_Status of the last pass-through attempt  */
    UBYTE pt_sense_len;   /* valid bytes in pt_sense (0..14)               */
    UBYTE pt_sense[14];
    UBYTE direct_tried;   /* the direct path was attempted                 */
    BYTE  direct_rc;      /* ATAD_*                                        */
    UBYTE ata_status;     /* status register after a direct command        */
    UBYTE ata_error;      /* error register after a direct ERR             */
};

/* Send one ATA command.
 *   cmd     : 0xEC IDENTIFY DEVICE, 0xB0 SMART (the 4F/C2 signature is
 *             filled in automatically)
 *   feature : FEATURES register (SMART sub-command), 0 for IDENTIFY
 *   buf     : 512-byte, word-aligned MEMF_PUBLIC buffer for a one-sector
 *             PIO data-in command, or NULL for a non-data command.
 *             Layout: ATA word N = buf[2N] (low) | buf[2N+1] << 8.
 *   diag    : may be NULL
 * Returns 0 ok, -1 failed, -2 driver has no HD_SCSICMD (and no direct path).
 */
int ATA_Command(struct BlockDev *bd, UBYTE cmd, UBYTE feature, UBYTE *buf,
                struct AtaDiag *diag);

/* Explain a failed ATA_Command as display lines (no trailing newline,
 * <= 79 characters each). */
typedef void (*AtaDiagEmitFn)(void *ctx, const char *line);
void ATA_DiagPrint(struct BlockDev *bd, const struct AtaDiag *diag,
                   AtaDiagEmitFn emit, void *ctx);

#endif /* ATACMD_H */
