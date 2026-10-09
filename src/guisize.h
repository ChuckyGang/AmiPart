/*
 * guisize.h - font-measured sizing helpers for AmiPart's GadTools windows.
 *
 * Windows used to be laid out with fixed pixel widths that assumed a wide
 * screen or a narrow font; on a bare ADF boot (640 px, Topaz 8) labels
 * and body text were clipped (issue #14/4).  Every window now measures its
 * own strings in the screen font and clamps itself to the screen.
 */
#ifndef GUISIZE_H
#define GUISIZE_H

#include <exec/types.h>
#include <intuition/screens.h>

/* Pixel width of s in the screen font. */
UWORD GUI_TextW(struct Screen *scr, const char *s);
/* Widest '\n'-separated line of a body text. */
UWORD GUI_MaxLineW(struct Screen *scr, const char *text);
/* Width a BUTTON_KIND gadget needs for label: text + one char each side + bevel. */
UWORD GUI_BtnW(struct Screen *scr, const char *label);
/* Widest button for a set of catalog string ids. */
UWORD GUI_MaxBtnW(struct Screen *scr, const LONG *ids, UWORD n);
/* Widest PLACETEXT_LEFT label (catalog ids) plus one char of gap. */
UWORD GUI_MaxLabelW(struct Screen *scr, const LONG *ids, UWORD n);
/* Clamp a wanted window width to the screen. */
UWORD GUI_FitW(struct Screen *scr, UWORD want);
/* Clamp a wanted window height to the screen. */
UWORD GUI_FitH(struct Screen *scr, UWORD want);
/* Screen font cell width (8 for Topaz 8). */
UWORD GUI_CharW(struct Screen *scr);

#endif /* GUISIZE_H */
