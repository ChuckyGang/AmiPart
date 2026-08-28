/* Host (Linux) compatibility shim for compiling DiskPart's rdb.c unmodified.
 * Provides the AmigaOS types, constants, struct stubs and library-call
 * prototypes rdb.c and rdb.h reference. The device-I/O path (SCSI/trackdisk)
 * compiles but is never executed for the BD_FILE backend used by the PoC. */
#ifndef AMIGA_COMPAT_H
#define AMIGA_COMPAT_H

#include <stddef.h>
#include <stdint.h>

/* ---- base types ---- */
typedef unsigned char  UBYTE;
typedef signed   char  BYTE;
typedef unsigned short UWORD;
typedef signed   short WORD;
typedef unsigned int   ULONG;   /* 32-bit on both m68k and host LP64 int */
typedef signed   int   LONG;
typedef short          BOOL;
typedef void          *APTR;
typedef char          *STRPTR;
typedef const char    *CONST_STRPTR;
typedef long           BPTR;     /* host: holds a FILE* cast to long */
typedef unsigned long long UQUAD;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* ---- exec/memory ---- */
#define MEMF_ANY    0
#define MEMF_PUBLIC (1<<0)
#define MEMF_CHIP   (1<<1)
#define MEMF_FAST   (1<<2)
#define MEMF_CLEAR  (1<<16)

/* ---- dos ---- */
#define MODE_OLDFILE   1005
#define MODE_NEWFILE   1006
#define MODE_READWRITE 1004
#define OFFSET_BEGINNING (-1)
#define OFFSET_CURRENT   0
#define OFFSET_END       1

/* ---- exec list/node/port/io stubs (only enough to compile) ---- */
struct MinNode { struct MinNode *mln_Succ, *mln_Pred; };
struct Node { struct Node *ln_Succ, *ln_Pred; UBYTE ln_Type; BYTE ln_Pri; char *ln_Name; };
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; UBYTE lh_Type; UBYTE l_pad; };
struct MsgPort { struct Node mp_Node; UBYTE mp_Flags; UBYTE mp_SigBit; APTR mp_SigTask; struct List mp_MsgList; };
struct Message { struct Node mn_Node; struct MsgPort *mn_ReplyPort; UWORD mn_Length; };

struct IORequest {
    struct Message io_Message;
    APTR   io_Device;
    APTR   io_Unit;
    UWORD  io_Command;
    UBYTE  io_Flags;
    BYTE   io_Error;
};
struct IOStdReq {
    struct Message io_Message;
    APTR   io_Device;
    APTR   io_Unit;
    UWORD  io_Command;
    UBYTE  io_Flags;
    BYTE   io_Error;
    ULONG  io_Actual;
    ULONG  io_Length;
    APTR   io_Data;
    ULONG  io_Offset;
};
struct IOExtTD {
    struct IOStdReq iotd_Req;
    ULONG iotd_Count;
    ULONG iotd_SecLabel;
};

/* trackdisk / scsidisk command constants (device path only) */
#define CMD_READ     2
#define CMD_WRITE    3
#define TD_MOTOR     9
#define TD_GETGEOMETRY 18
#define TD_READ64    24
#define TD_WRITE64   25
#define HD_SCSICMD   28
/* NSCMD_DEVICEQUERY / NSCMD_TD_READ64 intentionally NOT defined here:
 * rdb.c defines them itself (guarded by #ifndef) along with its own
 * struct NSDeviceQueryResult -- defining them here would suppress that. */

/* DosEnvec array indices (dos/filehandler.h) */
#define DE_TABLESIZE     0
#define DE_SIZEBLOCK     1
#define DE_SECORG        2
#define DE_NUMHEADS      3
#define DE_SECSPERBLK    4
#define DE_BLKSPERTRACK  5
#define DE_RESERVEDBLKS  6
#define DE_PREFAC        7
#define DE_INTERLEAVE    8
#define DE_LOWCYL        9
#define DE_UPPERCYL      10
#define DE_NUMHEADS_DUP  3
#define DE_NUMBUFFERS    11
#define DE_BUFMEMTYPE    12
#define DE_MEMBUFTYPE    12
#define DE_MAXTRANSFER   13
#define DE_MASK          14
#define DE_BOOTPRI       15
#define DE_DOSTYPE       16
#define DE_BAUD          17
#define DE_CONTROL       18
#define DE_BOOTBLOCKS    19

#define SCSIF_READ   (1<<0)
#define SCSIF_WRITE  0
#define SCSIF_AUTOSENSE (1<<2)

struct SCSICmd {
    UWORD *scsi_Data;
    ULONG  scsi_Length;
    UWORD  scsi_Actual;
    UBYTE *scsi_Command;
    UWORD  scsi_CmdLength;
    UWORD  scsi_CmdActual;
    UBYTE  scsi_Flags;
    UBYTE  scsi_Status;
    UBYTE *scsi_SenseData;
    UWORD  scsi_SenseLength;
    UWORD  scsi_SenseActual;
};

struct DriveGeometry {
    ULONG dg_SectorSize;
    ULONG dg_TotalSectors;
    ULONG dg_Cylinders;
    ULONG dg_CylSectors;
    ULONG dg_Heads;
    ULONG dg_TrackSectors;
    ULONG dg_BufMemType;
    UBYTE dg_DeviceType;
    UBYTE dg_Flags;
    UWORD dg_Reserved;
};
#define DG_DIRECT_ACCESS     0
#define DG_SEQUENTIAL_ACCESS 1
#define DG_PRINTER           2
#define DG_PROCESSOR         3
#define DG_WORM              4
#define DG_CDROM             5
#define DG_SCANNER           6
#define DG_OPTICAL_DISK      7
#define DG_MEDIUM_CHANGER    8
#define DG_COMMUNICATION     9
#define DG_UNKNOWN           31

/* NSDeviceQueryResult referenced device-side; provide a minimal form only if
 * rdb.c defines its own (it does, see rdb.c:48) so nothing needed here. */

/* errors */
#define IOERR_NOCMD (-3)

/* exec node/port types + signal alloc (device path, stubbed) */
#define NT_MSGPORT 4
#define PA_SIGNAL  0
BYTE AllocSignal(LONG bit);
void FreeSignal(LONG bit);
APTR FindTask(CONST_STRPTR name);
LONG DeleteFile(CONST_STRPTR name);

/* DosEnvec (dos/filehandler.h); PartitionBlock.pb_Environment overlays this */
struct DosEnvec {
    ULONG de_TableSize;
    ULONG de_SizeBlock;
    ULONG de_SecOrg;
    ULONG de_Surfaces;
    ULONG de_SectorPerBlock;
    ULONG de_BlocksPerTrack;
    ULONG de_Reserved;
    ULONG de_PreAlloc;
    ULONG de_Interleave;
    ULONG de_LowCyl;
    ULONG de_HighCyl;
    ULONG de_NumBuffers;
    ULONG de_BufMemType;
    ULONG de_MaxTransfer;
    ULONG de_Mask;
    LONG  de_BootPri;
    ULONG de_DosType;
    ULONG de_Baud;
    ULONG de_Control;
    ULONG de_BootBlocks;
};

/* ---- library call prototypes (implemented in amiga_shim.c) ---- */
APTR  AllocVec(ULONG size, ULONG flags);
void  FreeVec(APTR p);
APTR  AllocMem(ULONG size, ULONG flags);
void  FreeMem(APTR p, ULONG size);
void  CopyMem(const void *src, void *dst, ULONG size);

BPTR  Open(CONST_STRPTR name, LONG mode);
void  Close(BPTR fh);
LONG  Read(BPTR fh, void *buf, LONG len);
LONG  Write(BPTR fh, const void *buf, LONG len);
LONG  Seek(BPTR fh, LONG pos, LONG mode);
LONG  SetFileSize(BPTR fh, LONG pos, LONG mode);
LONG  IoErr(void);

struct MsgPort *CreateMsgPort(void);
void  DeleteMsgPort(struct MsgPort *p);
LONG  OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *io, ULONG flags);
void  CloseDevice(struct IORequest *io);
BYTE  DoIO(struct IORequest *io);
struct IORequest *CreateIORequest(struct MsgPort *p, ULONG size);
void  DeleteIORequest(APTR io);

#endif /* AMIGA_COMPAT_H */
