/* Host implementations of the AmigaOS calls rdb.c needs. Only the file
 * (dos.library) path is functional; the exec device path is stubbed to
 * fail, since the PoC uses BlockDev_OpenFile (BD_FILE) exclusively. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amiga_compat.h"

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

/* locale stub: return the constant's own placeholder (RDB_Read never calls GS) */
CONST_STRPTR GetDPString(LONG id) { (void)id; return "(str)"; }

BYTE AllocSignal(LONG bit) { (void)bit; return 0; }
void FreeSignal(LONG bit) { (void)bit; }
APTR FindTask(CONST_STRPTR name) { (void)name; return (APTR)1; }
LONG DeleteFile(CONST_STRPTR name) { (void)name; return 0; }
