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
    'up': 0x26, 'down': 0x28, 'left': 0x25, 'right': 0x27,
    'return': 0x0D, 'f1': 0x70, 'w': 0x57, 's': 0x53, 'a': 0x41, 'd': 0x44, 'z': 0x5A, 'space': 0x20, 'c': 0x43
}
KEYEVENTF_KEYUP = 0x0002

def find_kronos():
    found = []
    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def proc(h, l):
        buf = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, buf, 256)
        if buf.value.startswith('Kronos') and u.IsWindowVisible(h):
            found.append(h)
        return True
    u.EnumWindows(proc, 0)
    return found[0] if found else None

def send_key(key, down=True):
    h = find_kronos()
    if not h: return False
    vk = KEYCODES.get(key.lower(), 0)
    if not vk: return False
    scan = u.MapVirtualKeyW(vk, 0)
    flags = 0 if down else KEYEVENTF_KEYUP
    u.keybd_event(vk, scan, flags, 0)
    return True

def tap_key(key, ms=200):
    send_key(key, True)
    time.sleep(ms / 1000.0)
    send_key(key, False)
    time.sleep(0.1)

def capture_kronos(out_path):
    h = find_kronos()
    if not h: return False
    u.ShowWindow(h, 9)
    u.SetForegroundWindow(h)
    time.sleep(0.05)
    cr = wintypes.RECT()
    u.GetClientRect(h, ctypes.byref(cr))
    w, h_px = cr.right, cr.bottom
    if w <= 0 or h_px <= 0: return False
    pt = wintypes.POINT(0, 0)
    u.ClientToScreen(h, ctypes.byref(pt))
    screen_x, screen_y = pt.x, pt.y
    hdc = u.GetDC(0)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h_px)
    gdi32.SelectObject(mem, bmp)
    time.sleep(0.1)
    gdi32.BitBlt(mem, 0, 0, w, h_px, hdc, screen_x, screen_y, 0x00CC0020)
    
    class BMIH(ctypes.Structure):
        _fields_ = [('biSize', ctypes.c_uint32), ('biWidth', ctypes.c_int32),
                    ('biHeight', ctypes.c_int32), ('biPlanes', ctypes.c_uint16),
                    ('biBitCount', ctypes.c_uint16), ('biCompression', ctypes.c_uint32),
                    ('biSizeImage', ctypes.c_uint32), ('biXPelsPerMeter', ctypes.c_int32),
                    ('biYPelsPerMeter', ctypes.c_int32), ('biClrUsed', ctypes.c_uint32),
                    ('biClrImportant', ctypes.c_uint32)]

    bmi = BMIH()
    bmi.biSize = ctypes.sizeof(BMIH)
    bmi.biWidth = w
    bmi.biHeight = -h_px # top-down
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0
    buf = ctypes.create_string_buffer(w * h_px * 4)
    gdi32.GetDIBits(mem, bmp, 0, h_px, buf, ctypes.byref(bmi), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    u.ReleaseDC(0, hdc)
    im = Image.frombuffer('RGBA', (w, h_px), buf, 'raw', 'BGRA', 0, 1)
    im.convert('RGB').save(out_path)
    print(f'[+] Captured Kronos window ({w}x{h_px}) to {out_path}')
    trigger_kronos_screenshot()
    return True

def trigger_kronos_screenshot():
    h = find_kronos()
    if not h: return
    u.keybd_event(0x11, u.MapVirtualKeyW(0x11, 0), 0, 0)
    time.sleep(0.05)
    u.keybd_event(0x50, u.MapVirtualKeyW(0x50, 0), 0, 0)
    time.sleep(0.1)
    u.keybd_event(0x50, u.MapVirtualKeyW(0x50, 0), KEYEVENTF_KEYUP, 0)
    time.sleep(0.05)
    u.keybd_event(0x11, u.MapVirtualKeyW(0x11, 0), KEYEVENTF_KEYUP, 0)
    time.sleep(0.2)

if __name__ == '__main__':
    art_dir = r'C:\Users\charl\.gemini\antigravity\brain\4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4'
    kronos_exe = r'C:\Users\charl\Desktop\joengine-master\Emulators\kronos\kronos.exe'
    iso_path = r'C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\game.iso'
    
    # Kill any existing Kronos instance first
    subprocess.run(['taskkill', '/F', '/IM', 'kronos.exe'], capture_output=True)
    time.sleep(1)

    proc = subprocess.Popen([kronos_exe, '-i', iso_path], cwd=r'C:\Users\charl\Desktop\joengine-master\Emulators\kronos')
    try:
        time.sleep(4)
        # Press F1 to unpause / start Kronos execution
        tap_key('f1', 200)
        print('[+] Sent F1 to Kronos')
        
        # Wait for Sega boot / DOOM title screen (~20 seconds)
        print('[+] Waiting 20 seconds for DOOM title...')
        for _ in range(20):
            time.sleep(1)
        
        # Press Enter / Space / C to navigate menu
        print('[+] Navigating menu to E1M1...')
        for _ in range(5):
            tap_key('return', 200)
            time.sleep(0.3)
            tap_key('c', 200)
            time.sleep(0.3)
            tap_key('space', 200)
            time.sleep(0.8)
        time.sleep(4.0)
        
        # Capture spawn frame
        capture_kronos(os.path.join(art_dir, 'current_spawn.png'))
        
        # Turn right a bit
        tap_key('right', 300)
        time.sleep(0.5)
        capture_kronos(os.path.join(art_dir, 'current_turn_right.png'))
        
        # Turn back and walk forward
        tap_key('left', 300)
        time.sleep(0.5)
        tap_key('up', 800)
        time.sleep(0.5)
        capture_kronos(os.path.join(art_dir, 'current_forward.png'))
        
        # Navigate to computer room (west door from spawn)
        tap_key('left', 450)
        time.sleep(0.3)
        tap_key('up', 1800)
        time.sleep(0.3)
        tap_key('space', 200)
        tap_key('z', 200)
        time.sleep(1.2)
        tap_key('up', 2200)
        time.sleep(0.5)
        capture_kronos(os.path.join(art_dir, 'computer_room_1.png'))
        tap_key('right', 400)
        time.sleep(0.5)
        capture_kronos(os.path.join(art_dir, 'computer_room_fixture.png'))
        
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except:
            proc.kill()
