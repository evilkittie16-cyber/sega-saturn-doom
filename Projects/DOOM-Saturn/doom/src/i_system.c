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

void    I_Init(void)
{
    /* Jo Engine init is performed in main.c before D_DoomMain. */
}

/* ====== Boot / error text ====== */
volatile unsigned int saturn_hold_timeout = 0;
jo_font  *saturn_boot_font = NULL;

void Saturn_BootFontInit(void)
{
    if (saturn_boot_font)
        return;
    saturn_boot_font = jo_font_load(JO_ROOT_DIR, "FONT.TGA", JO_COLOR_Green, 8, 8, 2,
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!\"?=%&',.()*+-/");
}

void Saturn_BootText(int x, int y, const char *str)
{
    (void)x;
    (void)y;
    (void)str;
}

void Saturn_SetupTag(const char *str)
{
    (void)str;
}

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

void Saturn_BootRec_PaintPrev(void)
{
}

void Saturn_PumpFrame(void)
{
    extern int saturn_game_started;
    extern int saturn_vdp2_nbg0_live(void);

    if (saturn_game_started || saturn_vdp2_nbg0_live())
        return;
    __jo_core_synch();
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

void Saturn_BootMsg(const char *msg)
{
    (void)msg;
}

unsigned int I_GetTicks(void)
{
    return jo_get_ticks();
}

int I_GetTime(void)
{
    extern int gametic;
    return gametic;
}

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

int Saturn_Format(char *out, int outsz, const char *fmt, ...)
{
    va_list ap;
    int     r;
    va_start(ap, fmt);
    r = saturn_vsnprintf(out, outsz, fmt, ap);
    va_end(ap);
    return r;
}

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

    Saturn_StageColor(JO_COLOR_Red);
    {
        extern int saturn_quiet_hud;

        saturn_quiet_hud = 0;
    }

    va_start(ap, fmt);
    saturn_vsnprintf(buf, (int)sizeof(buf), fmt, ap);
    va_end(ap);

    for (i = 0; buf[i]; i++)
        if (buf[i] >= 'a' && buf[i] <= 'z')
            buf[i] = (char)(buf[i] - 'a' + 'A');

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
                    jo_font_print(saturn_boot_font, 8, 8, 2.0f, (char *)"ERROR");
                    for (l = 0; l < nlines; l++)
                        jo_font_print(saturn_boot_font, 8, 32 + l * 20, 2.0f,
                                      lines[l]);
                }
                else
                    jo_printf(0, 0, "DOOM-Saturn error: %.60s", buf);
                {
                    extern void Saturn_StageTextHi(int x, int y,
                                                   const char *s);
                    int l;

                    Saturn_StageTextHi(8, 8, "ERROR");
                    for (l = 0; l < nlines; l++)
                        Saturn_StageTextHi(8, 32 + l * 20, lines[l]);
                }
                __jo_core_synch();
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

byte *I_AllocLow(int length)
{
    return (byte *)jo_malloc(length);
}

byte *I_AllocHigh(int length)
{
    return (byte *)jo_malloc(length);
}

/* Pad presses latched at every vblank (2026-11-16). I_ReadControls only runs
 * inside the tic loop, and on a slow frame the owed tics run in one burst at
 * the end, so a press shorter than the frame was never sampled: "the pause
 * menu works intermittently" (pause frames are slow; standing vs moving
 * changes the frame time). Called from the VDP1 vblank callback. */
static volatile unsigned short saturn_pad_rise = 0;   /* jo key bits pressed since last read */

void Saturn_PadVblankSample(void)
{
    static unsigned short prev_held = 0;
    unsigned short raw = 0xFFFF;
    if (jo_inputs[0].id != 0xFF && jo_inputs[0].id != 0 && jo_inputs[0].data != 0)
        raw = jo_inputs[0].data;
    unsigned short held = (unsigned short)~raw;
    saturn_pad_rise |= (unsigned short)(held & ~prev_held);
    prev_held = held;
}

static int saturn_keys_to_bt(unsigned short k)
{
    int b = 0;
    if (k & JO_KEY_RIGHT) b |= BT_MOVERIGHT;
    if (k & JO_KEY_LEFT)  b |= BT_MOVELEFT;
    if (k & JO_KEY_UP)    b |= BT_MOVEUP;
    if (k & JO_KEY_DOWN)  b |= BT_MOVEDOWN;
    if (k & JO_KEY_A)     b |= BT_ATTACK;
    if (k & JO_KEY_B)     b |= BT_USE;
    if (k & JO_KEY_C)     b |= BT_SPEED;
    if (k & JO_KEY_START) b |= BT_START | BT_PAUSE;
    if (k & JO_KEY_L)     b |= BT_STRAFELEFT;
    if (k & JO_KEY_R)     b |= BT_STRAFERIGHT;
    if (k & JO_KEY_X)     b |= BT_PWEAPN;
    if (k & JO_KEY_Y)     b |= BT_NWEAPN;
    if (k & JO_KEY_Z)     b |= BT_AUTOMAP;
    return b;
}

int I_ReadControls(void)
{
    unsigned short raw_data = 0xFFFF;

    /* Jo Engine v2026.1 polls SMPC INTBACK on every VBlank-OUT and populates
     * jo_inputs[0].data (active-low: idle pad reads 0xFFFF). */
    if (jo_inputs[0].id != 0xFF && jo_inputs[0].id != 0 && jo_inputs[0].data != 0)
    {
        raw_data = jo_inputs[0].data;
    }

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
    if (held & JO_KEY_X)      buttons |= BT_PWEAPN;
    if (held & JO_KEY_Y)      buttons |= BT_NWEAPN;
    if (held & JO_KEY_Z)      buttons |= BT_AUTOMAP;

#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 4)
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

    {
        extern int saturn_hw_bsp_occl;
        static int prev_occl_toggle = 1;
        int occl_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_X))
                           == (JO_KEY_L | JO_KEY_R | JO_KEY_X));
        if (occl_toggle && prev_occl_toggle != 2)
            saturn_hw_bsp_occl = (saturn_hw_bsp_occl == 2) ? 1 : 2;
        prev_occl_toggle = occl_toggle ? 2 : 1;
    }

    {
        extern int saturn_hw_skydbg;
        static int prev_sky_toggle = 1;
        int sky_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_Y))
                          == (JO_KEY_L | JO_KEY_R | JO_KEY_Y));
        if (sky_toggle && prev_sky_toggle != 2)
            saturn_hw_skydbg = (saturn_hw_skydbg == 2) ? 1 : 2;
        prev_sky_toggle = sky_toggle ? 2 : 1;
    }

    {
        extern int saturn_show_fps;
        extern int saturn_stbar_dirty_latch;
        static int prev_fps_toggle = 1;
        int fps_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_DOWN))
                          == (JO_KEY_L | JO_KEY_R | JO_KEY_DOWN));
        if (fps_toggle && prev_fps_toggle != 2)
        {
            saturn_show_fps ^= 1;
            saturn_stbar_dirty_latch = 2;
        }
        prev_fps_toggle = fps_toggle ? 2 : 1;
    }

    {
        extern int saturn_show_perf;
        extern int saturn_stbar_dirty_latch;
        static int prev_perf_toggle = 1;
        int perf_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_UP))
                           == (JO_KEY_L | JO_KEY_R | JO_KEY_UP));
        if (perf_toggle && prev_perf_toggle != 2)
        {
            saturn_show_perf ^= 1;
            saturn_stbar_dirty_latch = 2;
        }
        prev_perf_toggle = perf_toggle ? 2 : 1;
    }

    {
        extern void Saturn_ToggleWallSubdivision(void);
        static int prev_subdiv_toggle = 1;
        int subdiv_toggle = ((held & (JO_KEY_L | JO_KEY_R | JO_KEY_LEFT))
                             == (JO_KEY_L | JO_KEY_R | JO_KEY_LEFT));
        if (subdiv_toggle && prev_subdiv_toggle != 2)
            Saturn_ToggleWallSubdivision();
        prev_subdiv_toggle = subdiv_toggle ? 2 : 1;
    }

    if (players && consoleplayer >= 0 && consoleplayer < MAXPLAYERS)
    {
        extern unsigned int saturn_pad_edges;
        /* presses latched per vblank (Saturn_PadVblankSample) ONLY - also
         * taking this read's own edge would count a press twice when the read
         * sees it before the next vblank sample does. Saturn_G_Ticker consumes
         * them once. */
        unsigned short rise = saturn_pad_rise;
        saturn_pad_rise = (unsigned short)(saturn_pad_rise & ~rise);
        saturn_pad_edges |= (unsigned int)saturn_keys_to_bt(rise);
        players[consoleplayer].oldticbuttons = players[consoleplayer].ticbuttons;
        players[consoleplayer].ticbuttons    = buttons;
    }
    return buttons;
}
