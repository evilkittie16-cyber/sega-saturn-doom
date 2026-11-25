/* i_wad.h — Saturn-side WAD loader API. Shared by i_wad.c (which owns
 * the IWAD directory buffer + the direct-GFS I/O) and d32xr_stubs.c
 * (which hosts the I_SeekCDFile / I_ReadCDFile / I_GetCDFileBuffer
 * shims that d32xr's w_wad.c calls when streaming from CD).
 *
 * Header fields are deliberately exposed (not static) so the stubs
 * file can advance saturn_cd_seek_pos across the CD-read chain.
 */

#ifndef __SATURN_I_WAD_H__
#define __SATURN_I_WAD_H__

#include <jo/jo.h>

/* Set to 1 when the IWAD was opened (one permanent GFS handle inside
 * i_wad.c — the IWAD deliberately never goes through Jo's jo_fs_*
 * wrapper; see the direct-GFS comment there). The stubs consult this
 * to decide whether to honor CD-read calls. */
extern int      saturn_wad_open;

/* Total IWAD size in bytes (computed from infotableofs + numlumps*16).
 * Returned by I_OpenCDFileByName so callers can detect "file present". */
extern unsigned saturn_wad_size;

/* Current absolute byte position in the IWAD. Advanced by I_SeekCDFile
 * (SEEK_SET) and by I_ReadCDFile (auto-increment after each read).
 * Used as the offset argument to saturn_iwad_pread. */
extern int      saturn_cd_seek_pos;

/* Read nbytes from IWAD at byte 0 (the 12-byte header, right after
 * open). Implemented as pread(0). */
int  saturn_iwad_read(void *dest, unsigned int nbytes);

/* Read nbytes from IWAD at absolute byte position pos: GFS_Seek to the
 * containing sector + staged sequential GFS_Freads. Returns nbytes or
 * -1 (reported through DOOM's I_Error, never Jo's blue screen). */
int  saturn_iwad_pread(unsigned int pos, void *dest, unsigned int nbytes);

/* Drain the background cart-fill queue for one frame. Called once per
 * displayed frame from the MAIN LOOP (D_DoomLoop) so the cart
 * progressively fills with HOT lump bodies during gameplay instead of
 * holding up boot for seconds. NEVER call it from an interrupt handler
 * (VBlank callback) or the slave CPU: it shares the IWAD handle and the
 * sector staging buffer with saturn_iwad_pread. Returns the number of
 * queue entries completed this frame (0 if the queue was empty or
 * disabled; negative when a copy failed and the queue was stopped). */
int  saturn_iwad_background_fill(void);

/* Light pump for the async cart-fill batch (no issuing): advances a
 * pending GFS_NwFread without touching anything else. Safe to call from
 * any main-thread idle spin; no-op when no batch is pending. */
void saturn_iwad_fill_pump(void);

/* Blocking full drain: loops saturn_iwad_background_fill until the
 * prewarm queue is empty (or a 60 s CD-stall watchdog fires). Call once
 * before the first rendered frame so no CD/DMA/cart traffic ever
 * overlaps rendering. */
void saturn_iwad_fill_drain_all(void);

/* Probe + init the extended-RAM cart. The probe runs once; later calls
 * return the remembered result (1 = a usable cart, 0 = none, unknown,
 * or failed write/verify -> CD-stream mode). Safe to call from main.c
 * before I_WadBase for the boot report. */
int  Saturn_CartInit(void);

/* 1 if Saturn_CartInit succeeded (cart present and usable). */
int  Saturn_CartPresent(void);

/* Z-zone base in the cart's upper half (cache-through address
 * 0x226xxxxx by default; the cached view 0x026xxxxx only with
 * SATURN_ZONE_CACHED), or NULL when no cart / cart smaller than 4 MB.
 * *size receives the zone size in bytes. */
unsigned char *Saturn_ZoneBase(int *size);

/* Boot-probe results for the on-screen boot report. */
extern unsigned char saturn_cart_id_seen;   /* raw ID: low byte of the word at 0x24FFFFFE */
extern int           saturn_cart_rw_ok;     /* every write/verify passed */
extern int           saturn_cart_rw_point; /* 0 = passed; 1-3 = 16-bit probe, 7-9 = 32-bit
                                            * probe, 10-12 = size march at 512K/1M/2M,
                                            * 20-22 = RAM mirrors from 512K/1M/2M (cart
                                            * kept at that size; see Saturn_CartInit) */
extern unsigned int  saturn_cart_base_addr;  /* 0x22400000 when the cart answered, 0 otherwise */
extern int           saturn_cart_probe_failed; /* cart seen but not writable — CD-stream mode */
extern int           saturn_cart_xram_state; /* SAROO XRAM: 0 = none/not eligible, 1 = 8 MB in use,
                                              * 31-40 = found but failed a check (unused) */
extern unsigned int  saturn_cart_size;       /* usable cart bytes for the hot-lump fill (0 = none;
                                              * 512 KB on a 1 MB cart: bank 0 only; the
                                              * march-verified size on a 4 MB cart, or
                                              * 1008 KB with -DSATURN_CART_CLAMP_1MB=1) */

#endif /* __SATURN_I_WAD_H__ */