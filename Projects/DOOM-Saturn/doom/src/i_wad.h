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

/* Drain one entry from the background cart-fill queue. Called once
 * per VBlank from saturn_vblank_callback so the cart progressively
 * fills with HOT lump bodies during gameplay instead of holding up
 * boot for seconds. Returns 1 if a copy was performed, 0 if the
 * queue was empty or disabled. */
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

/* Probe + init the extended-RAM cart. Idempotent; returns 1 when a
 * cart is present and initialized. Safe to call from main.c before
 * I_WadBase for the boot report. */
int  Saturn_CartInit(void);

/* 1 if Saturn_CartInit succeeded (cart present). */
int  Saturn_CartPresent(void);

/* Z-zone base in the cart's upper half (cached mirror), or NULL when
 * no cart / cart too small. *size receives the zone size in bytes. */
unsigned char *Saturn_ZoneBase(int *size);

/* Boot-probe results for the on-screen boot report. */
extern unsigned char saturn_cart_id_seen;   /* raw ID byte from 0x24FFFFFF */
extern int           saturn_cart_rw_ok;     /* 3-point write/verify passed */
extern int           saturn_cart_rw_point; /* 0 = passed; failure codes 1-9, see Saturn_CartInit */
extern unsigned int  saturn_cart_base_addr;  /* mirror the cart answers on (0x22400000 or 0x42400000) */
extern int           saturn_cart_probe_failed; /* cart seen but not writable — CD-stream mode */
extern unsigned int  saturn_cart_size;       /* cart size in bytes (0 = none) */

#endif /* __SATURN_I_WAD_H__ */