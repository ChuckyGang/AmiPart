/* Host implementations of the AmigaOS calls rdb.c needs. Only the file
 * (dos.library) path is functional; the exec device path is stubbed to
 * fail, since the PoC uses BlockDev_OpenFile (BD_FILE) exclusively. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "amiga_compat.h"
#include "rdb.h"
#include "quickformat.h"

static LONG g_ioerr = 0;

APTR AllocVec(ULONG size, ULONG flags) {
    void *p = malloc(size);
    if (p && (flags & MEMF_CLEAR)) memset(p, 0, size);
    return p;
}
void FreeVec(APTR p) { free(p); }
APTR AllocMem(ULONG size, ULONG flags) { return AllocVec(size, flags); }
void FreeMem(APTR p, ULONG size) { (void)size; free(p); }
void CopyMem(const void *src, void *dst, ULONG size) { memcpy(dst, src, size); }

/* BPTR holds a FILE* cast to long. */
BPTR Open(CONST_STRPTR name, LONG mode) {
    /* AmigaOS MODE_OLDFILE is read/WRITE on an existing file - map it to
       "r+b", falling back to "rb" for genuinely read-only files. */
    FILE *f;
    if (mode == MODE_NEWFILE) f = fopen(name, "w+b");
    else { f = fopen(name, "r+b"); if (!f) f = fopen(name, "rb"); }
    if (!f) { g_ioerr = 205; return 0; }   /* ERROR_OBJECT_NOT_FOUND */
    return (BPTR)(size_t)f;
}
void Close(BPTR fh) { if (fh) fclose((FILE *)(size_t)fh); }

LONG Read(BPTR fh, void *buf, LONG len) {
    LONG n;
    if (!fh) return -1;
    n = (LONG)fread(buf, 1, (size_t)len, (FILE *)(size_t)fh);
    return n;
}
LONG Write(BPTR fh, const void *buf, LONG len) {
    if (!fh) return -1;
    return (LONG)fwrite(buf, 1, (size_t)len, (FILE *)(size_t)fh);
}
/* AmigaOS Seek returns the OLD position; mode maps to SEEK_SET/CUR/END. */
LONG Seek(BPTR fh, LONG pos, LONG mode) {
    FILE *f = (FILE *)(size_t)fh;
    long old;
    int whence = (mode == OFFSET_END) ? SEEK_END
               : (mode == OFFSET_CURRENT) ? SEEK_CUR : SEEK_SET;
    if (!f) return -1;
    old = ftell(f);
    if (fseek(f, pos, whence) != 0) return -1;
    return (LONG)old;
}
/* 64-bit positioned file I/O for rdb.c's image backend (AMIPART_HOST):
   dos.library's LONG Seek() caps images at 2 GB on the Amiga, but 4 GB+
   HDFs are everyday objects on the host. */
LONG HostFilePRead(BPTR fh, void *buf, ULONG len, UQUAD off)
{
    FILE *f = (FILE *)(size_t)fh;
    if (!f) return -1;
    fflush(f);
    return (LONG)pread(fileno(f), buf, (size_t)len, (off_t)off);
}
LONG HostFilePWrite(BPTR fh, const void *buf, ULONG len, UQUAD off)
{
    FILE *f = (FILE *)(size_t)fh;
    if (!f) return -1;
    fflush(f);
    return (LONG)pwrite(fileno(f), buf, (size_t)len, (off_t)off);
}
UQUAD HostFileSize(BPTR fh)
{
    FILE *f = (FILE *)(size_t)fh;
    struct stat st;
    if (!f || fstat(fileno(f), &st) != 0) return 0;
    return (UQUAD)st.st_size;
}
BOOL HostFileTruncate(BPTR fh, UQUAD size)
{
    FILE *f = (FILE *)(size_t)fh;
    if (!f) return FALSE;
    fflush(f);
    return ftruncate(fileno(f), (off_t)size) == 0;
}

/* dos.library semantics: returns the new file size, -1 on failure. */
LONG SetFileSize(BPTR fh, LONG pos, LONG mode) {
    FILE *f = (FILE *)(size_t)fh;
    long cur, newsize;
    if (!f) return -1;
    fflush(f);
    cur = ftell(f);
    if (mode == OFFSET_END) {
        if (fseek(f, 0, SEEK_END) != 0) return -1;
        newsize = ftell(f) + pos;
        fseek(f, cur, SEEK_SET);
    } else if (mode == OFFSET_CURRENT) {
        newsize = cur + pos;
    } else {
        newsize = pos;
    }
    if (newsize < 0 || ftruncate(fileno(f), (off_t)newsize) != 0) return -1;
    return (LONG)newsize;
}
LONG IoErr(void) { return g_ioerr; }

/* ------------------------------------------------------------------ */
/* Device path: trackdisk.device emulation over a raw block device.   */
/*                                                                     */
/* OpenDevice("/dev/sdX", ...) opens the node with O_EXCL, so a disk  */
/* that is mounted (or held by LVM/another process) is REFUSED by the */
/* kernel - the same guard mkfs/wipefs rely on.  Only real block      */
/* devices are accepted; regular files must go through IMAGE=<file>.  */
/* DoIO serves the commands rdb.c's device backend uses:              */
/* TD_GETGEOMETRY, TD_READ64/CMD_READ, TD_WRITE64/CMD_WRITE (pread/   */
/* pwrite, full 64-bit offsets).  Everything else - HD_SCSICMD,       */
/* NSCMD_DEVICEQUERY - returns IOERR_NOCMD, which rdb.c already       */
/* handles gracefully on the Amiga for drivers without those commands.*/
/* ------------------------------------------------------------------ */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#ifdef __linux
	#include <linux/fs.h>
#endif
#ifdef __APPLE__
	#include <sys/disk.h>
#endif
#include <stdint.h>

struct HostDev {
    int      fd;
    int      read_only;
    uint64_t bytes;
    uint32_t sector;
};

struct MsgPort *CreateMsgPort(void) { return NULL; }
void DeleteMsgPort(struct MsgPort *p) { (void)p; }

LONG OpenDevice(CONST_STRPTR n, ULONG u, struct IORequest *io, ULONG f)
{
    const char *name = (const char *)n;
    struct HostDev *hd;
    struct stat st;
    int fd, ro = 0;
    (void)u; (void)f;

    if (!name || name[0] != '/' || !io) return -1;

    if (stat(name, &st) != 0) {
        fprintf(stderr, "%s: %s\n", name, strerror(errno));
        return -1;
    }
    if (!S_ISBLK(st.st_mode)) {
        fprintf(stderr, "%s: not a block device (use IMAGE=<file> for image files)\n", name);
        return -1;
    }

    fd = open(name, O_RDWR | O_EXCL);
    if (fd < 0 && errno == EACCES) {
        fd = open(name, O_RDONLY | O_EXCL);
        ro = 1;
    }
    if (fd < 0) {
        if (errno == EBUSY)
            fprintf(stderr, "%s: device is busy - a partition is mounted or "
                    "another program holds it. Unmount it first.\n", name);
        else if (errno == EACCES)
            fprintf(stderr, "%s: permission denied (try sudo, or add yourself "
                    "to the 'disk' group)\n", name);
        else
            fprintf(stderr, "%s: %s\n", name, strerror(errno));
        return -1;
    }
    if (ro)
        fprintf(stderr, "%s: opened READ-ONLY (no write permission) - "
                "writes will fail\n", name);

    hd = (struct HostDev *)calloc(1, sizeof(*hd));
    if (!hd) { close(fd); return -1; }
    hd->fd = fd;
    hd->read_only = ro;
    {
        unsigned long long b = 0;
        int ssz = 512;
#ifdef __APPLE__
    // macOS Implementation
    uint64_t block_count = 0;
    uint32_t block_size = 0;

    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &block_count) == 0 &&
        ioctl(fd, DKIOCGETBLOCKSIZE, &block_size) == 0) {
        b = block_count * block_size;
        ssz = block_size;
    } else {
        // Fallback to lseek if ioctls fail
        off_t end = lseek(fd, 0, SEEK_END);
        b = (end > 0) ? (unsigned long long)end : 0;
        lseek(fd, 0, SEEK_SET);
        ssz = 512; // Default safe assumption for sector size
    }
#else /* linux */    
    if (ioctl(fd, BLKGETSIZE64, &b) != 0) {
            off_t end = lseek(fd, 0, SEEK_END);
            b = (end > 0) ? (unsigned long long)end : 0;
            lseek(fd, 0, SEEK_SET);
        }
        ioctl(fd, BLKSSZGET, &ssz);
#endif
        hd->bytes  = b;
        hd->sector = (ssz >= 512) ? (uint32_t)ssz : 512;
    }
    io->io_Device = (APTR)hd;
    io->io_Error  = 0;
    return 0;
}

void CloseDevice(struct IORequest *io)
{
    struct HostDev *hd = io ? (struct HostDev *)io->io_Device : NULL;
    if (hd) { close(hd->fd); free(hd); io->io_Device = NULL; }
}

BYTE DoIO(struct IORequest *io)
{
    struct IOStdReq *req = (struct IOStdReq *)io;
    struct IOExtTD  *td  = (struct IOExtTD *)io;
    struct HostDev  *hd  = io ? (struct HostDev *)io->io_Device : NULL;
    uint64_t off;
    ssize_t  n;

    if (!hd) { if (io) io->io_Error = -1; return -1; }
    req->io_Error = 0;

    switch (req->io_Command) {

    case TD_GETGEOMETRY: {
        struct DriveGeometry *dg = (struct DriveGeometry *)req->io_Data;
        /* Report a conventional 512-byte-sector, 16 heads x 63 sectors
           geometry (the same convention the BD_FILE backend synthesizes)
           so a raw device and an image of the same size produce
           identical RDBs.  Linux block I/O is buffered, so 512-byte
           access works regardless of the drive's real sector size. */
        uint64_t sectors = hd->bytes / 512;
        memset(dg, 0, sizeof(*dg));
        dg->dg_SectorSize   = 512;
        /* dg fields are 32-bit; clamp huge disks rather than wrap. */
        dg->dg_TotalSectors = (sectors > 0xFFFFFFFFULL)
                              ? 0xFFFFFFFFUL : (ULONG)sectors;
        dg->dg_Heads        = 16;
        dg->dg_TrackSectors = 63;
        dg->dg_CylSectors   = 16 * 63;
        dg->dg_Cylinders    = dg->dg_TotalSectors / (16 * 63);
        dg->dg_DeviceType   = DG_DIRECT_ACCESS;
        req->io_Actual = sizeof(*dg);
        return 0;
    }

    case TD_READ64:
    case CMD_READ:
        off = (req->io_Command == TD_READ64)
              ? ((uint64_t)req->io_Actual << 32) | req->io_Offset
              : (uint64_t)req->io_Offset;
        n = pread(hd->fd, req->io_Data, req->io_Length, (off_t)off);
        if (n != (ssize_t)req->io_Length) { req->io_Error = 20; return 20; }
        req->io_Actual = req->io_Length;
        return 0;

    case TD_WRITE64:
    case CMD_WRITE:
        if (hd->read_only) { req->io_Error = 28; return 28; } /* write protect */
        off = (req->io_Command == TD_WRITE64)
              ? ((uint64_t)((struct IOExtTD *)req)->iotd_Req.io_Actual << 32) | req->io_Offset
              : (uint64_t)req->io_Offset;
        (void)td;
        n = pwrite(hd->fd, req->io_Data, req->io_Length, (off_t)off);
        if (n != (ssize_t)req->io_Length) { req->io_Error = 20; return 20; }
        req->io_Actual = req->io_Length;
        return 0;

    default:
        req->io_Error = IOERR_NOCMD;
        return IOERR_NOCMD;
    }
}

struct IORequest *CreateIORequest(struct MsgPort *p, ULONG s) { (void)p;(void)s; return NULL; }
void DeleteIORequest(APTR io) { (void)io; }


BYTE AllocSignal(LONG bit) { (void)bit; return 0; }
void FreeSignal(LONG bit) { (void)bit; }
APTR FindTask(CONST_STRPTR name) { (void)name; return (APTR)1; }
LONG DeleteFile(CONST_STRPTR name) { (void)name; return 0; }

/* ================================================================== */
/* Host-build additions: console I/O, dos misc, and host stubs for    */
/* the Amiga-only mount / quick-format layer.                          */
/* ================================================================== */
#include <unistd.h>

void host_set_ioerr(LONG err) { g_ioerr = err; }
void SetIoErr(LONG err)       { g_ioerr = err; }

void PutStr(CONST_STRPTR s)   { fputs((const char *)s, stdout); }
BPTR Output(void)             { return (BPTR)(size_t)stdout; }
BPTR Input(void)              { return (BPTR)(size_t)stdin; }
LONG Flush(BPTR fh)           { fflush((FILE *)(size_t)fh); return DOSTRUE; }
LONG FGetC(BPTR fh)           { return fgetc((FILE *)(size_t)fh); }
void PrintFault(LONG code, CONST_STRPTR hdr)
{
    fprintf(stderr, "%s: error %ld\n", hdr ? (const char *)hdr : "AmiPart",
            (long)code);
}
BOOL ExamineFH(BPTR fh, struct FileInfoBlock *fib)
{
    FILE *f = (FILE *)(size_t)fh;
    long  pos, sz;
    if (!f || !fib) return FALSE;
    pos = ftell(f);
    if (fseek(f, 0, SEEK_END) != 0) return FALSE;
    sz = ftell(f);
    fseek(f, pos, SEEK_SET);
    memset(fib, 0, sizeof(*fib));
    fib->fib_Size = (LONG)sz;
    fib->fib_DirEntryType = ST_FILE;
    return TRUE;
}
void  Delay(LONG ticks)       { usleep((useconds_t)ticks * 20000); }
/* Ctrl-C: the copy loops poll SetSignal(0, SIGBREAKF_CTRL_C) to cancel
   cleanly between blocks; a raw SIGINT would kill us mid-write instead. */
#include <signal.h>
static volatile sig_atomic_t g_ctrlc = 0;
static void on_sigint(int s) { (void)s; g_ctrlc = 1; }
ULONG SetSignal(ULONG new_sig, ULONG mask)
{
    static int installed = 0;
    ULONG old;
    if (!installed) { signal(SIGINT, on_sigint); installed = 1; }
    old = g_ctrlc ? SIGBREAKF_CTRL_C : 0;
    if (mask & SIGBREAKF_CTRL_C) g_ctrlc = (new_sig & SIGBREAKF_CTRL_C) ? 1 : 0;
    return old;
}
LONG  Inhibit(CONST_STRPTR name, LONG onoff) { (void)name; (void)onoff; return DOSTRUE; }

/* ---- mount layer: image files are never mounted on the host ---- */
static void host_note(char *errbuf, ULONG errlen, const char *msg)
{
    if (errbuf && errlen) { strncpy(errbuf, msg, errlen - 1); errbuf[errlen - 1] = 0; }
}
UWORD MountedPartitionsOnDevice(struct BlockDev *bd, char *names, ULONG nsz)
{   /* the kernel refuses O_EXCL on a mounted disk, so nothing we open is */
    (void)bd; if (names && nsz) names[0] = 0; return 0;
}
BOOL UnmountDevice(const char *name, char *errbuf, ULONG errlen)
{   /* nothing is ever mounted on the host - "already offline" = success */
    (void)name; (void)errbuf; (void)errlen; return TRUE;
}
BOOL UnmountPartition(struct BlockDev *bd, const char *name,
                      UnmountProgressFn progress, void *ud,
                      char *errbuf, ULONG errlen)
{
    (void)bd; (void)name; (void)progress; (void)ud;
    (void)errbuf; (void)errlen; return TRUE;
}
BOOL MountPartition(struct BlockDev *bd, const struct PartInfo *pi,
                    char *mounted_name, char *errbuf, ULONG errlen)
{
    (void)bd; (void)pi;
    if (mounted_name) mounted_name[0] = 0;
    host_note(errbuf, errlen, "host build: volumes are not mounted");
    return FALSE;
}
void MaterializeVolume(const char *name) { (void)name; }

/* ---- quick-format: needs real filesystem handlers - Amiga only ---- */
BOOL QuickFormat_EnsureHandler(const struct RDBInfo *rdb, ULONG dostype,
                               char *errbuf, ULONG errlen)
{
    (void)rdb; (void)dostype;
    host_note(errbuf, errlen, "quick-format is not available in the host build");
    return FALSE;
}
BOOL QuickFormat_Partition(struct BlockDev *bd, const struct PartInfo *pi,
                           char *mounted_name, char *errbuf, ULONG errlen)
{
    (void)bd; (void)pi;
    if (mounted_name) mounted_name[0] = 0;
    host_note(errbuf, errlen, "quick-format is not available in the host build");
    return FALSE;
}
BOOL QuickFormat_PFS3Tune(const char *mounted_name, ULONG dostype,
                          UWORD deldir_blocks, char *notebuf, ULONG notelen)
{
    (void)mounted_name; (void)dostype; (void)deldir_blocks;
    if (notebuf && notelen) notebuf[0] = 0;
    return FALSE;
}

/* ---- device enumeration lives in host_devices.c (sysfs scan) ---- */
void ColdReboot(void)
{
    printf("(host build: REBOOT ignored)\n");
}
