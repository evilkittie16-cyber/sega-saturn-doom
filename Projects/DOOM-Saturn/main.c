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
    jo_core_init(JO_COLOR_Black);
    jo_core_add_vblank_callback(boot_vbl_tick);

    (void)Saturn_CartInit();
    saturn_dsp_ok = Saturn_DspProbe();

    /* Real boot: WAD load -> Z zone -> Menu -> Level loop. Never
     * returns (D_DoomLoop is infinite). */
    D_DoomMain();

    for (;;) { }
}
