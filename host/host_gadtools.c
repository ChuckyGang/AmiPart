/* host_gadtools.c - the GadTools gadget kinds and menus AmiPart uses,
 * for the Linux build (see host_gui.c).
 *
 * Six kinds: button, checkbox, cycle, string, listview, text.  Gadgets are
 * drawn into their window's pixel buffer when the Amiga would draw them
 * (RefreshGList, GT_RefreshWindow, GT_SetGadgetAttrs, user input), so the
 * GUI's own refresh sequences behave as they do on the Amiga. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hg_int.h"

#define CB_W 26
#define CB_H 11

static int vi_dummy;

APTR GetVisualInfoA(struct Screen *scr, struct TagItem *tags) { (void)scr; (void)tags; return &vi_dummy; }
void FreeVisualInfo(APTR vi) { (void)vi; }

const char *hgad_kind_name(int kind)
{
    switch (kind) {
    case BUTTON_KIND:   return "button";
    case CHECKBOX_KIND: return "checkbox";
    case CYCLE_KIND:    return "cycle";
    case STRING_KIND:   return "string";
    case LISTVIEW_KIND: return "listview";
    case TEXT_KIND:     return "text";
    default:            return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Creation                                                             */
/* ------------------------------------------------------------------ */

static int lv_count(struct List *l)
{
    struct Node *n; int c = 0;
    if (!l || l == HG_DETACHED) return 0;
    for (n = l->lh_Head; n && n->ln_Succ; n = n->ln_Succ) c++;
    return c;
}

static struct List *tag_list(IPTR v)
{
    if (v == (IPTR)-1 || v == (IPTR)0xFFFFFFFFu) return HG_DETACHED;
    return (struct List *)v;
}

static int lv_rows(struct HGad *g) { int r = (g->g.Height - 4) / HG_LV_ROWH; return r < 1 ? 1 : r; }

static void lv_clamp(struct HGad *g)
{
    int rows = lv_rows(g);
    if (g->top > g->total - rows) g->top = g->total - rows;
    if (g->top < 0) g->top = 0;
}

static void str_set(struct HGad *g, const char *s)
{
    if (!g->si.Buffer) return;
    strncpy((char *)g->si.Buffer, s ? s : "", (size_t)g->maxchars);
    g->si.Buffer[g->maxchars] = '\0';
    g->si.NumChars = (WORD)strlen((char *)g->si.Buffer);
    g->si.BufferPos = g->si.NumChars;
    g->si.DispPos = 0;
}

static void apply_tags(struct HGad *g, struct TagItem *tags)
{
    struct TagItem *t;
    for (t = tags; t && t->ti_Tag != TAG_DONE; t++) {
        switch (t->ti_Tag) {
        case GA_Disabled:
            if (t->ti_Data) g->g.Flags |= GFLG_DISABLED; else g->g.Flags &= (UWORD)~GFLG_DISABLED;
            break;
        case GA_Text:      g->text = (const char *)t->ti_Data; break;
        case GTCB_Checked:
            if (t->ti_Data) g->g.Flags |= GFLG_SELECTED; else g->g.Flags &= (UWORD)~GFLG_SELECTED;
            break;
        case GTST_String:  if (g->kind == STRING_KIND) str_set(g, (const char *)t->ti_Data); break;
        case GTCY_Labels: {
            const char **l = (const char **)t->ti_Data; int n = 0;
            g->cy_labels = l;
            while (l && l[n]) n++;
            g->cy_count = n;
            if (g->cy_active >= n) g->cy_active = 0;
            break; }
        case GTCY_Active:  g->cy_active = (int)t->ti_Data;
                           if (g->cy_active >= g->cy_count) g->cy_active = 0;
                           break;
        case GTLV_Labels:
            g->labels = tag_list(t->ti_Data);
            g->total = lv_count(g->labels);
            if (g->labels != HG_DETACHED) lv_clamp(g);
            break;
        case GTLV_Top:     g->top = (int)t->ti_Data; lv_clamp(g); break;
        case GTLV_MakeVisible: {
            int v = (int)t->ti_Data, rows = lv_rows(g);
            if (v < g->top) g->top = v;
            else if (v >= g->top + rows) g->top = v - rows + 1;
            lv_clamp(g);
            break; }
        case GTLV_Selected: {
            IPTR v = t->ti_Data;
            g->sel = (v == (IPTR)-1 || v == (IPTR)0xFFFFFFFFu || v == 0xFFFF) ? -1 : (int)v;
            break; }
        case GTLV_ShowSelected: g->showsel = 1; break;
        case GTLV_ReadOnly:     g->readonly = t->ti_Data != 0; break;
        case GTLV_CallBack:     g->hook = (struct Hook *)t->ti_Data; break;
        case GTTX_Text:    g->tx_text = (const char *)t->ti_Data; break;
        case GTTX_Border:  g->tx_border = t->ti_Data != 0; break;
        default: break;
        }
    }
}

struct Gadget *CreateContext(struct Gadget **glist)
{
    struct HGad *g = (struct HGad *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->kind = -1;
    if (glist) *glist = &g->g;
    return &g->g;
}

struct Gadget *CreateGadgetA(ULONG kind, struct Gadget *prev, struct NewGadget *ng, struct TagItem *tags)
{
    struct HGad *g;
    struct TagItem *t;

    if (!prev || !ng) return NULL;
    g = (struct HGad *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->kind = (int)kind;
    g->g.LeftEdge = ng->ng_LeftEdge; g->g.TopEdge = ng->ng_TopEdge;
    g->g.Width = ng->ng_Width;       g->g.Height = ng->ng_Height;
    g->g.GadgetID = ng->ng_GadgetID; g->g.UserData = ng->ng_UserData;
    g->text = (const char *)ng->ng_GadgetText;
    g->ngflags = ng->ng_Flags;
    g->sel = -1; g->press_row = -1;

    if (kind == STRING_KIND) {
        g->maxchars = 64;
        for (t = tags; t && t->ti_Tag != TAG_DONE; t++)
            if (t->ti_Tag == GTST_MaxChars) g->maxchars = (int)t->ti_Data;
        if (g->maxchars < 1) g->maxchars = 1;
        g->si.Buffer = (UBYTE *)calloc(1, (size_t)g->maxchars + 2);
        if (!g->si.Buffer) { free(g); return NULL; }
        g->si.MaxChars = (WORD)(g->maxchars + 1);
        g->g.SpecialInfo = &g->si;
    }
    if (kind == CHECKBOX_KIND) { g->g.Width = CB_W; g->g.Height = CB_H; }
    apply_tags(g, tags);

    prev->NextGadget = &g->g;
    return &g->g;
}

void FreeGadgets(struct Gadget *glist)
{
    while (glist) {
        struct HGad *g = (struct HGad *)glist;
        glist = glist->NextGadget;
        free(g->si.Buffer);
        free(g);
    }
}

/* ------------------------------------------------------------------ */
/* Window gadget lists                                                  */
/* ------------------------------------------------------------------ */

UWORD AddGList(struct Window *win, struct Gadget *gl, ULONG pos, LONG num, APTR req)
{
    struct HWin *hw = (struct HWin *)win;
    struct Gadget **pp, *g;
    (void)pos; (void)num; (void)req;
    if (!hw || !gl) return 0;
    for (g = gl; g; g = g->NextGadget) ((struct HGad *)g)->win = hw;
    for (pp = &win->FirstGadget; *pp; pp = &(*pp)->NextGadget)
        if (*pp == gl) return 0;                  /* already there */
    *pp = gl;
    return 0;
}

UWORD RemoveGList(struct Window *win, struct Gadget *gl, LONG num)
{
    struct HWin *hw = (struct HWin *)win;
    struct Gadget **pp, *g;
    (void)num;
    if (!hw || !gl) return 0;
    for (pp = &win->FirstGadget; *pp; pp = &(*pp)->NextGadget)
        if (*pp == gl) { *pp = NULL; break; }
    for (g = gl; g; g = g->NextGadget) {
        if ((struct HGad *)g == hw->active) hw->active = NULL;
        ((struct HGad *)g)->win = NULL;
    }
    return 0;
}

void hgad_window_closing(struct HWin *hw)
{
    struct Gadget *g;
    for (g = hw->w.FirstGadget; g; g = g->NextGadget) ((struct HGad *)g)->win = NULL;
    hw->w.FirstGadget = NULL;
    hw->active = NULL;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */
/* ------------------------------------------------------------------ */

static void draw_label(struct HGad *g, struct HSurf *s)
{
    int len, w, x, y;
    ULONG pl = g->ngflags & (PLACETEXT_LEFT | PLACETEXT_RIGHT | PLACETEXT_ABOVE | PLACETEXT_BELOW | PLACETEXT_IN);
    if (!g->text || !*g->text) return;
    len = (int)strlen(g->text);
    w = len * HG_FW;
    if (!pl) {
        if (g->kind == BUTTON_KIND) pl = PLACETEXT_IN;
        else if (g->kind == LISTVIEW_KIND) pl = PLACETEXT_ABOVE;
        else pl = PLACETEXT_LEFT;
    }
    y = g->g.TopEdge + (g->g.Height - HG_FH) / 2;
    if (pl & PLACETEXT_IN)         x = g->g.LeftEdge + (g->g.Width - w) / 2;
    else if (pl & PLACETEXT_RIGHT) x = g->g.LeftEdge + g->g.Width + 8;
    else if (pl & PLACETEXT_ABOVE) { x = g->g.LeftEdge + (g->g.Width - w) / 2; y = g->g.TopEdge - HG_FH - 3; }
    else if (pl & PLACETEXT_BELOW) { x = g->g.LeftEdge + (g->g.Width - w) / 2; y = g->g.TopEdge + g->g.Height + 3; }
    else                           x = g->g.LeftEdge - w - 8;
    hs_text(s, x, y, g->text, len, P_TEXT);
}

static void draw_listview(struct HGad *g, struct HSurf *s)
{
    int x = g->g.LeftEdge, y = g->g.TopEdge, w = g->g.Width, h = g->g.Height;
    int rows = lv_rows(g), i;
    int ix0 = x + 2, ix1 = x + w - HG_LV_SBW - 3;
    int sbx = x + w - HG_LV_SBW;
    struct Node *n = NULL;

    hs_fill(s, x, y, x + w - 1, y + h - 1, P_BG);
    hs_bevel(s, x, y, w - HG_LV_SBW, h, 0);

    /* scroller: track, knob, two arrows at the bottom */
    {
        int ah = 10, th = h - 2 * ah;
        int ty = y, kh, ky;
        hs_bevel(s, sbx, ty, HG_LV_SBW, th, 0);
        if (th > 8) {
            int inner = th - 4;
            if (g->total > rows) {
                kh = inner * rows / g->total; if (kh < 6) kh = 6;
                ky = (g->total - rows) ? (inner - kh) * g->top / (g->total - rows) : 0;
            } else { kh = inner; ky = 0; }
            hs_fill(s, sbx + 4, ty + 2 + ky, sbx + HG_LV_SBW - 5, ty + 2 + ky + kh - 1, P_FILL);
        }
        for (i = 0; i < 2; i++) {
            int ay = y + th + i * ah, k;
            hs_bevel(s, sbx, ay, HG_LV_SBW, ah, 0);
            for (k = 0; k < 4; k++) {
                int yy = i == 0 ? ay + 3 + k : ay + 6 - k;
                hs_hline(s, sbx + 7 - k, sbx + 8 + k, yy, P_TEXT);
            }
        }
    }

    if (g->labels && g->labels != HG_DETACHED) {
        n = g->labels->lh_Head;
        for (i = 0; i < g->top && n && n->ln_Succ; i++) n = n->ln_Succ;
    }
    for (i = 0; i < rows; i++) {
        int ry = y + 2 + i * HG_LV_ROWH, idx = g->top + i;
        if (!n || !n->ln_Succ) break;
        if (g->hook && g->win) {
            struct LVDrawMsg msg;
            struct RastPort *rp = &g->win->rp;
            struct RastPort save = *rp;
            msg.lvdm_MethodID = LV_DRAW;
            msg.lvdm_RastPort = rp;
            msg.lvdm_DrawInfo = &hg_dri;
            msg.lvdm_Bounds.MinX = (WORD)ix0; msg.lvdm_Bounds.MaxX = (WORD)ix1;
            msg.lvdm_Bounds.MinY = (WORD)ry;  msg.lvdm_Bounds.MaxY = (WORD)(ry + HG_LV_ROWH - 1);
            msg.lvdm_State = (idx == g->press_row) ? LVR_SELECTED : LVR_NORMAL;
            rp->hg_clip = 1;
            rp->hg_cx0 = (WORD)ix0; rp->hg_cx1 = (WORD)ix1;
            rp->hg_cy0 = (WORD)ry;  rp->hg_cy1 = (WORD)(ry + HG_LV_ROWH - 1);
            host_reg_a0 = g->hook; host_reg_a1 = &msg; host_reg_a2 = n;
            g->hook->h_Entry();
            *rp = save;
        } else {
            const char *nm = n->ln_Name ? n->ln_Name : "";
            int len = (int)strlen(nm), maxc = (ix1 - ix0 - 2) / HG_FW;
            int selrow = (idx == g->sel && g->showsel) || idx == g->press_row;
            if (len > maxc) len = maxc;
            if (selrow) hs_fill(s, ix0, ry, ix1, ry + HG_LV_ROWH - 1, P_FILL);
            hs_text(s, ix0 + 2, ry, nm, len, selrow ? P_SHINE : P_TEXT);
        }
        n = n->ln_Succ;
    }
}

static void draw_string(struct HGad *g, struct HSurf *s)
{
    int x = g->g.LeftEdge, y = g->g.TopEdge, w = g->g.Width, h = g->g.Height;
    int active = g->win && g->win->active == g;
    int vis = (w - 12) / HG_FW, len = g->si.NumChars, ty = y + (h - HG_FH) / 2;
    if (vis < 1) vis = 1;
    hs_fill(s, x, y, x + w - 1, y + h - 1, P_BG);
    hs_bevel(s, x, y, w, h, 0);
    hs_bevel(s, x + 2, y + 1, w - 4, h - 2, 1);
    if (g->si.BufferPos < g->si.DispPos) g->si.DispPos = g->si.BufferPos;
    if (g->si.BufferPos >= g->si.DispPos + vis) g->si.DispPos = (WORD)(g->si.BufferPos - vis + 1);
    if (!active) g->si.DispPos = 0;
    if (len - g->si.DispPos > vis) len = g->si.DispPos + vis;
    if (len > g->si.DispPos)
        hs_text(s, x + 6, ty, (char *)g->si.Buffer + g->si.DispPos, len - g->si.DispPos, P_TEXT);
    if (active) {
        int cx = x + 6 + (g->si.BufferPos - g->si.DispPos) * HG_FW;
        hs_comp(s, cx, ty, cx + HG_FW - 1, ty + HG_FH - 1);
    }
}

void hgad_draw(struct HGad *g)
{
    struct HSurf *s;
    int x, y, w, h;
    if (!g || g->kind < 0 || !g->win) return;
    s = &g->win->surf;
    x = g->g.LeftEdge; y = g->g.TopEdge; w = g->g.Width; h = g->g.Height;

    switch (g->kind) {
    case BUTTON_KIND:
        hs_fill(s, x, y, x + w - 1, y + h - 1, g->pressed ? P_FILL : P_BG);
        hs_bevel(s, x, y, w, h, g->pressed);
        draw_label(g, s);
        break;
    case CHECKBOX_KIND:
        hs_fill(s, x, y, x + CB_W - 1, y + CB_H - 1, P_BG);
        hs_bevel(s, x, y, CB_W, CB_H, 0);
        if (g->g.Flags & GFLG_SELECTED) {
            int k;
            for (k = 0; k < 4; k++) hs_fill(s, x + 7 + k, y + 4 + k, x + 8 + k, y + 5 + k, P_TEXT);
            for (k = 0; k < 7; k++) hs_fill(s, x + 11 + k, y + 8 - k, x + 12 + k, y + 8 - k, P_TEXT);
        }
        draw_label(g, s);
        break;
    case CYCLE_KIND: {
        const char *lab = (g->cy_labels && g->cy_count) ? g->cy_labels[g->cy_active] : "";
        int len = (int)strlen(lab), maxc = (w - 28) / HG_FW;
        if (len > maxc) len = maxc;
        hs_fill(s, x, y, x + w - 1, y + h - 1, g->pressed ? P_FILL : P_BG);
        hs_bevel(s, x, y, w, h, g->pressed);
        /* cycle glyph + divider */
        hs_fill(s, x + 6, y + 3, x + 13, y + h - 4, P_TEXT);
        hs_fill(s, x + 8, y + 4, x + 11, y + h - 5, g->pressed ? P_FILL : P_BG);
        hs_fill(s, x + 11, y + h / 2 - 1, x + 15, y + h / 2, g->pressed ? P_FILL : P_BG);
        hs_vline(s, x + 20, y + 2, y + h - 3, P_TEXT);
        hs_vline(s, x + 21, y + 2, y + h - 3, P_SHINE);
        if (len > 0)
            hs_text(s, x + 22 + (w - 24 - len * HG_FW) / 2, y + (h - HG_FH) / 2, lab, len, P_TEXT);
        draw_label(g, s);
        break; }
    case STRING_KIND:
        draw_string(g, s);
        draw_label(g, s);
        break;
    case TEXT_KIND: {
        const char *t = g->tx_text ? g->tx_text : "";
        int len = (int)strlen(t), maxc = (w - 8) / HG_FW;
        if (len > maxc) len = maxc;
        hs_fill(s, x, y, x + w - 1, y + h - 1, P_BG);
        if (g->tx_border) hs_bevel(s, x, y, w, h, 1);
        if (len > 0) hs_text(s, x + 4, y + (h - HG_FH) / 2, t, len, P_TEXT);
        draw_label(g, s);
        break; }
    case LISTVIEW_KIND:
        draw_listview(g, s);
        draw_label(g, s);
        break;
    default: break;
    }
    if ((g->g.Flags & GFLG_DISABLED) && g->kind != TEXT_KIND)
        hs_ghost(s, x + 1, y + 1, x + w - 2, y + h - 2);
    hg_dirty = 1;
}

void hgad_draw_all(struct HWin *hw)
{
    struct Gadget *g;
    for (g = hw->w.FirstGadget; g; g = g->NextGadget) hgad_draw((struct HGad *)g);
}

void RefreshGList(struct Gadget *gl, struct Window *win, APTR req, LONG num)
{
    (void)req;
    for (; gl && num != 0; gl = gl->NextGadget, num--) {
        struct HGad *g = (struct HGad *)gl;
        if (!g->win && win) g->win = (struct HWin *)win;
        hgad_draw(g);
    }
}

void GT_RefreshWindow(struct Window *win, APTR req)
{
    (void)req;
    if (win) hgad_draw_all((struct HWin *)win);
}
void GT_BeginRefresh(struct Window *win) { (void)win; }
void GT_EndRefresh(struct Window *win, LONG complete) { (void)win; (void)complete; }

void GT_SetGadgetAttrsA(struct Gadget *gad, struct Window *win, APTR req, struct TagItem *tags)
{
    struct HGad *g = (struct HGad *)gad;
    (void)req;
    if (!g) return;
    apply_tags(g, tags);
    if (win && g->win == (struct HWin *)win) hgad_draw(g);
}

LONG GT_GetGadgetAttrsA(struct Gadget *gad, struct Window *win, APTR req, struct TagItem *tags)
{
    struct HGad *g = (struct HGad *)gad;
    struct TagItem *t;
    LONG n = 0;
    (void)win; (void)req;
    if (!g) return 0;
    for (t = tags; t && t->ti_Tag != TAG_DONE; t++) {
        ULONG *out = (ULONG *)t->ti_Data;
        if (!out) continue;
        switch (t->ti_Tag) {
        case GTLV_Top:      *out = (ULONG)g->top; n++; break;
        case GTLV_Selected: *out = (ULONG)g->sel; n++; break;
        case GTCB_Checked:  *out = (g->g.Flags & GFLG_SELECTED) ? 1 : 0; n++; break;
        case GTCY_Active:   *out = (ULONG)g->cy_active; n++; break;
        case GTST_String:   *(STRPTR *)t->ti_Data = (STRPTR)g->si.Buffer; n++; break;
        default: break;
        }
    }
    return n;
}

BOOL ActivateGadget(struct Gadget *gad, struct Window *win, APTR req)
{
    struct HGad *g = (struct HGad *)gad, *old;
    struct HWin *hw = (struct HWin *)win;
    (void)req;
    if (!g || !hw || g->kind != STRING_KIND || (g->g.Flags & GFLG_DISABLED)) return FALSE;
    old = hw->active;
    hw->active = g;
    g->win = hw;
    g->si.BufferPos = g->si.NumChars;
    if (old && old != g) hgad_draw(old);
    hgad_draw(g);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Input                                                                */
/* ------------------------------------------------------------------ */

static int inside(struct HGad *g, int x, int y)
{
    return x >= g->g.LeftEdge && y >= g->g.TopEdge &&
           x < g->g.LeftEdge + g->g.Width && y < g->g.TopEdge + g->g.Height;
}

struct HGad *hgad_hit(struct HWin *hw, int x, int y)
{
    struct Gadget *g;
    struct HGad *found = NULL;
    for (g = hw->w.FirstGadget; g; g = g->NextGadget) {
        struct HGad *hg = (struct HGad *)g;
        if (hg->kind < 0 || hg->kind == TEXT_KIND || (g->Flags & GFLG_DISABLED)) continue;
        if (inside(hg, x, y)) found = hg;
    }
    return found;
}

/* listview parts: 0 = rows, 1 = track, 2 = up arrow, 3 = down arrow */
static int lv_part(struct HGad *g, int x, int y, int *row)
{
    int rel_y = y - g->g.TopEdge;
    if (x >= g->g.LeftEdge + g->g.Width - HG_LV_SBW) {
        int th = g->g.Height - 20;
        if (rel_y < th) return 1;
        return rel_y < th + 10 ? 2 : 3;
    }
    *row = (rel_y - 2) / HG_LV_ROWH;
    if (rel_y < 2) *row = 0;
    return 0;
}

static void lv_track_to(struct HGad *g, int y)
{
    int rows = lv_rows(g), th = g->g.Height - 20 - 4;
    if (g->total <= rows || th <= 0) return;
    g->top = (y - g->g.TopEdge - 2) * g->total / th - rows / 2;
    lv_clamp(g);
}

/* Deactivate the active string gadget.  The Amiga sends GADGETUP only for
   Return/Tab; here leaving the field by clicking elsewhere commits it too,
   so a typed value is not silently dropped when the next click is "OK". */
void hgad_blur(struct HWin *hw)
{
    struct HGad *a = hw->active;
    if (!a) return;
    hw->active = NULL;
    a->si.LongInt = (LONG)strtol((char *)a->si.Buffer, NULL, 10);
    hgad_draw(a);
    hw_post(hw, IDCMP_GADGETUP, 0, 0, &a->g);
}

void hgad_mouse_down(struct HGad *g, int x, int y, int shift)
{
    struct HWin *hw = g->win;
    if (!hw) return;
    if (hw->active && hw->active != g) hgad_blur(hw);

    switch (g->kind) {
    case BUTTON_KIND: case CYCLE_KIND: case CHECKBOX_KIND:
        g->pressed = shift ? 2 : 1;
        hgad_draw(g);
        break;
    case STRING_KIND: {
        int pos = (x - g->g.LeftEdge - 6 + HG_FW / 2) / HG_FW + g->si.DispPos;
        if (hw->active != g) g->si.DispPos = 0;
        hw->active = g;
        if (pos < 0) pos = 0;
        if (pos > g->si.NumChars) pos = g->si.NumChars;
        g->si.BufferPos = (WORD)pos;
        hgad_draw(g);
        break; }
    case LISTVIEW_KIND: {
        int row = 0, part = lv_part(g, x, y, &row);
        if (part == 2) { g->top--; lv_clamp(g); }
        else if (part == 3) { g->top++; lv_clamp(g); }
        else if (part == 1) { g->pressed = 1; lv_track_to(g, y); }
        else if (!g->readonly && g->top + row < g->total && row < lv_rows(g)) {
            g->press_row = g->top + row;
            g->sel = g->press_row;
        }
        hgad_draw(g);
        break; }
    default: break;
    }
}

void hgad_mouse_move(struct HGad *g, int x, int y)
{
    (void)x;
    if (g->kind == LISTVIEW_KIND && g->pressed) { lv_track_to(g, y); hgad_draw(g); }
}

void hgad_mouse_up(struct HGad *g, int x, int y)
{
    struct HWin *hw = g->win;
    int in;
    if (!hw) return;
    in = inside(g, x, y);
    switch (g->kind) {
    case BUTTON_KIND:
        g->pressed = 0; hgad_draw(g);
        if (in) hw_post(hw, IDCMP_GADGETUP, 0, 0, &g->g);
        break;
    case CHECKBOX_KIND:
        g->pressed = 0;
        if (in) g->g.Flags ^= GFLG_SELECTED;
        hgad_draw(g);
        if (in) hw_post(hw, IDCMP_GADGETUP, (g->g.Flags & GFLG_SELECTED) ? 1 : 0, 0, &g->g);
        break;
    case CYCLE_KIND: {
        int back = g->pressed == 2;
        g->pressed = 0;
        if (in && g->cy_count > 0)
            g->cy_active = (g->cy_active + (back ? g->cy_count - 1 : 1)) % g->cy_count;
        hgad_draw(g);
        if (in) hw_post(hw, IDCMP_GADGETUP, (UWORD)g->cy_active, 0, &g->g);
        break; }
    case LISTVIEW_KIND: {
        int row = g->press_row;
        g->pressed = 0; g->press_row = -1;
        hgad_draw(g);
        if (row >= 0) hw_post(hw, IDCMP_GADGETUP, (UWORD)row, 0, &g->g);
        break; }
    default: break;
    }
}

void hgad_wheel(struct HWin *hw, int x, int y, int dy)
{
    struct Gadget *g;
    for (g = hw->w.FirstGadget; g; g = g->NextGadget) {
        struct HGad *hg = (struct HGad *)g;
        if (hg->kind == LISTVIEW_KIND && inside(hg, x, y)) {
            hg->top -= dy * 3; lv_clamp(hg); hgad_draw(hg);
            return;
        }
    }
}

static struct HGad *next_string(struct HWin *hw, struct HGad *from, int back)
{
    struct Gadget *g;
    struct HGad *first = NULL, *prev = NULL, *last = NULL;
    int seen = 0;
    for (g = hw->w.FirstGadget; g; g = g->NextGadget) {
        struct HGad *hg = (struct HGad *)g;
        if (hg->kind != STRING_KIND || (g->Flags & GFLG_DISABLED)) continue;
        if (!first) first = hg;
        if (hg == from) seen = 1;
        else if (seen == 1 && !back) return hg;
        if (!seen) prev = hg;
        last = hg;
    }
    if (back) return prev ? prev : last;
    return first;
}

BOOL hgad_key(struct HWin *hw, int code, int ctrl, int shift)
{
    struct HGad *g = hw->active;
    char *b;
    int len, pos;

    if (!g) {
        /* no active string gadget: cursor keys scroll the first listview */
        if (code >= HK_UP && code <= HK_PGDN) {
            struct Gadget *q;
            for (q = hw->w.FirstGadget; q; q = q->NextGadget) {
                struct HGad *lv = (struct HGad *)q;
                if (lv->kind != LISTVIEW_KIND) continue;
                lv->top += code == HK_UP ? -1 : code == HK_DOWN ? 1 :
                           code == HK_PGUP ? -lv_rows(lv) : lv_rows(lv);
                lv_clamp(lv); hgad_draw(lv);
                return TRUE;
            }
        }
        return code >= 256;
    }
    b = (char *)g->si.Buffer; len = g->si.NumChars; pos = g->si.BufferPos;

    if (ctrl && code == 'x') { b[0] = '\0'; len = pos = 0; }
    else if (ctrl) return FALSE;
    else switch (code) {
    case 13:
        hw->active = NULL;
        g->si.LongInt = (LONG)strtol(b, NULL, 10);
        hgad_draw(g);
        hw_post(hw, IDCMP_GADGETUP, 0, 0, &g->g);
        return TRUE;
    case 27:
        hw->active = NULL; hgad_draw(g);
        return FALSE;                      /* let the window see Esc too */
    case 9: {
        struct HGad *n = next_string(hw, g, shift);
        hw->active = n;
        g->si.LongInt = (LONG)strtol(b, NULL, 10);
        hgad_draw(g);
        hw_post(hw, IDCMP_GADGETUP, 9, 0, &g->g);
        if (n) { n->si.BufferPos = n->si.NumChars; hgad_draw(n); }
        return TRUE; }
    case 8:
        if (pos > 0) { memmove(b + pos - 1, b + pos, (size_t)(len - pos + 1)); pos--; len--; }
        break;
    case 127:
        if (pos < len) { memmove(b + pos, b + pos + 1, (size_t)(len - pos)); len--; }
        break;
    case HK_LEFT:  if (pos > 0) pos--; break;
    case HK_RIGHT: if (pos < len) pos++; break;
    case HK_HOME:  pos = 0; break;
    case HK_END:   pos = len; break;
    default: return code >= 256;
    }
    g->si.NumChars = (WORD)len; g->si.BufferPos = (WORD)pos;
    hgad_draw(g);
    return TRUE;
}

void hgad_text_input(struct HWin *hw, int ch)
{
    struct HGad *g = hw->active;
    char *b;
    int len, pos;
    if (!g) return;
    b = (char *)g->si.Buffer; len = g->si.NumChars; pos = g->si.BufferPos;
    if (len >= g->maxchars) return;
    memmove(b + pos + 1, b + pos, (size_t)(len - pos + 1));
    b[pos] = (char)ch;
    g->si.NumChars = (WORD)(len + 1); g->si.BufferPos = (WORD)(pos + 1);
    hgad_draw(g);
}

/* ------------------------------------------------------------------ */
/* Menus                                                                */
/* ------------------------------------------------------------------ */

#define MI_H   (HG_FH + 2)
#define MI_BAR 6

static int m_open = -1, m_item = -1;
static struct HSurf m_surf;
static int m_x, m_y;

struct Menu *CreateMenusA(struct NewMenu *nm, struct TagItem *tags)
{
    struct Menu *first = NULL, *cur = NULL;
    struct MenuItem *lasti = NULL;
    (void)tags;
    for (; nm && nm->nm_Type != NM_END; nm++) {
        if (nm->nm_Type == NM_TITLE) {
            struct Menu *m = (struct Menu *)calloc(1, sizeof(*m));
            if (!m) break;
            m->hg_title = (const char *)nm->nm_Label;
            m->Flags = (nm->nm_Flags & NM_MENUDISABLED) ? 0 : MENUENABLED;
            if (cur) cur->NextMenu = m; else first = m;
            cur = m; lasti = NULL;
        } else if (nm->nm_Type == NM_ITEM && cur) {
            struct MenuItem *it = (struct MenuItem *)calloc(1, sizeof(*it));
            if (!it) break;
            it->hg_label = (nm->nm_Label == NM_BARLABEL) ? NULL : (const char *)nm->nm_Label;
            it->hg_key = nm->nm_CommKey ? nm->nm_CommKey[0] : 0;
            it->hg_userdata = nm->nm_UserData;
            it->Flags = (UWORD)((nm->nm_Flags & (CHECKIT | CHECKED | MENUTOGGLE)) | ITEMTEXT | HIGHCOMP);
            if (!(nm->nm_Flags & NM_ITEMDISABLED) && it->hg_label) it->Flags |= ITEMENABLED;
            it->NextSelect = MENUNULL;
            if (lasti) lasti->NextItem = it; else cur->FirstItem = it;
            lasti = it;
        }
    }
    return first;
}

BOOL LayoutMenusA(struct Menu *menu, APTR vi, struct TagItem *tags)
{
    int x = 8;
    (void)vi; (void)tags;
    for (; menu; menu = menu->NextMenu) {
        menu->hg_x = (WORD)x;
        menu->hg_w = (WORD)(hg_textw(menu->hg_title) + 16);
        x += menu->hg_w;
    }
    return TRUE;
}

void FreeMenus(struct Menu *menu)
{
    while (menu) {
        struct Menu *nm = menu->NextMenu;
        struct MenuItem *it = menu->FirstItem;
        while (it) { struct MenuItem *ni = it->NextItem; free(it); it = ni; }
        free(menu);
        menu = nm;
    }
}

BOOL SetMenuStrip(struct Window *win, struct Menu *menu)
{
    if (!win) return FALSE;
    if (menu && !menu->hg_w) LayoutMenusA(menu, NULL, NULL);
    win->MenuStrip = menu;
    hg_bar_redraw();
    return TRUE;
}

void ClearMenuStrip(struct Window *win)
{
    if (!win) return;
    hmenu_close();
    win->MenuStrip = NULL;
    hg_bar_redraw();
}

static struct Menu *menu_at(int idx)
{
    struct Menu *m = hg_top ? hg_top->w.MenuStrip : NULL;
    while (m && idx-- > 0) m = m->NextMenu;
    return m;
}

struct MenuItem *ItemAddress(struct Menu *strip, ULONG num)
{
    struct Menu *m = strip;
    struct MenuItem *it;
    int mi = MENUNUM(num), ii = ITEMNUM(num);
    if (num == MENUNULL) return NULL;
    while (m && mi-- > 0) m = m->NextMenu;
    if (!m) return NULL;
    for (it = m->FirstItem; it && ii-- > 0; it = it->NextItem) ;
    return it;
}

void hmenu_draw_bar(struct HSurf *s)
{
    struct Menu *m;
    int i = 0;
    for (m = hg_top->w.MenuStrip; m; m = m->NextMenu, i++) {
        int len = (int)strlen(m->hg_title);
        if (i == m_open) {
            hs_fill(s, m->hg_x, 0, m->hg_x + m->hg_w - 1, hg_scr.BarHeight - 1, P_TEXT);
            hs_text(s, m->hg_x + 8, 1, m->hg_title, len, P_SHINE);
        } else {
            hs_text(s, m->hg_x + 8, 1, m->hg_title, len, P_TEXT);
        }
    }
}

int hmenu_bar_hit(int x)
{
    struct Menu *m;
    int i = 0;
    if (!hg_top) return -1;
    for (m = hg_top->w.MenuStrip; m; m = m->NextMenu, i++)
        if (x >= m->hg_x && x < m->hg_x + m->hg_w) return i;
    return -1;
}

int hmenu_is_open(void) { return m_open >= 0; }

static void menu_render(void)
{
    struct Menu *m = menu_at(m_open);
    struct MenuItem *it;
    int w = 0, h = 4, y, i, anykey = 0, anycheck = 0;

    free(m_surf.pix); m_surf.pix = NULL;
    if (!m) return;
    for (it = m->FirstItem; it; it = it->NextItem) {
        int lw = it->hg_label ? hg_textw(it->hg_label) : 0;
        if (lw > w) w = lw;
        if (it->hg_key) anykey = 1;
        if (it->Flags & CHECKIT) anycheck = 1;
        h += it->hg_label ? MI_H : MI_BAR;
    }
    w += 16 + (anycheck ? 16 : 0) + (anykey ? 8 * HG_FW : 0);
    m_surf.pix = (UBYTE *)malloc((size_t)w * h);
    if (!m_surf.pix) return;
    m_surf.w = w; m_surf.h = h;
    m_x = m->hg_x; m_y = hg_scr.BarHeight + 1;
    if (m_x + w > hg_scr.Width) m_x = hg_scr.Width - w;
    hs_fill(&m_surf, 0, 0, w - 1, h - 1, P_SHINE);
    hs_fill(&m_surf, 0, 0, w - 1, 0, P_TEXT);
    hs_fill(&m_surf, 0, h - 1, w - 1, h - 1, P_TEXT);
    hs_fill(&m_surf, 0, 0, 0, h - 1, P_TEXT);
    hs_fill(&m_surf, w - 1, 0, w - 1, h - 1, P_TEXT);
    y = 2;
    for (it = m->FirstItem, i = 0; it; it = it->NextItem, i++) {
        if (!it->hg_label) {
            hs_hline(&m_surf, 4, w - 5, y + MI_BAR / 2 - 1, P_TEXT);
            y += MI_BAR;
            continue;
        }
        {
            int hot = (i == m_item) && (it->Flags & ITEMENABLED);
            int tx = 8 + (anycheck ? 16 : 0);
            if (hot) hs_fill(&m_surf, 2, y, w - 3, y + MI_H - 1, P_TEXT);
            hs_text(&m_surf, tx, y + 1, it->hg_label, (int)strlen(it->hg_label), hot ? P_SHINE : P_TEXT);
            if (it->Flags & CHECKED) hs_text(&m_surf, 6, y + 1, "\xD7", 1, hot ? P_SHINE : P_TEXT);
            if (it->hg_key) {
                char k[8];
                snprintf(k, sizeof(k), "Ctrl+%c", it->hg_key);
                hs_text(&m_surf, w - 8 - (int)strlen(k) * HG_FW, y + 1, k, (int)strlen(k), hot ? P_SHINE : P_TEXT);
            }
            if (!(it->Flags & ITEMENABLED)) hs_ghost(&m_surf, 2, y, w - 3, y + MI_H - 1);
        }
        y += MI_H;
    }
    hg_dirty = 1;
}

void hmenu_open(int idx)
{
    if (idx == m_open) { hmenu_close(); return; }
    m_open = idx; m_item = -1;
    menu_render();
    hg_bar_redraw();
}

void hmenu_close(void)
{
    if (m_open < 0) return;
    m_open = -1; m_item = -1;
    free(m_surf.pix); m_surf.pix = NULL;
    hg_bar_redraw();
}

static int menu_item_at(int x, int y)
{
    struct Menu *m = menu_at(m_open);
    struct MenuItem *it;
    int yy = m_y + 2, i = 0;
    if (!m || !m_surf.pix || x < m_x || x >= m_x + m_surf.w) return -1;
    for (it = m->FirstItem; it; it = it->NextItem, i++) {
        int hh = it->hg_label ? MI_H : MI_BAR;
        if (y >= yy && y < yy + hh) return it->hg_label ? i : -1;
        yy += hh;
    }
    return -1;
}

void hmenu_motion(int x, int y)
{
    int it;
    if (y <= hg_scr.BarHeight) {
        int idx = hmenu_bar_hit(x);
        if (idx >= 0 && idx != m_open) { m_open = idx; m_item = -1; menu_render(); hg_bar_redraw(); }
        return;
    }
    it = menu_item_at(x, y);
    if (it != m_item) { m_item = it; menu_render(); }
}

void hmenu_pick(int mi, int ii)
{
    struct HWin *hw = hg_top;
    struct MenuItem *it;
    ULONG code = FULLMENUNUM(mi, ii, NOSUB);
    if (!hw || !hw->w.MenuStrip) return;
    it = ItemAddress(hw->w.MenuStrip, code);
    if (!it || !it->hg_label || !(it->Flags & ITEMENABLED)) return;
    if ((it->Flags & CHECKIT) && (it->Flags & MENUTOGGLE)) it->Flags ^= CHECKED;
    else if (it->Flags & CHECKIT) it->Flags |= CHECKED;
    it->NextSelect = MENUNULL;
    hw_post(hw, IDCMP_MENUPICK, (UWORD)code, 0, &hw->w);
}

BOOL hmenu_release(int x, int y)
{
    int it, m = m_open;
    if (m < 0) return FALSE;
    if (y <= hg_scr.BarHeight) return hmenu_bar_hit(x) >= 0;    /* stays open */
    it = menu_item_at(x, y);
    if (it < 0) {
        if (x >= m_x && x < m_x + m_surf.w && y < m_y + m_surf.h) return TRUE;   /* on a bar */
        return FALSE;
    }
    hmenu_close();
    hmenu_pick(m, it);
    return TRUE;
}

BOOL hmenu_shortcut(int key)
{
    struct Menu *m;
    struct MenuItem *it;
    int mi = 0, ii;
    if (!hg_top) return FALSE;
    for (m = hg_top->w.MenuStrip; m; m = m->NextMenu, mi++)
        for (it = m->FirstItem, ii = 0; it; it = it->NextItem, ii++)
            if (it->hg_key && (it->hg_key | 0x20) == (key | 0x20)) { hmenu_pick(mi, ii); return TRUE; }
    return FALSE;
}

BOOL hmenu_overlay(struct HSurf **s, int *x, int *y)
{
    if (m_open < 0 || !m_surf.pix) return FALSE;
    *s = &m_surf; *x = m_x; *y = m_y;
    return TRUE;
}
