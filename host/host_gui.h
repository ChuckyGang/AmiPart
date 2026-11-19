/* host_gui.h - Intuition / GadTools / graphics / ASL as the AmiPart GUI
 * uses them, for the Linux build.
 *
 * The Amiga GUI source (src/partview*.c, diskselect.c, main.c ...) is
 * compiled unchanged against these declarations; host_gui.c implements
 * them on SDL2.  Only what AmiPart actually touches is here: the structs
 * carry the fields the GUI reads, in whatever layout is convenient - none
 * of them is ever exchanged with real AmigaOS.
 *
 * Pointers do not fit in a ULONG on a 64-bit host, so tag data and
 * requester arguments are IPTR (pointer-width) - see src/clib.h. */
#ifndef HOST_GUI_H
#define HOST_GUI_H

#include "amiga_compat.h"

#ifndef DP_IPTR_DEFINED
#define DP_IPTR_DEFINED
typedef uintptr_t IPTR;
#endif

typedef void (*VOIDFUNC)(void);
typedef ULONG (*HOOKFUNC)(void);

/* ---- utility/tagitem.h ---- */
struct TagItem { ULONG ti_Tag; IPTR ti_Data; };
#define TAG_DONE   0UL
#define TAG_END    0UL
#define TAG_IGNORE 1UL
#define TAG_MORE   2UL
#define TAG_SKIP   3UL
#define TAG_USER   0x80000000UL

struct Hook {
    struct MinNode h_MinNode;
    HOOKFUNC h_Entry;
    HOOKFUNC h_SubEntry;
    APTR     h_Data;
};
/* The GUI's hook functions take their arguments in a0/a1/a2 on the Amiga.
 * On the host the caller stores them here before calling h_Entry. */
extern void *host_reg_a0, *host_reg_a1, *host_reg_a2;

/* ---- exec ---- */
#ifndef NT_USER
#define NT_USER 254
#endif
struct Library { struct Node lib_Node; UWORD lib_Version, lib_Revision; };
struct ExecBase      { struct Library LibNode; };
struct IntuitionBase { struct Library LibNode; };
struct GfxBase       { struct Library LibNode; };
struct DosLibrary    { struct Library dl_lib; };
struct Process       { APTR pr_CLI; };

struct Library *OpenLibrary(CONST_STRPTR name, ULONG version);
void   CloseLibrary(struct Library *lib);
APTR   OpenResource(CONST_STRPTR name);
void   Forbid(void);
void   Permit(void);
void   AddTail(struct List *l, struct Node *n);
struct Message *WaitPort(struct MsgPort *port);

/* ---- dos bits only the GUI uses ---- */
#define BADDR(b) ((APTR)(IPTR)(b))
#define LDF_DEVICES 4
#define LDF_READ    1
struct DosList {
    struct DosList *dol_Next;
    LONG  dol_Type;
    union { struct { IPTR dol_Startup; } dol_handler; } dol_misc;
    IPTR  dol_Name;
};
struct FileSysStartupMsg { ULONG fssm_Unit; IPTR fssm_Device; IPTR fssm_Environ; ULONG fssm_Flags; };
struct FileSysEntry    { struct Node fse_Node; ULONG fse_DosType; ULONG fse_Version; };
struct FileSysResource { struct Node fsr_Node; STRPTR fsr_Creator; struct List fsr_FileSysEntries; };
struct DosList *LockDosList(ULONG flags);
struct DosList *NextDosEntry(struct DosList *dl, ULONG flags);
void   UnLockDosList(ULONG flags);
BPTR   CurrentDir(BPTR lock);
BOOL   AddPart(STRPTR dirname, CONST_STRPTR filename, ULONG size);

/* ---- workbench / icon ---- */
struct WBArg     { BPTR wa_Lock; STRPTR wa_Name; };
struct WBStartup { LONG sm_NumArgs; struct WBArg *sm_ArgList; };
struct DiskObject { STRPTR *do_ToolTypes; };
struct DiskObject *GetDiskObject(STRPTR name);
void   FreeDiskObject(struct DiskObject *d);
STRPTR FindToolType(STRPTR *tt, STRPTR name);

/* ---- graphics ---- */
struct Rectangle { WORD MinX, MinY, MaxX, MaxY; };
struct TextAttr  { STRPTR ta_Name; UWORD ta_YSize; UBYTE ta_Style, ta_Flags; };
struct TextFont  { UWORD tf_YSize, tf_XSize, tf_Baseline; };
struct ColorMap  { int unused; };
struct ViewPort  { struct ColorMap *ColorMap; };
struct RastPort {
    BYTE  FgPen, BgPen, DrawMode;
    WORD  cp_x, cp_y;
    UWORD TxHeight, TxWidth, TxBaseline;
    struct TextFont *Font;
    /* host private */
    void *hg_surf;                /* target surface, NULL = measuring only */
    WORD  hg_cx0, hg_cy0, hg_cx1, hg_cy1;   /* clip (inclusive), if hg_clip */
    BYTE  hg_clip;
};
#define JAM1       0
#define JAM2       1
#define COMPLEMENT 2
#define INVERSVID  4

void  SetAPen(struct RastPort *rp, ULONG pen);
void  SetBPen(struct RastPort *rp, ULONG pen);
void  SetDrMd(struct RastPort *rp, ULONG mode);
void  Move(struct RastPort *rp, LONG x, LONG y);
void  Draw(struct RastPort *rp, LONG x, LONG y);
void  RectFill(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1);
void  EraseRect(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1);
LONG  Text(struct RastPort *rp, CONST_STRPTR s, ULONG len);
WORD  TextLength(struct RastPort *rp, CONST_STRPTR s, ULONG len);
void  InitRastPort(struct RastPort *rp);
void  SetFont(struct RastPort *rp, struct TextFont *tf);
struct TextFont *OpenFont(struct TextAttr *ta);
void  CloseFont(struct TextFont *tf);
LONG  ObtainBestPenA(struct ColorMap *cm, ULONG r, ULONG g, ULONG b, struct TagItem *tags);
void  ReleasePen(struct ColorMap *cm, ULONG pen);

/* ---- intuition ---- */
#define DETAILPEN        0
#define BLOCKPEN         1
#define TEXTPEN          2
#define SHINEPEN         3
#define SHADOWPEN        4
#define FILLPEN          5
#define FILLTEXTPEN      6
#define BACKGROUNDPEN    7
#define HIGHLIGHTTEXTPEN 8
#define NUMDRIPENS       12
struct DrawInfo { UWORD dri_Version, dri_NumPens; UWORD *dri_Pens; struct TextFont *dri_Font; };

struct Screen {
    WORD  LeftEdge, TopEdge, Width, Height;
    BYTE  BarHeight, WBorTop, WBorLeft, WBorRight, WBorBottom;
    struct TextAttr *Font;
    struct ViewPort  ViewPort;
    struct RastPort  RastPort;
};

struct StringInfo { UBYTE *Buffer; UBYTE *UndoBuffer; WORD BufferPos, MaxChars, DispPos, NumChars; LONG LongInt; };

struct Gadget {
    struct Gadget *NextGadget;
    WORD   LeftEdge, TopEdge, Width, Height;
    UWORD  Flags, Activation, GadgetType;
    APTR   SpecialInfo;
    UWORD  GadgetID;
    APTR   UserData;
};
#define GFLG_SELECTED 0x0080
#define GFLG_DISABLED 0x0100

struct MenuItem {
    struct MenuItem *NextItem;
    UWORD  Flags;
    UWORD  NextSelect;
    /* host private */
    const char *hg_label;       /* NULL = bar */
    char   hg_key;
    APTR   hg_userdata;
};
struct Menu {
    struct Menu *NextMenu;
    UWORD  Flags;
    struct MenuItem *FirstItem;
    const char *hg_title;
    WORD   hg_x, hg_w;
};
#define CHECKIT      0x0001
#define ITEMTEXT     0x0002
#define COMMSEQ      0x0004
#define MENUTOGGLE   0x0008
#define ITEMENABLED  0x0010
#define HIGHCOMP     0x0040
#define CHECKED      0x0100
#define MENUENABLED  0x0001
#define MENUNULL     0xFFFF
#define NOMENU       0x001F
#define NOITEM       0x003F
#define NOSUB        0x001F
#define MENUNUM(n)   ((n) & 0x1F)
#define ITEMNUM(n)   (((n) >> 5) & 0x003F)
#define SUBNUM(n)    (((n) >> 11) & 0x001F)
#define FULLMENUNUM(m,i,s) (((s) << 11) | ((i) << 5) | (m))

struct Window {
    WORD   LeftEdge, TopEdge, Width, Height;
    WORD   MouseX, MouseY;
    WORD   MinWidth, MinHeight;
    UWORD  MaxWidth, MaxHeight;
    ULONG  Flags;
    UBYTE *Title;
    struct RastPort *RPort;
    BYTE   BorderLeft, BorderTop, BorderRight, BorderBottom;
    struct Gadget  *FirstGadget;
    struct MsgPort *UserPort;
    struct Screen  *WScreen;
    struct Menu    *MenuStrip;
    ULONG  IDCMPFlags;
    APTR   UserData;
};

struct IntuiMessage {
    struct Message ExecMessage;
    ULONG  Class;
    UWORD  Code;
    UWORD  Qualifier;
    APTR   IAddress;
    WORD   MouseX, MouseY;
    ULONG  Seconds, Micros;
    struct Window *IDCMPWindow;
};

#define IDCMP_SIZEVERIFY     0x00000001UL
#define IDCMP_NEWSIZE        0x00000002UL
#define IDCMP_REFRESHWINDOW  0x00000004UL
#define IDCMP_MOUSEBUTTONS   0x00000008UL
#define IDCMP_MOUSEMOVE      0x00000010UL
#define IDCMP_GADGETDOWN     0x00000020UL
#define IDCMP_GADGETUP       0x00000040UL
#define IDCMP_MENUPICK       0x00000100UL
#define IDCMP_CLOSEWINDOW    0x00000200UL
#define IDCMP_RAWKEY         0x00000400UL
#define IDCMP_VANILLAKEY     0x00200000UL
#define IDCMP_INTUITICKS     0x00400000UL
#define IDCMP_ACTIVEWINDOW   0x00040000UL
#define IDCMP_INACTIVEWINDOW 0x00080000UL

#define SELECTDOWN 0x68
#define SELECTUP   0xE8
#define MENUDOWN   0x69
#define MENUUP     0xE9
#define IEQUALIFIER_LSHIFT      0x0001
#define IEQUALIFIER_RSHIFT      0x0002
#define IEQUALIFIER_CONTROL     0x0008
#define IEQUALIFIER_LALT        0x0010
#define IEQUALIFIER_DOUBLECLICK 0x8000

#define WFLG_SIZEGADGET     0x00000001UL
#define WFLG_DRAGBAR        0x00000002UL
#define WFLG_DEPTHGADGET    0x00000004UL
#define WFLG_CLOSEGADGET    0x00000008UL
#define WFLG_SIZEBRIGHT     0x00000010UL
#define WFLG_SIZEBBOTTOM    0x00000020UL
#define WFLG_SMART_REFRESH  0x00000000UL
#define WFLG_SIMPLE_REFRESH 0x00000040UL
#define WFLG_BACKDROP       0x00000100UL
#define WFLG_REPORTMOUSE    0x00000200UL
#define WFLG_GIMMEZEROZERO  0x00000400UL
#define WFLG_BORDERLESS     0x00000800UL
#define WFLG_ACTIVATE       0x00001000UL
#define WFLG_RMBTRAP        0x00010000UL
#define WFLG_NOCAREREFRESH  0x00020000UL

#define WA_Dummy     (TAG_USER + 99)
#define WA_Left      (WA_Dummy + 0x01)
#define WA_Top       (WA_Dummy + 0x02)
#define WA_Width     (WA_Dummy + 0x03)
#define WA_Height    (WA_Dummy + 0x04)
#define WA_IDCMP     (WA_Dummy + 0x07)
#define WA_Flags     (WA_Dummy + 0x08)
#define WA_Gadgets   (WA_Dummy + 0x09)
#define WA_Title     (WA_Dummy + 0x0B)
#define WA_MinWidth  (WA_Dummy + 0x0F)
#define WA_MinHeight (WA_Dummy + 0x10)
#define WA_MaxWidth  (WA_Dummy + 0x11)
#define WA_MaxHeight (WA_Dummy + 0x12)
#define WA_InnerWidth  (WA_Dummy + 0x13)
#define WA_InnerHeight (WA_Dummy + 0x14)
#define WA_PubScreen (WA_Dummy + 0x16)

#define GA_Dummy    (TAG_USER + 0x30000)
#define GA_Text     (GA_Dummy + 0x0009)
#define GA_Disabled (GA_Dummy + 0x000E)

struct EasyStruct {
    ULONG  es_StructSize;
    ULONG  es_Flags;
    UBYTE *es_Title;
    UBYTE *es_TextFormat;
    UBYTE *es_GadgetFormat;
};

struct Screen *LockPubScreen(CONST_STRPTR name);
void   UnlockPubScreen(CONST_STRPTR name, struct Screen *scr);
struct Window *OpenWindowTagList(APTR nw, struct TagItem *tags);
void   CloseWindow(struct Window *win);
void   SetWindowTitles(struct Window *win, UBYTE *wtitle, UBYTE *stitle);
BOOL   WindowLimits(struct Window *win, LONG minw, LONG minh, ULONG maxw, ULONG maxh);
UWORD  AddGList(struct Window *win, struct Gadget *g, ULONG pos, LONG num, APTR req);
UWORD  RemoveGList(struct Window *win, struct Gadget *g, LONG num);
void   RefreshGList(struct Gadget *g, struct Window *win, APTR req, LONG num);
BOOL   ActivateGadget(struct Gadget *g, struct Window *win, APTR req);
BOOL   SetMenuStrip(struct Window *win, struct Menu *menu);
void   ClearMenuStrip(struct Window *win);
struct MenuItem *ItemAddress(struct Menu *strip, ULONG num);
void   SetPointer(struct Window *win, UWORD *data, LONG h, LONG w, LONG xo, LONG yo);
void   ClearPointer(struct Window *win);
BOOL   DoubleClick(ULONG s1, ULONG m1, ULONG s2, ULONG m2);
/* Requester arguments: varargs integers are read as 32-bit, %s as a
   pointer; the array form holds one IPTR per argument. */
LONG   EasyRequest(struct Window *win, struct EasyStruct *es, ULONG *idcmp, ...);
LONG   EasyRequestArgs(struct Window *win, struct EasyStruct *es, ULONG *idcmp, APTR args);

/* ---- gadtools ---- */
#define GENERIC_KIND  0
#define BUTTON_KIND   1
#define CHECKBOX_KIND 2
#define INTEGER_KIND  3
#define LISTVIEW_KIND 4
#define MX_KIND       5
#define NUMBER_KIND   6
#define CYCLE_KIND    7
#define PALETTE_KIND  8
#define SCROLLER_KIND 9
#define SLIDER_KIND   11
#define STRING_KIND   12
#define TEXT_KIND     13

#define PLACETEXT_LEFT  0x0001
#define PLACETEXT_RIGHT 0x0002
#define PLACETEXT_ABOVE 0x0004
#define PLACETEXT_BELOW 0x0008
#define PLACETEXT_IN    0x0010
#define NG_HIGHLABEL    0x0020

struct NewGadget {
    WORD   ng_LeftEdge, ng_TopEdge, ng_Width, ng_Height;
    UBYTE *ng_GadgetText;
    struct TextAttr *ng_TextAttr;
    UWORD  ng_GadgetID;
    ULONG  ng_Flags;
    APTR   ng_VisualInfo;
    APTR   ng_UserData;
};

struct NewMenu {
    UBYTE  nm_Type;
    STRPTR nm_Label;
    STRPTR nm_CommKey;
    UWORD  nm_Flags;
    LONG   nm_MutualExclude;
    APTR   nm_UserData;
};
#define NM_TITLE 1
#define NM_ITEM  2
#define NM_SUB   3
#define NM_END   0
#define NM_BARLABEL ((STRPTR)-1)
#define NM_MENUDISABLED 0x0001
#define NM_ITEMDISABLED 0x0010

#define GT_TagBase        (TAG_USER + 0x80000)
#define GTCB_Checked      (GT_TagBase + 4)
#define GTLV_Top          (GT_TagBase + 5)
#define GTLV_Labels       (GT_TagBase + 6)
#define GTLV_ReadOnly     (GT_TagBase + 7)
#define GTLV_ScrollWidth  (GT_TagBase + 8)
#define GTTX_Text         (GT_TagBase + 11)
#define GTCY_Labels       (GT_TagBase + 14)
#define GTCY_Active       (GT_TagBase + 15)
#define GTTX_Border       (GT_TagBase + 57)
#define GTST_String       (GT_TagBase + 45)
#define GTST_MaxChars     (GT_TagBase + 46)
#define GTLV_ShowSelected (GT_TagBase + 53)
#define GTLV_Selected     (GT_TagBase + 54)
#define GTLV_MakeVisible  (GT_TagBase + 78)
#define GTLV_CallBack     (GT_TagBase + 83)

#define LV_DRAW              0x202
#define LVCB_OK              0
#define LVCB_UNKNOWN         1
#define LVR_NORMAL           0
#define LVR_SELECTED         1
#define LVR_NORMALDISABLED   2
#define LVR_SELECTEDDISABLED 8
struct LVDrawMsg {
    ULONG lvdm_MethodID;
    struct RastPort *lvdm_RastPort;
    struct DrawInfo *lvdm_DrawInfo;
    struct Rectangle lvdm_Bounds;
    ULONG lvdm_State;
};

APTR   GetVisualInfoA(struct Screen *scr, struct TagItem *tags);
void   FreeVisualInfo(APTR vi);
struct Gadget *CreateContext(struct Gadget **glist);
struct Gadget *CreateGadgetA(ULONG kind, struct Gadget *prev, struct NewGadget *ng, struct TagItem *tags);
void   FreeGadgets(struct Gadget *glist);
void   GT_SetGadgetAttrsA(struct Gadget *g, struct Window *win, APTR req, struct TagItem *tags);
LONG   GT_GetGadgetAttrsA(struct Gadget *g, struct Window *win, APTR req, struct TagItem *tags);
struct IntuiMessage *GT_GetIMsg(struct MsgPort *port);
void   GT_ReplyIMsg(struct IntuiMessage *msg);
void   GT_RefreshWindow(struct Window *win, APTR req);
void   GT_BeginRefresh(struct Window *win);
void   GT_EndRefresh(struct Window *win, LONG complete);
struct Menu *CreateMenusA(struct NewMenu *nm, struct TagItem *tags);
BOOL   LayoutMenusA(struct Menu *menu, APTR vi, struct TagItem *tags);
void   FreeMenus(struct Menu *menu);

/* ---- asl ---- */
#define ASL_FileRequest 0
#define ASL_TB (TAG_USER + 0x80000)
#define ASLFR_TitleText     (ASL_TB + 1)
#define ASLFR_InitialFile   (ASL_TB + 8)
#define ASLFR_InitialDrawer (ASL_TB + 9)
#define ASLFR_DoSaveMode    (ASL_TB + 44)
struct FileRequester { STRPTR fr_File; STRPTR fr_Drawer; };
APTR   AllocAslRequest(ULONG type, struct TagItem *tags);
BOOL   AslRequest(APTR req, struct TagItem *tags);
void   FreeAslRequest(APTR req);

/* ---- host layer control (host_gui.c) ---- */
BOOL hostgui_init(void);
void hostgui_quit(void);

#endif /* HOST_GUI_H */
