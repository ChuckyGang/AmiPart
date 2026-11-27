/*
 * atacmd.h - send one ATA command (IDENTIFY DEVICE, SMART) to a drive.
 *
 * The command goes through the device driver and nowhere else: SCSI ATA
 * PASS-THROUGH (16), then (12), down HD_SCSICMD.  A driver's hardware is
 * never touched directly - "scsi.device" alone can be IDE or SCSI, and
 * every controller's port belongs to its driver.
 *
 * Drivers that implement pass-through (lide.device, UAE, ...) answer.
 * Commodore's scsi.device translates a fixed list of SCSI opcodes for IDE
 * drives and answers CHECK CONDITION to everything else, so IDENTIFY and
 * SMART cannot be read there; ATA_DiagPrint() says so in the driver's own
 * numbers.
 */

#ifndef ATACMD_H
#define ATACMD_H

#include <exec/types.h>
#include "rdb.h"

/* BlockDev.ata_via / AtaDiag.via */
#define ATA_VIA_NONE    0
#define ATA_VIA_PT16    1
#define ATA_VIA_PT12    2

struct AtaDiag {
    UBYTE via;            /* ATA_VIA_* of the CDB that worked, 0 if none   */
    UBYTE pt_tried;       /* pass-through was attempted                    */
    UBYTE pt_nocmd;       /* ...and the driver has no HD_SCSICMD at all    */
    BYTE  pt_io_err;      /* io_Error of the last attempt                  */
    UBYTE pt_status;      /* scsi_Status of the last attempt               */
    UBYTE pt_sense_len;   /* valid bytes in pt_sense (0..14)               */
    UBYTE pt_sense[14];
};

/* Send one ATA command.
 *   cmd     : 0xEC IDENTIFY DEVICE, 0xB0 SMART (the 4F/C2 signature is
 *             filled in automatically)
 *   feature : FEATURES register (SMART sub-command), 0 for IDENTIFY
 *   buf     : 512-byte MEMF_PUBLIC buffer for a one-sector PIO data-in
 *             command, or NULL for a non-data command.
 *             Layout: ATA word N = buf[2N] (low) | buf[2N+1] << 8.
 *   diag    : may be NULL
 * Returns 0 ok, -1 failed, -2 driver has no HD_SCSICMD.
 */
int ATA_Command(struct BlockDev *bd, UBYTE cmd, UBYTE feature, UBYTE *buf,
                struct AtaDiag *diag);

/* Explain a failed ATA_Command as display lines (no trailing newline,
 * <= 79 characters each). */
typedef void (*AtaDiagEmitFn)(void *ctx, const char *line);
void ATA_DiagPrint(struct BlockDev *bd, const struct AtaDiag *diag,
                   AtaDiagEmitFn emit, void *ctx);

#endif /* ATACMD_H */
