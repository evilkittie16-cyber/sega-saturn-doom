import subprocess
import time
import os
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
    h = find_kronos()
    if h:
        u.ShowWindow(h, 9)
        u.SetForegroundWindow(h)
    send_key(key, True)
    time.sleep(ms / 1000.0)
    send_key(key, False)
    time.sleep(0.1)

def capture_kronos(out_path):
    h = find_kronos()
    if not h: return False
    u.ShowWindow(h, 9)
    u.SetForegroundWindow(h)
    time.sleep(0.1)
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
    bmi.biHeight = -h_px
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
    return True

if __name__ == '__main__':
    art_dir = r'C:\Users\charl\.gemini\antigravity\brain\4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4'
    kronos_exe = r'C:\Users\charl\Desktop\joengine-master\Emulators\kronos\kronos.exe'
    iso_path = r'C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\game.iso'
    out_snap = os.path.join(art_dir, 'e1m8_star_bench.png')

    subprocess.run(['taskkill', '/F', '/IM', 'kronos.exe'], capture_output=True)
    time.sleep(1)

    print(f'[*] Launching Kronos: {kronos_exe}')
    proc = subprocess.Popen([kronos_exe, '-i', iso_path], cwd=r'C:\Users\charl\Desktop\joengine-master\Emulators\kronos')
    try:
        time.sleep(4)
        print('[+] Sending F1 to unpause...')
        tap_key('f1', 200)
        time.sleep(0.5)
        tap_key('f1', 200)
        
        print('[+] Waiting 20 seconds for Sega boot...')
        for s in range(20):
            time.sleep(1)
            
        print('[+] Navigating start to level...')
        for _ in range(6):
            tap_key('return', 200)
            time.sleep(0.3)
            tap_key('c', 200)
            time.sleep(0.3)
            tap_key('space', 200)
            time.sleep(0.6)
            
        time.sleep(4.0)
        capture_kronos(out_snap)
        time.sleep(2.0)
        capture_kronos(out_snap)
    finally:
        print('[*] Terminating Kronos...')
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except:
            proc.kill()
        print('[*] Done.')
