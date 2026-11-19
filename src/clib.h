/*
 * clib.h - Minimal C library declarations for AmiPart.
 *
 * -nostdlib build: this replaces <string.h>, <stdio.h>, <stdlib.h>.
 *
 * strlen/memset/memcpy/memmove: support/gcc8_c_support.c
 * sprintf/snprintf/strcmp/strncpy/strcpy/strtoul/strtol: src/clib.c
 */

#ifndef CLIB_H
#define CLIB_H

/* Pointer-sized signed integer for ReadArgs result slots.  On the Amiga
   pointers are 32-bit and this is plain LONG (zero change); on a 64-bit
   host build the compat layer defines DP_SIPTR_DEFINED and provides a
   pointer-width type instead, so string pointers stored in the args
   array are not truncated. */
#ifndef DP_SIPTR_DEFINED
#define DP_SIPTR_DEFINED
typedef signed long SIPTR;   /* m68k: 32-bit, same as LONG */
#endif

/* Pointer-sized unsigned integer, for pointers that travel as integers:
   TagItem.ti_Data and requester argument lists.  On the Amiga it is ULONG
   (zero change); the Linux GUI build defines it pointer-width. */
#ifndef DP_IPTR_DEFINED
#define DP_IPTR_DEFINED
typedef unsigned long IPTR;  /* m68k: 32-bit, same as ULONG */
#endif

#include <stddef.h>   /* size_t */

extern size_t strlen(const char *s);
extern void  *memset(void *dst, int c, size_t n);
extern void  *memcpy(void *dst, const void *src, size_t n);
extern void  *memmove(void *dst, const void *src, size_t n);
extern int    memcmp (const void *a, const void *b, size_t n);

int   sprintf (char *buf,             const char *fmt, ...);
int   snprintf(char *buf, size_t size, const char *fmt, ...);

/* DP_SNPRINTF(arr, fmt, ...) - snprintf bounded by sizeof(arr).
 * Compile-time guard: if 'arr' is a pointer (not a real array) sizeof would
 * silently be the pointer size and truncate, so DP_IS_ARRAY forces a build
 * error instead.  Use only with in-scope fixed char arrays; for caller-owned
 * pointer buffers keep plain snprintf with the explicit size. */
#define DP_IS_ARRAY(a) \
    (!__builtin_types_compatible_p(__typeof__(a), __typeof__(&(a)[0])))
#define DP_BUILD_BUG_IF(e) \
    ((int)(sizeof(struct { int _dpchk : (1 - 2 * !!(e)); })) * 0)
#define DP_SNPRINTF(arr, ...) \
    snprintf((arr), sizeof(arr) + DP_BUILD_BUG_IF(!DP_IS_ARRAY(arr)), __VA_ARGS__)
int   strcmp(const char *a, const char *b);
char *strncpy(char *dst, const char *src, size_t n);
char *strcpy (char *dst, const char *src);

/* cur as a percentage of total (0..100).  NOT (cur * 100) / total: with
   32-bit arithmetic that product wraps once cur passes 42.9 million, i.e.
   20 GB counted in 512-byte blocks, and the percentage starts over. */
static inline unsigned long dp_pct(unsigned long cur, unsigned long total)
{
    unsigned long p;
    if (total == 0) return 0;
    if (cur >= total) return 100;
    p = (total >= 100000UL) ? cur / (total / 100UL) : (cur * 100UL) / total;
    return p > 99 ? 99 : p;
}

unsigned long strtoul(const char *s, char **end, int base);
long          strtol (const char *s, char **end, int base);

#endif /* CLIB_H */
