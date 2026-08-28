/* PoC: open a real Amiga disk image on Linux and parse its RDB using
 * DiskPart's UNMODIFIED rdb.c, proving the RDB/partition core is reusable
 * on the host through the existing BlockDev seam. */
#include <stdio.h>
#include "amiga_compat.h"
#include "rdb.h"

int main(int argc, char **argv) {
    struct BlockDev *bd;
    struct RDBInfo rdb;
    int i;

    if (argc < 2) { fprintf(stderr, "usage: %s <image.img>\n", argv[0]); return 2; }

    bd = BlockDev_OpenFile(argv[1]);
    if (!bd) { fprintf(stderr, "cannot open %s (IoErr=%ld)\n", argv[1], (long)IoErr()); return 1; }

    printf("Opened %s  (%llu bytes, %lu blocks of %lu)\n\n",
           argv[1], (unsigned long long)bd->total_bytes,
           (unsigned long)(bd->total_bytes / bd->block_size),
           (unsigned long)bd->block_size);

    if (!RDB_Read(bd, &rdb) || !rdb.valid) {
        printf("RDB_Read: no valid RDB found\n");
        BlockDev_Close(bd);
        return 1;
    }

    printf("RDB @ block %lu   geometry %lu cyl x %lu heads x %lu sect  (blk_size %lu)\n",
           (unsigned long)rdb.block_num, (unsigned long)rdb.cylinders,
           (unsigned long)rdb.heads, (unsigned long)rdb.sectors,
           (unsigned long)rdb.blk_size);
    printf("vendor '%s'  product '%s'  rev '%s'\n",
           rdb.disk_vendor, rdb.disk_product, rdb.disk_revision);
    printf("%u partition(s), %u filesystem(s)\n\n", rdb.num_parts, rdb.num_fs);

    for (i = 0; i < rdb.num_parts; i++) {
        struct PartInfo *p = &rdb.parts[i];
        char dt[16], sz[16];
        UQUAD bytes = (UQUAD)(p->high_cyl - p->low_cyl + 1) *
                      p->heads * p->sectors * p->block_size;
        FormatDosType(p->dos_type, dt);
        FormatSize(bytes, sz);
        printf("  [%d] %-12s cyl %5lu-%-5lu  %-6s  %s  bootpri %ld\n",
               i, p->drive_name,
               (unsigned long)p->low_cyl, (unsigned long)p->high_cyl,
               sz, dt, (long)p->boot_pri);
    }
    if (rdb.num_fs) {
        printf("\n  filesystems:\n");
        for (i = 0; i < rdb.num_fs; i++) {
            struct FSInfo *f = &rdb.filesystems[i];
            char dt[16];
            FormatDosType(f->dos_type, dt);
            printf("  [%d] %-6s ver %lu.%lu  '%s'  %lu code bytes\n",
                   i, dt, (unsigned long)(f->version >> 16),
                   (unsigned long)(f->version & 0xFFFF), f->fs_name,
                   (unsigned long)f->code_size);
        }
    }

    /* Write-back test: argv[2] = pre-zeroed image of the same size.
       The refactored (endian-explicit) rdb.c must reproduce the RDB
       byte-for-byte from this little-endian host. */
    if (argc >= 3) {
        struct BlockDev *bd2 = BlockDev_OpenFile(argv[2]);
        if (!bd2) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
        if (!RDB_Write(bd2, &rdb)) {
            printf("RDB_Write FAILED on host\n");
            BlockDev_Close(bd2);
            return 1;
        }
        BlockDev_Close(bd2);
        printf("\nRDB_Write to %s done (host, little-endian path).\n", argv[2]);
    }

    RDB_FreeCode(&rdb);
    BlockDev_Close(bd);
    printf("OK - parsed with AmiPart's endian-explicit rdb.c.\n");
    return 0;
}
