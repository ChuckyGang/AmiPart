/* guisize.c - see guisize.h */
#include <exec/types.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/screens.h>
#include <proto/graphics.h>

#include "clib.h"
#include "locale_support.h"
#include "guisize.h"

UWORD GUI_CharW(struct Screen *scr)
{
    if (scr && scr->RastPort.Font && scr->RastPort.Font->tf_XSize)
        return (UWORD)scr->RastPort.Font->tf_XSize;
    return 8;
}

UWORD GUI_TextW(struct Screen *scr, const char *s)
{
    if (!scr || !s || !*s) return 0;
    return (UWORD)TextLength(&scr->RastPort, (CONST_STRPTR)s, (ULONG)strlen(s));
}

UWORD GUI_MaxLineW(struct Screen *scr, const char *text)
{
    UWORD best = 0;
    const char *p = text, *q;
    if (!scr || !text) return 0;
    for (;;) {
        ULONG len;
        q = p;
        while (*q && *q != '\n') q++;
        len = (ULONG)(q - p);
        if (len) {
            UWORD w = (UWORD)TextLength(&scr->RastPort, (CONST_STRPTR)p, len);
            if (w > best) best = w;
        }
        if (!*q) break;
        p = q + 1;
    }
    return best;
}

UWORD GUI_BtnW(struct Screen *scr, const char *label)
{
    return (UWORD)(GUI_TextW(scr, label) + GUI_CharW(scr) * 2 + 6);
}

UWORD GUI_MaxBtnW(struct Screen *scr, const LONG *ids, UWORD n)
{
    UWORD i, best = 0;
    for (i = 0; i < n; i++) {
        UWORD w = GUI_BtnW(scr, GS(ids[i]));
        if (w > best) best = w;
    }
    return best;
}

UWORD GUI_MaxLabelW(struct Screen *scr, const LONG *ids, UWORD n)
{
    UWORD i, best = 0;
    for (i = 0; i < n; i++) {
        UWORD w = GUI_TextW(scr, GS(ids[i]));
        if (w > best) best = w;
    }
    return (UWORD)(best + GUI_CharW(scr));
}

UWORD GUI_FitW(struct Screen *scr, UWORD want)
{
    if (scr && want > scr->Width) return (UWORD)scr->Width;
    return want;
}

UWORD GUI_FitH(struct Screen *scr, UWORD want)
{
    if (scr && want > scr->Height) return (UWORD)scr->Height;
    return want;
}
