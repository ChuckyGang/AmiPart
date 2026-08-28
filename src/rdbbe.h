/*
 * rdbbe.h - endian-explicit access to on-disk RDB block fields.
 *
 * RDB metadata (RDSK/PART/FSHD/LSEG blocks) is big-endian on disk.  On
 * the Amiga (m68k, big-endian) a struct overlaid on the raw block reads
 * correctly as-is; on a little-endian host (the Linux AmiPart build)
 * every 32-bit field must be byte-swapped.  All rdb.c access to raw
 * block fields goes through BE32R/BE32W so the same code compiles
 * correctly for both.
 *
 * String/byte fields (BSTR drive names, rdb_DiskVendor/Product/Revision,
 * fhb_FileSysName, lsb_LoadData code payload) are raw bytes on disk and
 * must NOT go through these macros - copy them with memcpy.
 *
 * Block checksums sum the block as big-endian longwords, so checksum
 * loops must read each word with BE32R and store the result with BE32W;
 * the sum-to-zero property then holds identically on both hosts.
 */
#ifndef RDBBE_H
#define RDBBE_H

#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

/* Big-endian target (m68k Amiga): direct access, zero overhead. */
#define BE32R(f)     ((ULONG)(f))
#define BE32W(f, v)  ((void)((f) = (v)))

/* In-place whole-buffer swab for filesystem metadata blocks that are
   pure arrays of big-endian longwords (FFS boot/root/bitmap blocks):
   a no-op here.  On a little-endian host it swaps every longword, so
   engine code reads/writes native values; byte-string regions (volume
   names, boot code) survive because an untouched word swaps back to
   its original bytes on the way out. */
#define BE32_SWAB_BUF(p, nlongs) ((void)0)

#else

/* Little-endian host: assemble/store big-endian bytes. */
static inline ULONG rdbbe_r32(const void *p)
{
    const UBYTE *b = (const UBYTE *)p;
    return ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) |
           ((ULONG)b[2] <<  8) |  (ULONG)b[3];
}
static inline void rdbbe_w32(void *p, ULONG v)
{
    UBYTE *b = (UBYTE *)p;
    b[0] = (UBYTE)(v >> 24); b[1] = (UBYTE)(v >> 16);
    b[2] = (UBYTE)(v >>  8); b[3] = (UBYTE)(v);
}
#define BE32R(f)     rdbbe_r32(&(f))
#define BE32W(f, v)  rdbbe_w32(&(f), (ULONG)(v))

static inline void rdbbe_swab_buf(void *p, ULONG nlongs)
{
    UBYTE *b = (UBYTE *)p;
    ULONG  i;
    for (i = 0; i < nlongs; i++, b += 4) {
        UBYTE t;
        t = b[0]; b[0] = b[3]; b[3] = t;
        t = b[1]; b[1] = b[2]; b[2] = t;
    }
}
#define BE32_SWAB_BUF(p, nlongs) rdbbe_swab_buf((p), (nlongs))

#endif

#endif /* RDBBE_H */
