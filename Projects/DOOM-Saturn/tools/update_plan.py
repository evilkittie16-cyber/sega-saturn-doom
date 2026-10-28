import os
import re

plan_path = r"C:\Users\charl\.gemini\antigravity\brain\4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4\rendering_plan.md"

with open(plan_path, "r", encoding="utf-8") as f:
    text = f.read()

# Section 4.1.3 code snippet replacement
old_slave_loop = '''       while (1) {
           while (!(*FTCSR & 0x80)) ;               /* On-chip FRT spin-lock (0 bus cycles) */
           *FTCSR = 0x0;                            /* Clear flag */
           slaveDraw();                             /* Process visible geometry */
           *(Uint16 volatile *)0x21800000 = 0xffff; /* Hardware handshake */
       }'''

new_slave_loop = '''       /* Hardened Dual-CPU Ring Buffer Polling (ST-202 & ST-097 silicon compliance) */
       volatile uint8_t *job_ready = (volatile uint8_t *)0x20210000;
       while (1) {
           while (!*job_ready) {
               __asm__ volatile("nop; nop; nop; nop;"); // Yield B-Bus arbiter
           }
           *job_ready = 0;                          /* Acknowledge job */
           slaveDraw();                             /* Process visible geometry */
           *(Uint16 volatile *)0x21800000 = 0xffff; /* Hardware handshake / status */
       }'''

if old_slave_loop in text:
    text = text.replace(old_slave_loop, new_slave_loop)
    print("[+] Replaced Section 4.1.3 slave loop")

# Flowchart Phase 3B label
text = text.replace(
    'Phase 3B: Lobotomy FTCSR Slave Handshake (60 FPS)',
    'Phase 3B: Zero-Contention Dual-CPU Handshake (60 FPS)'
)

# Table Phase 3B description
text = text.replace(
    'Slave SH-2 Lobotomy FTCSR handshake, Low Work RAM completion flags, 60 FPS',
    'Slave SH-2 uncached ring-buffer handshake with NOP bus yielding, 60 FPS'
)

# Section 4.1.4 text
text = text.replace(
    'install the isolated Lobotomy `FTCSR` / `0x21000000` handshake routine.',
    'install the hardened uncached ring-buffer / NOP arbiter yielding handshake routine.'
)
text = text.replace(
    'with on-chip `FTCSR` spinning at `0xFFFFFE11` and hardware triggers at `0x21000000` / `0x21800000`.',
    'with uncached ring-buffer spinning at `0x20210000` padded with NOPs yielding the B-Bus arbiter.'
)

# Trap 13 table entry
text = text.replace(
    'Master spins 100% internally on `FTCSR`, while Slave pulses `0xFFFF` to `0x21800000`.',
    'Master reads from internal OCRAM or uncached ring buffer with NOP yielding.'
)

# Section 20.12.1 dual-CPU handshake text
text = re.sub(
    r'\* \*\*Master CPU \\to Slave CPU Signal\*\*:\n\s+\* Rather than invoking the 86-cycle interrupt handler.*?\(clearing via Master\'s `FTCSR`\)\.',
    '''* **Master CPU to Slave CPU Signal (Hardened Architecture)**:
  * Rather than invoking interrupt handler dispatch latency or falling into the `FTCSR` ICF trap (where `FTCI` is tied to V-BLANK), synchronization operates via an uncached ring-buffer flag in Low Work RAM (`0x20210000`).
  * The Slave CPU polls this memory address with `nop; nop; nop; nop;` instructions to continuously yield the B-Bus arbiter. Because the Master SH-2 executes from High Work RAM L1 cache on the CPU Bus, Master execution proceeds at maximum speed with zero bus contention.
* **Slave CPU to Master CPU Signal**:
  * Writing completion status into the Master's local On-Chip Scratchpad RAM (OCRAM) via cache-through mirror (`0x20000000 | 0xC0000000`) provides 1-cycle status reads for the Master.''',
    text,
    flags=re.DOTALL
)

# Architecture B4 box
text = text.replace(
    'Sub-microsecond FTCSR loop, zero interrupt overhead',
    'Uncached ring buffer loop with NOP yielding, zero bus contention'
)

# Section 20.12.5 text
text = text.replace(
    'Polling `FTCSR & 0x80` with interrupts masked (`set_imask(0xf)`) and `TIER = 0x01` provides sub-microsecond synchronization without interrupt dispatch latency.',
    'Polling uncached ring-buffer flags with NOP bus yielding provides sub-microsecond synchronization without bus arbiter starvation or interrupt latency.'
)

with open(plan_path, "w", encoding="utf-8") as f:
    f.write(text)

print("[+] Successfully completed full plan updates!")
