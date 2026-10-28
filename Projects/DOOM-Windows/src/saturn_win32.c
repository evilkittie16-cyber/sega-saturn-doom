/* saturn_win32.c - Win32 HAL, File IO, Memory Buffers & Stubs */

#include "saturn_win32.h"
#include "doomdef.h"
#include "dstrings.h"
#include "i_system.h"
#include "i_wad.h"
#include "r_local.h"

/* Simulated Sega Saturn Hardware Memory */
uint8_t saturn_win32_wram_l[0x100000];   /* 1 MB Low Work RAM */
uint8_t saturn_win32_wram_h[0x100000];   /* 1 MB High Work RAM */
uint8_t saturn_win32_vdp1_vram[0x80000]; /* 512 KB VDP1 VRAM */
uint8_t *saturn_win32_iwad = NULL;
uint8_t *saturn_win32_zone = NULL;

/* Performance timer emulation */
static LARGE_INTEGER qpc_freq;
static LARGE_INTEGER qpc_start;
static int qpc_inited = 0;

unsigned short Win32_GetFRT(void)
{
    if (!qpc_inited)
    {
        QueryPerformanceFrequency(&qpc_freq);
        QueryPerformanceCounter(&qpc_start);
        qpc_inited = 1;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    /* ~3.6 MHz equivalent tick (SH-2 FRT) */
    double elapsed_sec = (double)(now.QuadPart - qpc_start.QuadPart) / (double)qpc_freq.QuadPart;
    unsigned int frt_ticks = (unsigned int)(elapsed_sec * 3686400.0);
    return (unsigned short)(frt_ticks & 0xFFFF);
}

/* --- File Loading & Asset Streaming --- */

int Saturn_Leaf_BlobRead(void *buf, unsigned cap, const char *fname)
{
    const char *prefixes[] = {
        "cd/%s",
        "%s",
        "Projects/DOOM-Saturn/cd/%s",
        "Projects/DOOM-Saturn/%s",
        "../DOOM-Saturn/cd/%s",
        "../DOOM-Saturn/%s",
        "../../DOOM-Saturn/cd/%s",
        "../../DOOM-Saturn/%s"
    };
    char path[260];
    FILE *f = NULL;
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), prefixes[i], fname);
        f = fopen(path, "rb");
        if (f) break;
    }
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (buf == NULL) {
        fclose(f);
        return (int)sz;
    }

    size_t to_read = (size_t)sz;
    if (to_read > cap) to_read = cap;
    size_t rd = fread(buf, 1, to_read, f);
    fclose(f);
    return (int)rd;
}

byte *I_WadBase(void)
{
    if (saturn_win32_iwad)
        return saturn_win32_iwad;

    const char *names[] = {
        "cd/DOOM.WAD", "DOOM.WAD", "cd/doom.wad", "doom.wad",
        "cd/DOOM1.WAD", "DOOM1.WAD", "cd/doom1.wad", "doom1.wad",
        "../DOOM-Saturn/cd/DOOM.WAD", "../DOOM-Saturn/cd/doom.wad",
        "../DOOM-Saturn/cd/DOOM1.WAD", "../DOOM-Saturn/cd/doom1.wad",
        "../DOOM-Saturn/DOOM.WAD", "../DOOM-Saturn/doom.wad",
        "../DOOM-Saturn/DOOM1.WAD", "../DOOM-Saturn/doom1.wad",
        "../../DOOM-Saturn/cd/DOOM.WAD", "../../DOOM-Saturn/cd/doom.wad",
        "../../DOOM-Saturn/cd/DOOM1.WAD", "../../DOOM-Saturn/cd/doom1.wad"
    };
    FILE *f = NULL;
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        f = fopen(names[i], "rb");
        if (f) break;
    }

    if (!f) {
        MessageBoxA(NULL, "Failed to find DOOM.WAD!\nPlease make sure cd/DOOM.WAD or ../DOOM-Saturn/cd/DOOM.WAD exists.", "DOOM-Windows Error", MB_OK | MB_ICONERROR);
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    saturn_win32_iwad = (uint8_t *)malloc(sz + 16);
    if (!saturn_win32_iwad) {
        fclose(f);
        MessageBoxA(NULL, "Out of memory allocating IWAD buffer", "DOOM-Windows Error", MB_OK | MB_ICONERROR);
        exit(1);
    }

    fread(saturn_win32_iwad, 1, sz, f);
    fclose(f);

    wadinfo_t *hdr = (wadinfo_t *)saturn_win32_iwad;
    hdr->numlumps = LITTLELONG(hdr->numlumps);
    hdr->infotableofs = LITTLELONG(hdr->infotableofs);

    wadfile[0].fileptr = saturn_win32_iwad;
    wadfile[0].cdlength = 0; /* Direct in-memory fast access */
    wadfile[0].cdoffset = 0;
    wadfile[0].numlumps = hdr->numlumps;
    wadfile[0].lumpinfo = (lumpinfo_t *)(saturn_win32_iwad + hdr->infotableofs);

    for (int i = 0; i < hdr->numlumps; i++) {
        wadfile[0].lumpinfo[i].filepos = LITTLELONG(wadfile[0].lumpinfo[i].filepos);
        wadfile[0].lumpinfo[i].size = LITTLELONG(wadfile[0].lumpinfo[i].size);
    }

    saturn_wad_open = 1;
    saturn_wad_size = sz;
    return saturn_win32_iwad;
}

byte *I_ZoneBase(int *size)
{
    *size = 32 * 1024 * 1024; /* 32 MB zone */
    if (!saturn_win32_zone) {
        saturn_win32_zone = (byte *)malloc(*size);
        if (!saturn_win32_zone) {
            MessageBoxA(NULL, "Out of memory allocating zone buffer", "DOOM-Windows Error", MB_OK | MB_ICONERROR);
            exit(1);
        }
    }
    return saturn_win32_zone;
}

int saturn_wad_open = 0;
int saturn_wad_size = 0;
volatile int saturn_wad_cold_reads = 0;
unsigned char saturn_slave_enabled = 0;
volatile int dbg_pak_stage = 0;
volatile int saturn_wall_dbg = 0;
volatile int saturn_isr_paint = 0;
volatile int saturn_vbl_dead_verdict = 0;
volatile int saturn_demo_active = 0;
volatile int saturn_frame_done_vbl = 0;
volatile int saturn_isr_dirty = 0;
volatile int saturn_rv_done = 0;
volatile int saturn_sw1_built = 0;
volatile int saturn_sw1_ok = 0;
volatile int saturn_sw1_paint = 0;
volatile int saturn_vbl_dead = 0;
volatile int saturn_load_count = 0;

void saturn_vdp2_stage_fill(jo_color color) { (void)color; }
int Saturn_Demo_FeedTic(int real_pad) { (void)real_pad; return 0; }
int Saturn_Demo_Available(void) { return 0; }
int Saturn_Demo_Exists(int num) { (void)num; return 0; }
int Saturn_Demo_Open(int num) { (void)num; return 0; }
void Saturn_Demo_Begin(void) { }
void Saturn_Demo_Close(void) { }
void Saturn_RenderInvalidateLevel(void) { }
extern void Win32_PumpEvents(void);
void I_WaitVBL(int count) {
    extern volatile unsigned int saturn_vbl_count;
    if (count < 1) count = 1;
    for (int i = 0; i < count; i++) {
        Win32_PumpEvents();
        Sleep(16);
        saturn_vbl_count++;
    }
}
int I_GetTime(void) { extern volatile unsigned int saturn_vbl_count; return (int)(saturn_vbl_count / 2); }
void *I_PointStableLump(int lump) { return W_POINTLUMPNUM(lump); }

static int s_cd_pos = 0;
int I_OpenCDFileByName(const char *name, int *poffset) {
    if (poffset) *poffset = 0;
    if (name && name[0] != '\0') return -1;
    return saturn_wad_size;
}
void I_OpenCDFileByOffset(int length, int offset) { (void)length; (void)offset; }
int I_SeekCDFile(int offset, int whence) { (void)whence; s_cd_pos = offset; return 0; }
int I_ReadCDFile(int length) { s_cd_pos += length; return length; }
void *I_GetCDFileBuffer(void) { return saturn_win32_iwad ? (saturn_win32_iwad + s_cd_pos) : NULL; }

int Saturn_LumpResident(int lump, const unsigned char **cart_ptr_out, unsigned int *cd_off_out)
{
    if (lump < 0 || lump >= wadfile[0].numlumps) return 0;
    if (cart_ptr_out) *cart_ptr_out = saturn_win32_iwad + wadfile[0].lumpinfo[lump].filepos;
    if (cd_off_out) *cd_off_out = wadfile[0].lumpinfo[lump].filepos;
    return 1;
}

int saturn_iwad_pread(unsigned int pos, void *dest, unsigned int nbytes)
{
    if (!saturn_win32_iwad || !dest) return 0;
    if (pos + nbytes > (unsigned int)saturn_wad_size) return 0;
    memcpy(dest, saturn_win32_iwad + pos, nbytes);
    return (int)nbytes;
}

static byte *pvs_blob = NULL;
static unsigned short pvs_nssec = 0;
static unsigned short pvs_rowbytes = 0;

int Saturn_Pvs_Parse(byte *blob, unsigned fsize)
{
    unsigned nssec, rowbytes, need;
    if (!blob || fsize <= 16) return 0;
    if (blob[0] != 'P' || blob[1] != 'V' || blob[2] != 'S' || blob[3] != '1'
        || (((unsigned)blob[4] << 8) | blob[5]) != 1) return 0;
    nssec = ((unsigned)blob[6] << 8) | blob[7];
    rowbytes = ((unsigned)blob[8] << 8) | blob[9];
    if (nssec != (unsigned)numsubsectors || rowbytes != ((nssec + 7) >> 3)) return 0;
    need = 16u + nssec * rowbytes;
    if (need > (unsigned)fsize) return 0;
    pvs_blob = blob + 16;
    pvs_nssec = (unsigned short)nssec;
    pvs_rowbytes = (unsigned short)rowbytes;
    return 1;
}

void Saturn_PvsLevelLoad(const char *lumpname)
{
    pvs_blob = NULL;
    pvs_nssec = 0;
    pvs_rowbytes = 0;
    if (!lumpname || netgame) return;
    char fname[16];
    int k;
    for (k = 0; k < 8 && lumpname[k]; k++) {
        char c = lumpname[k];
        if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
        fname[k] = c;
    }
    fname[k++] = '.'; fname[k++] = 'P'; fname[k++] = 'V'; fname[k++] = 'S'; fname[k] = '\0';
    byte *blob = (byte *)malloc(131072);
    if (!blob) return;
    int fsize = Saturn_Leaf_BlobRead(blob, 131072, fname);
    if (fsize <= 16 || !Saturn_Pvs_Parse(blob, (unsigned)fsize)) {
        free(blob);
        return;
    }
}

static int seg_pvs_locate(fixed_t x, fixed_t y)
{
    int nodenum = (int)numnodes - 1;
    while ((int16_t)nodenum >= 0) {
        node_t *bsp = &nodes[nodenum];
        nodenum = (int)bsp->children[R_PointOnSide(x, y, bsp)];
    }
    return (nodenum == -1) ? 0 : (nodenum & ~NF_SUBSECTOR);
}

const byte *Saturn_Pvs_GetRow(fixed_t x, fixed_t y)
{
    if (pvs_blob != NULL) {
        int leaf = seg_pvs_locate(x, y);
        if (leaf >= 0 && leaf < pvs_nssec) {
            return pvs_blob + (unsigned)leaf * pvs_rowbytes;
        }
    }
    return NULL;
}

int Saturn_CartInit(void) { return 1; }
int Saturn_CartPresent(void) { return 1; }
int Saturn_DspProbe(void) { return 1; }
int Saturn_Pak_LevelLoad(const char *lumpname) { (void)lumpname; return 0; }

int saturn_iwad_background_fill(void) { return 0; }
void saturn_iwad_fill_pump(void) { }
void saturn_iwad_fill_drain_all(void) { }
void Saturn_BupPrealloc(void) { }

void saturn_slave_init(void) { }
void saturn_slave_frame_tick(void) { }

/* SGL & VDP1 display stubs */
int saturn_quiet_hud = 1;
void Saturn_Cram_ShadedBanks(void) { }
void slPriority(int a, int b) { (void)a; (void)b; }
void slSpriteColMode(int m) { (void)m; }
void slSpriteType(int t) { (void)t; }
void slInitSynch(void) { }
void slSynchFunction(void (*fn)(void)) { (void)fn; }

/* --- Telemetry & Boot Stubs --- */
void Saturn_StageColor(jo_color color) { (void)color; }
void Saturn_PaintNow(jo_color color) { (void)color; }
void Saturn_StageTextHi(int x, int y, const char *s) { (void)x; (void)y; (void)s; }
void Saturn_BootText(int x, int y, const char *str) { (void)x; (void)y; (void)str; }
void Saturn_SetupTag(const char *str) { (void)str; }
void Saturn_BootMsg(const char *msg) { (void)msg; }
void Saturn_BootRec(int n) { (void)n; }
void Saturn_BootRec_PaintPrev(void) { }
void Saturn_PumpFrame(void) { }
void Saturn_GsubStamp(unsigned int sub) { (void)sub; }

int Saturn_Format(char *out, int outsz, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int res = vsnprintf(out, outsz, fmt, args);
    va_end(args);
    return res;
}

void Saturn_Udec(char *out, unsigned int v)
{
    sprintf(out, "%u", v);
}

void Saturn_Udec0(char *out, unsigned int v, int width)
{
    char fmt[16];
    sprintf(fmt, "%%0%du", width);
    sprintf(out, fmt, v);
}

void Saturn_Uhex(char *out, unsigned int v)
{
    sprintf(out, "%X", v);
}

/* Error reporting */
void I_Error(char *error, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, error);
    vsnprintf(buf, sizeof(buf), error, args);
    va_end(args);
    printf("\nFATAL I_Error: %s\n\n", buf);
    fflush(stdout);
    fprintf(stderr, "\nFATAL I_Error: %s\n\n", buf);
    fflush(stderr);
    MessageBoxA(NULL, buf, "DOOM-Windows Error", MB_OK | MB_ICONERROR);
    exit(1);
}

void I_Quit(void)
{
    exit(0);
}
