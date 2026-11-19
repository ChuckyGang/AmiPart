/* host_req.c - requesters and the remaining small pieces of the Linux
 * GUI layer: EasyRequest, the ASL file requester, and the exec / dos /
 * icon calls the GUI source makes that have no meaning on Linux.
 *
 * The two requesters are ordinary clients of the layer: they open a
 * window, create GadTools gadgets and run a message loop, exactly as
 * AmiPart's own dialogs do. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "hg_int.h"

/* ------------------------------------------------------------------ */
/* argv for the shared ReadArgs shim (src/main.c has no argc/argv)      */
/* ------------------------------------------------------------------ */

int    host_argc;
char **host_argv;

__attribute__((constructor))
static void hg_grab_args(int argc, char **argv)
{
    host_argc = argc;
    host_argv = argv;
}

/* ------------------------------------------------------------------ */
/* exec / dos / icon odds and ends                                      */
/* ------------------------------------------------------------------ */

static struct Library hg_lib = { { NULL, NULL, 0, 0, NULL }, 47, 0 };

struct Library *OpenLibrary(CONST_STRPTR name, ULONG version) { (void)name; (void)version; return &hg_lib; }
void CloseLibrary(struct Library *lib) { (void)lib; }
APTR OpenResource(CONST_STRPTR name) { (void)name; return NULL; }
void Forbid(void) {}
void Permit(void) {}

void AddTail(struct List *l, struct Node *n)
{
    n->ln_Succ = (struct Node *)&l->lh_Tail;
    n->ln_Pred = l->lh_TailPred;
    l->lh_TailPred->ln_Succ = n;
    l->lh_TailPred = n;
}

struct DosList *LockDosList(ULONG flags) { (void)flags; return NULL; }
struct DosList *NextDosEntry(struct DosList *dl, ULONG flags) { (void)dl; (void)flags; return NULL; }
void UnLockDosList(ULONG flags) { (void)flags; }
BPTR CurrentDir(BPTR lock) { (void)lock; return 0; }

struct DiskObject *GetDiskObject(STRPTR name) { (void)name; return NULL; }
void FreeDiskObject(struct DiskObject *d) { (void)d; }
STRPTR FindToolType(STRPTR *tt, STRPTR name) { (void)tt; (void)name; return NULL; }

/* A lock is just the path. */
BPTR Lock(CONST_STRPTR name, LONG mode)
{
    struct stat st;
    (void)mode;
    if (!name || stat((const char *)name, &st) != 0) return 0;
    return (BPTR)(size_t)strdup((const char *)name);
}
void UnLock(BPTR lock) { free((void *)(size_t)lock); }
BOOL Examine(BPTR lock, struct FileInfoBlock *fib)
{
    struct stat st;
    const char *p = (const char *)(size_t)lock;
    if (!p || stat(p, &st) != 0) return FALSE;
    memset(fib, 0, sizeof(*fib));
    fib->fib_DirEntryType = S_ISDIR(st.st_mode) ? ST_USERDIR : ST_FILE;
    fib->fib_Size = st.st_size > 0x7FFFFFFF ? 0x7FFFFFFF : (LONG)st.st_size;
    return TRUE;
}

BOOL AddPart(STRPTR dirname, CONST_STRPTR filename, ULONG size)
{
    char *d = (char *)dirname;
    const char *f = (const char *)filename;
    size_t dl = strlen(d), fl = strlen(f);
    if (f[0] == '/') {
        if (fl + 1 > size) return FALSE;
        memcpy(d, f, fl + 1);
        return TRUE;
    }
    if (dl + fl + 2 > size) return FALSE;
    if (dl && d[dl - 1] != '/') d[dl++] = '/';
    memcpy(d + dl, f, fl + 1);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* EasyRequest                                                          */
/* ------------------------------------------------------------------ */

/* RawDoFmt-style formatting.  next() returns the next argument: as a
   pointer when want_ptr is set, as an integer otherwise. */
typedef IPTR (*ArgFn)(void *ud, int want_ptr);

static void req_format(char *out, size_t size, const char *fmt, ArgFn next, void *ud)
{
    size_t o = 0;
#define PUTC(c) do { if (o + 1 < size) out[o++] = (char)(c); } while (0)
    for (; *fmt; fmt++) {
        char spec[16], tmp[64];
        int n = 0, left = 0, zero = 0, width = 0, prec = -1, len;
        const char *s;
        if (*fmt != '%') { PUTC(*fmt); continue; }
        fmt++;
        if (*fmt == '%') { PUTC('%'); continue; }
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { prec = 0; fmt++; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        if (*fmt == 'l') fmt++;
        if (!*fmt) break;
        switch (*fmt) {
        case 's':
            s = next ? (const char *)next(ud, 1) : NULL;
            if (!s) s = "";
            len = (int)strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            break;
        case 'c':
            tmp[0] = (char)(next ? next(ud, 0) : '?'); tmp[1] = '\0'; s = tmp; len = 1;
            break;
        case 'd': case 'u': case 'x': case 'X': {
            unsigned int v = (unsigned int)(next ? next(ud, 0) : 0);
            spec[n++] = '%'; spec[n++] = *fmt; spec[n] = '\0';
            /* libc formatting of one int; clib's snprintf also takes %d/%u/%x */
            if (*fmt == 'd') len = sprintf(tmp, "%d", (int)v);
            else if (*fmt == 'u') len = sprintf(tmp, "%u", v);
            else if (*fmt == 'x') len = sprintf(tmp, "%x", v);
            else len = sprintf(tmp, "%X", v);
            s = tmp;
            break; }
        default:
            PUTC('%'); PUTC(*fmt); continue;
        }
        if (!left) for (n = len; n < width; n++) PUTC(zero ? '0' : ' ');
        for (n = 0; n < len; n++) PUTC(s[n]);
        if (left) for (n = len; n < width; n++) PUTC(' ');
    }
    out[o] = '\0';
#undef PUTC
}

static IPTR next_va(void *ud, int want_ptr)
{
    va_list *ap = (va_list *)ud;
    if (want_ptr) return (IPTR)va_arg(*ap, void *);
    return (IPTR)va_arg(*ap, unsigned int);
}
static IPTR next_arr(void *ud, int want_ptr)
{
    IPTR **pp = (IPTR **)ud;
    (void)want_ptr;
    return *(*pp)++;
}

#define REQ_MAXBTN 6

static LONG easy_run(struct Window *parent, struct EasyStruct *es, const char *body)
{
    struct Screen *scr = LockPubScreen(NULL);
    struct Window *win;
    struct Gadget *glist = NULL, *prev;
    struct NewGadget ng;
    char  labels[256], *lab[REQ_MAXBTN], *p;
    int   nbtn = 0, i, nlines = 1, maxw = 0, btn_total = 0, bw[REQ_MAXBTN];
    int   pad = 10, btn_h = HG_FH + 6, line_h = HG_FH + 1;
    int   W, H, text_h, inner_w, bl, bt;
    LONG  result = 0;
    BOOL  done = FALSE;
    const char *q;

    (void)parent;
    (void)body;
    if (!scr) return 0;               /* no display (already reported): answer "cancel" */

    strncpy(labels, es->es_GadgetFormat ? (char *)es->es_GadgetFormat : "OK", sizeof(labels) - 1);
    labels[sizeof(labels) - 1] = '\0';
    for (p = labels; nbtn < REQ_MAXBTN; ) {
        lab[nbtn++] = p;
        p = strchr(p, '|');
        if (!p) break;
        *p++ = '\0';
    }
    for (i = 0; i < nbtn; i++) { bw[i] = hg_textw(lab[i]) + 24; btn_total += bw[i]; }

    for (q = body; ; ) {
        const char *e = strchr(q, '\n');
        int w = (int)(e ? e - q : (int)strlen(q)) * HG_FW;
        if (w > maxw) maxw = w;
        if (!e) break;
        nlines++; q = e + 1;
    }
    text_h = nlines * line_h + 8;
    inner_w = maxw + 16;
    if (inner_w < btn_total + (nbtn - 1) * 12) inner_w = btn_total + (nbtn - 1) * 12;
    if (inner_w < 200) inner_w = 200;
    bl = scr->WBorLeft; bt = scr->WBorTop + HG_FH + 1;
    W = inner_w + 2 * pad + bl + scr->WBorRight;
    H = bt + pad + text_h + pad + btn_h + pad + scr->WBorBottom;
    if (W > scr->Width) W = scr->Width;

    if (!CreateContext(&glist)) return 0;
    prev = glist;
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr = scr->Font;
    ng.ng_TopEdge = (WORD)(bt + pad + text_h + pad);
    ng.ng_Height = (WORD)btn_h;
    ng.ng_Flags = PLACETEXT_IN;
    for (i = 0; i < nbtn; i++) {
        int avail = W - bl - scr->WBorRight - 2 * pad;
        int x = bl + pad;
        if (nbtn == 1) x += (avail - bw[0]) / 2;
        else {
            int gap = (avail - btn_total) / (nbtn - 1), k;
            for (k = 0; k < i; k++) x += bw[k] + gap;
        }
        ng.ng_LeftEdge = (WORD)x;
        ng.ng_Width = (WORD)bw[i];
        ng.ng_GadgetText = (UBYTE *)lab[i];
        ng.ng_GadgetID = (UWORD)(i + 1);
        prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);
        if (!prev) { FreeGadgets(glist); return 0; }
    }
    {
        struct TagItem wt[] = {
            { WA_Left,   (IPTR)((scr->Width - W) / 2) },
            { WA_Top,    (IPTR)((scr->Height - H) / 2) },
            { WA_Width,  (IPTR)W }, { WA_Height, (IPTR)H },
            { WA_Title,  (IPTR)(es->es_Title ? (char *)es->es_Title : "AmiPart") },
            { WA_Gadgets,(IPTR)glist },
            { WA_IDCMP,  IDCMP_GADGETUP | IDCMP_VANILLAKEY },
            { WA_Flags,  WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_ACTIVATE },
            { TAG_DONE, 0 } };
        win = OpenWindowTagList(NULL, wt);
    }
    if (!win) { FreeGadgets(glist); return 0; }
    ((struct HWin *)win)->note = body;

    {
        struct HSurf *s = &((struct HWin *)win)->surf;
        int y = bt + pad + 4;
        hs_bevel(s, bl + pad, bt + pad, W - bl - scr->WBorRight - 2 * pad, text_h, 1);
        for (q = body; ; ) {
            const char *e = strchr(q, '\n');
            int n = (int)(e ? e - q : (int)strlen(q));
            int maxc = (W - bl - scr->WBorRight - 2 * pad - 16) / HG_FW;
            hs_text(s, bl + pad + 8, y, q, n > maxc ? maxc : n, P_TEXT);
            y += line_h;
            if (!e) break;
            q = e + 1;
        }
        hg_dirty = 1;
    }

    while (!done) {
        struct IntuiMessage *m;
        WaitPort(win->UserPort);
        while ((m = GT_GetIMsg(win->UserPort)) != NULL) {
            ULONG cls = m->Class; UWORD code = m->Code;
            struct Gadget *g = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            if (cls == IDCMP_GADGETUP) {
                result = (g->GadgetID == nbtn) ? 0 : g->GadgetID;
                done = TRUE;
            } else if (cls == IDCMP_VANILLAKEY) {
                /* Esc always answers with the rightmost gadget (cancel).
                   Return only closes a one-button requester: a stray key
                   must never confirm a write. */
                if (code == 27 || (code == 13 && nbtn == 1)) { result = 0; done = TRUE; }
            }
        }
    }
    CloseWindow(win);
    FreeGadgets(glist);
    return result;
}

LONG EasyRequest(struct Window *win, struct EasyStruct *es, ULONG *idcmp, ...)
{
    char body[4096];
    va_list ap;
    (void)idcmp;
    va_start(ap, idcmp);
    req_format(body, sizeof(body), es->es_TextFormat ? (char *)es->es_TextFormat : "", next_va, &ap);
    va_end(ap);
    return easy_run(win, es, body);
}

LONG EasyRequestArgs(struct Window *win, struct EasyStruct *es, ULONG *idcmp, APTR args)
{
    char body[4096];
    IPTR *a = (IPTR *)args;
    (void)idcmp;
    req_format(body, sizeof(body), es->es_TextFormat ? (char *)es->es_TextFormat : "",
               args ? next_arr : NULL, &a);
    return easy_run(win, es, body);
}

/* ------------------------------------------------------------------ */
/* ASL file requester                                                   */
/* ------------------------------------------------------------------ */

#define FR_MAX 2000
enum { FRG_DRAWER = 1, FRG_LIST, FRG_FILE, FRG_OK, FRG_PARENT, FRG_CANCEL };

struct HFileReq {
    struct FileRequester fr;
    char  file[256];
    char  drawer[1024];
    char  title[128];
    int   save;
};

static void fr_tags(struct HFileReq *r, struct TagItem *t)
{
    for (; t && t->ti_Tag != TAG_DONE; t++) {
        const char *s = (const char *)t->ti_Data;
        switch (t->ti_Tag) {
        case ASLFR_TitleText:     strncpy(r->title, s ? s : "", sizeof(r->title) - 1); break;
        case ASLFR_InitialFile:   strncpy(r->file, s ? s : "", sizeof(r->file) - 1); break;
        case ASLFR_InitialDrawer: strncpy(r->drawer, s ? s : "", sizeof(r->drawer) - 1); break;
        case ASLFR_DoSaveMode:    r->save = t->ti_Data != 0; break;
        default: break;
        }
    }
}

APTR AllocAslRequest(ULONG type, struct TagItem *tags)
{
    struct HFileReq *r;
    if (type != ASL_FileRequest) return NULL;
    r = (struct HFileReq *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->fr.fr_File = (STRPTR)r->file;
    r->fr.fr_Drawer = (STRPTR)r->drawer;
    fr_tags(r, tags);
    return r;
}

void FreeAslRequest(APTR req) { free(req); }

static int is_dir(const char *p)
{
    struct stat st;
    return p && *p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

struct FrEntry { char name[256]; int dir; };

static int fr_cmp(const void *a, const void *b)
{
    const struct FrEntry *x = (const struct FrEntry *)a, *y = (const struct FrEntry *)b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static int fr_read(const char *drawer, struct FrEntry *ent, struct Node *nodes, struct List *list)
{
    DIR *d = opendir(drawer);
    struct dirent *de;
    int n = 0, i;
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
    if (!d) return 0;
    while ((de = readdir(d)) != NULL && n < FR_MAX) {
        char full[1400];
        if (de->d_name[0] == '.') continue;
        snprintf(full, sizeof(full), "%s/%s", drawer, de->d_name);
        ent[n].dir = is_dir(full);
        strncpy(ent[n].name, de->d_name, sizeof(ent[n].name) - 2);
        ent[n].name[sizeof(ent[n].name) - 2] = '\0';
        n++;
    }
    closedir(d);
    qsort(ent, (size_t)n, sizeof(*ent), fr_cmp);
    for (i = 0; i < n; i++) {
        if (ent[i].dir) strcat(ent[i].name, "/");
        nodes[i].ln_Name = ent[i].name;
        nodes[i].ln_Type = NT_USER;
        AddTail(list, &nodes[i]);
    }
    return n;
}

BOOL AslRequest(APTR req, struct TagItem *tags)
{
    struct HFileReq *r = (struct HFileReq *)req;
    struct Screen *scr = LockPubScreen(NULL);
    struct Window *win = NULL;
    struct Gadget *glist = NULL, *prev, *g_drawer, *g_list, *g_file;
    struct NewGadget ng;
    struct FrEntry *ent;
    struct Node *nodes;
    struct List list;
    BOOL ok = FALSE, done = FALSE;
    int  W = 440, H = 380, bl, bt, x0, iw, row = HG_FH + 6, lv_h, n, last_idx = -1;
    ULONG last_s = 0, last_m = 0;

    if (!r || !scr) return FALSE;
    fr_tags(r, tags);
    if (!is_dir(r->drawer)) {
        if (!getcwd(r->drawer, sizeof(r->drawer))) strcpy(r->drawer, "/");
    }
    ent = (struct FrEntry *)malloc(sizeof(*ent) * FR_MAX);
    nodes = (struct Node *)calloc(FR_MAX, sizeof(*nodes));
    if (!ent || !nodes) { free(ent); free(nodes); return FALSE; }
    n = fr_read(r->drawer, ent, nodes, &list);

    if (H > scr->Height - 20) H = scr->Height - 20;
    bl = scr->WBorLeft; bt = scr->WBorTop + HG_FH + 1;
    x0 = bl + 8 + 7 * HG_FW; iw = W - x0 - scr->WBorRight - 8;
    lv_h = H - bt - 6 - row - 6 - 6 - row - 6 - row - 6 - scr->WBorBottom;

    if (!CreateContext(&glist)) goto out;
    prev = glist;
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr = scr->Font;
    ng.ng_LeftEdge = (WORD)x0; ng.ng_TopEdge = (WORD)(bt + 6);
    ng.ng_Width = (WORD)iw; ng.ng_Height = (WORD)row;
    ng.ng_GadgetText = (UBYTE *)"Drawer"; ng.ng_GadgetID = FRG_DRAWER; ng.ng_Flags = PLACETEXT_LEFT;
    { struct TagItem t[] = { { GTST_MaxChars, 1000 }, { GTST_String, (IPTR)r->drawer }, { TAG_DONE, 0 } };
      g_drawer = prev = CreateGadgetA(STRING_KIND, prev, &ng, t); }
    ng.ng_LeftEdge = (WORD)(bl + 8); ng.ng_Width = (WORD)(W - bl - scr->WBorRight - 16);
    ng.ng_TopEdge = (WORD)(bt + 6 + row + 6); ng.ng_Height = (WORD)lv_h;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = FRG_LIST; ng.ng_Flags = 0;
    { struct TagItem t[] = { { GTLV_Labels, (IPTR)&list }, { GTLV_ShowSelected, 0 }, { TAG_DONE, 0 } };
      g_list = prev = CreateGadgetA(LISTVIEW_KIND, prev, &ng, t); }
    ng.ng_LeftEdge = (WORD)x0; ng.ng_Width = (WORD)iw;
    ng.ng_TopEdge = (WORD)(bt + 6 + row + 6 + lv_h + 6); ng.ng_Height = (WORD)row;
    ng.ng_GadgetText = (UBYTE *)"File"; ng.ng_GadgetID = FRG_FILE; ng.ng_Flags = PLACETEXT_LEFT;
    { struct TagItem t[] = { { GTST_MaxChars, 250 }, { GTST_String, (IPTR)r->file }, { TAG_DONE, 0 } };
      g_file = prev = CreateGadgetA(STRING_KIND, prev, &ng, t); }
    ng.ng_TopEdge = (WORD)(ng.ng_TopEdge + row + 6); ng.ng_Width = 90; ng.ng_Flags = PLACETEXT_IN;
    ng.ng_LeftEdge = (WORD)(bl + 8);
    ng.ng_GadgetText = (UBYTE *)(r->save ? "Save" : "Open"); ng.ng_GadgetID = FRG_OK;
    prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);
    ng.ng_LeftEdge = (WORD)((W - 90) / 2);
    ng.ng_GadgetText = (UBYTE *)"Parent"; ng.ng_GadgetID = FRG_PARENT;
    prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);
    ng.ng_LeftEdge = (WORD)(W - scr->WBorRight - 8 - 90);
    ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = FRG_CANCEL;
    prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);
    if (!prev || !g_drawer || !g_list || !g_file) goto out;

    {
        struct TagItem wt[] = {
            { WA_Left,   (IPTR)((scr->Width - W) / 2) },
            { WA_Top,    (IPTR)((scr->Height - H) / 2) },
            { WA_Width,  (IPTR)W }, { WA_Height, (IPTR)H },
            { WA_Title,  (IPTR)(r->title[0] ? r->title : "Select File") },
            { WA_Gadgets,(IPTR)glist },
            { WA_IDCMP,  IDCMP_GADGETUP | IDCMP_VANILLAKEY | IDCMP_CLOSEWINDOW },
            { WA_Flags,  WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE },
            { TAG_DONE, 0 } };
        win = OpenWindowTagList(NULL, wt);
    }
    if (!win) goto out;
    ActivateGadget(g_file, win, NULL);

    while (!done) {
        struct IntuiMessage *m;
        WaitPort(win->UserPort);
        while (!done && (m = GT_GetIMsg(win->UserPort)) != NULL) {
            ULONG cls = m->Class, sec = m->Seconds, mic = m->Micros;
            UWORD code = m->Code;
            struct Gadget *g = (struct Gadget *)m->IAddress;
            int reload = 0;
            GT_ReplyIMsg(m);
            if (cls == IDCMP_CLOSEWINDOW || (cls == IDCMP_VANILLAKEY && code == 27)) { done = TRUE; break; }
            if (cls != IDCMP_GADGETUP) continue;
            switch (g->GadgetID) {
            case FRG_CANCEL: done = TRUE; break;
            case FRG_OK: case FRG_FILE: ok = TRUE; done = TRUE; break;
            case FRG_PARENT: {
                char *sl;
                strncpy(r->drawer, (char *)((struct StringInfo *)g_drawer->SpecialInfo)->Buffer, sizeof(r->drawer) - 1);
                sl = strrchr(r->drawer, '/');
                if (sl && sl != r->drawer) *sl = '\0'; else strcpy(r->drawer, "/");
                reload = 1;
                break; }
            case FRG_DRAWER:
                strncpy(r->drawer, (char *)((struct StringInfo *)g_drawer->SpecialInfo)->Buffer, sizeof(r->drawer) - 1);
                if (!is_dir(r->drawer) && !getcwd(r->drawer, sizeof(r->drawer))) strcpy(r->drawer, "/");
                reload = 1;
                break;
            case FRG_LIST:
                if (code < n) {
                    if (ent[code].dir) {
                        size_t dl = strlen(r->drawer), el = strlen(ent[code].name) - 1;
                        if (dl + el + 2 < sizeof(r->drawer)) {
                            if (dl && r->drawer[dl - 1] != '/') r->drawer[dl++] = '/';
                            memcpy(r->drawer + dl, ent[code].name, el);
                            r->drawer[dl + el] = '\0';
                        }
                        reload = 1;
                    } else {
                        struct TagItem t[] = { { GTST_String, (IPTR)ent[code].name }, { TAG_DONE, 0 } };
                        GT_SetGadgetAttrsA(g_file, win, NULL, t);
                        if (code == last_idx && DoubleClick(last_s, last_m, sec, mic)) { ok = TRUE; done = TRUE; }
                        last_idx = code; last_s = sec; last_m = mic;
                    }
                }
                break;
            default: break;
            }
            if (reload) {
                struct TagItem det[] = { { GTLV_Labels, (IPTR)-1 }, { TAG_DONE, 0 } };
                struct TagItem att[] = { { GTLV_Labels, (IPTR)&list }, { GTLV_Top, 0 }, { GTLV_Selected, (IPTR)-1 }, { TAG_DONE, 0 } };
                struct TagItem st[]  = { { GTST_String, (IPTR)r->drawer }, { TAG_DONE, 0 } };
                GT_SetGadgetAttrsA(g_list, win, NULL, det);
                n = fr_read(r->drawer, ent, nodes, &list);
                GT_SetGadgetAttrsA(g_list, win, NULL, att);
                GT_SetGadgetAttrsA(g_drawer, win, NULL, st);
                last_idx = -1;
            }
        }
    }
    if (ok) {
        strncpy(r->file, (char *)((struct StringInfo *)g_file->SpecialInfo)->Buffer, sizeof(r->file) - 1);
        strncpy(r->drawer, (char *)((struct StringInfo *)g_drawer->SpecialInfo)->Buffer, sizeof(r->drawer) - 1);
        if (!r->file[0]) ok = FALSE;
    }
out:
    if (win) CloseWindow(win);
    if (glist) FreeGadgets(glist);
    free(ent); free(nodes);
    return ok;
}
