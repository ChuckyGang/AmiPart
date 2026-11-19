/* hg_int.h - internals shared by the host GUI layer files
 * (host_gui.c, host_gadtools.c, host_req.c). */
#ifndef HG_INT_H
#define HG_INT_H

#include <stdint.h>
#include "host_gui.h"

#define HG_FW 8          /* font cell */
#define HG_FH 13
#define HG_FB 11         /* baseline */

struct HSurf { UBYTE *pix; int w, h; };

struct HMsg { struct IntuiMessage im; struct HMsg *next; };

struct HGad;

struct HWin {
    struct Window   w;          /* must be first */
    struct RastPort rp;
    struct MsgPort  port;
    struct HSurf    surf;
    struct HWin    *below;      /* z-order, hg_top first */
    struct HMsg    *qhead, *qtail;
    char            title[200];
    struct HGad    *active;     /* active string gadget */
    const char     *note;       /* requester body, for the test dump */
};

/* HGad.kind: GadTools kinds, or -1 for the CreateContext() anchor */
struct HGad {
    struct Gadget g;            /* must be first */
    int    kind;
    const char *text;           /* label */
    ULONG  ngflags;
    struct HWin *win;
    int    pressed;
    /* string */
    struct StringInfo si;
    int    maxchars;
    /* listview */
    struct List *labels;        /* NULL = none, HG_DETACHED = detached */
    int    top, sel, total, showsel, press_row, readonly;
    struct Hook *hook;
    /* cycle */
    const char **cy_labels;
    int    cy_active, cy_count;
    /* scroller: sc_part while pressed = 1 knob, 2/3 arrows, 4/5 page */
    int    sc_top, sc_vis, sc_total, sc_arrow, sc_horiz, sc_part, sc_grab;
    /* text */
    const char *tx_text;
    int    tx_border;
};
#define HG_DETACHED ((struct List *)(IPTR)-1)
#define HG_LV_ROWH  (HG_FH + 1)
#define HG_LV_SBW   16

/* pens */
#define P_BG    0
#define P_TEXT  1
#define P_SHINE 2
#define P_FILL  3

extern struct Screen  hg_scr;
extern struct HWin   *hg_top;
extern struct DrawInfo hg_dri;
extern int            hg_dirty;

/* surface drawing (clip rect inclusive; pass NULL-clip via hs_noclip) */
void hs_fill(struct HSurf *s, int x0, int y0, int x1, int y1, int pen);
void hs_comp(struct HSurf *s, int x0, int y0, int x1, int y1);
void hs_hline(struct HSurf *s, int x0, int x1, int y, int pen);
void hs_vline(struct HSurf *s, int x, int y0, int y1, int pen);
void hs_text(struct HSurf *s, int x, int ytop, const char *t, int len, int pen);
void hs_bevel(struct HSurf *s, int x, int y, int w, int h, int recessed);
void hs_ghost(struct HSurf *s, int x0, int y0, int x1, int y1);
int  hg_textw(const char *t);

/* core */
BOOL hg_ensure(void);
void hg_pump(int block, struct HWin *waitfor);
void hw_post(struct HWin *hw, ULONG cls, UWORD code, UWORD qual, APTR iaddr);
struct HWin *hg_win_of_port(struct MsgPort *port);
void hg_bar_redraw(void);
ULONG hg_ticks(void);

/* gadtools side, called by the core */
void hgad_draw(struct HGad *g);
void hgad_draw_all(struct HWin *hw);
struct HGad *hgad_hit(struct HWin *hw, int x, int y);
void hgad_mouse_down(struct HGad *g, int x, int y, int shift);
void hgad_mouse_move(struct HGad *g, int x, int y);
void hgad_mouse_up(struct HGad *g, int x, int y);
BOOL hgad_key(struct HWin *hw, int code, int ctrl, int shift);   /* TRUE = consumed */
void hgad_text_input(struct HWin *hw, int ch);
BOOL hgad_wheel(struct HWin *hw, int x, int y, int dy);   /* TRUE = a gadget took it */
void hgad_window_closing(struct HWin *hw);
void hgad_blur(struct HWin *hw);
const char *hgad_kind_name(int kind);

/* menus (host_gadtools.c) */
int  hmenu_bar_hit(int x);                 /* menu index under x, or -1 */
void hmenu_open(int idx);
void hmenu_close(void);
int  hmenu_is_open(void);
void hmenu_motion(int x, int y);
BOOL hmenu_release(int x, int y);          /* TRUE if it picked or closed */
BOOL hmenu_shortcut(int key);
void hmenu_draw_bar(struct HSurf *s);
BOOL hmenu_overlay(struct HSurf **s, int *x, int *y);
void hmenu_pick(int m, int i);

/* special key codes handed to hgad_key (beyond Latin-1) */
#define HK_LEFT  0x101
#define HK_RIGHT 0x102
#define HK_HOME  0x103
#define HK_END   0x104
#define HK_UP    0x105
#define HK_DOWN  0x106
#define HK_PGUP  0x107
#define HK_PGDN  0x108
#define HK_PASTE 0x109

#endif
