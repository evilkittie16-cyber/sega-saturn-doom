---
name: sega-saturn-doom
description: >-
  Architectural guide, coding invariants, performance profiling rules, and test automation
  runbooks for developing the Sega Saturn DOOM hardware renderer (SH-2 dual-CPU SMP,
  VDP1 quad rasterization, VDP2 backgrounds, and sdbg / Kronos emulator harnesses).
  Activate whenever modifying, profiling, optimizing, or debugging Saturn DOOM C or assembly code.
---

# Sega Saturn DOOM Engine & Hardware Renderer Skill

This skill documents the hardware constraints, critical rendering invariants, mathematical rules, and automated testing workflows for the Sega Saturn DOOM port.

---

## 1. VDP1 Hardware Rasterizer Invariants

### 11-Bit Coordinate Limit
- **Rule:** All projected quad vertices sent to the VDP1 command list must be clamped to `[-1024, 1023]`.
- **Reason:** The VDP1 hardware uses 11-bit signed registers for vertex coordinates $(X, Y)$. Any value outside $[-1024, 1023]$ is bitwise truncated (`val & 0x7FF`), wrapping off-screen geometry back onto the viewport and producing visual vertex explosions / spikes.
- **Implementation:** Always route projected vertices through `HW_ClampProj`:
  ```c
  static inline int HW_ClampProj(int v) {
      return v < -1024 ? -1024 : (v > 1023 ? 1023 : v);
  }
  ```

### Texture Winding & Hardware Mirroring
- **Rule:** Never invert polygon winding by swapping $P1$ and $P3$.
- **Reason:** VDP1 command mapping associates texture coordinates rigidly to:
  $A = (0, 0)$, $B = (W, 0)$, $C = (W, H)$, $D = (0, H)$.
  Swapping $P1$ and $P3$ flips the U/V texture axes diagonally and violates the hardware rule that degenerate triangles must have $C = D$.
- **Fix:** When cross-product detects counter-clockwise winding ($cw < 0$):
  1. Swap vertices horizontally to $(P1, P0, P3, P2)$.
  2. Emit command word with `CMD_DISTORTED | H-FLIP` (`0x00120000u`).
- **Silicon Erratum:** Avoid `CMD_DISTORTED | V-FLIP` (`0x00220000u`) on distorted quads due to real VDP1 hardware errata causing texel phase shifts and pixel dropouts.

---

## 2. SH-2 Dual-CPU (Hitachi Law C) Pipeline Optimization

### Zero-Stall Interleaved Dual-MAC Scheduling
- The SH-2 `dmuls.l` multiplier has a 1-cycle execution latency before reading `macl` or `mach`.
- **Rule:** Never execute `sts macl/mach` immediately after `dmuls.l`. Always interleave independent ALU or memory operations (e.g. `mov`, `sub`, `neg`) into the latency slot to achieve 0-cycle stall pipeline execution.

### Register Allocation & Inline ASM
- **Rule:** Do not hardcode or clobber `"r0"` in inline assembly blocks unless strictly required by SH-2 instructions (like `stc sr, r0` or byte-load `mov.b @(r0, r1)`).
- **Practice:** Use early-clobber output constraints `"=&r"(tmp)` for scratch registers so GCC can optimize register allocation without register spills.

### Cache Coherency & Purge Synchronization
- When purging the SH-2 L1 cache by writing to CCR (`0xFFFFFE92u`), always place an 8-NOP instruction barrier immediately following the write:
  ```c
  (*(volatile unsigned char *)0xFFFFFE92u) = 0x10u; /* purge */
  (*(volatile unsigned char *)0xFFFFFE92u) = 0x01u; /* enable */
  __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop;" ::: "memory");
  ```

---

## 3. Asynchronous Dual-Core IPC & Bus Arbitration

### Low Work RAM IPC (0x00200000)
- The Master CPU walks the BSP tree and renders walls into `saturn_cmd_m`.
- The Slave CPU asynchronously transforms, clips, and subdivides floor/ceiling flats into `saturn_cmd_s`.
- Communication happens via the lock-free circular queue `hw_slave_ipc_t` located in uncached Low Work RAM.

### C Strict-Aliasing Rules
- **Rule:** When burst-copying job structures (`hw_flat_job_t`) via 32-bit `mov.l` loops, never cast directly between struct pointers and `uint32_t *`.
- **Fix:** Use `__attribute__((__may_alias__))`:
  ```c
  #ifndef _MSC_VER
  typedef uint32_t aliased_uint32_t __attribute__((__may_alias__));
  #else
  typedef uint32_t aliased_uint32_t;
  #endif
  ```

### Bus Contention Mitigation
- Spin-wait loops checking `ipc->job_tail` across the Saturn system bus arbiter must use at least 64 NOPs to avoid bus starvation of the Master CPU.

---

## 4. High Work RAM Memory Constraints

- **Strict Limit:** `__bend < 0x060C0000` in High Work RAM (`game.map`).
- Exceeding this boundary corrupts SGL dynamically allocated buffers and causes crashes or boot hangs.
- Always check `game.map` after building the Saturn target:
  ```powershell
  Select-String -Path Projects\DOOM-Saturn\game.map -Pattern "__bend"
  ```

---

## 5. Development & Verification Runbooks

### A. Windows Testbed Build & Flight Verification
- **Build Win32 Companion:**
  ```powershell
  cmd /c "Projects\DOOM-Windows\build.bat"
  ```
- **Automated 624-Frame Flight Tour:**
  ```powershell
  python Projects\DOOM-Windows\analyze_journey.py --map MAP_E1M1 --render saturn
  ```
  - Target Render FPS: $\ge 60.0\text{ FPS}$
  - Target Visual Diff Error: $\le 4.85\%$ full-level average

### B. Sega Saturn CD Disc Target
- **Compile CD ISO:**
  ```powershell
  cmd /c "Projects\DOOM-Saturn\compile.bat"
  ```
  Generates `Projects/DOOM-Saturn/game.iso` (114 MB) and `game.cue`.

### C. Live Saturn Hardware Emulation & Telemetry
- **Run in Kronos:**
  ```powershell
  cmd /c "Projects\DOOM-Saturn\run_with_kronos.bat"
  ```
- **Headless Saturn Oracle (`sdbg`):**
  ```powershell
  python Projects\DOOM-Windows\saturn_oracle.py snap --state Projects\DOOM-Windows\doom_e1m1_spawn.state --frames 60 --out snap.png
  ```
- **On-Screen HUD Diagnostics Telemetry Format:**
  `FPS: xx.x R{ms_r} V{ms_v} T{ms_t} C{ms_c} B{ms_b} G{ms_g} S{ms_s} J{ms_j} W{walls} F{flats} Q{quads} D{dropped}`
  `N{subs} O{occl_mode} K{culls} I{segs_iter} U{segs_vis} X{refused} H{misses} P{strips} L{lod0}/{lod1} -Q{lod_saved}`
