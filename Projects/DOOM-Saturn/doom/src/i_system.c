/* i_system.c — system services for Saturn DOOM. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <jo/jo.h>

#include "doomdef.h"
#include "i_system.h"
#include "i_video.h"
#include "saturn.h"     /* SATURN_CART_REC_OFF (boot-flight recorder) */

/* d32xr's i_system doesn't have I_GetTime -> doomtime_t; it returns
 * the integer time in tics. We provide a thin wrapper that maps
 * between the two so the rest of the code can use whatever it likes. */

void    I_Init(void)
{
    /* Jo Engine init is performed in main.c before D_DoomMain. */
}

/* ====== Boot / error text ====== */

/* r95: latched by Saturn_StageColor when a photo-hold gave up because
 * boot_vbl_count stopped advancing (dead vblank chain). The stage
 * ladder is the boot's only real-HW eyes — this flag keeps a dead
 * clock from turning the instrument itself into the wedge. */
volatile unsigned int saturn_hold_timeout = 0;

/* VDP1-sprite font — the proven-visible text channel under SGL (the
 * custom NBG0 cell renderer was a dead end and jo_printf's NBG0 cells
 * were unreliable on the test setup). cd/FONT.TGA is the 8x8 tile
 * sheet from the Jo Engine printf sample; the call below is the
 * sample's verbatim (uppercase letters + digits + symbols, no
 * lowercase and no space in the mapping — unmapped chars are
 * skipped, so compose messages accordingly). */
jo_font  *saturn_boot_font = NULL;

/* SGL frame primitive — under JO_COMPILE_USING_SGL the only way to
 * commit a VDP1 command table and swap to the next frame. Declared
 * locally to avoid depending on the SGL header's include order. */
extern void slSynch(void);

void Saturn_BootFontInit(void)
{
    if (saturn_boot_font)
        return;
    saturn_boot_font = jo_font_load(JO_ROOT_DIR, "FONT.TGA", JO_COLOR_Green, 8, 8, 2,
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!\"?=%&',.()*+-/");
}

/* Draw one line of static text (font-aware). x/y are pixel
 * coordinates; when the font failed to load we fall back to
 * jo_printf's character cells so there's at least something. */
void Saturn_BootText(int x, int y, const char *str)
{
    if (saturn_boot_font)
        jo_font_print(saturn_boot_font, x, y, 1.0f, (char *)str);
    else
        jo_printf(x / 8, y / 8, "%s", str);
}

/* p_setup.c's level-setup ladder: BootText alone is INVISIBLE during
 * level setup — jo_font prints to VDP1, and NBG0's direct display
 * (2026-09-04 takeover) covers it, so a real-HW wedge photo shows a
 * bare stage color with no tag. This variant also stamps the tag into
 * the NBG0 pages through the census-glyph channel (Saturn_StageTextHi,
 * a direct VDP2 write — visible immediately), preceded by a row of
 * unmapped glyphs (blank cells) so a shorter tag can't leave
 * leftovers from the previous one. Tags: <=15 chars, y=48 (in-view,
 * clear of the P/W rows at 140,168/188). */
void Saturn_SetupTag(const char *str)
{
    (void)str;
}

/* ====== BOOT-FLIGHT RECORDER (2026-09-09, title-path SYSTEM-RESET
 * wedge) ======
 * The crash.yss forensics landed the verdict: the boot wedge is the
 * level-start SYSTEM-RESET family — the machine hard-resets between
 * the STARTING paint and menu_title, WRAM is wiped, and the recycled
 * BIOS bootstrap parks (Kronos also wipes VDP2 VRAM/CRAM and the cart
 * block in the saved state; real HW keeps both). The ONLY record that
 * survives on real HW is the 4MB cart's record slot — so each boot
 * milestone is stamped into rc[44..45] (past GSUB rc[40..42], inside
 * the march-verified SATURN_CART_REC_OFF region), and the NEXT boot
 * paints PREV=<n> into the NBG0 pages right after the cyan STARTING
 * fill. On real HW VDP2 survives the reset, so the terminal wedge
 * screen shows the previous boot's death point:
 *   n=1 cyan STARTING painted   (died before STARTING slSynch)
 *   n=2 BootMsg("STARTING") returned (died before slave_init — no-op)
 *   n=3 saturn_slave_init returned (died before menu_title entry)
 *   n=4..9 = menu letters A..F (saturn_menu.c: A=entry, B=TITLEPIC
 *           cached, C=demo probe, D=first pump, E=fb drawn, F=first
 *           commit returned)
 * Zero new BSS: reuses unused record words. */
void Saturn_BootRec(int n)
{
    extern unsigned int saturn_cart_base_addr;  /* i_wad.c */
    volatile unsigned int *rc;

    if (saturn_cart_base_addr == 0u)
        return;
    rc = (volatile unsigned int *)
        (saturn_cart_base_addr + SATURN_CART_REC_OFF);
    rc[44] = 0x424F4F54u;               /* 'BOOT' */
    rc[45] = (unsigned int)n;
}

/* Paint the previous boot's last milestone — call right after the
 * STARTING cyan fill, BEFORE the first Saturn_BootRec overwrites the
 * record. No-op without a cart or without a recorded death. */
void Saturn_BootRec_PaintPrev(void)
{
}

/* Commit the current frame (SGL). Used by the boot report in main.c
 * and by Saturn_BootMsg. */
void Saturn_PumpFrame(void)
{
    extern int saturn_game_started;
    extern int saturn_vdp2_nbg0_live(void);

    if (saturn_game_started || saturn_vdp2_nbg0_live())
        return;
    slSynch();
}

extern void saturn_vdp2_stage_fill(jo_color color);

void Saturn_StageNamePaint(unsigned short color)
{
    (void)color;
}

void Saturn_StageColor(jo_color color)
{
    (void)color;
}

void Saturn_PaintNow(jo_color color)
{
    (void)color;
}

/* Progress message during the synchronous boot phases (WAD load,
 * level setup). */
void Saturn_BootMsg(const char *msg)
{
    (void)msg;
}

unsigned int I_GetTicks(void)
{
    return jo_get_ticks();
}

/* d32xr's int I_GetTime(void) returns the number of tics since
 * level start. We count VBlanks and divide by 2 (DOOM tic is 2 VBlanks
 * at 60 Hz). On Saturn we don't drive this from a real timer; we tick
 * from D_DoomLoop's I_WaitVBL calls. */
static unsigned int saturn_basetic;
int I_GetTime(void)
{
    extern int gametic;
    return gametic;
}

/* ---- minimal printf ----
 * vsnprintf() from the SGL library crashes this -nodefaultlibs build as
 * soon as real varargs are passed. Symptom (seen 2026-08-30): I_Error
 * painted solid red but never reached its draw loop — "LOADING WAD"
 * stayed on screen and no "ERROR" text ever appeared, because the
 * process died inside the formatter. (Arg-less messages rendered fine.)
 * This local formatter covers every spec this port uses — %d %i %u %x %X
 * %s %c %p %% with optional 0-pad width and 'l' length — and nothing
 * else. */
static char *sat_fmt_putc(char *p, char *end, char c)
{
    if (p < end)
        *p++ = c;
    return p;
}

static char *sat_fmt_num(char *p, char *end, unsigned long v, unsigned base,
                         int upper, int width, char pad)
{
    char        tmp[24];
    int         nd = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    do
    {
        tmp[nd++] = dig[v % base];
        v /= base;
    } while (v && nd < (int)sizeof(tmp));
    while (width > nd)
    {
        p = sat_fmt_putc(p, end, pad);
        width--;
    }
    while (nd > 0)
        p = sat_fmt_putc(p, end, tmp[--nd]);
    return p;
}

static int saturn_vsnprintf(char *out, int outsz, const char *fmt, va_list ap)
{
    char *p   = out;
    char *end = out + (outsz > 0 ? outsz - 1 : 0);

    while (*fmt && p < end)
    {
        char pad     = ' ';
        int  width   = 0;
        int  islong  = 0;

        if (*fmt != '%')
        {
            *p++ = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%')
        {
            *p++ = *fmt++;
            continue;
        }
        if (*fmt == '0')
        {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l')
        {
            islong = 1;
            fmt++;
        }
        switch (*fmt)
        {
            case 'd':
            case 'i':
            {
                long v = islong ? va_arg(ap, long) : (long)va_arg(ap, int);
                if (v < 0)
                {
                    p = sat_fmt_putc(p, end, '-');
                    v = -v;
                }
                p = sat_fmt_num(p, end, (unsigned long)v, 10, 0, width, pad);
                break;
            }
            case 'u':
                p = sat_fmt_num(p, end,
                                islong ? va_arg(ap, unsigned long)
                                       : (unsigned long)va_arg(ap, unsigned int),
                                10, 0, width, pad);
                break;
            case 'x':
            case 'X':
                p = sat_fmt_num(p, end,
                                islong ? va_arg(ap, unsigned long)
                                       : (unsigned long)va_arg(ap, unsigned int),
                                16, *fmt == 'X', width, pad);
                break;
            case 'p':
                p = sat_fmt_num(p, end, (unsigned long)va_arg(ap, void *),
                                16, 1, 8, '0');
                break;
            case 'c':
                p = sat_fmt_putc(p, end, (char)va_arg(ap, int));
                break;
            case 's':
            {
                const char *s = va_arg(ap, const char *);
                if (!s)
                    s = "(null)";
                while (*s)
                    p = sat_fmt_putc(p, end, *s++);
                break;
            }
            case '\0':
                goto done;
            default:
                p = sat_fmt_putc(p, end, *fmt);
                break;
        }
        fmt++;
    }
done:
    *p = '\0';
    return (int)(p - out);
}

/* Public wrapper around saturn_vsnprintf for other files that need a
 * crash-proof formatter (boot report lines in main.c, etc.).
 *
 * KNOWN-BROKEN ON TARGET (2026-09-04, evidence since 2026-08-30): the
 * %d/%u/%x numeric specs emit nothing or garbage — the 08-30
 * MAP-NOT-FOUND screen printed no digits at all, and the round-33c
 * boot stamp (two constant-argument Format calls) both garbled its
 * output and destabilized the boot into the stripe-garbage crash.
 * The varargs call itself is the suspect, not the digit loop, so
 * Saturn_Format cannot be trusted with ANY numeric argument. Call
 * sites needing numbers must use Saturn_Udec / Saturn_Uhex below
 * (plain arg passing, no varargs, no division) and concatenate. */
int Saturn_Format(char *out, int outsz, const char *fmt, ...)
{
    va_list ap;
    int     r;
    va_start(ap, fmt);
    r = saturn_vsnprintf(out, outsz, fmt, ap);
    va_end(ap);
    return r;
}

/* Varargs-free numeric writers — the ONLY safe way to get numbers into
 * text on this target (see the Saturn_Format warning above). Plain
 * argument passing, shift/subtract digits, no libgcc divide. */

static const unsigned int sat_pow10[10] = {
    1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
    10000u, 1000u, 100u, 10u, 1u
};

void Saturn_Udec(char *out, unsigned int v)
{
    int i, started = 0, n = 0;

    for (i = 0; i < 10; i++)
    {
        unsigned int pw = sat_pow10[i];
        int          d  = 0;
        while (v >= pw)
        {
            v -= pw;
            d++;
        }
        if (d || started || i == 9)
        {
            out[n++] = (char)('0' + d);
            started  = 1;
        }
    }
    out[n] = '\0';
}

/* r68: fixed-width zero-padded variant for the ISR's diagnostic rows.
 * Every value cell is rewritten on EVERY paint, so a frozen value reads
 * perfectly clean and a climbing value garbles only its changing cells —
 * instead of Saturn_StageText's never-erase cells accumulating a ghost
 * union of every past value (which made the r66/r67 freeze photos
 * undecodable). No division: reuse Udec's subtract loop, then pad. */
void Saturn_Udec0(char *out, unsigned int v, int width)
{
    char tmp[10];
    int  len = 0, i;

    if (width < 1)
        width = 1;
    if (width > 9)
        width = 9;
    Saturn_Udec(tmp, v);
    while (tmp[len] != '\0')
        len++;
    for (i = 0; i < width; i++)
        out[i] = (i < width - len) ? '0' : tmp[i - (width - len)];
    out[width] = '\0';
}

void Saturn_Uhex(char *out, unsigned int v)
{
    static const char hd[17] = "0123456789ABCDEF";
    int               i, started = 0, n = 0;

    for (i = 7; i >= 0; i--)
    {
        int d = (int)((v >> (i * 4)) & 15u);
        if (d || started || i == 0)
        {
            out[n++] = hd[d];
            started  = 1;
        }
    }
    out[n] = '\0';
}

void I_Error(char *fmt, ...)
{
    char     buf[256];
    va_list  ap;
    int      i;

    /* Paint the beacon FIRST — so if the formatting below crashes the
     * red screen still identifies an I_Error. (The formatter above is
     * local and crash-proof, but keep the ordering guarantee anyway.) */
    Saturn_StageColor(JO_COLOR_Red);
    {   /* fatal rows must always show, even in the VDP1-wall
         * prototype's quiet-HUD mode */
        extern int saturn_quiet_hud;

        saturn_quiet_hud = 0;
    }

    va_start(ap, fmt);
    saturn_vsnprintf(buf, (int)sizeof(buf), fmt, ap);
    va_end(ap);

    /* The font maps uppercase only (unmapped chars are skipped), so
     * upcase the message before drawing — otherwise most of a normal
     * mixed-case error string would be invisible. */
    for (i = 0; buf[i]; i++)
        if (buf[i] >= 'a' && buf[i] <= 'z')
            buf[i] = (char)(buf[i] - 'a' + 'A');

    /* Word-wrap in place: the draw loop below prints at 2.0f scale,
     * where the font's advance is (8 letter + 2 spacing) * 2 = 20 px
     * per glyph — only 15 columns fit from x=8 on the 320-px screen
     * (16 px glyph cells but a 20 px pitch; the earlier 38-char wrap
     * clipped every long line in half). A longer line would clip
     * exactly where the useful numbers start. '\n' in the format
     * string forces an explicit line break (the diagnostics use it so
     * each counter gets its own line); otherwise break at spaces into
     * lines of at most 15 chars, 8 lines max. */
    {
        char *lines[8];
        int   nlines = 1;
        int   start = 0, lastsp = -1;

        lines[0] = &buf[0];
        i = 0;
        while (buf[i] && nlines < 8)
        {
            if (buf[i] == '\n')
            {
                buf[i] = '\0';
                lines[nlines++] = &buf[i + 1];
                start = i + 1;
                lastsp = -1;
                i = start;
                continue;
            }
            if (buf[i] == ' ')
                lastsp = i;
            if (i - start >= 15)
            {
                /* Cut at the last space if one exists; otherwise hard-cut
                 * at the current char (consuming it as the terminator —
                 * only fires for a 15+ char unbroken token, which our
                 * dash-separated diagnostics never produce). */
                int cut = (lastsp > start) ? lastsp : i;
                buf[cut] = '\0';
                lines[nlines++] = &buf[cut + 1];
                start = cut + 1;
                lastsp = -1;
                i = start;
                continue;
            }
            i++;
        }

        /* FLASH the NBG1 background red/black so the error is unmissable
         * even when the VDP1 font path is dead — the bitmap background is
         * the one channel that always works. Under SGL the VDP1 command
         * table is transient, so the text is also redrawn every frame
         * forever. (Solid red was painted at function entry.)
         * 2026-09-04: the flash goes through Saturn_PaintNow so that
         * post-takeover (NBG0 direct display) it lands in the NBG0 pages
         * via the stage fill instead of scribbling the hidden NBG1
         * bitmap — which IS page A — with raw 16bpp color bytes. */
        {
            int frame = 0;
            for (;;)
            {
                if ((frame & 31) == 0)
                    Saturn_PaintNow((frame & 32) ? JO_COLOR_Red
                                                 : JO_COLOR_Black);
                if (saturn_boot_font)
                {
                    int l;
                    /* 2x scale: 16 px glyph cells on a 20 px pitch
                     * ((8 letter + 2 spacing) * 2) — the 8 px font is
                     * near-unreadable on TV, and the charset has no
                     * space glyph so dashed tokens are the norm. 15
                     * columns / 20 px pitch; 8 lines from y=32 fits
                     * (the wrap above must agree: 15, not 16). */
                    jo_font_print(saturn_boot_font, 8, 8, 2.0f, (char *)"ERROR");
                    for (l = 0; l < nlines; l++)
                        jo_font_print(saturn_boot_font, 8, 32 + l * 20, 2.0f,
                                      lines[l]);
                }
                else
                    jo_printf(0, 0, "DOOM-Saturn error: %.60s", buf);
                /* NBG0 glyph channel (2026-09-06): the VDP1 font above is
                 * INVISIBLE under the NBG0 direct-display takeover, which
                 * is why the E1M2 "Z_Malloc: failed on 6059" red screen
                 * carried no readable message. StageTextHi is a direct
                 * VDP2 write — the channel the rv verdict panel paints
                 * through — but PaintNow wipes both pages every 32
                 * frames, so re-stamp every frame (error path: cost
                 * irrelevant). Same geometry as the jo_font rows. */
                {
                    extern void Saturn_StageTextHi(int x, int y,
                                                   const char *s);
                    int l;

                    Saturn_StageTextHi(8, 8, "ERROR");
                    for (l = 0; l < nlines; l++)
                        Saturn_StageTextHi(8, 32 + l * 20, lines[l]);
                }
                slSynch();
                frame++;
            }
        }
    }
}

void I_Quit(void)
{
    for (;;) { }
}

void I_InitTimer(void)  { /* vblank-driven; nothing to do here */ }

/* The original DOOM used I_AllocLow/I_AllocHigh to grab memory below or
 * above the DOS 640K line. Saturn has flat 2 MB work RAM (1 MB slave);
 * we just return aligned memory from jo_malloc. */
byte *I_AllocLow(int length)
{
    return (byte *)jo_malloc(length);
}

byte *I_AllocHigh(int length)
{
    return (byte *)jo_malloc(length);
}

/* I_ReadControls — translate Jo Engine's gamepad bitmask into DOOM's
 * BT_* ticbuttons word for player 0. d32xr calls this from MiniLoop's
 * update() slot before each P_Ticker; we call it from D_DoomLoop right
 * before stepping the simulation.
 *
 * Jo Engine bitmask for the non-SGL build (jo/input.h:97-119):
 *   JO_KEY_RIGHT  = 1<<7
 *   JO_KEY_LEFT   = 1<<6
 *   JO_KEY_DOWN   = 1<<5
 *   JO_KEY_UP     = 1<<4
 *   JO_KEY_START  = 1<<11
 *   JO_KEY_A      = 1<<10
 *   JO_KEY_B      = 1<<8
 *   JO_KEY_C      = 1<<9
 *   JO_KEY_Z      = 1<<0
 *   JO_KEY_Y      = 1<<1
 *   JO_KEY_X      = 1<<2
 *   JO_KEY_R      = 1<<3
 *   JO_KEY_L      = 1<<15
 *
 * DOOM BT_* (doomdef.h:1148-1194):
 *   BT_MOVERIGHT     = 0x0001
 *   BT_MOVELEFT      = 0x0002
 *   BT_MOVEUP        = 0x0004
 *   BT_MOVEDOWN      = 0x0008
 *   BT_ATTACK        = 0x0010
 *   BT_USE           = 0x0020
 *   BT_STRAFE        = 0x0040  (held-strafe toggle; d32xr-am_main uses this)
 *   BT_SPEED         = 0x0080
 *   BT_START         = 0x0100
 *   BT_STRAFELEFT    = 0x2000  (d32xr extension; per-tic strafe-left)
 *   BT_STRAFERIGHT   = 0x4000  (d32xr extension; per-tic strafe-right)
 *
 * Mapping (final):
 *   D-pad Up/Down/Left/Right -> BT_MOVEUP/DOWN/LEFT/RIGHT
 *   A button                 -> BT_ATTACK
 *   B button                 -> BT_USE
 *   C button                 -> BT_SPEED (run)
 *   Start                    -> BT_START
 *   L shoulder               -> BT_STRAFELEFT  (tap-to-strafe-left)
 *   R shoulder               -> BT_STRAFERIGHT (tap-to-strafe-right)
 *   X button (6-b pad)       -> BT_PWEAPN      (previous weapon)
 *   Y button (6-b pad)       -> BT_NWEAPN      (next weapon)
 *
 * We use BT_STRAFELEFT/BT_STRAFERIGHT (per-tic strafe) instead of
 * BT_STRAFE (held toggle) because Saturn's shoulder buttons are
 * comfortable to tap rapidly during combat; a held toggle would force
 * the user to keep L/R pressed while trying to also use the d-pad,
 * which is awkward on the small 6-button Saturn gamepad.
 *
 * Both held ("pressed") and edge ("on_keydown") semantics are
 * consulted: held for movement/run/fire so the player can hold a
 * direction; edges are NOT used here because DOOM's ticbuttons are
 * a continuous state, not a queue of events (P_MovePspr reads the
 * current ticbuttons every tic). The old/new ticbuttons pair is
 * stored in player->oldticbuttons so menu code can detect rising
 * edges via (new ^ old) & new.
 */
int I_ReadControls(void)
{
    unsigned short raw_data = 0xFFFF;

    /* SGL's Smpc_Peripheral is populated by SGL's SMPC interrupt handler vector 71
     * and _slSmpcCashSend. Convention is active-low: idle pad reads 0xFFFF. */
    if (Smpc_Peripheral != NULL && Smpc_Peripheral[0].id != 0xFF && Smpc_Peripheral[0].id != 0)
    {
        raw_data = Smpc_Peripheral[0].data;
        jo_inputs[0].data = raw_data;
        jo_inputs[0].id   = Smpc_Peripheral[0].id;
    }
    else if (jo_inputs[0].id != 0xFF && jo_inputs[0].id != 0 && jo_inputs[0].data != 0)
    {
        raw_data = jo_inputs[0].data;
    }

    /* 0x0000 indicates uninitialized BSS; clamp to 0xFFFF so resting state has no buttons */
    if (raw_data == 0)
        raw_data = 0xFFFF;

    unsigned short  held = (unsigned short)~raw_data;
    int             buttons = 0;

    if (held & JO_KEY_RIGHT)  buttons |= BT_MOVERIGHT;
    if (held & JO_KEY_LEFT)   buttons |= BT_MOVELEFT;
    if (held & JO_KEY_UP)     buttons |= BT_MOVEUP;
    if (held & JO_KEY_DOWN)   buttons |= BT_MOVEDOWN;
    if (held & JO_KEY_A)      buttons |= BT_ATTACK;
    if (held & JO_KEY_B)      buttons |= BT_USE;
    if (held & JO_KEY_C)      buttons |= BT_SPEED;
    if (held & JO_KEY_START)  buttons |= BT_START | BT_PAUSE;
    if (held & JO_KEY_L)      buttons |= BT_STRAFELEFT;
    if (held & JO_KEY_R)      buttons |= BT_STRAFERIGHT;
    /* 6-button pad: weapon cycle (d32xr p_user.c 692-717 consumes
     * BT_PWEAPN/BT_NWEAPN; PWEAPN walks the list backwards). */
    if (held & JO_KEY_X)      buttons |= BT_PWEAPN;
    if (held & JO_KEY_Y)      buttons |= BT_NWEAPN;
    if (held & JO_KEY_Z)      buttons |= BT_AUTOMAP;

#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 4)
    /* OCRAM divide-LUT live A/B toggle (2026-09-08): hold A+B+C — an
     * edge-detected chord (fire+use+run together is no real input) —
     * to flip the LUT path between ON and DIVU-fallback on the SAME
     * boot. The state lives IN THE TABLE (slot 1: 0x22222222 ON /
     * 0x44444444 OFF) because the .bss on flag is zeroed every frame
     * by the uncaught BSS stomper. The .bss ok/on are re-synced from
     * the table here every frame as the consume-site shadow; the state
     * shows on the perf-rows X row. */
    {
#ifdef SATURN_DIVLUT_NOOCRAM
        extern volatile unsigned int sat_divlut_table[512];
#define SATURN_OCLUT sat_divlut_table
#else
#define SATURN_OCLUT ((volatile unsigned int *)0xC0000000u)
#endif
        extern volatile int saturn_divlut_ok;
        extern volatile int saturn_divlut_on;
        static int prev_chord = 0;
        int        chord = (held & (JO_KEY_A | JO_KEY_B | JO_KEY_C))
                           == (JO_KEY_A | JO_KEY_B | JO_KEY_C);

        saturn_divlut_ok = (SATURN_OCLUT[0] == 0x11111111u);
        saturn_divlut_on = (SATURN_OCLUT[1] == 0x22222222u);
        if (chord && !prev_chord && saturn_divlut_ok)
            SATURN_OCLUT[1] = saturn_divlut_on ? 0x44444444u
                                               : 0x22222222u;
        prev_chord = chord;
    }
#endif

    /* Viewport mode live toggle: Option A (DOS 320x168) vs Option B (Saturn Full 320x192).
     * Hold L + R + Z to toggle between modes on rising edge. */
    {
        extern void Saturn_ToggleViewMode(void);
        static int prev_view_toggle = 0;
        int view_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_Z))
                           == (JO_KEY_L | JO_KEY_R | JO_KEY_Z));
        if (view_toggle && !prev_view_toggle)
        {
            Saturn_ToggleViewMode();
        }
        prev_view_toggle = view_toggle;
    }

    /* BSP occlusion culling live A/B toggle: hold L + R + X (rising edge).
     * 1 = on, 2 = off; shown on the HUD perf row as O1 / O2. */
    {
        extern int saturn_hw_bsp_occl;
        static int prev_occl_toggle = 1;   /* 1 = released, 2 = held (.data) */
        int occl_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_X))
                           == (JO_KEY_L | JO_KEY_R | JO_KEY_X));
        if (occl_toggle && prev_occl_toggle != 2)
            saturn_hw_bsp_occl = (saturn_hw_bsp_occl == 2) ? 1 : 2;
        prev_occl_toggle = occl_toggle ? 2 : 1;
    }

    /* Sky diagnostic live toggle: hold L + R + Y (rising edge). 2 = paint
     * the sky area magenta too, so anything else up there is VDP1 geometry. */
    {
        extern int saturn_hw_skydbg;
        static int prev_sky_toggle = 1;    /* 1 = released, 2 = held (.data) */
        int sky_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_Y))
                          == (JO_KEY_L | JO_KEY_R | JO_KEY_Y));
        if (sky_toggle && prev_sky_toggle != 2)
            saturn_hw_skydbg = (saturn_hw_skydbg == 2) ? 1 : 2;
        prev_sky_toggle = sky_toggle ? 2 : 1;
    }

    /* FPS counter live toggle: hold L + R + DOWN (rising edge).
     * Works on both 3-button and 6-button pads. */
    {
        extern int saturn_show_fps;
        extern int saturn_stbar_dirty_latch;
        static int prev_fps_toggle = 1;     /* 1 = released, 2 = held (.data) */
        int fps_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_DOWN))
                          == (JO_KEY_L | JO_KEY_R | JO_KEY_DOWN));
        if (fps_toggle && prev_fps_toggle != 2)
        {
            saturn_show_fps ^= 1;
            saturn_stbar_dirty_latch = 2;
        }
        prev_fps_toggle = fps_toggle ? 2 : 1;
    }

    /* Profiler HUD live toggle: hold L + R + UP (rising edge). */
    {
        extern int saturn_show_perf;
        extern int saturn_stbar_dirty_latch;
        static int prev_perf_toggle = 1;    /* 1 = released, 2 = held (.data) */
        int perf_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_UP))
                           == (JO_KEY_L | JO_KEY_R | JO_KEY_UP));
        if (perf_toggle && prev_perf_toggle != 2)
        {
            saturn_show_perf ^= 1;
            saturn_stbar_dirty_latch = 2;
        }
        prev_perf_toggle = perf_toggle ? 2 : 1;
    }

    /* Wall subdivision live toggle: hold L + R + LEFT (rising edge). */
    {
        extern void Saturn_ToggleWallSubdivision(void);
        static int prev_subdiv_toggle = 1;  /* 1 = released, 2 = held (.data) */
        int subdiv_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_LEFT))
                             == (JO_KEY_L | JO_KEY_R | JO_KEY_LEFT));
        if (subdiv_toggle && prev_subdiv_toggle != 2)
            Saturn_ToggleWallSubdivision();
        prev_subdiv_toggle = subdiv_toggle ? 2 : 1;
    }

    if (players && consoleplayer >= 0 && consoleplayer < MAXPLAYERS)
    {
        players[consoleplayer].oldticbuttons = players[consoleplayer].ticbuttons;
        players[consoleplayer].ticbuttons    = buttons;
    }
    return buttons;
}