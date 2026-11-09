/*
 * mapview.c - zoom/pan viewport for the partition view's disk map.
 * See mapview.h.  Only unsigned 64-bit division is used (the Amiga build's
 * gcc_runtime.c provides __udivdi3 but not the signed variant).
 */

#include "mapview.h"

#ifndef UQUAD
typedef unsigned long long UQUAD;
#endif

/* Pixel offsets are saturated here so far-off-screen cylinders at high zoom
   stay comfortably inside a LONG (and well outside any real map). */
#define MV_PX_LIMIT 0x00100000UL

void MapView_Init(struct MapView *v, ULONG total, LONG mx, ULONG mw,
                  ULONG lo, ULONG span)
{
    v->total = total;
    v->mx    = mx;
    v->mw    = mw;
    v->lo    = lo;
    v->span  = span;
    MapView_Clamp(v);
}

ULONG MapView_MinSpan(ULONG total, ULONG mw)
{
    ULONG min = mw / 4;
    if (min < 4) min = 4;
    if (total == 0) total = 1;
    if (min > total) min = total;
    return min;
}

void MapView_Clamp(struct MapView *v)
{
    ULONG min;
    if (v->total == 0) v->total = 1;
    if (v->mw == 0) v->mw = 1;
    min = MapView_MinSpan(v->total, v->mw);
    if (v->span == 0 || v->span > v->total) v->span = v->total;
    if (v->span < min) v->span = min;
    if (v->lo > v->total - v->span) v->lo = v->total - v->span;
}

BOOL MapView_IsZoomed(const struct MapView *v)
{
    return (BOOL)(v->span < v->total);
}

LONG MapView_X(const struct MapView *v, ULONG cyl)
{
    UQUAD d;
    if (cyl >= v->lo) {
        d = (UQUAD)(cyl - v->lo) * v->mw / v->span;
        if (d > MV_PX_LIMIT) d = MV_PX_LIMIT;
        return v->mx + (LONG)d;
    }
    /* Left of the view: round away from the view so x stays monotonic
       (floor of a negative offset). */
    d = ((UQUAD)(v->lo - cyl) * v->mw + v->span - 1) / v->span;
    if (d > MV_PX_LIMIT) d = MV_PX_LIMIT;
    return v->mx - (LONG)d;
}

WORD MapView_ClipX(const struct MapView *v, ULONG cyl)
{
    LONG x = MapView_X(v, cyl);
    if (x < v->mx) x = v->mx;
    if (x > v->mx + (LONG)v->mw) x = v->mx + (LONG)v->mw;
    return (WORD)x;
}

ULONG MapView_Cyl(const struct MapView *v, LONG x)
{
    LONG  dx = x - v->mx;
    ULONG cyl;
    if (dx < 0) dx = 0;
    if ((ULONG)dx >= v->mw) dx = (LONG)v->mw - 1;
    /* Last cylinder drawn starting at or left of this pixel: the largest c
       with MapView_X(c) <= x, so MapView_Cyl(MapView_X(c)) == c whenever a
       cylinder is at least a pixel wide. */
    cyl = v->lo + (ULONG)(((UQUAD)((ULONG)dx + 1) * v->span - 1) / v->mw);
    if (cyl >= v->total) cyl = v->total - 1;
    return cyl;
}

ULONG MapView_DeltaCyl(const struct MapView *v, ULONG dpx)
{
    return (ULONG)((UQUAD)dpx * v->span / v->mw);
}

BOOL MapView_Overlaps(const struct MapView *v, ULONG lo, ULONG hi)
{
    return (BOOL)(hi >= v->lo && lo < v->lo + v->span);
}

static void set_span_keep(struct MapView *v, ULONG new_span,
                          ULONG anchor_cyl, LONG anchor_x)
{
    LONG  dx = anchor_x - v->mx;
    ULONG off;
    if (dx < 0) dx = 0;
    if ((ULONG)dx > v->mw) dx = (LONG)v->mw;
    v->span = new_span;
    MapView_Clamp(v);
    off = (ULONG)((UQUAD)(ULONG)dx * v->span / v->mw);
    v->lo = (anchor_cyl > off) ? anchor_cyl - off : 0;
    MapView_Clamp(v);
}

void MapView_Zoom(struct MapView *v, BOOL zoom_in, ULONG anchor_cyl,
                  LONG anchor_x)
{
    ULONG ns;
    if (zoom_in)
        ns = v->span / 2;
    else
        ns = (v->span > v->total / 2) ? v->total : v->span * 2;
    if (ns == 0) ns = 1;
    set_span_keep(v, ns, anchor_cyl, anchor_x);
}

void MapView_Fit(struct MapView *v)
{
    v->lo   = 0;
    v->span = v->total;
    MapView_Clamp(v);
}

void MapView_ZoomTo(struct MapView *v, ULONG lo, ULONG hi)
{
    ULONG n, c;
    if (hi < lo) { ULONG t = lo; lo = hi; hi = t; }
    n = hi - lo + 1;
    c = lo + n / 2;
    v->span = (n > v->total) ? v->total : n + n / 4;   /* range = ~80% */
    MapView_Clamp(v);
    v->lo = (c > v->span / 2) ? c - v->span / 2 : 0;
    MapView_Clamp(v);
}

void MapView_EnsureVisible(struct MapView *v, ULONG lo, ULONG hi)
{
    ULONG n;
    if (hi < lo) { ULONG t = lo; lo = hi; hi = t; }
    if (lo >= v->lo && hi < v->lo + v->span) return;   /* already shown */
    n = hi - lo + 1;
    if (n < v->span) {
        ULONG c = lo + n / 2;
        v->lo = (c > v->span / 2) ? c - v->span / 2 : 0;
    } else {
        v->lo = lo;
    }
    MapView_Clamp(v);
}

ULONG MapView_ZoomX10(const struct MapView *v)
{
    return (ULONG)((UQUAD)v->total * 10 / v->span);
}

ULONG MapView_ScrollerUnit(const struct MapView *v)
{
    return (v->total + MAPVIEW_SCROLLER_MAX - 1) / MAPVIEW_SCROLLER_MAX;
}

void MapView_ScrollerVals(const struct MapView *v, ULONG *top,
                          ULONG *visible, ULONG *total)
{
    ULONG u = MapView_ScrollerUnit(v);
    if (u == 0) u = 1;
    *total   = (v->total + u - 1) / u;
    *visible = (v->span  + u - 1) / u;
    *top     = v->lo / u;
    if (*visible > *total) *visible = *total;
    if (*top > *total - *visible) *top = *total - *visible;
}

void MapView_SetScrollerTop(struct MapView *v, ULONG top)
{
    ULONG u = MapView_ScrollerUnit(v);
    if (u == 0) u = 1;
    v->lo = top * u;
    MapView_Clamp(v);
}
