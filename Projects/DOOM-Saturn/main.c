/*
** DOOM for Sega Saturn — Jo Engine port
** main.c — thin boot: init video core, load the boot font, probe the
** extended-RAM cart, show a ~2-second probe report, then hand off to
** D_DoomMain (cart init is idempotent — the real WAD load reuses the
** cached probe result).
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

/* Non-static: i_system.c's Saturn_StageColor hold waits on this counter
 * so every boot-stage colour is visible long enough to photograph. */
volatile unsigned int boot_vbl_count;
static void boot_vbl_tick(void)
{
    boot_vbl_count++;
}

static void boot_report(void)
{
    char idline[16];

    boot_report_line(8,  "DOOM-SATURN");

    idline[0] = 'C'; idline[1] = 'A'; idline[2] = 'R'; idline[3] = 'T';
    idline[4] = '-'; idline[5] = 'I'; idline[6] = 'D'; idline[7] = '-';
    idline[8] = "0123456789ABCDEF"[(saturn_cart_id_seen >> 4) & 0xF];
    idline[9] = "0123456789ABCDEF"[saturn_cart_id_seen & 0xF];
    idline[10] = 0;
    boot_report_line(12, idline);

    if (Saturn_CartPresent())
    {
        /* the cart is in use; rw_ok = 0 here means the size march cut it
         * down (10-12: stopped answering, 20-22: mirrors), see i_wad.h */
        const char *kind = (saturn_cart_id_seen == 0x5C) ? "4MB-" :
                           (saturn_cart_id_seen == 0x5A) ? "1MB-" : "";
        char        line[32];

        if (saturn_cart_rw_ok)
            Saturn_Format(line, (int)sizeof(line), "CART-%sRW-OK", kind);
        else
            Saturn_Format(line, (int)sizeof(line), "CART-%sSIZE-%d-%dK", kind,
                          saturn_cart_rw_point, (int)(saturn_cart_size >> 10));
        boot_report_line(28, line);
        boot_report_line(60, (saturn_cart_base_addr == 0x22400000u) ? "BASE-22400000"
                                                                    : "BASE-NONE");
        boot_report_line(44, "CART-WAD-MODE");
        /* SAROO XRAM (i_wad.h): row 76, between SCU-DSP (68) and BOOTING (84) */
        if (saturn_cart_xram_state == 1)
            boot_report_line(76, "SAROO-XRAM-8MB");
        else if (saturn_cart_xram_state != 0)
        {
            Saturn_Format(line, (int)sizeof(line), "SAROO-XRAM-FAIL-%d",
                          saturn_cart_xram_state);
            boot_report_line(76, line);
        }
    }
    else if (saturn_cart_probe_failed)
    {
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
    (void)boot_report;
    /* 2026-11-25 (real HW): after the console's RESET button the VDP2 came
     * up broken (white border, no status bar, striped NBG0, rolling) in
     * every build; a power-on boot was always clean. jo_core_init() selects
     * the 28.6 MHz clock (352 px) through the boot ROM's SYS_CHGSYSCK, and
     * that clock change is what resets the VDPs - after a reset the console
     * may still be on 28.6 MHz, so the change (and the reset) does not
     * happen. Going to 26.8 MHz first makes jo's change a real one. */
    JO_BIOS_CHANGE_SYSTEM_CLOCK(JO_BIOS_SYSTEM_CLOCK_320);
    jo_core_init(JO_COLOR_Black);
    jo_core_add_vblank_callback(boot_vbl_tick);

    (void)Saturn_CartInit();
    saturn_dsp_ok = Saturn_DspProbe();

    /* Real boot: WAD load -> Z zone -> Menu -> Level loop. Never
     * returns (D_DoomLoop is infinite). */
    D_DoomMain();

    for (;;) { }
}
