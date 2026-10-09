/*
 * partview.h - Partition view window for AmiPart.
 */

#ifndef PARTVIEW_H
#define PARTVIEW_H

#include <exec/types.h>

/* Open the partition view for the selected device.
   Returns TRUE if the user requested to exit the program (close window),
   FALSE if they clicked Back. */
BOOL partview_run(const char *devname, ULONG unit);

/* LOWCHIP (CLI switch / icon tooltype): keep every window SIMPLE_REFRESH
   instead of SMART_REFRESH.  SMART_REFRESH lets Intuition repaint windows
   uncovered while a dialog runs its own event loop (issue #14/1), at the
   cost of a window-sized backing bitmap in chip RAM per open window. */
extern BOOL AmiPart_LowChip;
#define PV_REFRESH_FLAG (AmiPart_LowChip ? WFLG_SIMPLE_REFRESH : WFLG_SMART_REFRESH)

#endif /* PARTVIEW_H */
