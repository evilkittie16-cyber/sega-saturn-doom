#include "ram_test.h"
#include <jo/jo.h>

// Global State Definitions
int g_memory_test_passed = 1; 
int g_dma_test_passed = 1; 
char g_cart_type_str[40] = "Base System (No Expansion)";
int g_allocated_kb = 0;
void *g_expansion_buffer = NULL;
int g_total_expansion_mb = 0;
int g_cart_id = 0; 

// Buffer sizes for DMA tests
#define DMA_TEST_SIZE 256
unsigned char g_dma_src_buffer[DMA_TEST_SIZE];
unsigned char g_dma_dst_buffer[DMA_TEST_SIZE];

// --- Hardware Constants from TB#47 ---
#define CART_ID_ADDR        ((volatile unsigned char *)0x24FFFFFF)
#define CART_INIT_ADDR      ((volatile unsigned short *)0x257EFFFE)
#define A_BUS_SET_REG       ((volatile unsigned long *)0x25FE0080)
#define A_BUS_REF_REG       ((volatile unsigned long *)0x25FE0033)

// Memory Map for Extended RAM (DRAM0)
#define EXT_RAM_BASE        0x22400000
#define EXT_RAM_SIZE_1MB    (1024 * 1024)
#define EXT_RAM_SIZE_4MB    (4 * 1024 * 1024)

// Helper: Uncached Probe
int probe_memory(unsigned long addr, unsigned long pattern) {
    // Align to 4 bytes for longword access
    if (addr % 4 != 0) addr = addr & ~3;
    
    volatile unsigned long *ptr = (volatile unsigned long *)(addr | 0x20000000);
    *ptr = pattern;
    unsigned long read_val = *ptr;
    *ptr = 0; // Clear
    
    return (read_val == pattern);
}

void init_hardware_and_test(void) {
    // Default State
    g_total_expansion_mb = 0;
    g_expansion_buffer = NULL;
    g_cart_id = 0;
    strcpy(g_cart_type_str, "Base System");

    // 1. Check Cartridge ID
    unsigned char cart_id = *CART_ID_ADDR;
    
    if (cart_id == 0x5A) {
        g_cart_id = 1; // 8Mbit (1MB usable in DRAM0 usually, or split)
        strcpy(g_cart_type_str, "Extended RAM (8Mbit)");
        g_total_expansion_mb = 1;
    } else if (cart_id == 0x5C) {
        g_cart_id = 2; // 32Mbit (4MB usable in DRAM0/DRAM1)
        strcpy(g_cart_type_str, "Extended RAM (32Mbit)");
        g_total_expansion_mb = 4;
    } else {
        // No cartridge detected or invalid ID
        return; 
    }

    // 2. Initialize Cartridge (TB#47 Step 3)
    // Write "1" in word size to 257EFFFEh
    *CART_INIT_ADDR = 0x0001;

    // 3. Configure A-Bus Registers (TB#47 Section 8)
    // A-Bus set register (25FE0080h) = 2330 1FF0h
    *A_BUS_SET_REG = 0x23301FF0;
    // A-Bus refresh register (25FE0033h) = 0000 0013h
    *A_BUS_REF_REG = 0x00000013;

    // Set buffer pointer to DRAM0 base
    g_expansion_buffer = (void *)EXT_RAM_BASE;

    // 4. Memory Access Test
    // Test start, middle, and end of the configured RAM
    unsigned long test_addr_start = EXT_RAM_BASE;
    unsigned long test_addr_end   = EXT_RAM_BASE + (g_total_expansion_mb * 1024 * 1024) - 4;
    unsigned long test_addr_mid   = (test_addr_start + test_addr_end) / 2;

    if (!probe_memory(test_addr_start, 0xDEADBEEF)) {
        g_memory_test_passed = 0;
    }
    if (!probe_memory(test_addr_mid, 0xCAFEBABE)) {
        g_memory_test_passed = 0;
    }
    if (!probe_memory(test_addr_end, 0x12345678)) {
        g_memory_test_passed = 0;
    }

    // 5. DMA Tests
    // Fill source buffer
    for (int i = 0; i < DMA_TEST_SIZE; i++) {
        g_dma_src_buffer[i] = (unsigned char)i;
    }

    // Test SH2 DMA Write (CPU -> Ext RAM)
    // Note: jo_dma_copy handles SH2 DMA. 
    // We must ensure the destination is uncached or handled correctly.
    // jo engine usually handles the cache maintenance if configured, but for raw pointers:
    jo_dma_copy(g_dma_src_buffer, (void *)(EXT_RAM_BASE | 0x20000000), DMA_TEST_SIZE);
    
    // Test SH2 DMA Read (Ext RAM -> CPU)
    jo_dma_copy((void *)(EXT_RAM_BASE | 0x20000000), g_dma_dst_buffer, DMA_TEST_SIZE);

    // Verify Data
    for (int i = 0; i < DMA_TEST_SIZE; i++) {
        if (g_dma_src_buffer[i] != g_dma_dst_buffer[i]) {
            g_dma_test_passed = 0;
            break;
        }
    }

    // Additional SCU DMA Read Test (TB#47 allows SCU DMA Read)
    // Clear dest buffer
    memset(g_dma_dst_buffer, 0, DMA_TEST_SIZE);
    // Use jo_scu_dma_read if available in your jo setup, otherwise stick to SH2 DMA which is safer for general testing
    // For this example, we assume SH2 DMA is sufficient to prove connectivity as per "Test DMA transfers" requirement.
    
    // 6. Calculate Allocated KB
    g_allocated_kb = g_total_expansion_mb * 1024;

    // 7. Register with Joe Engine if tests passed
    if (g_memory_test_passed && g_dma_test_passed) {
        jo_add_memory_zone((unsigned char *)g_expansion_buffer, g_total_expansion_mb * 1024 * 1024);
    } else {
        strcpy(g_cart_type_str, "Expansion Detected BUT Failed Tests");
    }
}
