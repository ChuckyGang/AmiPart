#ifndef VERSION_H
#define VERSION_H

/* Release candidates of 1.0.  VERNUM is what the AmigaOS "Version" command
   compares, so it stays below 1.0 until the final release; VERLABEL is what
   people see (window titles, About, release names - CI reads it from here).
   For the final release: VERNUM "1.0", VERLABEL "1.0", VERSUFFIX "". */
#define AMIPART_VERNUM   "0.99"
#define AMIPART_VERLABEL "1.0 RC1"
#define AMIPART_VERSUFFIX " RC1"           /* after the date in $VER */
#define AMIPART_VERDATE  "22.11.2026"      /* dd.mm.yyyy, update with VERNUM */
#define AMIPART_VERSION  "V" AMIPART_VERLABEL
#define AMIPART_VERTITLE "AmiPart " AMIPART_VERSION
/* AmigaOS "Version" command string; defined in main.c (kept alive there). */
#define AMIPART_VERSTRING "$VER: AmiPart " AMIPART_VERNUM " (" AMIPART_VERDATE ")" AMIPART_VERSUFFIX

/* Defined in build.c, recompiled on every build (see Makefile). */
extern const char AmiPart_BuildStamp[];

#endif /* VERSION_H */
