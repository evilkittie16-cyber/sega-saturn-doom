# Phase 0 Baseline & Verification Audit

Grounded against the Vanilla DOOM Software Renderer Ground Truth.

## Canonical Pose Benchmark Suite

| Pose ID | Map | Coordinates $(X, Y, Z, \text{Angle})$ | Acceptable (Green+Yellow) | Perfect Match | Fatal Delta | Avg RGB Delta | Verdict | Notes |
|---|---|---|---|---|---|---|---|---|
| **Pose A** | E1M1 | `(1056.0, -3616.0, 41.0, 90.0°)` | **99.26%** | 86.28% | 0.28% | 2.42 | **PASS** | Spawn room; watertight geometry |
| **Pose B** | E1M1 | `(970.0, -3600.0, 41.0, 75.0°)` | **98.95%** | 84.86% | 0.46% | 2.98 | **PASS** | Acute grazing wall (75°); 1D affine LOD |
| **Pose C** | E1M1 | `(0.0, -3200.0, 41.0, 90.0°)` | **99.92%** | 77.01% | 0.07% | 2.66 | **PASS** | Courtyard / window; high quad density |
| **Pose D** | E1M3 | `(-1472.0, -3344.0, 105.0, 90.0°)` | **94.26%** | 65.54% | **2.35%** | 9.61 | **FAIL** | Lift room; isolates E20/E21 pegging & height pointer gap |
| **Pose E** | E2M8 | `(-96.0, -736.0, 41.0, 90.0°)` | **99.98%** | 50.17% | 0.00% | 3.20 | **PASS** | Cyberdemon arena; open sector geometry |

## Analysis
* **Poses A, B, C, E:** Pass with exceptional visual fidelity (≥ 98.95% match against original software renderer).
* **Pose D (E1M3):** Accurately exposes the exact defect identified in **Errata E20/E21** (texture pegging and height pointers on moving sectors / lifts). This will be our primary test pose for validating **Phase 3** fixes.
