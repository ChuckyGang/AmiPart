#ifndef DOS_RDARGS_H
#define DOS_RDARGS_H
#include "amiga_compat.h"
struct CSource { UBYTE *CS_Buffer; LONG CS_Length; LONG CS_CurChr; };
struct RDArgs  { struct CSource RDA_Source; LONG RDA_DAList;
                 UBYTE *RDA_Buffer; LONG RDA_BufSiz;
                 UBYTE *RDA_ExtHelp; LONG RDA_Flags; };
struct RDArgs *ReadArgs(CONST_STRPTR tmpl, SIPTR *array, struct RDArgs *rda);
void FreeArgs(struct RDArgs *rda);
#endif
