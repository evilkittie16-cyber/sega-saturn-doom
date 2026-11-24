#ifndef RAM_TEST_H
#define RAM_TEST_H

#include <jo/jo.h>
#include <string.h>

/* Set to 1 to hand the cart RAM to jo_malloc() after the tests pass.
 * Leave it at 0 in DOOM: i_wad.c (Saturn_CartInit) owns the cart and
 * places hot lumps, the texture arena and the Z zone there, so jo_malloc
 * must not hand the same RAM out. Also leave JO_COMPILE_WITH_RAM_CARD_MODULE
 * at 0 in the makefile if you enable this, or the RAM is registered twice. */
#ifndef RAM_TEST_ADD_TO_JO_MALLOC
#define RAM_TEST_ADD_TO_JO_MALLOC 0
#endif

/* Where the test stopped (g_cart_fail_point) */
#define RAM_TEST_OK             0
#define RAM_TEST_FAIL_PROBE16   1   /* 16-bit write/verify failed        */
#define RAM_TEST_FAIL_PROBE32   2   /* 32-bit write/verify failed        */
#define RAM_TEST_FAIL_ALIAS     3   /* two addresses hit the same RAM    */
#define RAM_TEST_FAIL_DMA       4   /* SH-2 DMA round trip mismatched    */

// Global State Variables (Accessible from main.c)
extern int g_cart_test_ran;          // 1 once a known DRAM cart was found and tested
extern int g_memory_test_passed;     // only meaningful when g_cart_test_ran == 1
extern int g_dma_test_passed;        // only meaningful when g_cart_test_ran == 1
extern int g_cart_fail_point;        // RAM_TEST_* code of the first failure
extern unsigned long g_cart_fail_addr; // address of the first failure (0 if none)
extern unsigned char g_cart_id_byte; // raw ID byte read from the cart
extern char g_cart_type_str[40];
extern int g_allocated_kb;           // KB handed to jo_malloc (0 unless enabled and passed)
extern void *g_expansion_buffer;     // start of bank 0, NULL if no usable cart
extern int g_total_expansion_mb;
extern int g_cart_id;                // 0=None, 1=8Mbit, 2=32Mbit

// Function Prototypes
// Detects and tests the extended RAM cart. Safe to call more than once:
// it restores every cart word it touches, so it can also run after
// Saturn_CartInit has filled the cart -- but only while nothing else
// accesses the cart (not during the background WAD fill, not from an
// interrupt handler or the slave CPU).
void init_hardware_and_test(void);

#endif
