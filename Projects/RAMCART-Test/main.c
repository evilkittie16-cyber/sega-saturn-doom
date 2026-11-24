/*******************************************************************
* RAMCART-Test: test program for the Saturn extended RAM cartridge
* (4 MB / 32 Mbit, ID 5C; the 1 MB / 8 Mbit cart, ID 5A, is tested too).
*
* The whole cartridge RAM is written and read back, 64 KB per frame, so
* the screen shows the progress of every test while it runs:
*
*   1 PROBE      16-bit and 32-bit write/verify at the start, middle
*                and end of each bank
*   2 ALIASING   every power-of-two offset gets its own value: two
*                addresses that reach the same RAM (a mirror, a cart
*                smaller than its ID says) are caught
*   3 ADDRESS    every longword = a value made from its own address
*   4 INVERTED   the same, all bits inverted (stuck bits)
*   5 CHECKER16  0x5555 / 0xAAAA, 16-bit accesses (the bus width)
*   6 RETENTION  write, wait 3 seconds, verify (DRAM refresh)
*   7 DMA SH2    SH-2 DMA to the cart and back (start, middle, end)
*
* All accesses go through the SH-2 cache-through area (0x2xxxxxxx), so
* the cache can never fake a result. The test DESTROYS the cart content.
*
* Init sequence (same as DOOM-Saturn's Saturn_CartInit, proven on real
* hardware): the ID is the low byte of the word at 0x24FFFFFE; only Sega
* DRAM cart IDs are written to; the 1 MB cart gets the TB#47 setup
* (enable write, A-Bus timings, refresh). The 4 MB cart is first tried
* with NO register write (a Saroo works like that, and the A-Bus
* registers break it); if its RAM does not answer, the TB#47 setup is
* applied (real hardware, 2026-11-23: an Action Replay with ID 5C fails
* every test without it). The line "SETUP THIS RUN" shows which.
*
* Pad: START = run again, A = loop forever on/off (soak test),
* B = setup of the next run: AUTO / NEVER / TB47 (always).
* Results are also in cart_res[] for a debugger.
*******************************************************************/
#include <jo/jo.h>

/* ---- hardware ------------------------------------------------------ */
#define CART_ID_WORD    (*(volatile unsigned short *)0x24FFFFFEu)
#define CART_INIT       (*(volatile unsigned short *)0x257EFFFEu)
#define SCU_ASR0        (*(volatile unsigned int *)0x25FE00B0u)   /* A-Bus set register */
#define SCU_AREF        (*(volatile unsigned int *)0x25FE00B8u)   /* A-Bus refresh register */
#define CART_BANK0      0x22400000u     /* DRAM0, cache-through */
#define CART_BANK1      0x22600000u     /* DRAM1, cache-through */

#define KB(n)           ((unsigned int)(n) * 1024u)
#define CHUNK           KB(64)          /* bytes written or verified per frame */
#define RETAIN_FRAMES   180u            /* 3 s at 60 Hz */
#define DMA_SIZE        4096u

typedef struct
{
    unsigned int        base;
    unsigned int        size;
}                       bank_t;

enum { KIND_NONE, KIND_1MB, KIND_4MB, KIND_UNKNOWN };
enum { T_PROBE, T_ALIAS, T_ADDR, T_INV, T_CHK16, T_RETAIN, T_DMA, T_COUNT };
enum { S_PENDING, S_RUNNING, S_PASS, S_FAIL };
enum { PH_START, PH_FILL, PH_WAIT, PH_VERIFY, PH_DONE, PH_NOCART };

static const char *const test_name[T_COUNT] =
{
    "PROBE 16/32", "ALIASING", "ADDRESS", "INVERTED", "CHECKER 16BIT", "RETENTION 3S", "DMA SH2"
};

static bank_t           banks[2];
static int              bank_count;
static unsigned int     total_bytes;
static unsigned int     cart_id;
static int              cart_kind;

static int              test_status[T_COUNT];
static unsigned int     test_errors[T_COUNT];
static int              cur_test;
static int              phase;
static unsigned int     pos;            /* bytes done in the current phase */
static unsigned int     wait_until;
static unsigned int     run_seed;

static unsigned int     total_errors;
static unsigned int     first_addr;
static unsigned int     first_exp;
static unsigned int     first_got;
static int              first_test;

static unsigned int     run_count;
static unsigned int     pass_count;
static unsigned int     run_start_vbl;
static unsigned int     run_frames;
static int              auto_loop;

/* Cartridge setup, changed with the B button: AUTO (see detect_cart()), never, always */
enum { INIT_AUTO, INIT_NONE, INIT_TB47, INIT_MODES };
static int              init_mode = INIT_AUTO;
static int              init_applied;   /* 1: the TB#47 setup was written for this run */

#define DMA_LOCATIONS   3u
static int              dma_timeout;    /* 0: none, 1: the write to the cart did not end, 2: the read */
static unsigned int     dma_chcr;       /* DMA registers when a transfer did not end */
static unsigned int     dma_dmaor;
static unsigned int     dma_tcr;
static unsigned char    dma_src[DMA_SIZE] __attribute__((aligned(16)));
static unsigned char    dma_dst[DMA_SIZE] __attribute__((aligned(16)));

/* Read by a debugger: magic, id, kind, total errors, first fail (address, expected, got, test), runs, passed runs, done */
volatile unsigned int   cart_res[12];

/* ---- helpers ------------------------------------------------------- */
static void             bus_settle(void)
{
    volatile int        d;

    for (d = 0; d < 0x200; d++)
        __asm__ volatile ("nop");
}

static void             record_error(const unsigned int addr, const unsigned int expected, const unsigned int got)
{
    if (total_errors == 0)
    {
        first_addr = addr;
        first_exp = expected;
        first_got = got;
        first_test = cur_test;
    }
    ++total_errors;
    ++test_errors[cur_test];
}

/* Address of the byte `offset` of the cartridge RAM (the banks one after the other) */
static unsigned int     addr_of(unsigned int offset)
{
    int                 i;

    for (i = 0; i < bank_count - 1; i++)
    {
        if (offset < banks[i].size)
            break;
        offset -= banks[i].size;
    }
    return (banks[i].base + offset);
}

/* Value of the longword at `addr`: different at every address and at every run */
static __jo_force_inline unsigned int   pattern32(const int test, const unsigned int addr)
{
    unsigned int        v;

    v = addr * 2654435761u + run_seed;
    if (test == T_INV)
        return (~v);
    if (test == T_RETAIN)
        return ((v << 7) | (v >> 25));
    return (v);
}

/* Checkerboard, swapped at every run */
static __jo_force_inline unsigned short pattern16(const unsigned int addr)
{
    return ((((addr >> 1) ^ run_count) & 1) ? 0xAAAA : 0x5555);
}

/* ---- detection ----------------------------------------------------- */

/* TB#47 cartridge setup: enable the cart, then the A-Bus timings and the DRAM refresh */
static void             apply_tb47_init(void)
{
    CART_INIT = 0x0001;
    bus_settle();
    SCU_ASR0 = 0x23301FF0u;
    SCU_AREF = 0x00000013u;
    bus_settle();
    init_applied = 1;
}

/* Does the RAM at `base` answer? Two different words are written before any is read back, so the value
   left floating on the bus by the last write cannot pass for a good read. */
static bool             quick_probe(const unsigned int base)
{
    volatile unsigned short *a = (volatile unsigned short *)base;
    volatile unsigned short *b = (volatile unsigned short *)(base + 0x1000u);
    bool                ok;

    *a = 0x5AA5;
    *b = 0xA55A;
    ok = (*a == 0x5AA5 && *b == 0xA55A);
    *a = 0xC33C;
    *b = 0x3CC3;
    return (ok && *a == 0xC33C && *b == 0x3CC3);
}

static void             detect_cart(void)
{
    cart_id = CART_ID_WORD & 0xFFu;
    bank_count = 0;
    total_bytes = 0;
    init_applied = 0;
    switch (cart_id)
    {
    case 0x5A:
    case 0x1A:
    case 0xDA:
        cart_kind = KIND_1MB;
        /* Two 512 KB banks: 0x22480000-0x225FFFFF only mirrors the first one */
        banks[0].base = CART_BANK0;
        banks[0].size = KB(512);
        banks[1].base = CART_BANK1;
        banks[1].size = KB(512);
        bank_count = 2;
        /* The 1 MB cart always gets the TB#47 setup (unless forced off) */
        if (init_mode != INIT_NONE)
            apply_tb47_init();
        break;
    case 0x5C:
        cart_kind = KIND_4MB;
        /* DRAM0 (2 MB) and DRAM1 (2 MB) are back to back: one 4 MB block */
        banks[0].base = CART_BANK0;
        banks[0].size = KB(4096);
        bank_count = 1;
        /* AUTO: first without any register write (a Saroo answers like that, and the A-Bus registers break
           it). A cart with real DRAM (Action Replay, Sega's own) does not answer until it is enabled and the
           A-Bus timings and refresh are set: only then the TB#47 setup is applied. */
        if (init_mode == INIT_TB47 || (init_mode == INIT_AUTO && !quick_probe(CART_BANK0)))
            apply_tb47_init();
        break;
    case 0x00:
    case 0xFF:
        cart_kind = KIND_NONE;
        break;
    default:
        /* Not a Sega DRAM cart (a Saroo answers 06): never written to */
        cart_kind = KIND_UNKNOWN;
        break;
    }
    if (bank_count)
        total_bytes = banks[0].size + (bank_count > 1 ? banks[1].size : 0);
}

/* ---- test 1: probes ------------------------------------------------ */
static void             probe16(const unsigned int addr, const unsigned short value)
{
    volatile unsigned short *p = (volatile unsigned short *)addr;
    unsigned short      got;

    *p = value;
    got = *p;
    if (got != value)
        record_error(addr, value, got);
}

static void             probe32(const unsigned int addr, const unsigned int value)
{
    volatile unsigned int   *p = (volatile unsigned int *)addr;
    unsigned int        got;

    *p = value;
    got = *p;
    if (got != value)
        record_error(addr, value, got);
}

static void             run_probe(void)
{
    int                 i;

    for (i = 0; i < bank_count; i++)
    {
        const unsigned int  base = banks[i].base;
        const unsigned int  size = banks[i].size;

        probe16(base, 0x5555);
        probe16(base, 0xAAAA);
        probe16(base + (size >> 1), 0xAAAA);
        probe16(base + (size >> 1), 0x5555);
        probe16(base + size - 2, 0x5A5A);
        probe16(base + size - 2, 0xA5A5);
        probe32(base, 0xDEADBEEFu);
        probe32(base + (size >> 1), 0xCAFEBABEu);
        probe32(base + size - 4, 0x12345678u);
    }
}

/* ---- test 2: aliasing ---------------------------------------------- */
#define ALIAS_MAX       48

static void             run_alias(void)
{
    unsigned int        addr[ALIAS_MAX];
    int                 count = 0;
    int                 i;
    unsigned int        off;

    for (i = 0; i < bank_count; i++)
    {
        addr[count++] = banks[i].base;
        for (off = 4; off < banks[i].size && count < ALIAS_MAX; off <<= 1)
            addr[count++] = banks[i].base + off;
    }
    /* All the writes first, then all the reads: a later write that lands on an earlier address shows */
    for (i = 0; i < count; i++)
        *(volatile unsigned int *)addr[i] = 0xA11A5000u + (unsigned int)i;
    for (i = 0; i < count; i++)
    {
        const unsigned int  got = *(volatile unsigned int *)addr[i];

        if (got != 0xA11A5000u + (unsigned int)i)
            record_error(addr[i], 0xA11A5000u + (unsigned int)i, got);
    }
}

/* ---- tests 3 to 6: whole RAM, one chunk per frame ------------------ */
static void             fill_chunk(const int test, const unsigned int offset)
{
    unsigned int        a = addr_of(offset);
    const unsigned int  end = a + CHUNK;

    if (test == T_CHK16)
    {
        for (; a < end; a += 2)
            *(volatile unsigned short *)a = pattern16(a);
        return;
    }
    for (; a < end; a += 4)
        *(volatile unsigned int *)a = pattern32(test, a);
}

static void             verify_chunk(const int test, const unsigned int offset)
{
    unsigned int        a = addr_of(offset);
    const unsigned int  end = a + CHUNK;

    if (test == T_CHK16)
    {
        for (; a < end; a += 2)
        {
            const unsigned short    expected = pattern16(a);
            const unsigned short    got = *(volatile unsigned short *)a;

            if (got != expected)
                record_error(a, expected, got);
        }
        return;
    }
    for (; a < end; a += 4)
    {
        const unsigned int  expected = pattern32(test, a);
        const unsigned int  got = *(volatile unsigned int *)a;

        if (got != expected)
            record_error(a, expected, got);
    }
}

/* ---- test 7: SH-2 DMA ---------------------------------------------- */
/* The DMA channel 0 of the master SH-2 is driven here, not through jo_dma_copy(): that one waits for the end
   of the transfer without a time limit, so a transfer that never ends (an emulator that does not do DMA on
   the A-Bus, an address error) froze the program with test 7 still marked "-". Here a transfer that does not
   end is stopped and reported as a failure, with the DMA registers. */
#define DMA_TIMEOUT     1000000u        /* polls: far more than a 4 KB transfer needs */

static __jo_force_inline bool   dma_busy(void)
{
    return ((JO_SH2_CHCR0 & (JO_SH2_CHCR_ENABLE | JO_SH2_CHCR_TRANSFER_END)) == JO_SH2_CHCR_ENABLE);
}

/* 32-bit units, both addresses incremented. Returns false if the transfer did not end */
static bool             dma_transfer(const unsigned int src, const unsigned int dst, const unsigned int size)
{
    unsigned int        sr;
    unsigned int        n;

    for (n = 0; dma_busy(); n++)
    {
        if (n > DMA_TIMEOUT)
            break;
    }
    sr = __jo_core_disable_interrupts();
    /* The transfer end flag has been read by dma_busy(): writing 0 clears it */
    JO_SH2_CHCR0 = 0;
    JO_SH2_SAR0 = src;
    JO_SH2_DAR0 = dst;
    JO_SH2_TCR0 = size >> 2;
    /* Clears the address error and NMI flags (read, then write) and enables the DMA */
    if (JO_SH2_DMAOR != JO_SH2_DMAOR_ENABLE)
        JO_SH2_DMAOR = JO_SH2_DMAOR_ENABLE;
    JO_SH2_CHCR0 = JO_SH2_CHCR_DEST_INCREMENT | JO_SH2_CHCR_SRC_INCREMENT | JO_SH2_CHCR_SIZE_32 |
                   JO_SH2_CHCR_AUTO_REQUEST | JO_SH2_CHCR_ENABLE;
    __jo_core_restore_interrupts(sr);
    for (n = 0; dma_busy(); n++)
    {
        if (n > DMA_TIMEOUT)
        {
            dma_chcr = JO_SH2_CHCR0;
            dma_dmaor = JO_SH2_DMAOR;
            dma_tcr = JO_SH2_TCR0;
            JO_SH2_CHCR0 = 0;
            return (false);
        }
    }
    return (true);
}

/* One location: Work RAM -> cart by DMA (checked by the CPU), then cart -> Work RAM by DMA */
static void             run_dma_at(const unsigned int cart_addr, const unsigned int salt)
{
    volatile unsigned char  *cart = (volatile unsigned char *)cart_addr;
    /* The DMA writes the Work RAM behind the cache: read it through the cache-through area */
    volatile unsigned char  *back = (volatile unsigned char *)((unsigned int)dma_dst | 0x20000000u);
    unsigned int        i;

    for (i = 0; i < DMA_SIZE; i++)
    {
        dma_src[i] = (unsigned char)((i * 7u) ^ (i >> 8) ^ salt ^ run_seed);
        back[i] = 0;
        cart[i] = 0;
    }
    /* A mismatch here is the DMA write to the cart */
    if (!dma_transfer((unsigned int)dma_src, cart_addr, DMA_SIZE))
    {
        dma_timeout = 1;
        record_error(cart_addr, 0, 0);
        return;
    }
    for (i = 0; i < DMA_SIZE; i++)
    {
        const unsigned char got = cart[i];

        if (got != dma_src[i])
            record_error(cart_addr + i, dma_src[i], got);
    }
    /* A mismatch here (and none above) is the DMA read from the cart */
    if (!dma_transfer(cart_addr, (unsigned int)dma_dst, DMA_SIZE))
    {
        dma_timeout = 2;
        record_error(cart_addr, 0, 0);
        return;
    }
    for (i = 0; i < DMA_SIZE; i++)
    {
        const unsigned char got = back[i];

        if (got != dma_src[i])
            record_error(cart_addr + i, dma_src[i], got);
    }
}

/* Location `index` of the DMA test: start, middle, end of the cartridge RAM */
static void             run_dma(const unsigned int index)
{
    if (index == 0)
        run_dma_at(addr_of(0), 0x11);
    else if (index == 1)
        run_dma_at(addr_of(total_bytes >> 1), 0x22);
    else
        run_dma_at(addr_of(total_bytes - DMA_SIZE), 0x33);
}

/* ---- sequencer ----------------------------------------------------- */
static void             publish(const unsigned int done)
{
    cart_res[0] = 0xCA27CA27u;
    cart_res[1] = cart_id;
    cart_res[2] = (unsigned int)cart_kind;
    cart_res[3] = total_errors;
    cart_res[4] = first_addr;
    cart_res[5] = first_exp;
    cart_res[6] = first_got;
    cart_res[7] = (unsigned int)first_test;
    cart_res[8] = run_count;
    cart_res[9] = pass_count;
    cart_res[10] = done;
}

static void             start_run(void)
{
    int                 i;

    for (i = 0; i < T_COUNT; i++)
    {
        test_status[i] = S_PENDING;
        test_errors[i] = 0;
    }
    total_errors = 0;
    first_addr = 0;
    first_exp = 0;
    first_got = 0;
    first_test = 0;
    dma_timeout = 0;
    cur_test = 0;
    pos = 0;
    ++run_count;
    run_seed = run_count * 0x01234567u + 0x13579BDFu;
    run_start_vbl = jo_core_get_vblank_count();
    run_frames = 0;
    detect_cart();
    phase = (cart_kind == KIND_1MB || cart_kind == KIND_4MB) ? PH_START : PH_NOCART;
    publish(0);
    jo_clear_screen();
}

static void             finish_test(void)
{
    test_status[cur_test] = test_errors[cur_test] ? S_FAIL : S_PASS;
    ++cur_test;
    pos = 0;
    phase = PH_START;
    if (cur_test >= T_COUNT)
    {
        cur_test = T_COUNT - 1;
        phase = PH_DONE;
        run_frames = jo_core_get_vblank_count() - run_start_vbl;
        if (total_errors == 0)
            ++pass_count;
        publish(1);
    }
}

static void             step(void)
{
    if (jo_is_pad1_key_down(JO_KEY_A))
        auto_loop = !auto_loop;
    /* The setup mode is used by the next run */
    if (jo_is_pad1_key_down(JO_KEY_B))
        init_mode = (init_mode + 1) % INIT_MODES;
    if (phase == PH_DONE || phase == PH_NOCART)
    {
        if (jo_is_pad1_key_down(JO_KEY_START) || (auto_loop && phase == PH_DONE))
            start_run();
        return;
    }
    run_frames = jo_core_get_vblank_count() - run_start_vbl;
    test_status[cur_test] = S_RUNNING;
    switch (cur_test)
    {
    case T_PROBE:
        run_probe();
        finish_test();
        return;
    case T_ALIAS:
        run_alias();
        finish_test();
        return;
    case T_DMA:
        /* One step per frame: the screen shows the test RUNNING and the location before each transfer */
        if (phase == PH_START)
        {
            pos = 0;
            phase = PH_VERIFY;
            return;
        }
        run_dma(pos);
        if (++pos >= DMA_LOCATIONS || dma_timeout)
            finish_test();
        return;
    default:
        break;
    }
    /* Whole RAM tests */
    switch (phase)
    {
    case PH_START:
        pos = 0;
        phase = PH_FILL;
        break;
    case PH_FILL:
        fill_chunk(cur_test, pos);
        pos += CHUNK;
        if (pos >= total_bytes)
        {
            pos = 0;
            if (cur_test == T_RETAIN)
            {
                wait_until = jo_core_get_vblank_count() + RETAIN_FRAMES;
                phase = PH_WAIT;
            }
            else
                phase = PH_VERIFY;
        }
        break;
    case PH_WAIT:
        if ((int)(jo_core_get_vblank_count() - wait_until) >= 0)
            phase = PH_VERIFY;
        break;
    case PH_VERIFY:
        verify_chunk(cur_test, pos);
        pos += CHUNK;
        if (pos >= total_bytes)
            finish_test();
        break;
    default:
        break;
    }
}

/* ---- display ------------------------------------------------------- */
static void             draw_bar(const int y, const unsigned int percent)
{
    char                bar[24];
    unsigned int        i;
    const unsigned int  filled = percent / 5u;

    bar[0] = '[';
    for (i = 0; i < 20; i++)
        bar[1 + i] = i < filled ? '#' : '.';
    bar[21] = ']';
    bar[22] = '\0';
    jo_printf(1, y, "%s %3u%%", bar, percent);
}

static void             draw(void)
{
    int                 i;
    unsigned int        percent;

    jo_set_printf_color_index(JO_COLOR_INDEX_White);
    jo_printf(1, 1, "RAM CARTRIDGE TEST 1.0     RUN %u", run_count);
    switch (cart_kind)
    {
    case KIND_4MB:
        jo_printf(1, 3, "CART ID %02X  EXTENDED RAM 4MB (32MBIT)", cart_id);
        jo_printf(1, 4, "RAM 4096 KB AT %08X", banks[0].base);
        break;
    case KIND_1MB:
        jo_printf(1, 3, "CART ID %02X  EXTENDED RAM 1MB (8MBIT)", cart_id);
        jo_printf(1, 4, "RAM 2 X 512 KB AT %08X %08X", banks[0].base, banks[1].base);
        break;
    case KIND_UNKNOWN:
        jo_printf(1, 3, "CART ID %02X  NOT A SEGA RAM CARTRIDGE", cart_id);
        jo_printf_with_color(1, 5, JO_COLOR_INDEX_Yellow, "NOT TESTED: NOTHING WAS WRITTEN TO IT");
        break;
    default:
        jo_printf(1, 3, "CART ID %02X  NO CARTRIDGE", cart_id);
        jo_printf_with_color(1, 5, JO_COLOR_INDEX_Yellow, "INSERT A RAM CARTRIDGE (POWER OFF)");
        break;
    }
    if (phase == PH_NOCART)
    {
        jo_set_printf_color_index(JO_COLOR_INDEX_White);
        jo_printf(1, 7, "EMULATOR: SELECT THE 4MB RAM CART");
        jo_printf(1, 8, "IN THE CARTRIDGE SETTINGS");
        jo_printf(1, 25, "START: TRY AGAIN");
        return;
    }

    jo_set_printf_color_index(JO_COLOR_INDEX_White);
    jo_printf(1, 5, "SETUP THIS RUN: %s", init_applied ? "TB47 (ENABLE+A-BUS)" : "NONE               ");

    for (i = 0; i < T_COUNT; i++)
    {
        jo_set_printf_color_index(JO_COLOR_INDEX_White);
        jo_printf(1, 6 + i, "%d %-14s", i + 1, test_name[i]);
        switch (test_status[i])
        {
        case S_RUNNING:
            jo_printf_with_color(18, 6 + i, JO_COLOR_INDEX_Yellow, "RUNNING         ");
            break;
        case S_PASS:
            jo_printf_with_color(18, 6 + i, JO_COLOR_INDEX_Green, "PASS            ");
            break;
        case S_FAIL:
            jo_printf_with_color(18, 6 + i, JO_COLOR_INDEX_Red, "FAIL %-10u ", test_errors[i]);
            break;
        default:
            jo_printf(18, 6 + i, "-               ");
            break;
        }
    }

    /* Progress of the current test */
    jo_set_printf_color_index(JO_COLOR_INDEX_White);
    percent = total_bytes ? ((pos >> 10) * 100u) / (total_bytes >> 10) : 0;
    if (percent > 100)
        percent = 100;
    if (cur_test == T_DMA && phase != PH_DONE)
    {
        /* pos is the location about to be transferred */
        jo_printf(1, 14, "%-14s TRANSFER %u OF %u   ", test_name[cur_test],
                  pos < DMA_LOCATIONS ? pos + 1 : DMA_LOCATIONS, DMA_LOCATIONS);
        draw_bar(15, (pos * 100u) / DMA_LOCATIONS);
    }
    else
    {
    switch (phase)
    {
    case PH_FILL:
        jo_printf(1, 14, "%-14s WRITING   %4u KB", test_name[cur_test], pos >> 10);
        break;
    case PH_VERIFY:
        jo_printf(1, 14, "%-14s VERIFYING %4u KB", test_name[cur_test], pos >> 10);
        break;
    case PH_WAIT:
        {
            const int   left = (int)(wait_until - jo_core_get_vblank_count());

            jo_printf(1, 14, "%-14s WAITING   %4d   ", test_name[cur_test], left > 0 ? left : 0);
            percent = left > 0 ? 100u - ((unsigned int)left * 100u) / RETAIN_FRAMES : 100u;
        }
        break;
    case PH_DONE:
        jo_printf(1, 14, "ALL TESTS DONE                    ");
        percent = 100;
        break;
    default:
        jo_printf(1, 14, "%-14s                   ", test_name[cur_test]);
        break;
    }
    draw_bar(15, percent);
    }

    /* Errors */
    if (total_errors)
    {
        jo_set_printf_color_index(JO_COLOR_INDEX_Red);
        jo_printf(1, 17, "ERRORS %u", total_errors);
        jo_printf(1, 18, "FIRST AT %08X  TEST %d", first_addr, first_test + 1);
        jo_printf(1, 19, "EXPECTED %08X GOT %08X", first_exp, first_got);
        if (dma_timeout)
        {
            jo_printf(1, 20, "DMA %s NEVER ENDED", dma_timeout == 1 ? "WRITE" : "READ");
            jo_printf(1, 24, "CHCR %08X DMAOR %X TCR %X", dma_chcr, dma_dmaor, dma_tcr);
        }
    }
    else
    {
        jo_set_printf_color_index(JO_COLOR_INDEX_White);
        jo_printf(1, 17, "ERRORS 0");
    }

    /* Verdict */
    if (phase == PH_DONE)
    {
        if (total_errors)
            jo_printf_with_color(1, 21, JO_COLOR_INDEX_Red, "RESULT: FAIL   ");
        else
            jo_printf_with_color(1, 21, JO_COLOR_INDEX_Green, "RESULT: PASS   ");
    }
    else
        jo_printf_with_color(1, 21, JO_COLOR_INDEX_Yellow, "RESULT: TESTING");
    jo_set_printf_color_index(JO_COLOR_INDEX_White);
    jo_printf(1, 22, "TIME %u FRAMES   ", run_frames);
    jo_printf(1, 23, "RUNS PASSED %u OF %u   ", pass_count, phase == PH_DONE ? run_count : run_count - 1);
    jo_printf(1, 25, "START: RUN AGAIN   A: LOOP %s", auto_loop ? "ON " : "OFF");
    jo_printf(1, 26, "B: SETUP NEXT RUN %s",
              init_mode == INIT_AUTO ? "AUTO " : init_mode == INIT_NONE ? "NEVER" : "TB47 ");
}

void                    jo_main(void)
{
    jo_core_init(JO_COLOR_Black);
    start_run();
    jo_core_add_callback(step);
    jo_core_add_callback(draw);
    jo_core_run();
}
