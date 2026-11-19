/*
 * mapview.h - zoom/pan viewport for the partition view's disk map.
 *
 * The map used to be one fixed linear bar (x = cyl * width / total), which
 * squeezes small partitions on a large disk down to a couple of pixels.
 * A MapView shows the visible cylinder window [lo, lo+span) across the map
 * width instead; at lo = 0, span = total every conversion below gives the
 * exact same pixels as the old fixed bar.
 *
 * Pure integer math, no GUI calls - the whole map (drawing, hit tests,
 * edge/move/new-partition drags) converts through these functions.
 */
#ifndef MAPVIEW_H
#define MAPVIEW_H

#include <exec/types.h>

struct MapView {
    ULONG total;   /* cylinders on the whole map (>= 1)          */
    ULONG lo;      /* first visible cylinder                     */
    ULONG span;    /* visible cylinder count, 1..total           */
    LONG  mx;      /* map inner left edge, window pixels         */
    ULONG mw;      /* map inner width in pixels (>= 1)           */
};

/* GadTools scrollers take 16-bit values; larger disks are scaled down. */
#define MAPVIEW_SCROLLER_MAX 30000UL

/* Set up a view for total cylinders on a map of mw pixels at mx, using the
   saved lo/span (span 0 = whole disk) and normalise it. */
void  MapView_Init(struct MapView *v, ULONG total, LONG mx, ULONG mw,
                   ULONG lo, ULONG span);
/* Smallest span allowed (maximum zoom: about 4 pixels per cylinder). */
ULONG MapView_MinSpan(ULONG total, ULONG mw);
/* Keep lo/span inside the disk and the zoom limits. */
void  MapView_Clamp(struct MapView *v);
BOOL  MapView_IsZoomed(const struct MapView *v);

/* Cylinder -> pixel x (left edge of that cylinder).  Not clipped to the
   map: cylinders outside the view land left of mx or right of mx+mw. */
LONG  MapView_X(const struct MapView *v, ULONG cyl);
/* Same, clipped to [mx, mx+mw] - for drawing. */
WORD  MapView_ClipX(const struct MapView *v, ULONG cyl);
/* Pixel x -> cylinder under it, clamped to the visible window. */
ULONG MapView_Cyl(const struct MapView *v, LONG x);
/* A pixel distance -> cylinder distance at the current zoom. */
ULONG MapView_DeltaCyl(const struct MapView *v, ULONG dpx);
/* Any part of [lo, hi] visible? */
BOOL  MapView_Overlaps(const struct MapView *v, ULONG lo, ULONG hi);

/* Zoom in (2x) or out (0.5x), keeping anchor_cyl at pixel anchor_x. */
void  MapView_Zoom(struct MapView *v, BOOL zoom_in, ULONG anchor_cyl,
                   LONG anchor_x);
/* Show the whole disk. */
void  MapView_Fit(struct MapView *v);
/* Zoom so [lo, hi] fills about 80% of the map, centred. */
void  MapView_ZoomTo(struct MapView *v, ULONG lo, ULONG hi);
/* Pan (no zoom change) so [lo, hi] is fully visible - centred when it
   was not; a range wider than the view is shown from its start. */
void  MapView_EnsureVisible(struct MapView *v, ULONG lo, ULONG hi);
/* Zoom factor x10 (total / span), e.g. 80 for "8.0x". */
ULONG MapView_ZoomX10(const struct MapView *v);

/* Scroller <-> view.  unit = cylinders per scroller step. */
ULONG MapView_ScrollerUnit(const struct MapView *v);
void  MapView_ScrollerVals(const struct MapView *v, ULONG *top,
                           ULONG *visible, ULONG *total);
void  MapView_SetScrollerTop(struct MapView *v, ULONG top);

#endif /* MAPVIEW_H */
