/*
 * ataident.c - ATA IDENTIFY DEVICE via SCSI ATA PASS-THROUGH.  See ataident.h.
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
#include "ataident.h"

/* The 512-byte block is handed to us exactly as the SMART code sees its
   data: ATA word N = buf[2N] (low byte) | buf[2N+1] << 8 (high byte).
   ATA strings put the FIRST character of each pair in the HIGH byte. */
#define W(n) ((UWORD)buf[2*(n)] | ((UWORD)buf[2*(n)+1] << 8))

static void ata_string(char *dst, const UBYTE *buf, int first_word, int nwords)
{
    int i, n = 0;
    for (i = 0; i < nwords; i++) {
        dst[n++] = (char)buf[2*(first_word+i) + 1];
        dst[n++] = (char)buf[2*(first_word+i)];
    }
    dst[n] = '\0';
    /* trim trailing blanks, then leading blanks (some drives right-align) */
    while (n > 0 && (dst[n-1] == ' ' || dst[n-1] == '\0')) dst[--n] = '\0';
    i = 0;
    while (dst[i] == ' ') i++;
    if (i) memmove(dst, dst + i, (size_t)(n - i + 1));
    /* scrub anything non-printable */
    for (i = 0; dst[i]; i++)
        if ((UBYTE)dst[i] < 32 || (UBYTE)dst[i] > 126) dst[i] = '?';
}

static WORD highest_bit(UWORD v, int nbits)
{
    int b;
    for (b = nbits - 1; b >= 0; b--)
        if (v & (1 << b)) return (WORD)b;
    return -1;
}

/* One IDENTIFY DEVICE attempt with the given CDB length (16 or 12). */
static BYTE ata_identify_cdb(struct BlockDev *bd, UBYTE *buf, int cdblen,
                             UBYTE *status)
{
    struct SCSICmd scmd;
    UBYTE          cdb[16];
    UBYTE          sense[32];
    BYTE           err;

    memset(&scmd, 0, sizeof(scmd));
    memset(cdb,   0, sizeof(cdb));
    memset(sense, 0, sizeof(sense));

    if (cdblen == 16) {
        cdb[0]  = 0x85;       /* ATA PASS-THROUGH(16)                      */
        cdb[1]  = 0x08;       /* PROTOCOL = 4 (PIO Data-In) << 1           */
        cdb[2]  = 0x0E;       /* T_DIR=1 | BYT_BLOK=1 | T_LENGTH=2 (count) */
        cdb[6]  = 1;          /* SECTOR COUNT = 1                           */
        cdb[14] = 0xEC;       /* COMMAND = IDENTIFY DEVICE                  */
    } else {
        cdb[0]  = 0xA1;       /* ATA PASS-THROUGH(12)                      */
        cdb[1]  = 0x08;
        cdb[2]  = 0x0E;
        cdb[4]  = 1;
        cdb[9]  = 0xEC;
    }

    scmd.scsi_Data        = (UWORD *)buf;
    scmd.scsi_Length      = 512;
    scmd.scsi_Command     = cdb;
    scmd.scsi_CmdLength   = (UWORD)cdblen;
    scmd.scsi_Flags       = SCSIF_READ;
    scmd.scsi_SenseData   = sense;
    scmd.scsi_SenseLength = sizeof(sense);

    bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
    bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
    bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
    *status = scmd.scsi_Status;
    return err;
}

int ATA_Identify(struct BlockDev *bd, struct AtaIdent *id)
{
    UBYTE *buf;
    UBYTE  status = 0;
    BYTE   err;
    UWORD  w;
    int    rc = -1;

    memset(id, 0, sizeof(*id));
    id->pio_max = id->mwdma_max = id->udma_max = -1;
    if (!bd) return -1;

    /* MEMF_PUBLIC + full sector: lide.device PIOs a whole 512-byte block. */
    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return -1;

    err = ata_identify_cdb(bd, buf, 16, &status);
    if (err == IOERR_NOCMD) { FreeVec(buf); return -2; }
    if (err != 0 || status != 0) {
        memset(buf, 0, 512);
        err = ata_identify_cdb(bd, buf, 12, &status);
        if (err == IOERR_NOCMD) { FreeVec(buf); return -2; }
    }
    if (err != 0 || status != 0) goto out;

    /* Sanity: word 0 bit 15 set = ATAPI (IDENTIFY DEVICE aborts there
       anyway); an all-blank model string means we got no real block. */
    if (W(0) & 0x8000) goto out;
    ata_string(id->model,    buf, 27, 20);
    ata_string(id->serial,   buf, 10, 10);
    ata_string(id->firmware, buf, 23,  4);
    if (id->model[0] == '\0' && W(1) == 0 && W(60) == 0 && W(61) == 0)
        goto out;

    id->cylinders = W(1);
    id->heads     = W(3);
    id->sectors   = W(6);
    id->chs_placeholder = (BOOL)(id->cylinders == 0 ||
                                 (id->cylinders == 16383 && id->heads == 16 &&
                                  id->sectors == 63));

    w = W(49);
    id->lba = (BOOL)((w & 0x0200) != 0);
    id->dma = (BOOL)((w & 0x0100) != 0);

    w = W(83);
    id->lba48 = (BOOL)(w != 0 && w != 0xFFFF && (w & 0x0400));

    id->lba_sectors = (UQUAD)W(60) | ((UQUAD)W(61) << 16);
    if (id->lba48) {
        UQUAD s48 = (UQUAD)W(100) | ((UQUAD)W(101) << 16)
                  | ((UQUAD)W(102) << 32) | ((UQUAD)W(103) << 48);
        if (s48) id->lba_sectors = s48;
    }

    w = W(82);
    id->smart_sup = (BOOL)(w != 0 && w != 0xFFFF && (w & 0x0001));
    w = W(85);
    id->smart_en  = (BOOL)(id->smart_sup && w != 0 && w != 0xFFFF && (w & 0x0001));

    /* PIO: word 64 (modes 3/4) is valid when word 53 bit 1 is set;
       otherwise fall back to the pre-ATA-2 word 51 high byte (0..2). */
    if (W(53) & 0x0002) {
        WORD hb = highest_bit((UWORD)(W(64) & 0x0003), 2);
        id->pio_max = (hb >= 0) ? (WORD)(hb + 3) : (WORD)(W(51) >> 8);
    } else {
        id->pio_max = (WORD)(W(51) >> 8);
    }
    if (id->pio_max > 6) id->pio_max = -1;

    id->mwdma_max = highest_bit((UWORD)(W(63) & 0x0007), 3);
    if (W(53) & 0x0004)
        id->udma_max = highest_bit((UWORD)(W(88) & 0x007F), 7);

    id->multi_sectors = (UWORD)(W(47) & 0x00FF);
    id->buffer_kb     = (ULONG)W(21) / 2;      /* 512-byte units -> KB */
    id->ecc_bytes     = W(22);

    w = W(80);
    if (w != 0 && w != 0xFFFF) {
        WORD hb = highest_bit((UWORD)(w & 0x7FFE), 15);
        if (hb > 0) id->ata_version = (UWORD)hb;
    }

    w = W(217);
    if (w == 1 || (w >= 0x0401 && w <= 0xFFFE)) id->rotation = w;

    rc = 0;
out:
    FreeVec(buf);
    return rc;
}

static const char *mode_str(WORD m, char *tmp)
{
    if (m < 0) return "-";
    tmp[0] = (char)('0' + m); tmp[1] = '\0';
    return tmp;
}

void ATA_IdentPrint(const struct AtaIdent *id, AtaEmitFn emit, void *ctx)
{
    char line[80], sz[20], t1[4], t2[4], t3[4];

    emit(ctx, GS(MSG_ATA_HDR));

    DP_SNPRINTF(line, GS(MSG_ATA_MODEL_FMT), id->model);     emit(ctx, line);
    DP_SNPRINTF(line, GS(MSG_ATA_SERIAL_FMT), id->serial);   emit(ctx, line);
    DP_SNPRINTF(line, GS(MSG_ATA_FIRMWARE_FMT), id->firmware); emit(ctx, line);

    DP_SNPRINTF(line, GS(MSG_ATA_CHS_FMT),
            (unsigned long)id->cylinders, (unsigned long)id->heads,
            (unsigned long)id->sectors,
            id->chs_placeholder ? GS(MSG_ATA_CHS_PLACEHOLDER)
                                : GS(MSG_ATA_CHS_REAL));
    emit(ctx, line);

    if (id->lba_sectors) {
        FormatSize(id->lba_sectors * 512ULL, sz);
        DP_SNPRINTF(line, GS(MSG_ATA_CAPACITY_FMT),
                (unsigned long)id->lba_sectors, sz,
                id->lba48 ? GS(MSG_ATA_LBA48) : GS(MSG_ATA_LBA28));
        emit(ctx, line);
    }

    DP_SNPRINTF(line, GS(MSG_ATA_FLAGS_FMT),
            id->lba ? GS(MSG_YES) : GS(MSG_NO),
            id->dma ? GS(MSG_YES) : GS(MSG_NO),
            !id->smart_sup ? GS(MSG_NO)
                           : (id->smart_en ? GS(MSG_ATA_YES_ENABLED)
                                           : GS(MSG_ATA_YES_DISABLED)));
    emit(ctx, line);

    DP_SNPRINTF(line, GS(MSG_ATA_MODES_FMT),
            mode_str(id->pio_max, t1), mode_str(id->mwdma_max, t2),
            mode_str(id->udma_max, t3));
    emit(ctx, line);

    if (id->multi_sectors) {
        DP_SNPRINTF(line, GS(MSG_ATA_MULTI_FMT), (unsigned)id->multi_sectors);
        emit(ctx, line);
    }
    if (id->buffer_kb || id->ecc_bytes) {
        DP_SNPRINTF(line, GS(MSG_ATA_LEGACY_FMT),
                (unsigned long)id->buffer_kb, (unsigned)id->ecc_bytes);
        emit(ctx, line);
    }
    if (id->ata_version) {
        DP_SNPRINTF(line, GS(MSG_ATA_VERSION_FMT), (unsigned)id->ata_version);
        emit(ctx, line);
    }
    if (id->rotation == 1)
        emit(ctx, GS(MSG_ATA_SSD));
    else if (id->rotation) {
        DP_SNPRINTF(line, GS(MSG_ATA_RPM_FMT), (unsigned)id->rotation);
        emit(ctx, line);
    }
}
