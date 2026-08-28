/* Host implementations of the AmigaOS calls rdb.c needs. Only the file
 * (dos.library) path is functional; the exec device path is stubbed to
 * fail, since the PoC uses BlockDev_OpenFile (BD_FILE) exclusively. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Demonstration endianness layer: an RDB metadata block is a sequence of
 * big-endian longwords. On a little-endian host, DiskPart's struct-overlay
 * reads (and its whole-block longword checksum) only work if those longs
 * are byte-swapped to native order. We do that here for recognised RDB
 * block signatures so the UNMODIFIED rdb.c parses correctly.
 *
 * CAVEAT (the crux finding): the checksum needs ALL longs in true-BE-native
 * form, but embedded byte-string fields (pb_DriveName BSTR, rdb_DiskVendor,
 * fhb_FileSysName) need RAW bytes -- and for PART the name sits INSIDE the
 * checksummed region, so no block-level transform satisfies both at once.
 * Here we swap every long (checksum + all NUMERIC fields correct); the
 * name fields consequently read 4-char-swapped. A real port makes field
 * access endian-explicit (be32 for numbers, raw bytes for strings) instead. */
static void swab_rdb_block(UBYTE *b) {
    static const char sigs[4][4] = {{'R','D','S','K'},{'P','A','R','T'},{'F','S','H','D'},{'L','S','E','G'}};
    int s, match = -1, i;
    for (s = 0; s < 4; s++)
        if (b[0]==sigs[s][0]&&b[1]==sigs[s][1]&&b[2]==sigs[s][2]&&b[3]==sigs[s][3]) { match = s; break; }
    if (match < 0) return;
    /* LSEG carries raw FS code after its 5-long header: swap header only. */
    int nlongs = (match == 3) ? 5 : 128;
    for (i = 0; i < nlongs; i++) {
        UBYTE t;
        t=b[i*4+0]; b[i*4+0]=b[i*4+3]; b[i*4+3]=t;
        t=b[i*4+1]; b[i*4+1]=b[i*4+2]; b[i*4+2]=t;
    }
}
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
LONG SetFileSize(BPTR fh, LONG pos, LONG mode) { (void)fh;(void)pos;(void)mode; return 0; }
LONG IoErr(void) { return g_ioerr; }

/* Device path: stubbed to fail (never reached for BD_FILE). */
struct MsgPort *CreateMsgPort(void) { return NULL; }
void DeleteMsgPort(struct MsgPort *p) { (void)p; }
LONG OpenDevice(CONST_STRPTR n, ULONG u, struct IORequest *io, ULONG f) { (void)n;(void)u;(void)io;(void)f; return -1; }
void CloseDevice(struct IORequest *io) { (void)io; }
BYTE DoIO(struct IORequest *io) { (void)io; return -1; }
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
ULONG SetSignal(ULONG new_sig, ULONG mask) { (void)new_sig; (void)mask; return 0; }
LONG  Inhibit(CONST_STRPTR name, LONG onoff) { (void)name; (void)onoff; return DOSTRUE; }

/* ---- mount layer: image files are never mounted on the host ---- */
static void host_note(char *errbuf, ULONG errlen, const char *msg)
{
    if (errbuf && errlen) { strncpy(errbuf, msg, errlen - 1); errbuf[errlen - 1] = 0; }
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

/* ---- device enumeration / reboot: no exec devices on the host ---- */
#include "devices.h"
void Devices_Scan(struct DevNameList *nl)
{
    if (nl) nl->count = 0;   /* host build: use IMAGE=<file> targets */
}
void Devices_GetUnitsForName(const char *devname, struct UnitList *ul,
                             UnitProbeCallback cb, void *cb_data)
{
    (void)devname; (void)cb; (void)cb_data;
    if (ul) ul->count = 0;
}
void ColdReboot(void)
{
    printf("(host build: REBOOT ignored)\n");
}
