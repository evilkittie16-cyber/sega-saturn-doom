#include "ram_test.h"

/* Extended RAM cart detection and test.
 *
 * The init sequence follows Saturn_CartInit (i_wad.c), which was brought
 * up on real hardware:
 *   - the ID is read as a 16-bit word at the even address 0x24FFFFFE
 *     (low byte = ID), not as a byte at 0x24FFFFFF;
 *   - only Sega DRAM cart IDs are driven; anything else (a Saroo answers
 *     0x06, an empty slot 0x00/0xFF) is treated as "no cart";
 *   - CART_INIT and the A-Bus registers are written ONLY for the 1 MB
 *     cart. The 4 MB cart works without them, and touching the A-Bus
 *     registers breaks Saroo flashcarts;
 *   - probes are 16-bit first, every probe restores the word it touched.
 *
 * Fixed compared with the previous version of this file:
 *   - A-Bus registers: ASR0 is 0x25FE00B0 and AREF is 0x25FE00B8. The old
 *     0x25FE0080 is the SCU DSP program control port, and 0x25FE0033 is
 *     odd, so a 32-bit write there raised an address error on real HW.
 *   - The 1 MB cart is two 512 KB banks (0x22400000 and 0x22600000), not
 *     1 MB in a row: 0x22480000-0x224FFFFF mirrors the first bank.
 *   - An aliasing test catches mirrored RAM, which plain write/read-back
 *     probes cannot see.
 *   - DMA buffers are aligned so jo_dma_copy really uses the DMA (it falls
 *     back to a CPU copy for unaligned or < 64-byte transfers). jo_dma_copy
 *     waits for the end of the transfer and purges the destination from the
 *     cache, so the read-back is not stale.
 *   - The cart RAM is handed to jo_malloc only if RAM_TEST_ADD_TO_JO_MALLOC
 *     is set (see ram_test.h): in DOOM i_wad.c owns the cart.
 */

// Global State Definitions
int g_cart_test_ran = 0;
int g_memory_test_passed = 0;
int g_dma_test_passed = 0;
int g_cart_fail_point = RAM_TEST_OK;
unsigned long g_cart_fail_addr = 0;
unsigned char g_cart_id_byte = 0;
char g_cart_type_str[40] = "Base System (No Expansion)";
int g_allocated_kb = 0;
void *g_expansion_buffer = NULL;
int g_total_expansion_mb = 0;
int g_cart_id = 0;

// --- Hardware constants (TB#47, SCU manual) ---
#define CART_ID_ADDR_16     ((volatile unsigned short *)0x24FFFFFE)
#define CART_ID_8MBIT       0x5A
#define CART_ID_32MBIT      0x5C
#define CART_INIT_ADDR      ((volatile unsigned short *)0x257EFFFE)
#define A_BUS_SET_REG       ((volatile unsigned long *)0x25FE00B0)  // SCU ASR0
#define A_BUS_REF_REG       ((volatile unsigned long *)0x25FE00B8)  // SCU AREF
#define A_BUS_SET_VALUE     0x23301FF0UL
#define A_BUS_REF_VALUE     0x00000013UL

/* Cart RAM through the SH-2 cache-through area (0x2xxxxxxx), so a stale
 * cache line can never fake a probe result. Do not use 0x4xxxxxxx: on the
 * SH-2 that is the associative purge area, not a mirror of the RAM. */
#define CART_BANK0          0x22400000UL    // DRAM0
#define CART_BANK1          0x22600000UL    // DRAM1
#define KB(n)               ((unsigned long)(n) * 1024UL)

typedef struct
{
    unsigned long   base;
    unsigned long   size;
} cart_bank;

// DMA round trip: 256 bytes, 16-byte aligned (>= 64 bytes and aligned = real DMA in jo)
#define DMA_TEST_SIZE 256
static unsigned char g_dma_src_buffer[DMA_TEST_SIZE] __attribute__((aligned(16)));
static unsigned char g_dma_dst_buffer[DMA_TEST_SIZE] __attribute__((aligned(16)));
static unsigned short g_dma_saved_words[DMA_TEST_SIZE / 2];

// Most addresses checked by the aliasing test (4 MB cart: 7, 1 MB cart: 8)
#define ALIAS_MAX_POINTS 16

/* Same settle delay Saturn_CartInit uses after the cart/A-Bus writes */
static void bus_settle(void)
{
    volatile int d;

    for (d = 0; d < 0x200; d++)
        __asm__ volatile ("nop");
}

static void record_fail(int point, unsigned long addr)
{
    if (g_cart_fail_point != RAM_TEST_OK)
        return;     // keep the first failure
    g_cart_fail_point = point;
    g_cart_fail_addr = addr;
}

/* Write/verify one 16-bit word, restoring the original value */
static int probe16(unsigned long addr, unsigned short pattern)
{
    volatile unsigned short *p = (volatile unsigned short *)addr;
    unsigned short saved = *p;
    unsigned short rb;

    *p = pattern;
    rb = *p;
    *p = saved;
    return (rb == pattern);
}

/* Write/verify one 32-bit word, restoring the original value */
static int probe32(unsigned long addr, unsigned long pattern)
{
    volatile unsigned long *p = (volatile unsigned long *)addr;
    unsigned long saved = *p;
    unsigned long rb;

    *p = pattern;
    rb = *p;
    *p = saved;
    return (rb == pattern);
}

/* Start / middle / last word of one bank: 16-bit first (the width proven on
 * real hardware), then 32-bit (memcpy into the cart moves longwords). */
static int test_bank(const cart_bank *b)
{
    const unsigned long mid = b->base + (b->size >> 1);
    const unsigned long last16 = b->base + b->size - 2;
    const unsigned long last32 = b->base + b->size - 4;

    if (!probe16(b->base, 0x5555)) { record_fail(RAM_TEST_FAIL_PROBE16, b->base); return 0; }
    if (!probe16(mid, 0xAAAA))     { record_fail(RAM_TEST_FAIL_PROBE16, mid);     return 0; }
    if (!probe16(last16, 0x5A5A))  { record_fail(RAM_TEST_FAIL_PROBE16, last16);  return 0; }

    if (!probe32(b->base, 0xDEADBEEFUL)) { record_fail(RAM_TEST_FAIL_PROBE32, b->base); return 0; }
    if (!probe32(mid, 0xCAFEBABEUL))     { record_fail(RAM_TEST_FAIL_PROBE32, mid);     return 0; }
    if (!probe32(last32, 0x12345678UL))  { record_fail(RAM_TEST_FAIL_PROBE32, last32);  return 0; }
    return 1;
}

/* Aliasing test: each bank's start plus every power-of-two offset from
 * 64 KB up to the bank size. All points get a different value first, then
 * all are read back: if two addresses reach the same RAM (a mirror, a
 * smaller cart than its ID says), the later write overwrote the earlier
 * one. All words are saved before the first write and restored at the end. */
static int test_aliasing(const cart_bank *banks, int bank_count)
{
    unsigned long   addr[ALIAS_MAX_POINTS];
    unsigned short  saved[ALIAS_MAX_POINTS];
    int             count = 0;
    int             ok = 1;
    int             i;

    for (i = 0; i < bank_count; i++)
    {
        unsigned long off;

        addr[count++] = banks[i].base;
        for (off = KB(64); off < banks[i].size && count < ALIAS_MAX_POINTS; off <<= 1)
            addr[count++] = banks[i].base + off;
    }

    for (i = 0; i < count; i++)
        saved[i] = *(volatile unsigned short *)addr[i];

    for (i = 0; i < count; i++)
        *(volatile unsigned short *)addr[i] = (unsigned short)(0xA500 | i);

    for (i = 0; i < count; i++)
    {
        if (*(volatile unsigned short *)addr[i] != (unsigned short)(0xA500 | i))
        {
            record_fail(RAM_TEST_FAIL_ALIAS, addr[i]);
            ok = 0;
            break;
        }
    }

    // Aliased points saved the same original value, so any order restores it
    for (i = count - 1; i >= 0; i--)
        *(volatile unsigned short *)addr[i] = saved[i];
    return ok;
}

/* SH-2 DMA round trip: Work RAM -> cart -> Work RAM, then compare.
 * The cart bytes it uses are saved and restored with 16-bit CPU accesses. */
static int test_dma(unsigned long cart_addr)
{
    volatile unsigned short *cart = (volatile unsigned short *)cart_addr;
    int ok = 1;
    int i;

    for (i = 0; i < DMA_TEST_SIZE / 2; i++)
        g_dma_saved_words[i] = cart[i];

    for (i = 0; i < DMA_TEST_SIZE; i++)
        g_dma_src_buffer[i] = (unsigned char)(i ^ 0xA5);
    memset(g_dma_dst_buffer, 0, DMA_TEST_SIZE);

    // jo_dma_copy() is synchronous: it waits for the end of the transfer
    // and purges the destination lines from the cache
    jo_dma_copy(g_dma_src_buffer, (void *)cart_addr, DMA_TEST_SIZE);
    jo_dma_copy((const void *)cart_addr, g_dma_dst_buffer, DMA_TEST_SIZE);

    for (i = 0; i < DMA_TEST_SIZE; i++)
    {
        if (g_dma_src_buffer[i] != g_dma_dst_buffer[i])
        {
            record_fail(RAM_TEST_FAIL_DMA, cart_addr + (unsigned long)i);
            ok = 0;
            break;
        }
    }

    for (i = 0; i < DMA_TEST_SIZE / 2; i++)
        cart[i] = g_dma_saved_words[i];
    return ok;
}

void init_hardware_and_test(void)
{
    cart_bank   banks[2];
    int         bank_count;
    int         i;

    // Default State: no cart, nothing tested
    g_cart_test_ran = 0;
    g_memory_test_passed = 0;
    g_dma_test_passed = 0;
    g_cart_fail_point = RAM_TEST_OK;
    g_cart_fail_addr = 0;
    g_allocated_kb = 0;
    g_expansion_buffer = NULL;
    g_total_expansion_mb = 0;
    g_cart_id = 0;
    strcpy(g_cart_type_str, "Base System (No Expansion)");

    // 1. Cartridge ID: 16-bit read at the even address, ID in the low byte
    g_cart_id_byte = (unsigned char)(*CART_ID_ADDR_16 & 0x00FF);

    switch (g_cart_id_byte)
    {
    case CART_ID_8MBIT:
    case 0x1A:      // 1 MB variants also accepted by Saturn_CartInit
    case 0xDA:
        g_cart_id = 1;
        g_total_expansion_mb = 1;
        strcpy(g_cart_type_str, "Extended RAM (8Mbit)");
        banks[0].base = CART_BANK0;
        banks[0].size = KB(512);
        banks[1].base = CART_BANK1;
        banks[1].size = KB(512);
        bank_count = 2;

        // 2. Init + A-Bus timings: 1 MB cart only (TB#47)
        *CART_INIT_ADDR = 0x0001;
        bus_settle();
        *A_BUS_SET_REG = A_BUS_SET_VALUE;
        *A_BUS_REF_REG = A_BUS_REF_VALUE;
        bus_settle();
        break;

    case CART_ID_32MBIT:
        g_cart_id = 2;
        g_total_expansion_mb = 4;
        strcpy(g_cart_type_str, "Extended RAM (32Mbit)");
        // DRAM0 (2 MB) and DRAM1 (2 MB) are back to back: one 4 MB block
        banks[0].base = CART_BANK0;
        banks[0].size = KB(4096);
        bank_count = 1;
        // No CART_INIT / A-Bus writes: not needed, and they break Saroo carts
        break;

    default:
        // 0x00 / 0xFF = empty slot; any other ID (a Saroo answers 0x06)
        // is not a Sega DRAM cart and must not be written to
        if (g_cart_id_byte != 0x00 && g_cart_id_byte != 0xFF)
            strcpy(g_cart_type_str, "Unknown Cart (Not DRAM)");
        return;
    }

    g_cart_test_ran = 1;

    // 3. Memory tests: every bank, then aliasing across all of them
    g_memory_test_passed = 1;
    for (i = 0; i < bank_count; i++)
    {
        if (!test_bank(&banks[i]))
        {
            g_memory_test_passed = 0;
            break;
        }
    }
    if (g_memory_test_passed && !test_aliasing(banks, bank_count))
        g_memory_test_passed = 0;

    // 4. DMA test, only on RAM that answered the CPU probes
    if (g_memory_test_passed)
        g_dma_test_passed = test_dma(banks[0].base);

    if (!g_memory_test_passed || !g_dma_test_passed)
    {
        strcpy(g_cart_type_str, "Expansion Detected BUT Failed Tests");
        return;
    }

    g_expansion_buffer = (void *)banks[0].base;

    // 5. Optionally hand the RAM to jo_malloc (see ram_test.h)
#if RAM_TEST_ADD_TO_JO_MALLOC
    for (i = 0; i < bank_count; i++)
    {
        jo_add_memory_zone((unsigned char *)banks[i].base, (unsigned int)banks[i].size);
        g_allocated_kb += (int)(banks[i].size / 1024UL);
    }
#endif
}
