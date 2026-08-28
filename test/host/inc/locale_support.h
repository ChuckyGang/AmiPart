#ifndef LOCALE_SUPPORT_H
#define LOCALE_SUPPORT_H
#include "amiga_compat.h"
#include "diskpart_strings.h"
CONST_STRPTR GetDPString(LONG id);
#define GS(id) ((char *)GetDPString(id))
#endif
