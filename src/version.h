#ifndef VERSION_H
#define VERSION_H

#define AMIPART_VERNUM   "0.9"
#define AMIPART_VERDATE  "07.11.2026"      /* dd.mm.yyyy, update with VERNUM */
#define AMIPART_VERSION  "V" AMIPART_VERNUM
#define AMIPART_VERTITLE "AmiPart " AMIPART_VERSION
/* AmigaOS "Version" command string; defined in main.c (kept alive there). */
#define AMIPART_VERSTRING "$VER: AmiPart " AMIPART_VERNUM " (" AMIPART_VERDATE ")"

/* Defined in build.c, recompiled on every build (see Makefile). */
extern const char AmiPart_BuildStamp[];

#endif /* VERSION_H */
