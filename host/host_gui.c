/* host_gui.c - Intuition + graphics.library as AmiPart uses them, on SDL2.
 *
 * One SDL window is the "screen".  AmiPart's windows live inside it as
 * 8-bit indexed pixel buffers (pens, as on the Amiga) that this file
 * composes and presents.  Each window keeps its contents (no damage /
 * refresh cycle), and input always goes to the most recently opened
 * window - every AmiPart dialog is modal in practice.
 *
 * Gadgets and menus are in host_gadtools.c, requesters in host_req.c.
 *
 * Testing without a display: AMIPART_GUI_HEADLESS=1 skips SDL video
 * entirely, AMIPART_GUI_SCRIPT=<file> feeds input events and takes
 * screenshots (see hg_script_step). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <SDL.h>
#include "hg_int.h"
#include "gui_font.h"

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

struct Screen   hg_scr;
struct HWin    *hg_top;
struct DrawInfo hg_dri;
int             hg_dirty;

void *host_reg_a0, *host_reg_a1, *host_reg_a2;

static struct TextAttr hg_ta = { (STRPTR)"fixed.font", HG_FH, 0, 0 };
static struct TextFont hg_tf = { HG_FH, HG_FW, HG_FB };
static UWORD hg_pens[NUMDRIPENS] = { 0, 1, 1, 2, 1, 3, 1, 0, 2, 1, 1, 1 };
static struct ColorMap hg_cm;

static SDL_Window   *sdl_win;
static SDL_Renderer *sdl_ren;
static SDL_Texture  *sdl_tex;
static SDL_Cursor   *cur_size, *cur_arrow;
static int      hg_ready, hg_headless, hg_scale = 1;
static uint32_t *hg_argb;
static struct HSurf hg_base;            /* desktop + screen bar */
static uint32_t hg_pal[256];
static int      hg_penref[128];
static Uint32   hg_last_present;
static char     hg_scr_title[160] = "AmiPart";

/* mouse tracking */
enum { TR_NONE, TR_DRAG, TR_SIZE, TR_CLOSE, TR_GAD };
static int  tr_mode, tr_dx, tr_dy, tr_w, tr_h;
static struct HGad *tr_gad;
static int  hg_mx, hg_my, hg_shift, hg_ctrl;

static FILE *hg_script;

#define BAR_H (hg_scr.BarHeight + 1)

/* ------------------------------------------------------------------ */
/* Surface drawing                                                      */
/* ------------------------------------------------------------------ */

static inline UBYTE comp_pen(UBYTE p) { return p < 4 ? (UBYTE)(p ^ 3) : (UBYTE)(p ^ 0x80); }

void hs_fill(struct HSurf *s, int x0, int y0, int x1, int y1, int pen)
{
    int y;
    if (!s || !s->pix) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    if (y1 >= s->h) y1 = s->h - 1;
    if (x0 > x1 || y0 > y1) return;
    for (y = y0; y <= y1; y++)
        memset(s->pix + (size_t)y * s->w + x0, pen, (size_t)(x1 - x0 + 1));
}

void hs_comp(struct HSurf *s, int x0, int y0, int x1, int y1)
{
    int x, y;
    if (!s || !s->pix) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    if (y1 >= s->h) y1 = s->h - 1;
    for (y = y0; y <= y1; y++) {
        UBYTE *p = s->pix + (size_t)y * s->w + x0;
        for (x = x0; x <= x1; x++, p++) *p = comp_pen(*p);
    }
}

void hs_hline(struct HSurf *s, int x0, int x1, int y, int pen) { hs_fill(s, x0, y, x1, y, pen); }
void hs_vline(struct HSurf *s, int x, int y0, int y1, int pen) { hs_fill(s, x, y0, x, y1, pen); }

/* Text with a clip rectangle; mode: 0 = JAM1, 1 = JAM2 (bg pen), 2 = complement */
static void hs_text_clip(struct HSurf *s, int x, int ytop, const char *t, int len,
                         int pen, int bg, int mode,
                         int cx0, int cy0, int cx1, int cy1)
{
    int i, r, c;
    if (!s || !s->pix || !t) return;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= s->w) cx1 = s->w - 1;
    if (cy1 >= s->h) cy1 = s->h - 1;
    for (i = 0; i < len; i++, x += HG_FW) {
        const unsigned char *gl = gui_font[(unsigned char)t[i]];
        if (x + HG_FW <= cx0 || x > cx1) continue;
        for (r = 0; r < HG_FH; r++) {
            int y = ytop + r;
            unsigned char bits = gl[r];
            UBYTE *p;
            if (y < cy0 || y > cy1) continue;
            p = s->pix + (size_t)y * s->w;
            for (c = 0; c < HG_FW; c++) {
                int px = x + c;
                if (px < cx0 || px > cx1) continue;
                if (bits & (0x80 >> c)) {
                    if (mode == 2) p[px] = comp_pen(p[px]);
                    else           p[px] = (UBYTE)pen;
                } else if (mode == 1) {
                    p[px] = (UBYTE)bg;
                }
            }
        }
    }
}

void hs_text(struct HSurf *s, int x, int ytop, const char *t, int len, int pen)
{
    if (!s) return;
    hs_text_clip(s, x, ytop, t, len, pen, 0, 0, 0, 0, s->w - 1, s->h - 1);
}

int hg_textw(const char *t) { return t ? (int)strlen(t) * HG_FW : 0; }

void hs_bevel(struct HSurf *s, int x, int y, int w, int h, int recessed)
{
    int a = recessed ? P_TEXT : P_SHINE, b = recessed ? P_SHINE : P_TEXT;
    if (w < 2 || h < 2) return;
    hs_hline(s, x, x + w - 2, y, a);
    hs_vline(s, x, y, y + h - 2, a);
    hs_vline(s, x + 1, y + 1, y + h - 2, a);
    hs_hline(s, x + 1, x + w - 1, y + h - 1, b);
    hs_vline(s, x + w - 1, y, y + h - 1, b);
    hs_vline(s, x + w - 2, y + 1, y + h - 2, b);
}

/* Ghost pattern for disabled gadgets: every other pixel in the text pen. */
void hs_ghost(struct HSurf *s, int x0, int y0, int x1, int y1)
{
    int x, y;
    if (!s || !s->pix) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    if (y1 >= s->h) y1 = s->h - 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            if ((x ^ y) & 1) s->pix[(size_t)y * s->w + x] = P_BG;
}

static void hs_line(struct HSurf *s, int x0, int y0, int x1, int y1, int pen, int comp,
                    int cx0, int cy0, int cx1, int cy1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    if (!s || !s->pix) return;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= s->w) cx1 = s->w - 1;
    if (cy1 >= s->h) cy1 = s->h - 1;
    for (;;) {
        if (x0 >= cx0 && x0 <= cx1 && y0 >= cy0 && y0 <= cy1) {
            UBYTE *p = s->pix + (size_t)y0 * s->w + x0;
            *p = comp ? comp_pen(*p) : (UBYTE)pen;
        }
        if (x0 == x1 && y0 == y1) break;
        { int e2 = 2 * err;
          if (e2 >= dy) { err += dy; x0 += sx; }
          if (e2 <= dx) { err += dx; y0 += sy; } }
    }
}

/* ------------------------------------------------------------------ */
/* graphics.library                                                     */
/* ------------------------------------------------------------------ */

static void rp_clip(struct RastPort *rp, int *x0, int *y0, int *x1, int *y1)
{
    struct HSurf *s = (struct HSurf *)rp->hg_surf;
    *x0 = 0; *y0 = 0; *x1 = s->w - 1; *y1 = s->h - 1;
    if (rp->hg_clip) {
        if (rp->hg_cx0 > *x0) *x0 = rp->hg_cx0;
        if (rp->hg_cy0 > *y0) *y0 = rp->hg_cy0;
        if (rp->hg_cx1 < *x1) *x1 = rp->hg_cx1;
        if (rp->hg_cy1 < *y1) *y1 = rp->hg_cy1;
    }
}

void InitRastPort(struct RastPort *rp)
{
    memset(rp, 0, sizeof(*rp));
    rp->FgPen = 1;
    rp->Font = &hg_tf;
    rp->TxHeight = HG_FH; rp->TxWidth = HG_FW; rp->TxBaseline = HG_FB;
}
void SetFont(struct RastPort *rp, struct TextFont *tf) { (void)tf; rp->Font = &hg_tf; }
struct TextFont *OpenFont(struct TextAttr *ta) { (void)ta; return &hg_tf; }
void CloseFont(struct TextFont *tf) { (void)tf; }

void SetAPen(struct RastPort *rp, ULONG pen) { rp->FgPen = (BYTE)pen; }
void SetBPen(struct RastPort *rp, ULONG pen) { rp->BgPen = (BYTE)pen; }
void SetDrMd(struct RastPort *rp, ULONG mode) { rp->DrawMode = (BYTE)mode; }
void Move(struct RastPort *rp, LONG x, LONG y) { rp->cp_x = (WORD)x; rp->cp_y = (WORD)y; }

void Draw(struct RastPort *rp, LONG x, LONG y)
{
    if (rp->hg_surf) {
        int cx0, cy0, cx1, cy1;
        rp_clip(rp, &cx0, &cy0, &cx1, &cy1);
        hs_line((struct HSurf *)rp->hg_surf, rp->cp_x, rp->cp_y, (int)x, (int)y,
                (UBYTE)rp->FgPen, (rp->DrawMode & COMPLEMENT) != 0, cx0, cy0, cx1, cy1);
        hg_dirty = 1;
    }
    rp->cp_x = (WORD)x; rp->cp_y = (WORD)y;
}

static void rp_rect(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1, int pen, int comp)
{
    int cx0, cy0, cx1, cy1;
    if (!rp->hg_surf) return;
    rp_clip(rp, &cx0, &cy0, &cx1, &cy1);
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 > x1 || y0 > y1) return;
    if (comp) hs_comp((struct HSurf *)rp->hg_surf, (int)x0, (int)y0, (int)x1, (int)y1);
    else      hs_fill((struct HSurf *)rp->hg_surf, (int)x0, (int)y0, (int)x1, (int)y1, pen);
    hg_dirty = 1;
}

void RectFill(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1)
{
    rp_rect(rp, x0, y0, x1, y1, (UBYTE)rp->FgPen, (rp->DrawMode & COMPLEMENT) != 0);
}
void EraseRect(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1)
{
    rp_rect(rp, x0, y0, x1, y1, P_BG, 0);
}

WORD TextLength(struct RastPort *rp, CONST_STRPTR s, ULONG len)
{
    (void)rp; (void)s;
    return (WORD)(len * HG_FW);
}

LONG Text(struct RastPort *rp, CONST_STRPTR s, ULONG len)
{
    if (rp->hg_surf && s) {
        int cx0, cy0, cx1, cy1, mode = 0;
        int fg = (UBYTE)rp->FgPen, bg = (UBYTE)rp->BgPen;
        rp_clip(rp, &cx0, &cy0, &cx1, &cy1);
        if (rp->DrawMode & COMPLEMENT) mode = 2;
        else if (rp->DrawMode & JAM2)  mode = 1;
        if (rp->DrawMode & INVERSVID) { int t = fg; fg = bg; bg = t; mode = 1; }
        hs_text_clip((struct HSurf *)rp->hg_surf, rp->cp_x, rp->cp_y - HG_FB,
                     (const char *)s, (int)len, fg, bg, mode, cx0, cy0, cx1, cy1);
        hg_dirty = 1;
    }
    rp->cp_x = (WORD)(rp->cp_x + len * HG_FW);
    return 0;
}

static void pal_set(int pen, uint32_t rgb)
{
    hg_pal[pen] = 0xFF000000u | rgb;
    if (pen >= 4 && pen < 128) hg_pal[pen | 0x80] = 0xFF000000u | (~rgb & 0xFFFFFF);
}

LONG ObtainBestPenA(struct ColorMap *cm, ULONG r, ULONG g, ULONG b, struct TagItem *tags)
{
    uint32_t rgb = ((r >> 24) << 16) | ((g >> 24) << 8) | (b >> 24);
    int i;
    (void)cm; (void)tags;
    for (i = 4; i < 128; i++)
        if (hg_penref[i] && (hg_pal[i] & 0xFFFFFF) == rgb) { hg_penref[i]++; return i; }
    for (i = 4; i < 128; i++)
        if (!hg_penref[i]) { hg_penref[i] = 1; pal_set(i, rgb); return i; }
    return -1;
}

void ReleasePen(struct ColorMap *cm, ULONG pen)
{
    (void)cm;
    if (pen >= 4 && pen < 128 && hg_penref[pen] > 0) hg_penref[pen]--;
}

/* ------------------------------------------------------------------ */
/* Time                                                                 */
/* ------------------------------------------------------------------ */

ULONG hg_ticks(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (ULONG)(tv.tv_sec * 1000UL + tv.tv_usec / 1000);
}

BOOL DoubleClick(ULONG s1, ULONG m1, ULONG s2, ULONG m2)
{
    long long d = ((long long)s2 - (long long)s1) * 1000000LL + ((long long)m2 - (long long)m1);
    return (BOOL)(d >= 0 && d <= 500000LL);
}

/* ------------------------------------------------------------------ */
/* Screen bar, window frames                                            */
/* ------------------------------------------------------------------ */

void hg_bar_redraw(void)
{
    struct HSurf *s = &hg_base;
    if (!s->pix) return;
    hs_fill(s, 0, 0, s->w - 1, BAR_H - 2, P_SHINE);
    hs_hline(s, 0, s->w - 1, BAR_H - 1, P_TEXT);
    if (hg_top && hg_top->w.MenuStrip)
        hmenu_draw_bar(s);
    else
        hs_text(s, 6, 1, hg_scr_title, (int)strlen(hg_scr_title), P_TEXT);
    hg_dirty = 1;
}

static int frame_close_w(struct HWin *hw) { return (hw->w.Flags & WFLG_CLOSEGADGET) ? 20 : 0; }
static int frame_depth_w(struct HWin *hw) { return (hw->w.Flags & WFLG_DEPTHGADGET) ? 24 : 0; }

static void hw_draw_frame(struct HWin *hw)
{
    struct Window *w = &hw->w;
    struct HSurf *s = &hw->surf;
    int W = w->Width, H = w->Height;
    int active = (hw == hg_top);
    int fill = active ? P_FILL : P_BG;
    int bt = w->BorderTop, bl = w->BorderLeft, br = w->BorderRight, bb = w->BorderBottom;

    if (w->Flags & WFLG_BORDERLESS) return;

    hs_fill(s, 0, 0, W - 1, bt - 1, fill);
    hs_fill(s, 0, H - bb, W - 1, H - 1, fill);
    hs_fill(s, 0, 0, bl - 1, H - 1, fill);
    hs_fill(s, W - br, 0, W - 1, H - 1, fill);
    /* outer 3D edge */
    hs_hline(s, 0, W - 1, 0, P_SHINE);
    hs_vline(s, 0, 0, H - 1, P_SHINE);
    hs_hline(s, 1, W - 1, H - 1, P_TEXT);
    hs_vline(s, W - 1, 1, H - 1, P_TEXT);
    /* inner edge */
    hs_hline(s, bl - 1, W - br, bt - 1, P_TEXT);
    hs_vline(s, bl - 1, bt - 1, H - bb, P_TEXT);
    hs_hline(s, bl - 1, W - br, H - bb, P_SHINE);
    hs_vline(s, W - br, bt - 1, H - bb, P_SHINE);

    if (bt > 4) {
        int cw = frame_close_w(hw), dw = frame_depth_w(hw);
        int tx = cw + 6, maxc, len = (int)strlen(hw->title);
        if (cw) {
            hs_vline(s, cw - 1, 1, bt - 2, P_TEXT);
            hs_vline(s, cw, 1, bt - 2, P_SHINE);
            hs_fill(s, 7, bt / 2 - 3, 12, bt / 2 + 2, P_TEXT);
            hs_fill(s, 8, bt / 2 - 2, 11, bt / 2 + 1, P_SHINE);
        }
        if (dw) {
            int x = W - dw;
            hs_vline(s, x, 1, bt - 2, P_TEXT);
            hs_vline(s, x + 1, 1, bt - 2, P_SHINE);
            hs_fill(s, x + 5, 3, x + 15, bt - 7, P_TEXT);
            hs_fill(s, x + 6, 4, x + 14, bt - 8, P_BG);
            hs_fill(s, x + 9, 6, x + 19, bt - 4, P_TEXT);
            hs_fill(s, x + 10, 7, x + 18, bt - 5, P_SHINE);
        }
        maxc = (W - dw - tx - 4) / HG_FW;
        if (len > maxc) len = maxc;
        if (len > 0) hs_text(s, tx, (bt - HG_FH) / 2, hw->title, len, P_TEXT);
    }
    if (w->Flags & WFLG_SIZEGADGET) {
        int x = W - 18, y = H - 10;
        hs_fill(s, x, y, W - 2, H - 2, fill);
        hs_vline(s, x, y, H - 2, P_TEXT);
        hs_hline(s, x, W - 2, y, P_TEXT);
        hs_fill(s, x + 4, y + 2, x + 13, y + 7, P_TEXT);
        hs_fill(s, x + 5, y + 3, x + 12, y + 6, P_SHINE);
        hs_fill(s, x + 5, y + 3, x + 8, y + 4, P_TEXT);
    }
    hg_dirty = 1;
}

static void hg_reframe_all(void)
{
    struct HWin *hw;
    for (hw = hg_top; hw; hw = hw->below) hw_draw_frame(hw);
    hg_bar_redraw();
}

/* ------------------------------------------------------------------ */
/* Compose + present                                                    */
/* ------------------------------------------------------------------ */

static void blit(struct HSurf *s, int dx, int dy)
{
    int x, y, W = hg_base.w, H = hg_base.h;
    for (y = 0; y < s->h; y++) {
        int sy = dy + y;
        const UBYTE *src;
        uint32_t *dst;
        if (sy < 0 || sy >= H) continue;
        src = s->pix + (size_t)y * s->w;
        dst = hg_argb + (size_t)sy * W;
        for (x = 0; x < s->w; x++) {
            int sx = dx + x;
            if (sx >= 0 && sx < W) dst[sx] = hg_pal[src[x]];
        }
    }
}

static void compose_windows(struct HWin *hw)
{
    if (!hw) return;
    compose_windows(hw->below);
    blit(&hw->surf, hw->w.LeftEdge, hw->w.TopEdge);
}

static void hg_compose(void)
{
    struct HSurf *ms; int mx, my;
    if (!hg_argb) return;
    blit(&hg_base, 0, 0);
    compose_windows(hg_top);
    if (tr_mode == TR_SIZE && hg_top) {       /* rubber band while sizing */
        int x0 = hg_top->w.LeftEdge, y0 = hg_top->w.TopEdge;
        int x1 = x0 + tr_w - 1, y1 = y0 + tr_h - 1, x, y;
        for (x = x0; x <= x1; x++) for (y = y0; y <= y1; y += (y1 - y0 ? y1 - y0 : 1)) {
            if (x >= 0 && x < hg_base.w && y >= 0 && y < hg_base.h)
                hg_argb[(size_t)y * hg_base.w + x] ^= 0x00FFFFFF;
        }
        for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x += (x1 - x0 ? x1 - x0 : 1)) {
            if (x >= 0 && x < hg_base.w && y >= 0 && y < hg_base.h)
                hg_argb[(size_t)y * hg_base.w + x] ^= 0x00FFFFFF;
        }
    }
    if (hmenu_overlay(&ms, &mx, &my)) blit(ms, mx, my);
}

static void hg_present(void)
{
    hg_compose();
    hg_dirty = 0;
    hg_last_present = SDL_GetTicks();
    if (hg_headless || !sdl_ren) return;
    SDL_UpdateTexture(sdl_tex, NULL, hg_argb, hg_base.w * 4);
    SDL_RenderClear(sdl_ren);
    SDL_RenderCopy(sdl_ren, sdl_tex, NULL, NULL);
    SDL_RenderPresent(sdl_ren);
}

static void hg_shot(const char *path)
{
    FILE *f;
    int i, n = hg_base.w * hg_base.h;
    hg_compose();
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "gui-script: cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", hg_base.w, hg_base.h);
    for (i = 0; i < n; i++) {
        unsigned char px[3] = { (unsigned char)(hg_argb[i] >> 16),
                                (unsigned char)(hg_argb[i] >> 8),
                                (unsigned char)hg_argb[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

static BOOL hg_set_size(int W, int H)
{
    UBYTE *np = (UBYTE *)calloc((size_t)W * H, 1);
    uint32_t *na = (uint32_t *)malloc((size_t)W * H * 4);
    if (!np || !na) { free(np); free(na); return FALSE; }
    free(hg_base.pix); free(hg_argb);
    hg_base.pix = np; hg_base.w = W; hg_base.h = H;
    hg_argb = na;
    hg_scr.Width = (WORD)W; hg_scr.Height = (WORD)H;
    if (sdl_ren) {
        if (sdl_tex) SDL_DestroyTexture(sdl_tex);
        sdl_tex = SDL_CreateTexture(sdl_ren, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, W, H);
    }
    hg_bar_redraw();
    return TRUE;
}

BOOL hg_ensure(void)
{
    int W = 800, H = 600;
    const char *e;

    if (hg_ready) return TRUE;

    hg_headless = getenv("AMIPART_GUI_HEADLESS") != NULL;
    if ((e = getenv("AMIPART_GUI_SIZE")) != NULL) sscanf(e, "%dx%d", &W, &H);
    if (W < 640) W = 640;
    if (H < 400) H = 400;
    if ((e = getenv("AMIPART_GUI_SCRIPT")) != NULL) {
        hg_script = fopen(e, "r");
        if (!hg_script) fprintf(stderr, "gui-script: cannot open %s\n", e);
    }

    pal_set(0, 0xAAAAAA); pal_set(1, 0x000000); pal_set(2, 0xFFFFFF); pal_set(3, 0x6688BB);

    hg_scr.BarHeight = HG_FH + 2;
    hg_scr.WBorTop = 2; hg_scr.WBorLeft = 4; hg_scr.WBorRight = 4; hg_scr.WBorBottom = 2;
    hg_scr.Font = &hg_ta;
    hg_scr.ViewPort.ColorMap = &hg_cm;
    InitRastPort(&hg_scr.RastPort);
    hg_dri.dri_Version = 2; hg_dri.dri_NumPens = NUMDRIPENS;
    hg_dri.dri_Pens = hg_pens; hg_dri.dri_Font = &hg_tf;

    if (!hg_headless) {
        SDL_DisplayMode dm;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            fprintf(stderr, "AmiPart: cannot open a display (%s).\n"
                            "Use the command-line mode instead: amipart-gui ?\n", SDL_GetError());
            return FALSE;
        }
        if ((e = getenv("AMIPART_GUI_SCALE")) != NULL) hg_scale = atoi(e);
        else if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.h >= 1500) hg_scale = 2;
        if (hg_scale < 1 || hg_scale > 4) hg_scale = 1;
        sdl_win = SDL_CreateWindow("AmiPart", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   W * hg_scale, H * hg_scale, SDL_WINDOW_RESIZABLE);
        if (!sdl_win) { fprintf(stderr, "AmiPart: %s\n", SDL_GetError()); return FALSE; }
        SDL_SetWindowMinimumSize(sdl_win, 640 * hg_scale, 400 * hg_scale);
        sdl_ren = SDL_CreateRenderer(sdl_win, -1, 0);
        if (!sdl_ren) sdl_ren = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_SOFTWARE);
        if (!sdl_ren) { fprintf(stderr, "AmiPart: %s\n", SDL_GetError()); return FALSE; }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
        cur_size  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
        SDL_StartTextInput();
    } else {
        SDL_Init(SDL_INIT_TIMER);
    }
    hg_ready = 1;
    if (!hg_set_size(W, H)) return FALSE;
    return TRUE;
}

BOOL hostgui_init(void) { return hg_ensure(); }
void hostgui_quit(void)
{
    if (!hg_ready) return;
    if (sdl_tex) SDL_DestroyTexture(sdl_tex);
    if (sdl_ren) SDL_DestroyRenderer(sdl_ren);
    if (sdl_win) SDL_DestroyWindow(sdl_win);
    SDL_Quit();
    hg_ready = 0;
}

/* ------------------------------------------------------------------ */
/* Messages                                                             */
/* ------------------------------------------------------------------ */

void hw_post(struct HWin *hw, ULONG cls, UWORD code, UWORD qual, APTR iaddr)
{
    struct HMsg *m;
    struct timeval tv;
    if (!hw || !(hw->w.IDCMPFlags & cls)) return;
    /* fold a run of mouse moves into the newest one */
    if (cls == IDCMP_MOUSEMOVE && hw->qtail && hw->qtail->im.Class == IDCMP_MOUSEMOVE) {
        hw->qtail->im.MouseX = hw->w.MouseX;
        hw->qtail->im.MouseY = hw->w.MouseY;
        return;
    }
    m = (struct HMsg *)calloc(1, sizeof(*m));
    if (!m) return;
    gettimeofday(&tv, NULL);
    m->im.Class = cls; m->im.Code = code; m->im.Qualifier = qual; m->im.IAddress = iaddr;
    m->im.MouseX = hw->w.MouseX; m->im.MouseY = hw->w.MouseY;
    m->im.Seconds = (ULONG)tv.tv_sec; m->im.Micros = (ULONG)tv.tv_usec;
    m->im.IDCMPWindow = &hw->w;
    if (hw->qtail) hw->qtail->next = m; else hw->qhead = m;
    hw->qtail = m;
}

struct HWin *hg_win_of_port(struct MsgPort *port)
{
    struct HWin *hw;
    for (hw = hg_top; hw; hw = hw->below)
        if (&hw->port == port) return hw;
    return NULL;
}

struct Message *WaitPort(struct MsgPort *port)
{
    struct HWin *hw = hg_win_of_port(port);
    if (!hw) return NULL;
    while (!hw->qhead) hg_pump(1, hw);
    return &hw->qhead->im.ExecMessage;
}

struct IntuiMessage *GT_GetIMsg(struct MsgPort *port)
{
    struct HWin *hw = hg_win_of_port(port);
    struct HMsg *m;
    if (!hw) return NULL;
    if (!hw->qhead) hg_pump(0, hw);
    m = hw->qhead;
    if (!m) return NULL;
    hw->qhead = m->next;
    if (!hw->qhead) hw->qtail = NULL;
    return &m->im;
}

void GT_ReplyIMsg(struct IntuiMessage *msg) { free(msg); }

/* ------------------------------------------------------------------ */
/* Windows                                                              */
/* ------------------------------------------------------------------ */

struct Screen *LockPubScreen(CONST_STRPTR name)
{
    (void)name;
    return hg_ensure() ? &hg_scr : NULL;
}
void UnlockPubScreen(CONST_STRPTR name, struct Screen *scr) { (void)name; (void)scr; }

static BOOL hw_alloc_surf(struct HWin *hw, int W, int H)
{
    UBYTE *np = (UBYTE *)calloc((size_t)W * H, 1);
    if (!np) return FALSE;
    if (hw->surf.pix) {
        /* keep what was inside the old borders */
        int y, ow = hw->surf.w, oh = hw->surf.h;
        int il = hw->w.BorderLeft, it = hw->w.BorderTop;
        int ir = ow - hw->w.BorderRight, ib = oh - hw->w.BorderBottom;
        if (ir > W) ir = W;
        if (ib > H) ib = H;
        for (y = it; y < ib; y++)
            if (ir > il) memcpy(np + (size_t)y * W + il, hw->surf.pix + (size_t)y * ow + il,
                                (size_t)(ir - il));
        free(hw->surf.pix);
    }
    hw->surf.pix = np; hw->surf.w = W; hw->surf.h = H;
    hw->w.Width = (WORD)W; hw->w.Height = (WORD)H;
    return TRUE;
}

static void hw_clamp_pos(struct HWin *hw)
{
    struct Window *w = &hw->w;
    if (w->LeftEdge + w->Width > hg_scr.Width)  w->LeftEdge = (WORD)(hg_scr.Width - w->Width);
    if (w->TopEdge + w->Height > hg_scr.Height) w->TopEdge = (WORD)(hg_scr.Height - w->Height);
    if (w->LeftEdge < 0) w->LeftEdge = 0;
    if (w->TopEdge < BAR_H) w->TopEdge = BAR_H;
}

struct Window *OpenWindowTagList(APTR nw, struct TagItem *tags)
{
    struct HWin *hw;
    struct TagItem *t;
    int W = 200, H = 100, L = 0, T = 0;
    struct Gadget *gl = NULL;
    const char *title = NULL;

    (void)nw;
    if (!hg_ensure()) return NULL;
    hw = (struct HWin *)calloc(1, sizeof(*hw));
    if (!hw) return NULL;
    hw->w.MinWidth = 50; hw->w.MinHeight = 30;
    hw->w.MaxWidth = (UWORD)~0; hw->w.MaxHeight = (UWORD)~0;

    for (t = tags; t && t->ti_Tag != TAG_DONE; t++) {
        switch (t->ti_Tag) {
        case WA_Left:      L = (int)(LONG)t->ti_Data; break;
        case WA_Top:       T = (int)(LONG)t->ti_Data; break;
        case WA_Width:     W = (int)t->ti_Data; break;
        case WA_Height:    H = (int)t->ti_Data; break;
        case WA_IDCMP:     hw->w.IDCMPFlags = (ULONG)t->ti_Data; break;
        case WA_Flags:     hw->w.Flags = (ULONG)t->ti_Data; break;
        case WA_Gadgets:   gl = (struct Gadget *)t->ti_Data; break;
        case WA_Title:     title = (const char *)t->ti_Data; break;
        case WA_MinWidth:  hw->w.MinWidth  = (WORD)t->ti_Data; break;
        case WA_MinHeight: hw->w.MinHeight = (WORD)t->ti_Data; break;
        case WA_MaxWidth:  hw->w.MaxWidth  = (UWORD)t->ti_Data; break;
        case WA_MaxHeight: hw->w.MaxHeight = (UWORD)t->ti_Data; break;
        default: break;
        }
    }
    if (W > hg_scr.Width)  W = hg_scr.Width;
    if (H > hg_scr.Height - BAR_H) H = hg_scr.Height - BAR_H;
    if (W < 20) W = 20;
    if (H < 20) H = 20;

    if (title) { strncpy(hw->title, title, sizeof(hw->title) - 1); hw->w.Title = (UBYTE *)hw->title; }
    if (!(hw->w.Flags & WFLG_BORDERLESS)) {
        int bar = title || (hw->w.Flags & (WFLG_DRAGBAR | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET));
        hw->w.BorderTop    = (BYTE)(bar ? hg_scr.WBorTop + HG_FH + 1 : hg_scr.WBorTop);
        hw->w.BorderLeft   = hg_scr.WBorLeft;
        hw->w.BorderRight  = hg_scr.WBorRight;
        hw->w.BorderBottom = hg_scr.WBorBottom;
        if (hw->w.Flags & WFLG_SIZEGADGET) {
            if (hw->w.Flags & WFLG_SIZEBBOTTOM) hw->w.BorderBottom = 10;
            else                                hw->w.BorderRight = 18;
        }
    }
    hw->w.LeftEdge = (WORD)L; hw->w.TopEdge = (WORD)T;
    hw->w.WScreen = &hg_scr;
    hw->w.UserPort = &hw->port;
    hw->w.RPort = &hw->rp;
    if (!hw_alloc_surf(hw, W, H)) { free(hw); return NULL; }
    hw_clamp_pos(hw);
    InitRastPort(&hw->rp);
    hw->rp.hg_surf = &hw->surf;

    hmenu_close();
    hw->below = hg_top;
    hg_top = hw;
    tr_mode = TR_NONE; tr_gad = NULL;
    hw->w.MouseX = (WORD)(hg_mx - hw->w.LeftEdge);
    hw->w.MouseY = (WORD)(hg_my - hw->w.TopEdge);
    hg_reframe_all();
    if (gl) { AddGList(&hw->w, gl, ~0, -1, NULL); hgad_draw_all(hw); }
    return &hw->w;
}

void CloseWindow(struct Window *win)
{
    struct HWin *hw = (struct HWin *)win, **pp;
    struct HMsg *m, *n;
    if (!hw) return;
    hmenu_close();
    hgad_window_closing(hw);
    for (pp = &hg_top; *pp; pp = &(*pp)->below)
        if (*pp == hw) { *pp = hw->below; break; }
    for (m = hw->qhead; m; m = n) { n = m->next; free(m); }
    free(hw->surf.pix);
    free(hw);
    tr_mode = TR_NONE; tr_gad = NULL;
    if (sdl_win && cur_arrow) SDL_SetCursor(cur_arrow);
    hg_reframe_all();
}

void SetWindowTitles(struct Window *win, UBYTE *wtitle, UBYTE *stitle)
{
    struct HWin *hw = (struct HWin *)win;
    if (!hw) return;
    if (wtitle != (UBYTE *)(IPTR)-1 && wtitle != (UBYTE *)(IPTR)0xFFFFFFFFu) {
        hw->title[0] = '\0';
        if (wtitle) strncpy(hw->title, (const char *)wtitle, sizeof(hw->title) - 1);
        hw_draw_frame(hw);
    }
    if (stitle != (UBYTE *)(IPTR)-1 && stitle != (UBYTE *)(IPTR)0xFFFFFFFFu && stitle) {
        strncpy(hg_scr_title, (const char *)stitle, sizeof(hg_scr_title) - 1);
        hg_bar_redraw();
    }
}

/* As Intuition: 0 leaves a limit alone, and a limit the current window
   size already violates is not set (the call then returns FALSE). */
BOOL WindowLimits(struct Window *win, LONG minw, LONG minh, ULONG maxw, ULONG maxh)
{
    BOOL ok = TRUE;
    if (!win) return FALSE;
    if (minw) { if (minw <= win->Width)  win->MinWidth  = (WORD)minw; else ok = FALSE; }
    if (minh) { if (minh <= win->Height) win->MinHeight = (WORD)minh; else ok = FALSE; }
    if (maxw) { if (maxw >= (ULONG)win->Width)  win->MaxWidth  = (UWORD)maxw; else ok = FALSE; }
    if (maxh) { if (maxh >= (ULONG)win->Height) win->MaxHeight = (UWORD)maxh; else ok = FALSE; }
    return ok;
}

void SetPointer(struct Window *win, UWORD *data, LONG h, LONG w, LONG xo, LONG yo)
{
    (void)win; (void)data; (void)h; (void)w; (void)xo; (void)yo;
    if (sdl_win && cur_size) SDL_SetCursor(cur_size);
}
void ClearPointer(struct Window *win)
{
    (void)win;
    if (sdl_win && cur_arrow) SDL_SetCursor(cur_arrow);
}

static void hw_resize(struct HWin *hw, int W, int H)
{
    if (W < hw->w.MinWidth)  W = hw->w.MinWidth;
    if (H < hw->w.MinHeight) H = hw->w.MinHeight;
    if (W > (int)hw->w.MaxWidth)  W = hw->w.MaxWidth;
    if (H > (int)hw->w.MaxHeight) H = hw->w.MaxHeight;
    if (hw->w.LeftEdge + W > hg_scr.Width)  W = hg_scr.Width - hw->w.LeftEdge;
    if (hw->w.TopEdge + H > hg_scr.Height) H = hg_scr.Height - hw->w.TopEdge;
    if (W == hw->w.Width && H == hw->w.Height) return;
    if (!hw_alloc_surf(hw, W, H)) return;
    hw_draw_frame(hw);
    hgad_draw_all(hw);
    hw_post(hw, IDCMP_NEWSIZE, 0, 0, &hw->w);
    hw_post(hw, IDCMP_REFRESHWINDOW, 0, 0, &hw->w);
}

/* ------------------------------------------------------------------ */
/* Input                                                                */
/* ------------------------------------------------------------------ */

static UWORD hg_qual(void)
{
    return (UWORD)((hg_shift ? IEQUALIFIER_LSHIFT : 0) | (hg_ctrl ? IEQUALIFIER_CONTROL : 0));
}

static void hg_mouse_move(int x, int y)
{
    struct HWin *hw = hg_top;
    hg_mx = x; hg_my = y;
    if (hmenu_is_open()) { hmenu_motion(x, y); return; }
    if (!hw) return;
    switch (tr_mode) {
    case TR_DRAG:
        hw->w.LeftEdge = (WORD)(x - tr_dx);
        hw->w.TopEdge  = (WORD)(y - tr_dy);
        if (hw->w.LeftEdge < 0) hw->w.LeftEdge = 0;
        if (hw->w.TopEdge < BAR_H) hw->w.TopEdge = BAR_H;
        if (hw->w.LeftEdge > hg_scr.Width - 40)  hw->w.LeftEdge = (WORD)(hg_scr.Width - 40);
        if (hw->w.TopEdge > hg_scr.Height - 16)  hw->w.TopEdge = (WORD)(hg_scr.Height - 16);
        hg_dirty = 1;
        return;
    case TR_SIZE:
        tr_w = x - hw->w.LeftEdge + tr_dx;
        tr_h = y - hw->w.TopEdge + tr_dy;
        if (tr_w < hw->w.MinWidth)  tr_w = hw->w.MinWidth;
        if (tr_h < hw->w.MinHeight) tr_h = hw->w.MinHeight;
        if (tr_w > (int)hw->w.MaxWidth)  tr_w = hw->w.MaxWidth;
        if (tr_h > (int)hw->w.MaxHeight) tr_h = hw->w.MaxHeight;
        hg_dirty = 1;
        return;
    default: break;
    }
    hw->w.MouseX = (WORD)(x - hw->w.LeftEdge);
    hw->w.MouseY = (WORD)(y - hw->w.TopEdge);
    if (tr_mode == TR_GAD && tr_gad) { hgad_mouse_move(tr_gad, hw->w.MouseX, hw->w.MouseY); return; }
    if (tr_mode == TR_CLOSE) return;
    hw_post(hw, IDCMP_MOUSEMOVE, 0, hg_qual(), &hw->w);
}

static void hg_mouse_down(int x, int y, int right)
{
    struct HWin *hw = hg_top;
    int wx, wy;

    hg_mouse_move(x, y);
    if (y < BAR_H) {
        int idx = (hw && hw->w.MenuStrip) ? hmenu_bar_hit(x) : -1;
        if (idx >= 0) hmenu_open(idx); else hmenu_close();
        return;
    }
    if (hmenu_is_open()) { if (!hmenu_release(x, y)) hmenu_close(); return; }
    if (right || !hw) return;

    wx = x - hw->w.LeftEdge; wy = y - hw->w.TopEdge;
    if (wx < 0 || wy < 0 || wx >= hw->w.Width || wy >= hw->w.Height) return;   /* modal */

    if ((hw->w.Flags & WFLG_SIZEGADGET) && wx >= hw->w.Width - 18 && wy >= hw->w.Height - 10) {
        tr_mode = TR_SIZE;
        tr_dx = hw->w.Width - wx; tr_dy = hw->w.Height - wy;
        tr_w = hw->w.Width; tr_h = hw->w.Height;
        return;
    }
    if (wy < hw->w.BorderTop && hw->w.BorderTop > 4) {
        if (wx < frame_close_w(hw)) { tr_mode = TR_CLOSE; return; }
        if (wx >= hw->w.Width - frame_depth_w(hw)) return;
        if (hw->w.Flags & WFLG_DRAGBAR) { tr_mode = TR_DRAG; tr_dx = wx; tr_dy = wy; }
        return;
    }
    {
        struct HGad *g = hgad_hit(hw, wx, wy);
        if (g) {
            tr_mode = TR_GAD; tr_gad = g;
            hgad_mouse_down(g, wx, wy, hg_shift);
            return;
        }
    }
    hgad_blur(hw);
    hw_post(hw, IDCMP_MOUSEBUTTONS, SELECTDOWN, hg_qual(), &hw->w);
}

static void hg_mouse_up(int x, int y, int right)
{
    struct HWin *hw = hg_top;
    int mode = tr_mode;

    hg_mx = x; hg_my = y;
    if (hmenu_is_open()) { hmenu_release(x, y); return; }
    if (right || !hw) return;
    tr_mode = TR_NONE;
    switch (mode) {
    case TR_DRAG: return;
    case TR_SIZE: hg_dirty = 1; hw_resize(hw, tr_w, tr_h); return;
    case TR_CLOSE: {
        int wx = x - hw->w.LeftEdge, wy = y - hw->w.TopEdge;
        if (wx >= 0 && wx < frame_close_w(hw) && wy >= 0 && wy < hw->w.BorderTop)
            hw_post(hw, IDCMP_CLOSEWINDOW, 0, 0, &hw->w);
        return; }
    case TR_GAD: {
        struct HGad *g = tr_gad;
        tr_gad = NULL;
        hw->w.MouseX = (WORD)(x - hw->w.LeftEdge);
        hw->w.MouseY = (WORD)(y - hw->w.TopEdge);
        if (g) hgad_mouse_up(g, hw->w.MouseX, hw->w.MouseY);
        return; }
    default: break;
    }
    hw->w.MouseX = (WORD)(x - hw->w.LeftEdge);
    hw->w.MouseY = (WORD)(y - hw->w.TopEdge);
    hw_post(hw, IDCMP_MOUSEBUTTONS, SELECTUP, hg_qual(), &hw->w);
}

/* code: Latin-1 / control char, or an HK_* value */
static void hg_key(int code)
{
    struct HWin *hw = hg_top;
    if (!hw) return;
    if (hmenu_is_open()) { if (code == 27) hmenu_close(); return; }
    if (hgad_key(hw, code, hg_ctrl, hg_shift)) return;
    if (code < 256) hw_post(hw, IDCMP_VANILLAKEY, (UWORD)code, hg_qual(), &hw->w);
}

static void hg_char(int ch)
{
    struct HWin *hw = hg_top;
    if (!hw || hmenu_is_open()) return;
    if (hw->active) { hgad_text_input(hw, ch); return; }
    hw_post(hw, IDCMP_VANILLAKEY, (UWORD)ch, hg_qual(), &hw->w);
}

static void hg_utf8_input(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int ch;
        if (*p < 0x80) ch = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { ch = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else { p++; while ((*p & 0xC0) == 0x80) p++; ch = '?'; }
        if (ch >= 0x20 && ch < 0x100 && ch != 0x7F) hg_char(ch);
    }
}

/* ------------------------------------------------------------------ */
/* Test script                                                          */
/* ------------------------------------------------------------------ */

static void hg_dump(void)
{
    struct HWin *hw = hg_top;
    struct Gadget *g;
    if (!hw) { printf("DUMP no window\n"); return; }
    printf("DUMP window \"%s\" at %d,%d size %dx%d\n", hw->title,
           hw->w.LeftEdge, hw->w.TopEdge, hw->w.Width, hw->w.Height);
    if (hw->note) {
        const char *p = hw->note;
        while (*p) {
            const char *q = strchr(p, '\n');
            int n = q ? (int)(q - p) : (int)strlen(p);
            printf("  | %.*s\n", n, p);
            p += n; if (*p) p++;
        }
    }
    for (g = hw->w.FirstGadget; g; g = g->NextGadget) {
        struct HGad *hg = (struct HGad *)g;
        if (hg->kind < 0) continue;
        printf("  gad id=%d %s \"%s\" at %d,%d %dx%d%s", g->GadgetID, hgad_kind_name(hg->kind),
               hg->text ? hg->text : "", g->LeftEdge, g->TopEdge, g->Width, g->Height,
               (g->Flags & GFLG_DISABLED) ? " DISABLED" : "");
        if (hg->kind == STRING_KIND)   printf(" value=\"%s\"", (char *)hg->si.Buffer);
        if (hg->kind == CHECKBOX_KIND) printf(" checked=%d", (g->Flags & GFLG_SELECTED) ? 1 : 0);
        if (hg->kind == CYCLE_KIND && hg->cy_labels && hg->cy_count)
            printf(" active=\"%s\"", hg->cy_labels[hg->cy_active]);
        if (hg->kind == TEXT_KIND)     printf(" value=\"%s\"", hg->tx_text ? hg->tx_text : "");
        printf("\n");
        if (hg->kind == LISTVIEW_KIND && hg->labels && hg->labels != HG_DETACHED) {
            struct Node *n; int i = 0;
            for (n = hg->labels->lh_Head; n && n->ln_Succ; n = n->ln_Succ, i++)
                printf("    %c[%d] %s\n", i == hg->sel ? '*' : ' ', i, n->ln_Name ? n->ln_Name : "");
        }
    }
    fflush(stdout);
}

static struct HGad *script_find_gad(const char *text, int id)
{
    struct Gadget *g;
    if (!hg_top) return NULL;
    for (g = hg_top->w.FirstGadget; g; g = g->NextGadget) {
        struct HGad *hg = (struct HGad *)g;
        if (hg->kind < 0) continue;
        if (text ? (hg->text && strcmp(hg->text, text) == 0) : (g->GadgetID == id)) return hg;
    }
    return NULL;
}

static void script_click(int x, int y)
{
    hg_mouse_down(x, y, 0);
    hg_mouse_up(x, y, 0);
}

/* One script line.  Returns FALSE when the script has ended. */
static BOOL hg_script_step(void)
{
    char line[512], *a;
    int x, y, n;

    if (!hg_script) return FALSE;
    if (!fgets(line, sizeof(line), hg_script)) {
        fclose(hg_script); hg_script = NULL;
        if (hg_headless) { printf("gui-script: end of script\n"); exit(0); }
        return FALSE;
    }
    line[strcspn(line, "\r\n")] = '\0';
    a = strchr(line, ' ');
    if (a) *a++ = '\0'; else a = line + strlen(line);

    if (!line[0] || line[0] == '#') return TRUE;
    if (!strcmp(line, "move") && sscanf(a, "%d %d", &x, &y) == 2) hg_mouse_move(x, y);
    else if (!strcmp(line, "down") && sscanf(a, "%d %d", &x, &y) == 2) hg_mouse_down(x, y, 0);
    else if (!strcmp(line, "up") && sscanf(a, "%d %d", &x, &y) == 2) hg_mouse_up(x, y, 0);
    else if (!strcmp(line, "click") && sscanf(a, "%d %d", &x, &y) == 2) script_click(x, y);
    else if (!strcmp(line, "key") && sscanf(a, "%d", &n) == 1) hg_key(n);
    else if (!strcmp(line, "enter")) hg_key(13);
    else if (!strcmp(line, "esc")) hg_key(27);
    else if (!strcmp(line, "type")) { for (; *a; a++) hg_char((unsigned char)*a); }
    else if (!strcmp(line, "shot")) hg_shot(a);
    else if (!strcmp(line, "dump")) hg_dump();
    else if (!strcmp(line, "menu") && sscanf(a, "%d %d", &x, &y) == 2) hmenu_pick(x, y);
    else if (!strcmp(line, "quit")) { printf("gui-script: quit\n"); exit(0); }
    else if (!strcmp(line, "gad") || !strcmp(line, "gadid")) {
        struct HGad *g = !strcmp(line, "gad") ? script_find_gad(a, 0) : script_find_gad(NULL, atoi(a));
        if (!g || !hg_top) { printf("gui-script: no gadget %s\n", a); hg_dump(); exit(2); }
        script_click(hg_top->w.LeftEdge + g->g.LeftEdge + g->g.Width / 2,
                     hg_top->w.TopEdge + g->g.TopEdge + (g->g.Height > 12 ? 6 : g->g.Height / 2));
    }
    else if (!strcmp(line, "lvrow")) {            /* lvrow <gadget id> <visible row> */
        struct HGad *g; int id, row;
        if (sscanf(a, "%d %d", &id, &row) != 2 || !(g = script_find_gad(NULL, id)) || !hg_top) {
            printf("gui-script: bad lvrow %s\n", a); exit(2);
        }
        script_click(hg_top->w.LeftEdge + g->g.LeftEdge + 12,
                     hg_top->w.TopEdge + g->g.TopEdge + 2 + row * HG_LV_ROWH + HG_LV_ROWH / 2);
    }
    else if (!strcmp(line, "set")) {              /* set <gadget id> <text>: fill a string gadget */
        struct HGad *g; char *v = strchr(a, ' ');
        if (v) *v++ = '\0'; else v = a + strlen(a);
        g = script_find_gad(NULL, atoi(a));
        if (!g || g->kind != STRING_KIND) { printf("gui-script: no string gadget %s\n", a); exit(2); }
        strncpy((char *)g->si.Buffer, v, (size_t)g->maxchars);
        g->si.Buffer[g->maxchars] = '\0';
        g->si.NumChars = g->si.BufferPos = (WORD)strlen((char *)g->si.Buffer);
        hgad_draw(g);
        if (g->win) hw_post(g->win, IDCMP_GADGETUP, 0, 0, &g->g);   /* as if Return was pressed */
    }
    else if (!strcmp(line, "expect")) {           /* expect <text in the window title or body> */
        struct HWin *hw = hg_top;
        if (!hw || (!strstr(hw->title, a) && !(hw->note && strstr(hw->note, a)))) {
            printf("gui-script: EXPECT FAILED: %s\n", a); hg_dump(); exit(3);
        }
    }
    else { printf("gui-script: bad line: %s %s\n", line, a); exit(2); }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Event pump                                                           */
/* ------------------------------------------------------------------ */

static void hg_sdl_event(SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT:
        /* The desktop's close button: close the top window if it can be
           closed, otherwise answer it like Esc (requesters cancel). */
        if (hg_top) {
            if (hg_top->w.IDCMPFlags & IDCMP_CLOSEWINDOW)
                hw_post(hg_top, IDCMP_CLOSEWINDOW, 0, 0, &hg_top->w);
            else
                hg_key(27);
        }
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            int W = e->window.data1 / hg_scale, H = e->window.data2 / hg_scale;
            struct HWin *hw;
            if (W < 640) W = 640;
            if (H < 400) H = 400;
            if (W != hg_scr.Width || H != hg_scr.Height) {
                hg_set_size(W, H);
                for (hw = hg_top; hw; hw = hw->below) hw_clamp_pos(hw);
            }
        }
        hg_dirty = 1;
        break;
    case SDL_MOUSEMOTION:
        hg_mouse_move(e->motion.x / hg_scale, e->motion.y / hg_scale);
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT || e->button.button == SDL_BUTTON_RIGHT)
            hg_mouse_down(e->button.x / hg_scale, e->button.y / hg_scale,
                          e->button.button == SDL_BUTTON_RIGHT);
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT || e->button.button == SDL_BUTTON_RIGHT)
            hg_mouse_up(e->button.x / hg_scale, e->button.y / hg_scale,
                        e->button.button == SDL_BUTTON_RIGHT);
        break;
    case SDL_MOUSEWHEEL:
        if (hg_top && !hmenu_is_open())
            hgad_wheel(hg_top, hg_mx - hg_top->w.LeftEdge, hg_my - hg_top->w.TopEdge, e->wheel.y);
        break;
    case SDL_TEXTINPUT:
        if (!hg_ctrl) hg_utf8_input(e->text.text);
        break;
    case SDL_KEYDOWN: {
        SDL_Keycode k = e->key.keysym.sym;
        Uint16 mod = e->key.keysym.mod;
        hg_shift = (mod & KMOD_SHIFT) != 0;
        hg_ctrl  = (mod & KMOD_CTRL) != 0;
        switch (k) {
        case SDLK_RETURN: case SDLK_KP_ENTER: hg_key(13); break;
        case SDLK_ESCAPE:    hg_key(27); break;
        case SDLK_BACKSPACE: hg_key(8); break;
        case SDLK_TAB:       hg_key(9); break;
        case SDLK_DELETE:    hg_key(127); break;
        case SDLK_LEFT:      hg_key(HK_LEFT); break;
        case SDLK_RIGHT:     hg_key(HK_RIGHT); break;
        case SDLK_HOME:      hg_key(HK_HOME); break;
        case SDLK_END:       hg_key(HK_END); break;
        case SDLK_UP:        hg_key(HK_UP); break;
        case SDLK_DOWN:      hg_key(HK_DOWN); break;
        case SDLK_PAGEUP:    hg_key(HK_PGUP); break;
        case SDLK_PAGEDOWN:  hg_key(HK_PGDN); break;
        default:
            if (hg_ctrl && k >= 'a' && k <= 'z') {
                if (k == 'v' && hg_top && hg_top->active) {
                    char *clip = SDL_GetClipboardText();
                    if (clip) { hg_utf8_input(clip); SDL_free(clip); }
                } else if (!hmenu_is_open()) {
                    if (!(hg_top && hg_top->active && hgad_key(hg_top, k, 1, hg_shift)))
                        hmenu_shortcut(k);
                }
            }
            break;
        }
        break; }
    case SDL_KEYUP:
        hg_shift = (e->key.keysym.mod & KMOD_SHIFT) != 0;
        hg_ctrl  = (e->key.keysym.mod & KMOD_CTRL) != 0;
        break;
    default: break;
    }
}

/* block = 0: handle what is pending and return (progress loops).
   block = 1: do not return before something happened; called in a loop
   by WaitPort until the window has a message. */
void hg_pump(int block, struct HWin *waitfor)
{
    SDL_Event e;
    int got = 0;

    if (!hg_ready) return;
    if (!hg_headless) {
        while (SDL_PollEvent(&e)) { hg_sdl_event(&e); got = 1; }
    }
    if (block && waitfor && waitfor->qhead) return;

    if (block && !got) {
        if (hg_script) {
            hg_script_step();
        } else if (hg_headless) {
            fprintf(stderr, "gui: headless and nothing left to do - exiting\n");
            exit(4);
        } else {
            if (hg_dirty) hg_present();
            if (SDL_WaitEventTimeout(&e, 100)) hg_sdl_event(&e);
        }
    }
    if (hg_dirty && (block || SDL_GetTicks() - hg_last_present >= 40)) hg_present();
}
