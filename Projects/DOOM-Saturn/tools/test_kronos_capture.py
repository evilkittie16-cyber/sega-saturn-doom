import subprocess
import time
import os
import sys
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

def tap_key(vk, ms=200):
    h = find_kronos()
    if not h:
        return False
    u.ShowWindow(h, 9)
    u.SetForegroundWindow(h)
    time.sleep(0.1)
    scan = u.MapVirtualKeyW(vk, 0)
    u.keybd_event(vk, scan, 0, 0)
    time.sleep(ms / 1000.0)
    u.keybd_event(vk, scan, KEYEVENTF_KEYUP, 0)
    return True

def tap_ctrl_p():
    h = find_kronos()
    if not h:
        return False
    u.ShowWindow(h, 9)
    u.SetForegroundWindow(h)
    time.sleep(0.1)
    
    # VK_CONTROL = 0x11, 'P' = 0x50
    scan_ctrl = u.MapVirtualKeyW(0x11, 0)
    scan_p = u.MapVirtualKeyW(0x50, 0)
    
    u.keybd_event(0x11, scan_ctrl, 0, 0) # Ctrl down
    time.sleep(0.05)
    u.keybd_event(0x50, scan_p, 0, 0)    # P down
    time.sleep(0.1)
    u.keybd_event(0x50, scan_p, KEYEVENTF_KEYUP, 0) # P up
    time.sleep(0.05)
    u.keybd_event(0x11, scan_ctrl, KEYEVENTF_KEYUP, 0) # Ctrl up
    print("[+] Triggered Ctrl+P screenshot in Kronos")
    return True

def main():
    kronos_exe = r"C:\Users\charl\Desktop\joengine-master\Emulators\kronos\kronos.exe"
    iso_path = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\game.iso"
    shot_dir = r"C:\Users\charl\AppData\Local\kronos\screenshots"
    art_dir = r"C:\Users\charl\.gemini\antigravity\brain\4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4"
    
    existing = set(glob.glob(os.path.join(shot_dir, "*.bmp")))
    print(f"[*] Found {len(existing)} existing screenshots in {shot_dir}")
    
    print(f"[*] Starting Kronos: {kronos_exe} -i {iso_path}")
    proc = subprocess.Popen([kronos_exe, "-i", iso_path], cwd=r"C:\Users\charl\Desktop\joengine-master\Emulators\kronos")
    
    try:
        # Emulate for 40 seconds
        for s in range(1, 41):
            time.sleep(1)
            print(f"[*] Time: {s}s")
            if s in (16, 20, 24, 28):
                # Tap space (Start) and 'C' (A button)
                tap_key(0x20) # Space
                time.sleep(0.1)
                tap_key(0x43) # 'C'
            if s in (22, 28, 34, 39):
                tap_ctrl_p()
                time.sleep(0.5)
                new_files = set(glob.glob(os.path.join(shot_dir, "*.bmp"))) - existing
                if new_files:
                    latest = max(new_files, key=os.path.getmtime)
                    print(f"[+] Got new screenshot: {latest} ({os.path.getsize(latest)} bytes)")
                    im = Image.open(latest)
                    out_png = os.path.join(art_dir, f"kronos_e1m1_{s}s.png")
                    im.save(out_png)
                    print(f"[+] Saved PNG: {out_png}")
                    existing.update(new_files)
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
