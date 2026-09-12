/* gt_compat.h - GadTools inline-header workaround.
 *
 * Bartman's inline/macros.h declares every LP argument as
 * "register volatile t1 _n1".  For CreateContext, t1 is "struct Gadget **",
 * and the textual "volatile" lands on the pointed-to type: the temporary
 * becomes "pointer to pointer to VOLATILE Gadget", which is not implicitly
 * convertible from a plain "struct Gadget **".  GCC 14 treats that as a hard
 * error (-Wincompatible-pointer-types is no longer just a warning), so every
 * CreateContext(&glist) in the GUI would fail to compile.
 *
 * Re-declaring the argument as "void *" sidesteps it: void * accepts any
 * object pointer and "volatile void *" is a plain qualifier addition.  The
 * call sequence is identical.  Include this AFTER <proto/gadtools.h>. */
#ifndef GT_COMPAT_H
#define GT_COMPAT_H

#if defined(LP1) && defined(CreateContext) && defined(GADTOOLS_BASE_NAME)
#undef CreateContext
#define CreateContext(___glistptr) \
    LP1(0x72, struct Gadget *, CreateContext, void *, ___glistptr, a0, \
        , GADTOOLS_BASE_NAME)
#endif

#endif /* GT_COMPAT_H */
