/*
 * ataident.h - ATA IDENTIFY DEVICE (0xEC) via SCSI ATA PASS-THROUGH.
 *
 * Fetches the drive's own identification block - the data the old DOS
 * tool "WhatIDE" showed: model / serial / firmware strings, CHS geometry,
 * LBA sector count, LBA/DMA capability bits, PIO/MWDMA/UDMA modes,
 * sectors per interrupt and the (obsolete) buffer-size / ECC words.
 *
 * It is sent down the same HD_SCSICMD pipe as the SMART commands, so it
 * works exactly where SMART works (scsi.device IDE, lide.device, UAE) and
 * fails cleanly on real SCSI drives and drivers without pass-through.
 */

#ifndef ATAIDENT_H
#define ATAIDENT_H

#include <exec/types.h>
#include "rdb.h"

struct AtaIdent {
    char   model[41];       /* words 27-46, byte-swapped, trimmed */
    char   serial[21];      /* words 10-19 */
    char   firmware[9];     /* words 23-26 */
    ULONG  cylinders;       /* word 1  (legacy CHS)  */
    ULONG  heads;           /* word 3                */
    ULONG  sectors;         /* word 6                */
    BOOL   chs_placeholder; /* 16383/16/63 or zero = not the real geometry */
    UQUAD  lba_sectors;     /* words 60-61, or 100-103 when LBA48 */
    BOOL   lba;             /* word 49 bit 9  */
    BOOL   dma;             /* word 49 bit 8  */
    BOOL   lba48;           /* word 83 bit 10 */
    BOOL   smart_sup;       /* word 82 bit 0  */
    BOOL   smart_en;        /* word 85 bit 0  */
    WORD   pio_max;         /* highest PIO mode, -1 = unknown   */
    WORD   mwdma_max;       /* highest MWDMA mode, -1 = none    */
    WORD   udma_max;        /* highest UDMA mode, -1 = none     */
    UWORD  multi_sectors;   /* word 47 low byte (R/W MULTIPLE)  */
    ULONG  buffer_kb;       /* word 21 / 2 (obsolete, 0 = n/a)  */
    UWORD  ecc_bytes;       /* word 22 (obsolete, 0 = n/a)      */
    UWORD  ata_version;     /* highest ATA/ATAPI-n bit in word 80, 0 = n/a */
    UWORD  rotation;        /* word 217: 1 = SSD, 0x401..0xFFFE = RPM, else 0 */
};

/* Issue IDENTIFY DEVICE and decode it.
 * Returns 0 ok, -1 command failed / no data, -2 HD_SCSICMD unsupported. */
int ATA_Identify(struct BlockDev *bd, struct AtaIdent *id);

/* Emit the decoded block as display lines (no trailing newline) through
 * `emit`.  Lines are <= 79 characters so the GUI list viewer never clips. */
typedef void (*AtaEmitFn)(void *ctx, const char *line);
void ATA_IdentPrint(const struct AtaIdent *id, AtaEmitFn emit, void *ctx);

#endif /* ATAIDENT_H */
