/* host_devices.c - Linux implementation of AmiPart's device enumeration.
 *
 * Devices_Scan mirrors the Amiga contract (a pure memory walk, no disk
 * I/O): it reads sysfs only.  names[] holds openable /dev paths - the
 * same strings DEV= accepts - and display[] a one-line summary that the
 * CLI prints in place of the bare driver name.
 *
 * Devices_GetUnitsForName (LISTDEV UNITS) is the probing step, like the
 * Amiga unit probe: it opens the disk O_RDONLY (non-exclusive - probing
 * a mounted disk is fine read-only) and looks for an RDB in the first
 * 16 blocks, else GPT/MBR signatures.  A whole disk is one "unit 0".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "amiga_compat.h"
#include "devices.h"
#include "rdb.h"

#define SYS_BLOCK "/sys/block"

/* ------------------------------------------------------------------ */
/* sysfs helpers                                                       */
/* ------------------------------------------------------------------ */

/* Read the first line of a sysfs attribute, trimmed. Returns FALSE if
   the file does not exist. */
static BOOL sysfs_read(const char *devname, const char *attr,
                       char *buf, size_t len)
{
    char  path[256];
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), SYS_BLOCK "/%s/%s", devname, attr);
    f = fopen(path, "r");
    if (!f) return FALSE;
    if (!fgets(buf, (int)len, f)) buf[0] = '\0';
    fclose(f);

    n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
    return TRUE;
}

static unsigned long long sysfs_num(const char *devname, const char *attr)
{
    char buf[32];
    if (!sysfs_read(devname, attr, buf, sizeof(buf))) return 0;
    return strtoull(buf, NULL, 10);
}

static BOOL sysfs_exists(const char *devname, const char *attr)
{
    char path[256];
    snprintf(path, sizeof(path), SYS_BLOCK "/%s/%s", devname, attr);
    return access(path, F_OK) == 0;
}

/* ------------------------------------------------------------------ */
/* in-use detection: any /dev/<name>* mounted or swapped on           */
/* ------------------------------------------------------------------ */

static BOOL table_uses_dev(const char *table, const char *devname)
{
    char  prefix[80];
    char  line[512];
    FILE *f;
    size_t plen;
    BOOL  used = FALSE;

    snprintf(prefix, sizeof(prefix), "/dev/%s", devname);
    plen = strlen(prefix);

    f = fopen(table, "r");
    if (!f) return FALSE;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, prefix, plen) != 0) continue;
        /* match whole disk or a partition of it (sda1, nvme0n1p2),
           not a longer sibling name (sda vs sdaa doesn't exist, but
           mmcblk0 vs mmcblk01 alike shapes do) */
        {
            char c = line[plen];
            if (c == ' ' || c == '\t' ||
                (c >= '0' && c <= '9') ||
                (c == 'p' && line[plen+1] >= '0' && line[plen+1] <= '9')) {
                used = TRUE;
                break;
            }
        }
    }
    fclose(f);
    return used;
}

static BOOL dev_in_use(const char *devname)
{
    return table_uses_dev("/proc/self/mounts", devname) ||
           table_uses_dev("/proc/swaps",       devname);
}

/* ------------------------------------------------------------------ */
/* model string: "<vendor> <model>", or the loop backing file          */
/* ------------------------------------------------------------------ */

static void dev_model(const char *devname, char *buf, size_t len)
{
    char vendor[64] = "", model[64] = "";

    if (sysfs_read(devname, "loop/backing_file", model, sizeof(model))) {
        const char *base = strrchr(model, '/');
        snprintf(buf, len, "loop: %s", base ? base + 1 : model);
        return;
    }

    sysfs_read(devname, "device/vendor", vendor, sizeof(vendor));
    if (!sysfs_read(devname, "device/model", model, sizeof(model)))
        sysfs_read(devname, "device/name", model, sizeof(model)); /* mmc */

    /* NVMe puts the maker in the model string; ATA vendors often say
       just "ATA". Drop a vendor the model already starts with. */
    if (vendor[0] && strncmp(model, vendor, strlen(vendor)) != 0 &&
        strcmp(vendor, "ATA") != 0)
        snprintf(buf, len, "%s %s", vendor, model);
    else
        snprintf(buf, len, "%s", model);
}

/* ------------------------------------------------------------------ */
/* Devices_Scan                                                        */
/* ------------------------------------------------------------------ */

static BOOL scan_accept(const char *n)
{
    char type[16];

    /* virtual/stacked devices are not partitioning targets */
    if (strncmp(n, "dm-",  3) == 0 || strncmp(n, "md",   2) == 0 ||
        strncmp(n, "ram",  3) == 0 || strncmp(n, "zram", 4) == 0 ||
        strncmp(n, "fd",   2) == 0)
        return FALSE;

    /* loop devices only when they carry a backing file */
    if (strncmp(n, "loop", 4) == 0)
        return sysfs_exists(n, "loop/backing_file");

    /* eMMC boot/RPMB pseudo-disks (mmcblk0boot0, mmcblk0rpmb) */
    if (strncmp(n, "mmcblk", 6) == 0) {
        size_t l = strlen(n);
        if ((l >= 5 && strncmp(n + l - 5, "boot", 4) == 0) ||
            (l >= 4 && strcmp(n + l - 4, "rpmb") == 0))
            return FALSE;
    }

    /* real hardware has a device/ link; SCSI type 5 = CD/DVD */
    if (!sysfs_exists(n, "device")) return FALSE;
    if (sysfs_read(n, "device/type", type, sizeof(type)) &&
        strtol(type, NULL, 10) == 5)
        return FALSE;

    return TRUE;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

void Devices_Scan(struct DevNameList *nl)
{
    DIR *d;
    struct dirent *de;
    char  found[MAX_DEV_NAMES][64];
    UWORD nfound = 0, i;

    memset(nl, 0, sizeof(*nl));

    d = opendir(SYS_BLOCK);
    if (!d) return;
    while ((de = readdir(d)) != NULL && nfound < MAX_DEV_NAMES) {
        if (de->d_name[0] == '.') continue;
        if (!scan_accept(de->d_name)) continue;
        snprintf(found[nfound], sizeof(found[0]), "%s", de->d_name);
        nfound++;
    }
    closedir(d);

    qsort(found, nfound, sizeof(found[0]), name_cmp);

    for (i = 0; i < nfound; i++) {
        const char *n = found[i];
        unsigned long long bytes = sysfs_num(n, "size") * 512ULL;
        char  devpath[64], szbuf[20], model[80], flags[40];

        snprintf(devpath, sizeof(devpath), "/dev/%.56s", n);
        memcpy(nl->names[nl->count], devpath, sizeof(devpath));

        dev_model(n, model, sizeof(model));

        flags[0] = '\0';
        if (dev_in_use(n))
            strcat(flags, " [IN USE]");
        else if (sysfs_num(n, "ro"))
            strcat(flags, " [read-only]");
        if (sysfs_num(n, "removable"))
            strcat(flags, " (removable)");

        /* precision caps keep the whole line inside display[80] */
        if (bytes == 0)
            snprintf(nl->display[nl->count], sizeof(nl->display[0]),
                     "%-14.14s  no media   %.28s%.24s",
                     devpath, model, flags);
        else {
            FormatSize((UQUAD)bytes, szbuf);
            snprintf(nl->display[nl->count], sizeof(nl->display[0]),
                     "%-14.14s %9.9s   %.28s%.24s",
                     devpath, szbuf, model, flags);
        }
        nl->count++;
    }
}

/* GUI-only on the Amiga; the host CLI prints display[] as scanned. */
void DevNameList_FormatDisplay(struct DevNameList *nl, UWORD col_chars)
{
    (void)nl; (void)col_chars;
}

/* ------------------------------------------------------------------ */
/* Devices_GetUnitsForName - signature probe of one disk               */
/* ------------------------------------------------------------------ */

static ULONG be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8)  |  (ULONG)p[3];
}

/* Walk the RDB partition chain, appending drive names to buf. */
static void rdb_part_names(int fd, ULONG blk, char *buf, size_t len)
{
    UBYTE b[512];
    int   guard;
    UWORD nparts = 0;

    for (guard = 0; blk != 0xFFFFFFFFUL && guard < 32; guard++) {
        if (pread(fd, b, 512, (off_t)blk * 512) != 512) break;
        if (memcmp(b, "PART", 4) != 0) break;
        {
            UBYTE nl = b[36];                  /* pb_DriveName BSTR */
            char  name[32];
            if (nl > 31) nl = 31;
            memcpy(name, b + 37, nl);
            name[nl] = '\0';
            if (name[0] && strlen(buf) + strlen(name) + 2 < len) {
                strcat(buf, " ");
                strcat(buf, name);
            }
            nparts++;
        }
        blk = be32(b + 16);                    /* pb_Next */
    }
    if (nparts == 0 && strlen(buf) + 8 < len)
        strcat(buf, " (empty)");
}

void Devices_GetUnitsForName(const char *devname, struct UnitList *ul,
                             UnitProbeCallback cb, void *cb_data)
{
    struct UnitEntry *ue;
    char  szbuf[20];
    UBYTE blk[512];
    unsigned long long bytes;
    int   fd, i, rdb_at = -1;

    ul->count = 0;
    if (!devname || strncmp(devname, "/dev/", 5) != 0) return;

    if (cb && !cb(cb_data, 0, PROBE_START, NULL)) return;

    ue = &ul->entries[0];
    ue->unit = 0;

    fd = open(devname, O_RDONLY);
    if (fd < 0) {
        snprintf(ue->display, sizeof(ue->display), "Unit 0   %s",
                 errno == EACCES
                 ? "no read permission (sudo, or join the 'disk' group)"
                 : strerror(errno));
        ul->count = 1;
        if (cb) cb(cb_data, 0, PROBE_EMPTY, NULL);
        return;
    }

    bytes = (unsigned long long)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    FormatSize((UQUAD)bytes, szbuf);

    /* RDB lives in the first RDB_LOCATION_LIMIT (16) blocks */
    for (i = 0; i < 16; i++) {
        if (pread(fd, blk, 512, (off_t)i * 512) != 512) break;
        if (memcmp(blk, "RDSK", 4) == 0) { rdb_at = i; break; }
    }

    if (rdb_at >= 0) {
        char parts[96] = "";
        rdb_part_names(fd, be32(blk + 28), parts, sizeof(parts));
        snprintf(ue->display, sizeof(ue->display),
                 "Unit 0   %-9s RDB @ block %d:%s", szbuf, rdb_at, parts);
    } else if (i == 0) {
        snprintf(ue->display, sizeof(ue->display),
                 "Unit 0   no media / not readable");
    } else {
        const char *sig = "no RDB / MBR / GPT signature";
        UBYTE b0[512], b1[512];
        if (pread(fd, b0, 512, 0) == 512) {
            if (pread(fd, b1, 512, 512) == 512 &&
                memcmp(b1, "EFI PART", 8) == 0)
                sig = "GPT partition table (not an Amiga disk)";
            else if (b0[510] == 0x55 && b0[511] == 0xAA)
                sig = "MBR partition table (no RDB)";
        }
        snprintf(ue->display, sizeof(ue->display),
                 "Unit 0   %-9s %s", szbuf, sig);
    }

    close(fd);
    ul->count = 1;
    if (cb) cb(cb_data, 0, PROBE_FOUND, ue->display);
}
