/*
 * atacmd.c - one ATA command to a drive: pass-through first, then the
 * IDE port itself where the driver is known to have no pass-through.
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
/* Path 1: SCSI ATA PASS-THROUGH                                       */
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
/* Path 2: the IDE task-file registers                                 */
/*                                                                     */
/* This goes behind the back of a driver that owns the port, so:       */
/*                                                                     */
/*  - It is only used for "scsi.device" units 0/1 whose INQUIRY data   */
/*    has the exact shape Commodore's IDE code builds, on a machine    */
/*    with a Gayle (card.resource) or an AGA chipset.  Nothing in the  */
/*    $DAxxxx / $DDxxxx range is touched before that is established -  */
/*    on an A3000 the SDMAC answers at $DD20xx and reads there have    */
/*    side effects.                                                    */
/*  - Under Forbid(), the alternate status register must show the bus  */
/*    idle (no BSY, no DRQ) or nothing is written.                     */
/*  - The driver's task may have been preempted between loading the    */
/*    task file and writing the command register, so the readable      */
/*    task-file registers are saved first and put back afterwards.     */
/*  - nIEN is set for the duration: the driver's interrupt server      */
/*    never sees an interrupt it did not ask for.  It is cleared again */
/*    after the status register has been read (the driver runs with    */
/*    interrupts enabled, device control = 0).                         */
/*  - Before the first real use the drive is asked for its IDENTIFY    */
/*    block, and its model string must match what the driver reported  */
/*    for this unit.                                                   */
/* ------------------------------------------------------------------ */

#ifndef AMIPART_HOST

#define ATAF_BSY   0x80
#define ATAF_DRDY  0x40
#define ATAF_DRQ   0x08
#define ATAF_ERR   0x01

struct AtaPort {
    volatile UWORD *data;
    volatile UBYTE *feat;     /* read: error */
    volatile UBYTE *cnt;
    volatile UBYTE *lba0;
    volatile UBYTE *lba1;
    volatile UBYTE *lba2;
    volatile UBYTE *dh;
    volatile UBYTE *cmd;      /* read: status (clears the interrupt) */
    volatile UBYTE *alt;      /* read: alt status, write: device control */
};

/* Register layouts as scsi.device itself uses them (board.i). */
static void port_setup(struct AtaPort *p, ULONG base, ULONG first, ULONG alt)
{
    p->data = (volatile UWORD *)base;
    p->feat = (volatile UBYTE *)(base + first);
    p->cnt  = (volatile UBYTE *)(base + first + 4);
    p->lba0 = (volatile UBYTE *)(base + first + 8);
    p->lba1 = (volatile UBYTE *)(base + first + 12);
    p->lba2 = (volatile UBYTE *)(base + first + 16);
    p->dh   = (volatile UBYTE *)(base + first + 20);
    p->cmd  = (volatile UBYTE *)(base + first + 24);
    p->alt  = (volatile UBYTE *)(base + alt);
}

static BOOL name_is(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return FALSE;
        if (!x) return TRUE;
    }
}

/* INQUIRY through the driver; fills inq[36].  No hardware access. */
static BOOL driver_inquiry(struct BlockDev *bd, UBYTE *inq)
{
    struct SCSICmd scmd;
    UBYTE          cdb[6];
    UBYTE          sense[18];
    UBYTE         *buf;
    BOOL           ok = FALSE;

    buf = (UBYTE *)AllocVec(256, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return FALSE;

    memset(&scmd, 0, sizeof(scmd));
    memset(cdb,   0, sizeof(cdb));
    memset(sense, 0, sizeof(sense));
    cdb[0] = 0x12;
    cdb[4] = 36;

    scmd.scsi_Data        = (UWORD *)buf;
    scmd.scsi_Length      = 36;
    scmd.scsi_Command     = cdb;
    scmd.scsi_CmdLength   = 6;
    scmd.scsi_Flags       = SCSIF_READ | SCSIF_AUTOSENSE;
    scmd.scsi_SenseData   = sense;
    scmd.scsi_SenseLength = sizeof(sense);

    bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
    bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
    bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    if (DoIO((struct IORequest *)&bd->iotd) == 0 && scmd.scsi_Status == 0 &&
        scmd.scsi_Actual >= 36) {
        memcpy(inq, buf, 36);
        ok = TRUE;
    }
    FreeVec(buf);
    return ok;
}

/* Which IDE port is behind this unit?  Decided without touching the port. */
static int direct_port(struct BlockDev *bd, struct AtaPort *p, UBYTE *inq)
{
    int i;

    if (bd->backend != BD_DEVICE)             return ATAD_NOPORT;
    if (!name_is(bd->devname, "scsi.device")) return ATAD_NOPORT;
    if (bd->unit > 1)                         return ATAD_NOPORT;
    if (!driver_inquiry(bd, inq))             return ATAD_NOPORT;

    /* The fixed header Commodore's IDE code puts in front of the strings
       (attask.asm XT_INQUIRY): SCSI-2, response format 2, 31 more bytes,
       two zero bytes, WBus16 and nothing else.  A real SCSI disk on an
       A3000's scsi.device does not look like this. */
    if ((inq[0] != 0 && inq[0] != 7) || inq[2] != 2 || inq[3] != 2 ||
        inq[4] != 31 || inq[5] != 0 || inq[6] != 0 || inq[7] != 0x20)
        return ATAD_NOPORT;

    if (OpenResource((STRPTR)"card.resource")) {
        port_setup(p, 0xDA2000UL, 4, 0x1018);      /* A600 / A1200 Gayle */
        return ATAD_OK;
    }
    for (i = 0; i < 8; i++)                         /* LISAID: AGA only   */
        if ((*(volatile UWORD *)0xDFF07CUL & 0x00FF) != 0x00F8)
            return ATAD_NOPORT;
    port_setup(p, 0xDD2020UL, 6, 0x101A);          /* A4000 / A4000T     */
    return ATAD_OK;
}

/* >= 400 ns: one CIA access is a full E-clock cycle (1.4 us). */
static void ata_delay(void)
{
    (void)*(volatile UBYTE *)0xBFE001UL;
    (void)*(volatile UBYTE *)0xBFE001UL;
}

/* Wait for BSY to drop.  Bounded by video frames (about 3 seconds) with an
   iteration cap behind it; every pass costs at least one chip-bus read. */
static BOOL wait_not_busy(const struct AtaPort *p, UBYTE *st)
{
    ULONG frames = 0, iters = 0;
    ULONG prev = (*(volatile ULONG *)0xDFF004UL >> 8) & 0x7FF;

    for (;;) {
        UBYTE s = *p->alt;
        ULONG cur;
        if (!(s & ATAF_BSY)) { *st = s; return TRUE; }
        cur = (*(volatile ULONG *)0xDFF004UL >> 8) & 0x7FF;
        if (cur < prev) frames++;
        prev = cur;
        if (frames > 180 || ++iters > 20000000UL) { *st = s; return FALSE; }
    }
}

static int direct_raw(const struct AtaPort *p, ULONG unit, UBYTE cmd,
                      UBYTE feature, UBYTE *buf, struct AtaDiag *diag)
{
    UBYTE st, s_cnt, s_lba0, s_lba1, s_lba2, s_dh;
    int   rc = ATAD_OK;
    UWORD i;

    Forbid();

    st = *p->alt;
    if (st == 0xFF)                  { Permit(); return ATAD_NODRIVE; }
    if (st & (ATAF_BSY | ATAF_DRQ))  { Permit(); return ATAD_BUSY; }

    s_cnt  = *p->cnt;
    s_lba0 = *p->lba0;
    s_lba1 = *p->lba1;
    s_lba2 = *p->lba2;
    s_dh   = *p->dh;

    *p->dh = (UBYTE)(0xA0 | (unit << 4));
    ata_delay();
    st = *p->alt;
    if (st == 0xFF || st == 0x00 || !(st & ATAF_DRDY)) rc = ATAD_NODRIVE;
    else if (st & (ATAF_BSY | ATAF_DRQ))               rc = ATAD_BUSY;
    if (rc != ATAD_OK) {
        if (!(st & ATAF_BSY)) *p->dh = s_dh;
        Permit();
        return rc;
    }

    *p->alt  = 0x02;                             /* nIEN */
    *p->feat = feature;
    *p->cnt  = (UBYTE)(buf ? 1 : 0);
    *p->lba0 = 0;
    *p->lba1 = (UBYTE)(cmd == ATA_CMD_SMART ? 0x4F : 0);
    *p->lba2 = (UBYTE)(cmd == ATA_CMD_SMART ? 0xC2 : 0);
    *p->cmd  = cmd;
    ata_delay();

    if (!wait_not_busy(p, &st)) {
        rc = ATAD_TIMEOUT;
    } else if (st & ATAF_ERR) {
        diag->ata_error = *p->feat;
        rc = ATAD_ABORTED;
    } else if (buf) {
        if (!(st & ATAF_DRQ)) {
            rc = ATAD_ABORTED;                   /* no error, but no data */
        } else {
            for (i = 0; i < 256; i++) {
                UWORD w = *p->data;
                buf[2*i]     = (UBYTE)(w >> 8);
                buf[2*i + 1] = (UBYTE)w;
            }
        }
    }
    diag->ata_status = st;

    if (rc != ATAD_TIMEOUT) {
        (void)*p->cmd;                           /* status: drops INTRQ */
        st = *p->alt;
        if (!(st & (ATAF_BSY | ATAF_DRQ))) {
            *p->cnt  = s_cnt;
            *p->lba0 = s_lba0;
            *p->lba1 = s_lba1;
            *p->lba2 = s_lba2;
            *p->dh   = s_dh;
        }
    }
    *p->alt = 0x00;                              /* interrupts back on */

    Permit();
    return rc;
}

/* Copy src without blanks; returns the length. */
static int squeeze(char *dst, const UBYTE *src, int n, int max)
{
    int i, o = 0;
    for (i = 0; i < n && o < max - 1; i++)
        if (src[i] != ' ' && src[i] != 0) dst[o++] = (char)src[i];
    dst[o] = '\0';
    return o;
}

/* Is the drive that answered IDENTIFY on the port the one the driver
   reported for this unit?  scsi.device builds INQUIRY vendor (8) +
   product (16) from the first characters of the IDENTIFY model string. */
static BOOL same_drive(const UBYTE *inq, const UBYTE *ident)
{
    UBYTE model[40];
    char  a[28], b[44];
    int   i, la;

    for (i = 0; i < 20; i++) {                    /* words 27..46 */
        model[2*i]     = ident[2*(27 + i) + 1];
        model[2*i + 1] = ident[2*(27 + i)];
    }
    la = squeeze(a, inq + 8, 24, (int)sizeof(a));
    squeeze(b, model, 40, (int)sizeof(b));
    if (la < 4) return FALSE;
    return (BOOL)(memcmp(a, b, (size_t)la) == 0);
}

static int direct_cmd(struct BlockDev *bd, UBYTE cmd, UBYTE feature,
                      UBYTE *buf, struct AtaDiag *diag)
{
    struct AtaPort port;
    UBYTE          inq[36];
    int            rc;

    if (bd->ata_direct == 2) return bd->ata_direct_rc;

    rc = direct_port(bd, &port, inq);
    if (rc != ATAD_OK) goto refuse;

    if (bd->ata_direct == 0) {
        UBYTE *id = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
        if (!id) return ATAD_NOMEM;
        rc = direct_raw(&port, bd->unit, ATA_CMD_IDENTIFY, 0, id, diag);
        if (rc == ATAD_OK && !same_drive(inq, id)) rc = ATAD_MISMATCH;
        if (rc == ATAD_OK && cmd == ATA_CMD_IDENTIFY && buf)
            memcpy(buf, id, 512);
        FreeVec(id);
        if (rc == ATAD_BUSY) return rc;           /* may work next time */
        if (rc != ATAD_OK) goto refuse;
        bd->ata_direct = 1;
        if (cmd == ATA_CMD_IDENTIFY && buf) return ATAD_OK;
    }
    return direct_raw(&port, bd->unit, cmd, feature, buf, diag);

refuse:
    bd->ata_direct    = 2;
    bd->ata_direct_rc = (BYTE)rc;
    return rc;
}

#else  /* AMIPART_HOST: image files and host block devices have no IDE port */

static int direct_cmd(struct BlockDev *bd, UBYTE cmd, UBYTE feature,
                      UBYTE *buf, struct AtaDiag *diag)
{
    (void)bd; (void)cmd; (void)feature; (void)buf; (void)diag;
    return ATAD_NOPORT;
}

#endif

/* ------------------------------------------------------------------ */

int ATA_Command(struct BlockDev *bd, UBYTE cmd, UBYTE feature, UBYTE *buf,
                struct AtaDiag *diag)
{
    struct AtaDiag local;
    BYTE           err;
    int            rc;

    if (!diag) diag = &local;
    memset(diag, 0, sizeof(*diag));
    if (!bd || bd->backend != BD_DEVICE) return -1;

    if (bd->ata_via != ATA_VIA_DIRECT) {
        if (bd->ata_via != ATA_VIA_PT12) {
            err = pt_send(bd, 16, cmd, feature, buf, diag);
            if (err == 0) {
                bd->ata_via = diag->via = ATA_VIA_PT16;
                return 0;
            }
            if (err == IOERR_NOCMD) diag->pt_nocmd = 1;
        }
        if (!diag->pt_nocmd && bd->ata_via != ATA_VIA_PT16) {
            if (buf) memset(buf, 0, 512);
            err = pt_send(bd, 12, cmd, feature, buf, diag);
            if (err == 0) {
                bd->ata_via = diag->via = ATA_VIA_PT12;
                return 0;
            }
        }
        /* Pass-through has worked on this drive before: the drive itself
           turned this command down, the port would say the same. */
        if (bd->ata_via != ATA_VIA_NONE) return -1;
        if (buf) memset(buf, 0, 512);
    }

    diag->direct_tried = 1;
    rc = direct_cmd(bd, cmd, feature, buf, diag);
    diag->direct_rc = (BYTE)rc;
    if (rc == ATAD_OK) {
        bd->ata_via = diag->via = ATA_VIA_DIRECT;
        return 0;
    }
    return (diag->pt_nocmd && rc == ATAD_NOPORT) ? -2 : -1;
}

void ATA_DiagPrint(struct BlockDev *bd, const struct AtaDiag *diag,
                   AtaDiagEmitFn emit, void *ctx)
{
    char line[96];

    if (!bd || !diag) return;
    if (!diag->pt_tried && !diag->direct_tried) return;   /* image file */

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
        }
    }

    if (!diag->direct_tried) return;
    switch (diag->direct_rc) {
    case ATAD_NOPORT:
        emit(ctx, GS(MSG_ATA_DIAG_DIRECT_NOPORT));
        break;
    case ATAD_BUSY:
        emit(ctx, GS(MSG_ATA_DIAG_DIRECT_BUSY_1));
        emit(ctx, GS(MSG_ATA_DIAG_DIRECT_BUSY_2));
        break;
    case ATAD_NODRIVE:
        emit(ctx, GS(MSG_ATA_DIAG_DIRECT_NODRIVE));
        break;
    case ATAD_TIMEOUT:
        emit(ctx, GS(MSG_ATA_DIAG_DIRECT_TIMEOUT));
        break;
    case ATAD_ABORTED:
        DP_SNPRINTF(line, GS(MSG_ATA_DIAG_DIRECT_ABORT_FMT),
                    (unsigned long)diag->ata_status,
                    (unsigned long)diag->ata_error);
        emit(ctx, line);
        break;
    case ATAD_MISMATCH:
        emit(ctx, GS(MSG_ATA_DIAG_DIRECT_MISMATCH));
        break;
    default:
        break;
    }
}
