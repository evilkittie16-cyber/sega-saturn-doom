import subprocess
import time
import os
import sys
import ctypes
from ctypes import wintypes
from PIL import Image

u = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

KEYCODES = {
    "z": 0x5A, "x": 0x58, "c": 0x43, "a": 0x41, "s": 0x53, "d": 0x44,
    "q": 0x51, "w": 0x57, "e": 0x45, "up": 0x26, "down": 0x28,
    "left": 0x25, "right": 0x27, "space": 0x20, "return": 0x0D,
    "f1": 0x70, "esc": 0x1B, "f8": 0x77, "f12": 0x7B
}
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

def tap_key(key, ms=250):
    h = find_kronos()
    if not h:
        print("[!] Kronos not found for key tap")
        return False
    u.ShowWindow(h, 9)
    u.SetForegroundWindow(h)
    time.sleep(0.2)
    
    # Click client center
    cr = wintypes.RECT()
    u.GetClientRect(h, ctypes.byref(cr))
    pt = wintypes.POINT()
    pt.x = cr.right // 2
    pt.y = cr.bottom // 2
    u.ClientToScreen(h, ctypes.byref(pt))
    u.SetCursorPos(pt.x, pt.y)
    u.mouse_event(0x0002, 0, 0, 0, 0) # left down
    u.mouse_event(0x0004, 0, 0, 0, 0) # left up
    time.sleep(0.2)
    
    vk = KEYCODES.get(key.lower())
    if not vk:
        return False
    scan = u.MapVirtualKeyW(vk, 0)
    u.keybd_event(vk, scan, 0, 0)
    time.sleep(ms / 1000.0)
    u.keybd_event(vk, scan, KEYEVENTF_KEYUP, 0)
    print(f"[+] Tapped {key} on Kronos")
    return True

def capture_kronos(out_path):
    h = find_kronos()
    if not h:
        print("[!] Kronos window not found")
        return False
        
    u.ShowWindow(h, 9)
    u.SetForegroundWindow(h)
    time.sleep(0.05)

    cr = wintypes.RECT()
    u.GetClientRect(h, ctypes.byref(cr))
    w, h_px = cr.right, cr.bottom
    if w <= 0 or h_px <= 0:
        print("[!] Invalid window dimensions:", w, h_px)
        return False
        
    pt = wintypes.POINT(0, 0)
    u.ClientToScreen(h, ctypes.byref(pt))
    screen_x, screen_y = pt.x, pt.y

    hdc = u.GetDC(0)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h_px)
    gdi32.SelectObject(mem, bmp)
    gdi32.BitBlt(mem, 0, 0, w, h_px, hdc, screen_x, screen_y, 0x00CC0020)
    
    class BMIH(ctypes.Structure):
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                    ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                    ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                    ("biSizeImage", ctypes.c_uint32), ("biXPels", ctypes.c_int32),
                    ("biYPels", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                    ("biClrImportant", ctypes.c_uint32)]

    bi = BMIH()
    bi.biSize = ctypes.sizeof(BMIH)
    bi.biWidth = w
    bi.biHeight = -h_px
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0

    buf = ctypes.create_string_buffer(w * h_px * 4)
    gdi32.GetDIBits(mem, bmp, 0, h_px, buf, ctypes.byref(bi), 0)

    raw = bytearray(buf.raw)
    for i in range(0, len(raw), 4):
        raw[i], raw[i + 2] = raw[i + 2], raw[i]

    img = Image.frombytes("RGBA", (w, h_px), bytes(raw))
    img.convert("RGB").save(out_path)
    print(f"[+] Saved screenshot to: {out_path} ({w}x{h_px})")
    
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    u.ReleaseDC(0, hdc)
    return True

def main():
    kronos_exe = r"C:\Users\charl\Desktop\joengine-master\Emulators\kronos\kronos.exe"
    iso_path = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\game.iso"
    
    print(f"[*] Starting Kronos with ISO: {iso_path}")
    proc = subprocess.Popen([kronos_exe, "-a", "-i", iso_path], cwd=r"C:\Users\charl\Desktop\joengine-master\Emulators\kronos")
    
    try:
        # Give Kronos 2 seconds to initialize window
        time.sleep(2)
        print("[*] Sending F1 to unpause / run emulation...")
        tap_key("f1")
        time.sleep(1)
        tap_key("f1") # double tap
        
        # Emulate for 20 seconds to give ample time for boot and preload
        for sec in range(1, 21):
            time.sleep(1)
            print(f"[*] Emulating... {sec}/20 s")
            if sec == 6:
                # Tap start / enter in case of start screen
                tap_key("return")
            if sec in (5, 10, 15, 19):
                capture_kronos(f"kronos_boot_{sec}s.png")
                
        out_final = r"C:\Users\charl\.gemini\antigravity\brain\4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4\e1m1_kronos_bench.png"
        capture_kronos(out_final)
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
