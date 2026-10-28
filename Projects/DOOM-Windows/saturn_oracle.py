#!/usr/bin/env python3
"""
Saturn DOOM Oracle & 5thPlanet Hardware Debugger Harness
Provides headless Sega Saturn emulation, SH-2 CPU/DSP debugging,
VDP1/VDP2 hardware state inspection, memory probes, and automated
screenshot comparison against reference PC/Win32 DOOM renders.
"""

import os
import sys
import time
import argparse
import subprocess
from PIL import Image, ImageChops

DEFAULT_SDBG = r"C:\Users\charl\Desktop\5thPlanet-main\5thPlanet-main\target\release\sdbg.exe"
DEFAULT_BIOS = r"C:\Users\charl\Desktop\5thPlanet-main\5thPlanet-main\bios\Sega Saturn BIOS (USA).bin"
DEFAULT_CUE  = r"c:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\game.cue"
DEFAULT_MAP  = r"c:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\game.map"
DEFAULT_CWD  = r"C:\Users\charl\Desktop\5thPlanet-main\5thPlanet-main"
DEFAULT_GOLDEN_STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "doom_e1m1_spawn.state")

# Pad button bitmasks (SMPC digital pad)
PAD_BUTTONS = {
    "B": 0x0100,
    "C": 0x0200,
    "A": 0x0400,
    "START": 0x0800,
    "UP": 0x1000,
    "DOWN": 0x2000,
    "LEFT": 0x4000,
    "RIGHT": 0x8000,
}

WEAPON_NAMES = [
    "Fist", "Pistol", "Shotgun", "Chaingun", "Missile", "Plasma", "BFG", "Chainsaw", "SuperShotgun"
]

class SaturnOracle:
    def __init__(self, sdbg_exe=DEFAULT_SDBG, bios_bin=DEFAULT_BIOS, cue_path=DEFAULT_CUE,
                 map_path=DEFAULT_MAP, cwd=DEFAULT_CWD, cart="ram4m"):
        self.sdbg_exe = sdbg_exe
        self.bios_bin = bios_bin
        self.cue_path = cue_path
        self.map_path = map_path
        self.cwd = cwd
        self.cart = cart
        self.proc = None
        self.symbols = {}
        self.load_symbols(self.map_path)

    def load_symbols(self, map_path):
        if not os.path.exists(map_path):
            return
        with open(map_path, "r", errors="ignore") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2 and parts[0].startswith("0x"):
                    try:
                        addr = int(parts[0], 16)
                        name = parts[1]
                        self.symbols[name] = addr
                    except ValueError:
                        pass

    def start(self):
        if self.proc is not None:
            return
        cmd = [self.sdbg_exe, self.bios_bin, self.cue_path, "--region=us"]
        if self.cart:
            cmd.append(f"--cart={self.cart}")
        if self.map_path and os.path.exists(self.map_path):
            cmd.append(f"--syms={self.map_path}")

        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            cwd=self.cwd,
            text=True,
            bufsize=1
        )
        self.read_until_prompt(timeout=20)

    def read_until_prompt(self, timeout=30):
        if not self.proc:
            return ""
        buf = []
        start = time.time()
        while time.time() - start < timeout:
            ch = self.proc.stdout.read(1)
            if not ch:
                break
            buf.append(ch)
            if len(buf) >= 6 and "".join(buf[-6:]) == "sdbg> ":
                return "".join(buf[:-6])
        return "".join(buf)

    def cmd(self, command, timeout=30):
        if not self.proc:
            self.start()
        self.proc.stdin.write(command.strip() + "\n")
        self.proc.stdin.flush()
        return self.read_until_prompt(timeout)

    def close(self):
        if self.proc:
            try:
                self.proc.stdin.write("q\n")
                self.proc.stdin.flush()
                self.proc.wait(timeout=3)
            except Exception:
                self.proc.kill()
            self.proc = None

    def load_state(self, state_path):
        state_path = os.path.abspath(state_path)
        out = self.cmd(f"load {state_path}")
        return "loaded" in out.lower(), out

    def save_state(self, state_path):
        state_path = os.path.abspath(state_path)
        out = self.cmd(f"save {state_path}")
        return "saved" in out.lower(), out

    def step_frames(self, n=1):
        return self.cmd(f"f {n}")

    def continue_frames(self, max_frames=600):
        return self.cmd(f"fc {max_frames}", timeout=max(60, max_frames // 30))

    def set_bp(self, sym_or_addr):
        return self.cmd(f"b {sym_or_addr}")

    def clear_bps(self):
        return self.cmd("bd *")

    def dump_regs(self):
        return self.cmd("r")

    def dump_mem(self, sym_or_addr, length=64):
        # Normalize address if hex or int
        if isinstance(sym_or_addr, int):
            addr_val = sym_or_addr
        elif isinstance(sym_or_addr, str) and sym_or_addr.startswith("0x"):
            addr_val = int(sym_or_addr, 16)
        elif sym_or_addr in self.symbols:
            addr_val = self.symbols[sym_or_addr]
        else:
            try:
                addr_val = int(sym_or_addr, 16)
            except ValueError:
                addr_val = 0

        # Mask uncached bit 29 for cart or HWRAM addresses
        if addr_val & 0x20000000:
            addr_val &= ~0x20000000

        target_str = f"0x{addr_val:08X}"
        return self.cmd(f"m {target_str} {length}")

    def read_bytes(self, sym_or_addr, length=64):
        if isinstance(sym_or_addr, int):
            req_addr = sym_or_addr
        elif isinstance(sym_or_addr, str) and sym_or_addr.startswith("0x"):
            req_addr = int(sym_or_addr, 16)
        elif sym_or_addr in self.symbols:
            req_addr = self.symbols[sym_or_addr]
        else:
            try:
                req_addr = int(sym_or_addr, 16)
            except ValueError:
                req_addr = 0

        if req_addr & 0x20000000:
            req_addr &= ~0x20000000

        raw = self.dump_mem(req_addr, length + 16)
        all_bytes = {}
        for line in raw.splitlines():
            line = line.strip()
            if ":" not in line:
                continue
            addr_part, hex_part = line.split(":", 1)
            try:
                line_addr = int(addr_part.strip(), 16)
            except ValueError:
                continue
            parts = hex_part.split()
            idx = 0
            for p in parts:
                if len(p) == 2:
                    try:
                        b = int(p, 16)
                        all_bytes[line_addr + idx] = b
                        idx += 1
                    except ValueError:
                        break
                else:
                    break

        res = []
        for i in range(length):
            res.append(all_bytes.get(req_addr + i, 0))
        return bytes(res)

    def poke(self, sym_or_addr, val, size=4):
        return self.cmd(f"poke {sym_or_addr} {hex(val)} {size}")

    def render_ppm(self, ppm_path):
        return self.cmd(f"render {ppm_path}")

    def render_png(self, png_path):
        ppm_path = png_path + ".tmp.ppm"
        self.render_ppm(ppm_path)
        if os.path.exists(ppm_path):
            img = Image.open(ppm_path)
            img.save(png_path)
            try:
                os.remove(ppm_path)
            except OSError:
                pass
            return True
        return False

    def pad_tap(self, button_name, hold_frames=2, release_frames=2):
        btn = button_name.upper()
        mask = PAD_BUTTONS.get(btn, 0)
        if not mask and btn.startswith("0X"):
            mask = int(btn, 16)
        if not mask:
            raise ValueError(f"Unknown button: {button_name}. Choices: {list(PAD_BUTTONS.keys())}")
        self.cmd(f"pad {hex(mask)}")
        self.step_frames(hold_frames)
        self.cmd("pad 0")
        self.step_frames(release_frames)

    def inspect_player(self):
        player_addr = self.symbols.get("players", 0x0608BC68)
        # Read player_t pointer from players table
        p_bytes = self.read_bytes(player_addr, 8)
        player_ptr = int.from_bytes(p_bytes[0:4], "big")
        if player_ptr & 0x20000000:
            player_ptr &= ~0x20000000

        res = {"player_ptr": hex(player_ptr)}
        if player_ptr == 0:
            return res

        # Read player_t structure (at least 160 bytes)
        pt_data = self.read_bytes(player_ptr, 160)
        if len(pt_data) >= 80:
            # mobj pointer is at offset 0
            mobj_ptr = int.from_bytes(pt_data[0:4], "big")
            if mobj_ptr & 0x20000000:
                mobj_ptr &= ~0x20000000
            res["mobj_ptr"] = hex(mobj_ptr)

            # Health at offset 0x30 (VINT = 2 bytes), armor at 0x32 (VINT = 2 bytes), readyweapon at 0x4A (VINT = 2 bytes)
            health = int.from_bytes(pt_data[0x30:0x32], "big", signed=True)
            armor = int.from_bytes(pt_data[0x32:0x34], "big", signed=True)
            weapon_id = int.from_bytes(pt_data[0x4A:0x4C], "big")
            weapon_name = WEAPON_NAMES[weapon_id] if weapon_id < len(WEAPON_NAMES) else f"Weapon {weapon_id}"
            res.update({
                "health": health,
                "armor": armor,
                "weapon": weapon_name,
                "weapon_id": weapon_id
            })

            # Read mobj_t if valid
            if mobj_ptr > 0:
                mo_data = self.read_bytes(mobj_ptr, 128)
                if len(mo_data) >= 72:
                    # x at 0x00, y at 0x04, z at 0x08, angle at 0x44
                    x = int.from_bytes(mo_data[0:4], "big", signed=True) / 65536.0
                    y = int.from_bytes(mo_data[4:8], "big", signed=True) / 65536.0
                    z = int.from_bytes(mo_data[8:12], "big", signed=True) / 65536.0
                    angle_u = int.from_bytes(mo_data[0x44:0x48], "big", signed=False)
                    angle_deg = (angle_u / 0x100000000) * 360.0
                    res.update({"x": x, "y": y, "z": z, "angle_deg": angle_deg, "angle_raw": hex(angle_u)})
        return res

    def inspect_vdp1_counts(self):
        addr = self.symbols.get("saturn_cmd_w_count", 0x06087CD8)
        data = self.read_bytes(addr, 16)
        counts = {}
        if len(data) >= 6:
            counts["w_count"] = int.from_bytes(data[0:2], "big")
            counts["s_count"] = int.from_bytes(data[2:4], "big")
            counts["m_count"] = int.from_bytes(data[4:6], "big")
            counts["total"] = counts["w_count"] + counts["s_count"] + counts["m_count"]
        return counts

    def decode_vdp1_quads(self, limit=16):
        # Read saturn_cmd_buf pointers:
        # saturn_cmd_buf[0] = Master, [1] = Slave, [2] = Wall
        # Located at 0x06087CE0
        buf_ptrs = self.read_bytes(0x06087CE0, 16)
        quads = []
        if len(buf_ptrs) >= 12:
            m_buf = int.from_bytes(buf_ptrs[0:4], "big") & 0x0FFFFFFF
            s_buf = int.from_bytes(buf_ptrs[4:8], "big") & 0x0FFFFFFF
            w_buf = int.from_bytes(buf_ptrs[8:12], "big") & 0x0FFFFFFF

            # Decode first few commands from Master buffer
            raw = self.read_bytes(m_buf, limit * 32)
            for i in range(min(limit, len(raw) // 32)):
                chunk = raw[i*32:(i+1)*32]
                cmdctrl = int.from_bytes(chunk[0:2], "big")
                cmdlink = int.from_bytes(chunk[2:4], "big")
                cmdpmod = int.from_bytes(chunk[4:6], "big")
                cmdcolr = int.from_bytes(chunk[6:8], "big")
                cmdsrc  = int.from_bytes(chunk[8:10], "big")
                cmdsize = int.from_bytes(chunk[10:12], "big")
                xa = int.from_bytes(chunk[12:14], "big", signed=True)
                ya = int.from_bytes(chunk[14:16], "big", signed=True)
                xb = int.from_bytes(chunk[16:18], "big", signed=True)
                yb = int.from_bytes(chunk[18:20], "big", signed=True)
                xc = int.from_bytes(chunk[20:22], "big", signed=True)
                yc = int.from_bytes(chunk[22:24], "big", signed=True)
                xd = int.from_bytes(chunk[24:26], "big", signed=True)
                yd = int.from_bytes(chunk[26:28], "big", signed=True)
                quads.append({
                    "idx": i,
                    "ctrl": hex(cmdctrl),
                    "link": hex(cmdlink),
                    "pmod": hex(cmdpmod),
                    "colr": hex(cmdcolr),
                    "src": hex(cmdsrc),
                    "size": hex(cmdsize),
                    "verts": [(xa, ya), (xb, yb), (xc, yc), (xd, yd)]
                })
        return quads

def compare_images(img1_path, img2_path, diff_out_path=None, view_only=False):
    im1 = Image.open(img1_path).convert("RGB")
    im2 = Image.open(img2_path).convert("RGB")

    if view_only:
        # DOOM 3D view is top 168 rows (status bar starts at y=168)
        box = (0, 0, 320, 168)
        im1 = im1.crop(box)
        im2 = im2.crop(box)
    else:
        # Full DOOM canvas is top 200 rows (status bar ends at y=200)
        common_h = min(200, im1.height, im2.height)
        box = (0, 0, 320, common_h)
        im1 = im1.crop(box)
        im2 = im2.crop(box)

    diff = ImageChops.difference(im1, im2)
    w, h = im1.size
    total_px = w * h
    mismatch_px = 0
    total_diff_val = 0

    raw1 = im1.tobytes()
    raw2 = im2.tobytes()
    pixels1 = [(raw1[i], raw1[i+1], raw1[i+2]) for i in range(0, len(raw1), 3)]
    pixels2 = [(raw2[i], raw2[i+1], raw2[i+2]) for i in range(0, len(raw2), 3)]

    for (r1, g1, b1), (r2, g2, b2) in zip(pixels1, pixels2):
        dr = abs(r1 - r2)
        dg = abs(g1 - g2)
        db = abs(b1 - b2)
        d = max(dr, dg, db)
        if d > 4: # Small tolerance for 1-bit RGB555 rounding
            mismatch_px += 1
        total_diff_val += (dr*dr + dg*dg + db*db) / 3.0

    mse = total_diff_val / total_px
    match_pct = 100.0 * (total_px - mismatch_px) / total_px

    if diff_out_path:
        comp = Image.new("RGB", (w * 3, h))
        comp.paste(im1, (0, 0))
        comp.paste(im2, (w, 0))

        # 5x boosted diff heatmap
        diff_boosted = diff.point(lambda p: min(255, p * 5))
        comp.paste(diff_boosted, (w * 2, 0))
        comp.save(diff_out_path)

    return {
        "total_pixels": total_px,
        "mismatched_pixels": mismatch_px,
        "match_percentage": match_pct,
        "mse": mse
    }

def main():
    parser = argparse.ArgumentParser(description="Saturn DOOM 5thPlanet Oracle & Hardware Debugger")
    sub = parser.add_subparsers(dest="cmd", help="Sub-commands")

    # snap
    p_snap = sub.add_parser("snap", help="Advance emulation and capture screenshot")
    p_snap.add_argument("--state", default=DEFAULT_GOLDEN_STATE, help="Save-state to load before capture")
    p_snap.add_argument("--frames", type=int, default=1, help="Number of frames to step before capture")
    p_snap.add_argument("--out", default="saturn_snap.png", help="Output PNG path")

    # player
    p_play = sub.add_parser("player", help="Inspect player coordinates and state")
    p_play.add_argument("--state", default=DEFAULT_GOLDEN_STATE)

    # regs
    p_regs = sub.add_parser("regs", help="Dump SH-2 Master and Slave CPU registers")
    p_regs.add_argument("--state", default=DEFAULT_GOLDEN_STATE)

    # vdp1
    p_vdp1 = sub.add_parser("vdp1", help="Dump VDP1 command counts and decoded command list")
    p_vdp1.add_argument("--state", default=DEFAULT_GOLDEN_STATE)
    p_vdp1.add_argument("--limit", type=int, default=8, help="Number of quads to decode")

    # inspect
    p_insp = sub.add_parser("inspect", help="Inspect memory at symbol or hex address")
    p_insp.add_argument("target", help="Symbol name (e.g. players) or hex address")
    p_insp.add_argument("length", nargs="?", type=int, default=64, help="Byte length")
    p_insp.add_argument("--state", default=DEFAULT_GOLDEN_STATE)

    # pad
    p_pad = sub.add_parser("pad", help="Inject gamepad button press and capture result")
    p_pad.add_argument("button", help="Button name: UP, DOWN, LEFT, RIGHT, A, B, C, START")
    p_pad.add_argument("--frames", type=int, default=5, help="Frames to hold")
    p_pad.add_argument("--state", default=DEFAULT_GOLDEN_STATE)
    p_pad.add_argument("--out", default="saturn_after_pad.png")

    # compare
    p_cmp = sub.add_parser("compare", help="Compare two frames with diff heatmap")
    p_cmp.add_argument("saturn_img", help="Saturn screenshot")
    p_cmp.add_argument("ref_img", help="Reference PC/Win32 screenshot")
    p_cmp.add_argument("--out", default="diff_comparison.png", help="Side-by-side diff output")
    p_cmp.add_argument("--view-only", action="store_true", help="Crop status bar and compare only 3D view")

    args = parser.parse_args()
    if not args.cmd:
        parser.print_help()
        return

    if args.cmd == "compare":
        res = compare_images(args.saturn_img, args.ref_img, args.out, getattr(args, "view_only", False))
        print("=== Frame Comparison Results ===")
        print(f"Total Pixels:      {res['total_pixels']}")
        print(f"Mismatched Pixels: {res['mismatched_pixels']}")
        print(f"Match Percentage:  {res['match_percentage']:.2f}%")
        print(f"Mean Squared Error:{res['mse']:.4f}")
        print(f"Diff image saved:  {args.out}")
        return

    oracle = SaturnOracle()
    try:
        oracle.start()
        if hasattr(args, "state") and args.state and os.path.exists(args.state):
            ok, out = oracle.load_state(args.state)
            if not ok:
                print(f"Failed to load state {args.state}: {out}")
                return

        if args.cmd == "snap":
            if args.frames > 0:
                oracle.step_frames(args.frames)
            out_path = os.path.abspath(args.out)
            if oracle.render_png(out_path):
                print(f"Captured Saturn frame to: {out_path}")
            else:
                print("Failed to capture frame")

        elif args.cmd == "player":
            p = oracle.inspect_player()
            print("=== DOOM Player State (5thPlanet Saturn) ===")
            print(f"Player Pointer: {p.get('player_ptr')}")
            print(f"Mobj Pointer:   {p.get('mobj_ptr')}")
            if "health" in p:
                print(f"Health:         {p['health']}%")
                print(f"Armor:          {p['armor']}%")
                print(f"Weapon:         {p['weapon']} (ID: {p['weapon_id']})")
            if "x" in p:
                print(f"Position (X,Y,Z): ({p['x']:.2f}, {p['y']:.2f}, {p['z']:.2f})")
                print(f"Angle:            {p['angle_deg']:.2f} deg ({p['angle_raw']})")

        elif args.cmd == "regs":
            regs = oracle.dump_regs()
            print(regs)

        elif args.cmd == "vdp1":
            counts = oracle.inspect_vdp1_counts()
            quads = oracle.decode_vdp1_quads(args.limit)
            print("=== VDP1 Command Buffer & Hardware Status ===")
            print(f"Master Quad Commands: {counts.get('m_count')}")
            print(f"Slave Quad Commands:  {counts.get('s_count')}")
            print(f"Wall Commands:        {counts.get('w_count')}")
            print(f"Total Active Quads:   {counts.get('total')}")
            print("\n--- Decoded Master VDP1 Quads ---")
            for q in quads:
                v = q["verts"]
                print(f"Quad #{q['idx']:02d}: ctrl={q['ctrl']} pmod={q['pmod']} src={q['src']} | A:({v[0][0]},{v[0][1]}) B:({v[1][0]},{v[1][1]}) C:({v[2][0]},{v[2][1]}) D:({v[3][0]},{v[3][1]})")

        elif args.cmd == "inspect":
            target = args.target
            if not target.startswith("0x") and target in oracle.symbols:
                target = hex(oracle.symbols[target])
            out = oracle.dump_mem(target, args.length)
            print(out)

        elif args.cmd == "pad":
            oracle.pad_tap(args.button, hold_frames=args.frames, release_frames=2)
            out_path = os.path.abspath(args.out)
            oracle.render_png(out_path)
            print(f"Injected button {args.button} for {args.frames} frames. Frame saved: {out_path}")

    finally:
        oracle.close()

if __name__ == "__main__":
    main()
