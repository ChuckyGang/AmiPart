/*
 * atacmd.c - one ATA command to a drive, through its device driver.
 * See atacmd.h.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
#include <proto/exec.h>

#include "clib.h"
#include "locale_support.h"
#include "rdb.h"
#include "atacmd.h"

#ifndef HFERR_BadStatus
#define HFERR_BadStatus 45
#endif

#define ATA_CMD_IDENTIFY  0xEC
#define ATA_CMD_SMART     0xB0

/* ------------------------------------------------------------------ */
/* SCSI ATA PASS-THROUGH down HD_SCSICMD                               */
/* ------------------------------------------------------------------ */

static BYTE pt_send(struct BlockDev *bd, int cdblen, UBYTE cmd, UBYTE feature,
                    UBYTE *buf, struct AtaDiag *diag)
{
    struct SCSICmd scmd;
    UBYTE          cdb[16];
    UBYTE          sense[32];
    BYTE           err;
    BOOL           has_data = (BOOL)(buf != NULL);
    BOOL           smart    = (BOOL)(cmd == ATA_CMD_SMART);
    UWORD          n;

    memset(&scmd, 0, sizeof(scmd));
    memset(cdb,   0, sizeof(cdb));
    memset(sense, 0, sizeof(sense));

    /* SAT encoding.  PROTOCOL: 4 = PIO Data-In, 3 = Non-data.
       Byte 2: T_DIR | BYT_BLOK | T_LENGTH = 2 (sector count field). */
    cdb[1] = has_data ? 0x08 : 0x06;
    cdb[2] = has_data ? 0x0E : 0x00;
    if (cdblen == 16) {
        cdb[0]  = 0x85;                 /* ATA PASS-THROUGH(16) */
        cdb[4]  = feature;
        cdb[6]  = has_data ? 1 : 0;     /* SECTOR COUNT         */
        cdb[10] = smart ? 0x4F : 0;     /* LBA MID              */
        cdb[12] = smart ? 0xC2 : 0;     /* LBA HIGH             */
        cdb[14] = cmd;
    } else {
        cdb[0]  = 0xA1;                 /* ATA PASS-THROUGH(12) */
        cdb[3]  = feature;
        cdb[4]  = has_data ? 1 : 0;
        cdb[6]  = smart ? 0x4F : 0;
        cdb[7]  = smart ? 0xC2 : 0;
        cdb[9]  = cmd;
    }

    scmd.scsi_Data        = has_data ? (UWORD *)buf : NULL;
    scmd.scsi_Length      = has_data ? 512 : 0;
    scmd.scsi_Command     = cdb;
    scmd.scsi_CmdLength   = (UWORD)cdblen;
    scmd.scsi_Flags       = (UBYTE)((has_data ? SCSIF_READ : 0) | SCSIF_AUTOSENSE);
    scmd.scsi_SenseData   = sense;
    scmd.scsi_SenseLength = sizeof(sense);

    bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
    bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
    bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);

    diag->pt_tried  = 1;
    diag->pt_io_err = err;
    diag->pt_status = scmd.scsi_Status;
    n = scmd.scsi_SenseActual;
    if (n > sizeof(diag->pt_sense)) n = sizeof(diag->pt_sense);
    diag->pt_sense_len = (UBYTE)n;
    memcpy(diag->pt_sense, sense, n);

    if (err == 0 && scmd.scsi_Status != 0)
        err = HFERR_BadStatus;
    return err;
}

/* ------------------------------------------------------------------ */

int ATA_Command(struct BlockDev *bd, UBYTE cmd, UBYTE feature, UBYTE *buf,
                struct AtaDiag *diag)
{
    struct AtaDiag local;
    BYTE           err;

    if (!diag) diag = &local;
    memset(diag, 0, sizeof(*diag));
    if (!bd || bd->backend != BD_DEVICE) return -1;

    if (bd->ata_via != ATA_VIA_PT12) {
        err = pt_send(bd, 16, cmd, feature, buf, diag);
        if (err == 0) {
            bd->ata_via = diag->via = ATA_VIA_PT16;
            return 0;
        }
        if (err == IOERR_NOCMD) { diag->pt_nocmd = 1; return -2; }
        if (bd->ata_via == ATA_VIA_PT16) return -1;
    }
    if (buf) memset(buf, 0, 512);
    err = pt_send(bd, 12, cmd, feature, buf, diag);
    if (err == 0) {
        bd->ata_via = diag->via = ATA_VIA_PT12;
        return 0;
    }
    if (err == IOERR_NOCMD) { diag->pt_nocmd = 1; return -2; }
    return -1;
}

void ATA_DiagPrint(struct BlockDev *bd, const struct AtaDiag *diag,
                   AtaDiagEmitFn emit, void *ctx)
{
    char line[96];

    if (!bd || !diag) return;
    if (!diag->pt_tried) return;                          /* image file */

    DP_SNPRINTF(line, GS(MSG_ATA_DIAG_DRIVER_FMT), bd->devname,
                (unsigned long)bd->unit);
    emit(ctx, line);

    if (diag->pt_tried) {
        if (diag->pt_nocmd) {
            emit(ctx, GS(MSG_ATA_DIAG_PT_NOCMD));
        } else {
            char  hex[2 * sizeof(diag->pt_sense) + 2];
            UWORD i, o = 0;
            for (i = 0; i < diag->pt_sense_len; i++) {
                static const char digits[] = "0123456789ABCDEF";
                hex[o++] = digits[diag->pt_sense[i] >> 4];
                hex[o++] = digits[diag->pt_sense[i] & 15];
            }
            if (o == 0) hex[o++] = '-';
            hex[o] = '\0';
            DP_SNPRINTF(line, GS(MSG_ATA_DIAG_PT_REFUSED_FMT),
                        (long)diag->pt_io_err, (unsigned long)diag->pt_status,
                        hex);
            emit(ctx, line);
            emit(ctx, GS(MSG_ATA_DIAG_NO_PT));
        }
    }

}
