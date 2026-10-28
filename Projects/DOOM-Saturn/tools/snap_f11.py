import subprocess
import time
import os
import glob
import ctypes
from ctypes import wintypes
from PIL import Image

u = ctypes.windll.user32
KEYEVENTF_KEYUP = 0x0002

def find_kronos():
    found = []
    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def proc(h, l):
        buf = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, buf, 256)
        if buf.value.startswith("Kronos") and u.IsWindowVisible(h):
            found.append(h)
        return True
    u.EnumWindows(proc, 0)
    return found[0] if found else None

def tap_key(vk, ms=150):
    h = find_kronos()
    if not h: return False
    u.ShowWindow(h, 9)
    u.SetForegroundWindow(h)
    time.sleep(0.05)
    scan = u.MapVirtualKeyW(vk, 0)
    u.keybd_event(vk, scan, 0, 0)
    time.sleep(ms / 1000.0)
    u.keybd_event(vk, scan, KEYEVENTF_KEYUP, 0)
    return True

def main():
    kronos_exe = r"C:\Users\charl\Desktop\joengine-master\Emulators\kronos\kronos.exe"
    iso_path = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\game.iso"
    shot_dir = r"C:\Users\charl\AppData\Local\kronos\screenshots"
    art_dir = r"C:\Users\charl\.gemini\antigravity\brain\4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4"

    # Kill existing
    subprocess.run(["taskkill", "/F", "/IM", "kronos.exe"], capture_output=True)
    time.sleep(1)

    existing = set(glob.glob(os.path.join(shot_dir, "*.bmp")))
    print(f"[*] Found {len(existing)} existing screenshots in {shot_dir}")

    print(f"[*] Starting Kronos: {kronos_exe} -i {iso_path}")
    proc = subprocess.Popen([kronos_exe, "-a", "-i", iso_path], cwd=r"C:\Users\charl\Desktop\joengine-master\Emulators\kronos")

    try:
        # Give Kronos time to open window and boot
        for s in range(1, 30):
            time.sleep(1)
            print(f"[*] Waiting for boot: {s}s")
            if s == 3:
                tap_key(0x70) # F1 run
            if s in (16, 18, 20, 22, 24):
                tap_key(0x20) # Space
                time.sleep(0.1)
                tap_key(0x43) # C (A button)
                time.sleep(0.1)
                tap_key(0x0D) # Return

        print("[*] Now in game. Taking screenshot via F11...")
        for attempt in range(5):
            tap_key(0x7A) # F11 (Screenshot)
            time.sleep(1.0)
            new_files = set(glob.glob(os.path.join(shot_dir, "*.bmp"))) - existing
            if new_files:
                latest = max(new_files, key=os.path.getmtime)
                print(f"[+] Got new Kronos screenshot: {latest} ({os.path.getsize(latest)} bytes)")
                im = Image.open(latest)
                out_png = os.path.join(art_dir, "kronos_e1m1_fixed_verified.png")
                im.save(out_png)
                out_local = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\kronos_verified.png"
                im.save(out_local)
                print(f"[+] Successfully saved verified PNG: {out_png}")
                return True
        print("[!] No new BMP screenshot found")
        return False
    finally:
        print("[*] Terminating Kronos...")
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        print("[*] Kronos closed.")

if __name__ == "__main__":
    main()
