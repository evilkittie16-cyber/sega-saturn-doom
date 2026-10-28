#ifndef RAM_TEST_H
#define RAM_TEST_H

#include <jo/jo.h>
#include <string.h>

// Global State Variables (Accessible from main.c)
extern int g_memory_test_passed;
extern int g_dma_test_passed; 
extern char g_cart_type_str[40];
extern int g_allocated_kb;
extern void *g_expansion_buffer;
extern int g_total_expansion_mb;
extern int g_cart_id; // 0=None, 1=8Mbit, 2=32Mbit

// Function Prototypes
void init_hardware_and_test(void);

#endif
