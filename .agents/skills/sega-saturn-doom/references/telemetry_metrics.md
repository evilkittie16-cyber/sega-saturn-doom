# Sega Saturn DOOM Telemetry & Profiler Reference

The engine outputs real-time cycle-accurate hardware telemetry onto the bottom two lines of the screen in yellow 3x5 font over the status bar (`saturn_hud.c`).

## Line 1: Timing & Quad Counts

```text
FPS: 60.1  R10 V1 T0 C2 B8 G9 S6 J1 W98 F190 Q288 D0
```

| Tag | Name | Unit | Target | Description |
| :--- | :--- | :--- | :--- | :--- |
| **`FPS`** | Frame Rate | fps | **`60.0`** | Average frames displayed per second across a 15-frame window. |
| **`R`** | Render Window | ms | $\le 10$ | Total Master CPU rendering time (BSP walk + wall math + flat submission + HUD). |
| **`V`** | VDP1 Sync | ms | $\le 1$ | VDP1 hardware rasterizer command pipeline wait and finish sync. |
| **`T`** | Game Tics | ms | $\le 1$ | DOOM gameplay ticker, monster AI, thinker logic, player inputs. |
| **`C`** | VDP2 Copy | ms | $\le 2$ | Framebuffer conversion and transfer across Saturn B-Bus to VDP2 VRAM. |
| **`B`** | BSP Traversal | ms | $\le 8$ | BSP tree node evaluation, PVS checks, and bounding-box culling. |
| **`G`** | Seg Geometry | ms | $\le 10$ | Subsector wall coordinate transformation, near clipping, and projection. |
| **`S`** | V-Blank Idle | ms | $\ge 2$ | Idle CPU sleep time waiting for the next V-Blank display flip. |
| **`J`** | Slave Join Wait| ms | $\le 1$ | Master CPU spin-wait time for the Slave flat worker queue to drain. |
| **`W`** | Wall Quads | count| -- | Number of VDP1 distorted quads emitted for walls this frame. |
| **`F`** | Flat Quads | count| -- | Number of VDP1 distorted quads emitted for floors and ceilings. |
| **`Q`** | Total Quads | count| $\le 600$ | Total VDP1 quads passed to the command list. |
| **`D`** | Quads Dropped | count| **`0`** | Quads dropped due to command buffer overflow (must always be 0). |

---

## Line 2: Pipeline & Cache Statistics

```text
N29 O1 K9 I223 U77 X0 H0 P8 L3/22 -Q23
```

| Tag | Name | Description |
| :--- | :--- | :--- |
| **`N`** | Visited Subsectors | Number of subsectors reached by the BSP traversal. |
| **`O`** | Occlusion Mode | 1 = 1D solid wall span occlusion buffer active; 2 = disabled. |
| **`K`** | Occlusion Culls | Number of subsectors or nodes completely culled by span occlusion. |
| **`I`** | Segs Iterated | Total raw segs evaluated by the subsector wall loop. |
| **`U`** | Segs Visible | Segs that survived 2D backface rejection, frustum, and span occlusion. |
| **`X`** | Texture Refusals | VDP1 texture allocations rejected due to VRAM fragmentation. |
| **`H`** | Texture Misses | Texture cache misses requiring decompression from CD WAD. |
| **`P`** | Wall LOD Strips | Number of perspective wall columns subdivided into multi-quad strips. |
| **`L`** | Flat LOD Counts | `LOD0 / LOD1` subsectors selected for floor/ceiling tessellation. |
| **`-Q`**| Saved Quads | Number of quads saved by switching distant subsectors to LOD1. |
