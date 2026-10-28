/*
** DOOM for Sega Saturn — Jo Engine port
** main.c — thin boot: init video core, load the boot font, probe the
** extended-RAM cart, show a ~2-second probe report, then hand off to
** D_DoomMain (cart init is idempotent — the real WAD load reuses the
** cached probe result).
**
** Boot text uses the VDP1 sprite font (cd/FONT.TGA) — the proven-visible
** text channel under SGL. The font maps uppercase letters, digits and
** symbols only (no lowercase, no space), so report lines are composed
** accordingly.
**
** Because text and sprites depend on the VDP1 path that black screens
** can hide, each boot stage ALSO paints the NBG1 background bitmap a
** solid color (Saturn_StageColor — the channel jo_core_init and Jo's
** own error screen use, independent of VDP1). Decode table:
**   DARK BLUE  program reached jo_main (display + VDP2 alive)
**   OLIVE      jo_core_init returned; CD spin-up wait (~2 s) running.
**              Held here = VBlank chain dead (the wait is vbl-gated)
**   HALF BLUE  spin-up done — first CD access (font) is NEXT. Held
**              here = GFS/CD-block hang on the first read
**   MAGENTA    font load RETURNED — cart probe next; a hang before
**              this color is CD, never the cart
**   PINE       cart CS2 ID byte read returned (i_wad.c)
**   SMOKE      cart mirror write-probes entered (first cart WRITE)
**   BLUE       font loaded + 4MB cart probe OK
**   RED        font failed to load (persists — not overwritten)
**   GRAY       font OK, no cart (CD streaming mode)
**   DARK GREEN stuck inside boot_report() (font draw crash)
**   DARK YELLOW VBlank interrupt chain dead (tick never fires)
**   CYAN       slSynch() never returns (stuck BEFORE white ever shows)
**   DARK CYAN  all boot primitives proven, holding the report
**   ORANGE     SCU DSP probe passed (load/start/DMA round trip OK)
**   DARK RED   SCU DSP probe failed — DSP offload is not viable here
**   WHITE      report loop done, D_DoomMain entered
**   YELLOW     I_InitVideo OK, WAD directory loading (d_main.c)
**   MEDIUM GRN IWAD opened, 12-byte header validated — reading the lump
**              directory (the ~11.8 MB GFS_Seek on the rerelease WAD)
**   SEMI GREEN directory parsed — classifying lumps / sync-copying
**              PLAYPAL/COLORMAP/FONT into cart (i_wad.c)
**   SEMI YELLOW first-frame-critical lump sync-copying CD->cart — the
**              lump name is drawn one row under LOADING WAD (i_wad.c)
**   GREEN      I_WadBase complete — Z_Init + player arrays next
**   GRAY BANDS I_InitVideo display smoke test — bands visible = the
**              whole fb-copy->NBG0-bitmap->slSynch path works (the
**              VDP2 8bpp direct display; VDP1 is out of the frame path)
**   PURPLE     WAD + Z zone + players OK, level setup starting
**   CYAN       entering P_SetupLevel ("STARTING" on screen)
**   TEAL       r95 wedge ladder: inside P_LoadingPlaque
**   PINK       r95 wedge ladder: inside P_InitThinkers
**   VIOLET     r95 wedge ladder: inside R_ClearTextures
**   WHITE      r95 wedge ladder: site-5 stamp + sky defaults
**   CHARTREUSE r95 wedge ladder: inside W_LoadPWAD (CD streaming)
**   SALMON     r95 wedge ladder: inside W_CheckNumForName
**              (colors creeping ~2s apart = dead vblank chain, holds
**              timing out — the ISR is wedged, not the loader)
**   DARK YELLOW map lump found — P_Load* chain next
**   (FIRST-FRAME r97 TEXT LADDER — WHITE framebuffer glyphs
**   (palette index 2), visible over any stage fill on real HW;
**   r96's index-0 glyphs showed as back-color DarkBlue (0,0,8) on
**   the dark-blue m3 fill (0,0,24) — painted but invisible:)
**   "A"   master half of the first render frame RETURNED (same point
**         as the dark-blue m3 fill)
**   "B"   entering the join block
**   "C"   the join POLL loop is about to be entered
**   "D"   the join RETURNED (poll + re-posts + wait all completed)
**   "SLVDEAD" the slave never serviced 12 re-posts — degraded to
**         master-only rendering
**   LIVE rows inside a stalled poll (redrawn ~10x/s):
**   B:xx      master's beat — CYCLING = master alive in the poll;
**             FROZEN = wedged entering the loop body / hard crash
**   E=xxxx    0 = slave never entered its callback (dispatch lost);
**             5217 = it entered (then S says where it is)
**   S=xxxx    slave phase: A000 armed / A001 entered / A002 ticket /
**             A003 purged+rendering / A004 half done / DEAD bailed
**   (red verdict fires -> red/magenta fill + full report rows
**   replace the ladder; then the recorder save runs)
**   MEDIUM PURPLE streaming the nine map lumps — the tag on screen
**              names the guilty lump (LOAD-LINEDEFS .. LOAD-BLOCKMAP)
**   SEMI GREEN all nine map structs loaded (every COLD read returned)
**   SEMI YELLOW P_GroupLines done
**   MEDIUM GRN P_SortSegs done
**   GRAY       P_LoadThings done (all mobjs spawned)
**   DARK GREEN sky / aux-texture post-pass done
**   MEDIUM BLUE R_FixupTextures entered (texture loop; its own
**              MEDIUM PURPLE FIX-FLAT marker fires between the texture
**              and flat loops) — persistent here = texture-loop hang
**   MEDIUM PURPLE R_FixupTextures texture loop done, flat loop next —
**              persistent here = flat-loop hang
**   SEMI CYAN  R_FixupTextures returned, P_SpawnSpecials entered —
**              persistent here = specials hang. Inside it, a tag at
**              y=100 + its color name the exact stage:
**                SPC-SECTORS   SEMI CYAN     sector loop entered
**                CASE-FLICK    SEMI BLUE     P_SpawnLightFlash
**                CASE-STROBE   MEDIUM CYAN   P_SpawnStrobeFlash
**                CASE-STROBE-SY DARK RED     synced strobe
**                CASE-GLOW     SEMI PURPLE   P_SpawnGlowingLight
**                CASE-FIREFLK  DARK PURPLE   P_SpawnFireFlicker
**                CASE-DOOR-C30 SEMI RED      P_SpawnDoorCloseIn30
**                CASE-DOOR-R5M MEDIUM RED    P_SpawnDoorRaiseIn5Mins
**                SPC-LINES     DARK GRAY     line 48/142 scan
**                SPC-MISC      MEDIUM YELLOW trailing memsets
**                SPC-DONE      GREEN         (flash — STBAR white next)
**              RED "ZMALLOC-SCAN-LOOP" = corrupted zone free-list
**              cycle; RED "ZMALLOC-BADSIZE" = negative/oversize
**              request (garbage header upstream)
**   MEDIUM WHITE ST_InitEveryLevel entered — persistent here =
**              status-bar-init hang
**   WHITE      P_SetupLevel complete (specials + ST_Init done)
**   DARK CYAN  entering D_DoomLoop ("IN-LOOP") — from here the DOOM
**              frame should cover the screen; persistent DARK CYAN =
**              hang inside the loop (first background fill / render)
**   RED/BLACK  flashing = I_Error fired (message drawn if font works)
*/

#include <stdio.h>

#include <jo/jo.h>

#include "doomdef.h"
#include "d_main.h"
#include "i_system.h"
#include "i_wad.h"
#include "saturn_dsp.h"

/* SCU DSP capability probe result (Saturn_DspProbe, main.c stage 3.5).
 * 1 = the DSP can be loaded, started and DMA to/from WRAM-H — the
 * gating fact for the flat-painter offload. */
int saturn_dsp_ok = -1;

static void boot_report_line(int y, const char *str)
{
    Saturn_BootText(8, y, str);
}

/* VBlank tick registered right after jo_core_init so the boot can test
 * whether the SGL VBlank-in interrupt chain is firing before it trusts
 * slSynch (which waits on the same interrupt). */
/* Non-static: i_system.c's Saturn_StageColor hold waits on this counter
 * so every boot-stage colour is visible long enough to photograph. */
volatile unsigned int boot_vbl_count;
static void boot_vbl_tick(void)
{
    boot_vbl_count++;
}

/* Redraw the probe report. Call once per frame; the SGL VDP1 command
 * table is transient, so every visible frame must be re-issued.
 * NOTE: no snprintf here — the toolchain builds with -nodefaultlibs
 * -nostdlib, and the SGL library's *printf family is unproven; static
 * strings only. */
static void boot_report(void)
{
    char idline[16];

    boot_report_line(8,  "DOOM-SATURN");

    /* Row 12 — the raw CS2 ID byte the cart slot answered with, in hex.
     * Manual hex (no snprintf — see the note above). 00 = nothing on the
     * bus; FF = pulled high (also "nothing"); 5A/1A/DA = 1 MB DRAM;
     * 5C = 4 MB DRAM; anything else = flashcart-specific. */
    idline[0] = 'C'; idline[1] = 'A'; idline[2] = 'R'; idline[3] = 'T';
    idline[4] = '-'; idline[5] = 'I'; idline[6] = 'D'; idline[7] = '-';
    idline[8] = "0123456789ABCDEF"[(saturn_cart_id_seen >> 4) & 0xF];
    idline[9] = "0123456789ABCDEF"[saturn_cart_id_seen & 0xF];
    idline[10] = 0;
    boot_report_line(12, idline);

    if (Saturn_CartPresent())
    {
        /* Point codes: 1/2/3 = 16-bit write/verify failed at 0x22400000
         * (start/middle/last word), 4/5/6 = same at the 0x42400000
         * mirror, 7/8/9 = 32-bit longword failed on the chosen base
         * (memcpy needs longword writes to work). */
        if (saturn_cart_id_seen == 0x5C)
        {
            if (saturn_cart_rw_ok)
                boot_report_line(28, "CART-4MB-RW-OK");
            else if (saturn_cart_rw_point == 1) boot_report_line(28, "CART-4MB-RW-FAIL-1");
            else if (saturn_cart_rw_point == 2) boot_report_line(28, "CART-4MB-RW-FAIL-2");
            else if (saturn_cart_rw_point == 3) boot_report_line(28, "CART-4MB-RW-FAIL-3");
            else if (saturn_cart_rw_point == 4) boot_report_line(28, "CART-4MB-RW-FAIL-4");
            else if (saturn_cart_rw_point == 5) boot_report_line(28, "CART-4MB-RW-FAIL-5");
            else if (saturn_cart_rw_point == 6) boot_report_line(28, "CART-4MB-RW-FAIL-6");
            else if (saturn_cart_rw_point == 7) boot_report_line(28, "CART-4MB-RW-FAIL-7");
            else if (saturn_cart_rw_point == 8) boot_report_line(28, "CART-4MB-RW-FAIL-8");
            else if (saturn_cart_rw_point == 9) boot_report_line(28, "CART-4MB-RW-FAIL-9");
            else boot_report_line(28, "CART-4MB-RW-FAIL");
        }
        else if (saturn_cart_id_seen == 0x5A)
        {
            if (saturn_cart_rw_ok)
                boot_report_line(28, "CART-1MB-RW-OK");
            else if (saturn_cart_rw_point == 1) boot_report_line(28, "CART-1MB-RW-FAIL-1");
            else if (saturn_cart_rw_point == 2) boot_report_line(28, "CART-1MB-RW-FAIL-2");
            else if (saturn_cart_rw_point == 3) boot_report_line(28, "CART-1MB-RW-FAIL-3");
            else if (saturn_cart_rw_point == 7) boot_report_line(28, "CART-1MB-RW-FAIL-7");
            else if (saturn_cart_rw_point == 8) boot_report_line(28, "CART-1MB-RW-FAIL-8");
            else if (saturn_cart_rw_point == 9) boot_report_line(28, "CART-1MB-RW-FAIL-9");
            else boot_report_line(28, "CART-1MB-RW-FAIL");
        }
        else
            boot_report_line(28, saturn_cart_rw_ok ? "CART-RW-OK"
                                                   : "CART-RW-FAIL");
        /* Which virtual address the cart answers on — the user's proven
         * tester runs on 0x22400000; 0x42400000 is the fallback. */
        if (saturn_cart_base_addr == 0x22400000u)
            boot_report_line(60, "BASE-22400000");
        else if (saturn_cart_base_addr == 0x42400000u)
            boot_report_line(60, "BASE-42400000");
        else
            boot_report_line(60, "BASE-NONE");
        boot_report_line(44, "CART-WAD-MODE");
    }
    else if (saturn_cart_probe_failed)
    {
        /* Cart seen on the bus (ID byte ok) but not writable through
         * either mirror — running in CD-stream mode. Point code: 1/2/3 =
         * 0x22400000 failed start/middle/end, 4/5/6 = 0x42400000. */
        char    line[32];
        Saturn_Format(line, (int)sizeof(line), "CART-RW-FAIL-%d", saturn_cart_rw_point);
        boot_report_line(28, line);
        boot_report_line(44, "CD-STREAM-MODE");
    }
    else
    {
        boot_report_line(28, "NO-RAM-CART");
        boot_report_line(44, "CD-STREAM-MODE");
    }

    /* SCU DSP capability probe (stage 3.5) — -1 = not yet run. */
    if (saturn_dsp_ok == 1)
        boot_report_line(68, "SCU-DSP-OK");
    else if (saturn_dsp_ok == 0)
        boot_report_line(68, "SCU-DSP-FAIL");
    else
        boot_report_line(68, "SCU-DSP-..");

    boot_report_line(84, "BOOTING");
}

void    jo_main(void)
{
    int frame;

    /* Stage 1 — DARK BLUE: jo_core_init painted the NBG1 bitmap with
     * this color. If the screen shows anything else, the program isn't
     * even reaching jo_main (bad boot / wrong ISO), not a code bug. */
    jo_core_init(JO_COLOR_DarkBlue);
    /* BOOT-WEDGE GATE (2026-09-09, made PERMANENT): SGL's VBlank-OUT
     * ISR (0x6051040, GBR=0x060FFC00) runs a gated SCU-DMA section
     * every vblank. Its spin at 0x60510EA waits for bit 2 of the
     * DMA-end flag @(188,gbr) = 0x060FFCBC — but that flag depends on
     * SGL's sprite sort-table DMA completing, which this build never
     * uses (we own VDP1 with our own command pipeline; SortList still
     * points at BSS garbage). Caught PC=0x060510EA in boot1/2/3.yss
     * (tick-1456 DarkCyan family) and, after menu-up, ~60 nesting
     * levels in boot4.yss (Kronos, ch0 DMA stalled 0x8FC words short,
     * flag never completes, every vblank nests — boot1/2/3 only
     * reached it sooner by luck of CD timing).
     * All three entry paths of the section (gate checks at 0x6051070 /
     * 0x60510A4 / 0x60510CC) exit when bit 1 of the gate byte @(27,gbr)
     * = 0x060FFC1B is SET (tst #2 / bf.s skip). slInitInterrupt is the
     * byte's ONLY writer and finished inside jo_core_init, so this
     * write persists. The VBlank-IN handler's section (bit 0) is NOT
     * gated — slSynch's frame flag is untouched. Write the CACHED
     * address — the ISR reads via cached GBR, and SH-2 cache is write-
     * through, so the WRAM cell is coherent for it.
     * ZERO BSS: one byte RMW, __bend unchanged. */
    *(volatile unsigned char *)0x060FFC1Bu |= 0x02u;
    /* The VBlank tick MUST be registered before the first StageColor
     * hold — Saturn_StageColor now waits on boot_vbl_count to give each
     * stage marker a photographable hold (~10 vblanks). */
    jo_core_add_vblank_callback(boot_vbl_tick);
    /* OLIVE (8,8,0): jo_core_init returned. This color also covers the
     * CD SPIN-UP WAIT: a real drive needs ~1-2 s before the first GFS
     * access succeeds (Kronos' instant "drive" never exposed this —
     * the 2026-09-03 real-HW bring-up hung on dark blue exactly here).
     * Held on OLIVE = the VBlank chain is dead (boot_vbl_count frozen),
     * because the wait below is the only vbl-gated loop in this window. */
    Saturn_StageColor(JO_COLOR_SATURN_RGB(8, 8, 0));
    while (boot_vbl_count < 120u) { }   /* ~2 s: real CD spin-up */
    /* HALF BLUE (0,16,24): spin-up wait done — first CD access (the
     * font) is next. Held here = GFS/CD-block hang on the first read. */
    Saturn_StageColor(JO_COLOR_SATURN_RGB(0, 16, 24));

    /* Stage 2/3 — font then cart probe. If the font failed we paint
     * RED and LEAVE it; otherwise bright BLUE = font + cart OK. */
    Saturn_BootFontInit();
    /* MAGENTA (24,0,24): font load RETURNED — hang before this color
     * points at the first CD/GFS access, never at the cart. */
    Saturn_StageColor(JO_COLOR_SATURN_RGB(24, 0, 24));
    (void)Saturn_CartInit();
    if (!saturn_boot_font)
        Saturn_StageColor(JO_COLOR_Red);
    else
        Saturn_StageColor(Saturn_CartPresent() ? JO_COLOR_Blue : JO_COLOR_Gray);

    /* CART-ID BLINK (2026-09-03 real-HW bring-up): the report screen's
     * CART-ID line is VDP1 text — invisible on the user's TV, and the
     * Saroo's cart-slot answer decides the whole WAD strategy (RAM cart
     * = instant hot-lump copies; no cart = all-CD streaming where the
     * flat fixup loop takes minutes). Encode the raw CS2 ID byte as
     * COUNTABLE BLINKS: high nibble = N GREEN blinks, low nibble = N
     * YELLOW blinks, one double-length cyan separator between them.
     * Count from the boot video (each blink ~1/3 s, each StageColor
     * holds ~10 vblanks). Example: ID 5C = 5 green, 12 yellow. */
    {
        unsigned char id = saturn_cart_id_seen;
        int n;

        for (n = 0; n < (id >> 4); n++)
        {
            Saturn_StageColor(JO_COLOR_Green);
            Saturn_StageColor(JO_COLOR_Black);
        }
        Saturn_StageColor(JO_COLOR_Cyan);
        Saturn_StageColor(JO_COLOR_Cyan);
        for (n = 0; n < (id & 15); n++)
        {
            Saturn_StageColor(JO_COLOR_Yellow);
            Saturn_StageColor(JO_COLOR_Black);
        }
    }

    /* Stage 3.5 — SCU DSP capability probe (saturn_dsp.c): load + run
     * a tiny DSP program that DMA-round-trips 64 words through WRAM-H.
     * ORANGE = all four capabilities work (load/start/both DMA dirs);
     * DARK RED = the DSP plan is dead on this emulator/hardware. The
     * color holds for the report frame (~1.5 s) so a capture can't
     * miss it; result also cached for D_DoomMain. */
    saturn_dsp_ok = Saturn_DspProbe();
    /* Tri-state display (2026-09-07): -1 = probe skipped (SATURN_DSP_OFF)
     * must not read as the ORANGE "all capabilities work" verdict. */
    Saturn_StageColor(saturn_dsp_ok == 1 ? JO_COLOR_SATURN_RGB(31, 16, 0)
                                         : JO_COLOR_DarkRed);

    /* ===== Instrumented first report frame =====
     * Each primitive gets a marker so a hang names itself:
     *   DARK GREEN  entering boot_report()  (stuck here = font draw crash)
     *   DARK YELLOW boot_report returned, waiting on VBlank interrupts
     *               (stuck here = SGL VBlank-in interrupt chain dead)
     *   CYAN        interrupts alive, testing slSynch()
     *               (stuck here = slSynch never returns)
     *   DARK CYAN   slSynch returned — from here everything is proven. */
    Saturn_StageColor(JO_COLOR_DarkGreen);
    boot_report();
    Saturn_StageColor(JO_COLOR_DarkYellow);
    while (boot_vbl_count < 60u) { }
    Saturn_StageColor(JO_COLOR_Cyan);
    Saturn_PumpFrame();
    Saturn_StageColor(JO_COLOR_DarkCyan);

    /* Hold the report ~0.25 s (15 frames — the VDP1 text is invisible
     * on real hardware anyway; the stage colours carry the diagnosis). */
    for (frame = 1; frame < 15; frame++)
    {
        boot_report();
        Saturn_PumpFrame();
    }

    /* Real boot: WAD load -> Z zone -> MAP01/E1M1 -> main loop. Never
     * returns (D_DoomLoop is infinite). */
    D_DoomMain();

    for (;;) { }
}