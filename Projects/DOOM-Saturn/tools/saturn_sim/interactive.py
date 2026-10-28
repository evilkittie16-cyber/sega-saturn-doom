import sys
import os
import math
import time
import pygame
import numpy as np

sys.path.insert(0, os.path.abspath("."))
from tools.saturn_sim.map_data import MapData
from tools.saturn_sim.rasterizer import Rasterizer
from tools.saturn_sim.renderer import SaturnRenderer

def run_interactive(map_name="E1M1", wad_path="cd/DOOM.WAD", flt_dir="cd"):
    pygame.init()
    pygame.display.set_caption(f"Saturn VDP1 Hardware Simulator - {map_name} ({os.path.basename(wad_path)})")
    
    SIM_W, SIM_H = 320, 224
    SCALE = 3
    WIN_W, WIN_H = SIM_W * SCALE, SIM_H * SCALE
    
    screen = pygame.display.set_mode((WIN_W, WIN_H))
    clock = pygame.time.Clock()
    
    print(f"Loading map {map_name} from {wad_path}...")
    map_data = MapData(wad_path=wad_path, map_name=map_name, flt_dir=flt_dir)
    rasterizer = Rasterizer(SIM_W, SIM_H)
    renderer = SaturnRenderer(map_data, rasterizer)
    
    # Initialize camera to player spawn
    cam_x, cam_y, cam_z, cam_ang = map_data.player_spawn
    if map_data.nodes:
        def locate(bspnum):
            if bspnum & 0x8000:
                return bspnum & ~0x8000
            node = map_data.nodes[bspnum]
            dx = cam_x - node['x']
            dy = cam_y - node['y']
            side = 0 if (dy * node['dx'] < node['dy'] * dx) else 1
            return locate(node['children'][side])
        try:
            ss = locate(len(map_data.nodes) - 1)
            sec = map_data.sectors[map_data.subsectors[ss]['sector']]
            cam_z = float(sec['floorheight'] + 41)
        except Exception:
            pass
    
    font = pygame.font.SysFont("Consolas", 14, bold=True)
    
    running = True
    mouse_locked = False
    
    MOVE_SPEED = 200.0 # units per second
    ROT_SPEED = 90.0   # degrees per second
    
    while running:
        dt = clock.tick(60) / 1000.0 # delta time in seconds
        
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE:
                    running = False
                elif event.key == pygame.K_w and (pygame.key.get_mods() & pygame.KMOD_CTRL):
                    running = False
                elif event.key == pygame.K_w and not mouse_locked and not (pygame.key.get_mods() & pygame.KMOD_SHIFT):
                    pass # will handle in held keys
                elif event.key == pygame.K_1:
                    cam_x, cam_y, cam_z, cam_ang = 0.0, -3300.0, 145.0, 18.0
                elif event.key == pygame.K_2:
                    cam_x, cam_y, cam_z, cam_ang = 1056.0, -3648.0, 80.0, 90.0
                elif event.key == pygame.K_3:
                    cam_x, cam_y, cam_z, cam_ang = 310.0, -3250.0, 129.0, 177.0
                elif event.key == pygame.K_F1:
                    rasterizer.show_wireframe = not rasterizer.show_wireframe
                elif event.key == pygame.K_o:
                    renderer.enable_occlusion = not renderer.enable_occlusion
                elif event.key == pygame.K_p:
                    rasterizer.mode = "affine" if rasterizer.mode == "perspective" else "perspective"
                elif event.key == pygame.K_f:
                    renderer.enable_flats = not renderer.enable_flats
                elif event.key == pygame.K_l:
                    renderer.enable_walls = not renderer.enable_walls
                elif event.key == pygame.K_c:
                    os.makedirs("scratch", exist_ok=True)
                    pygame.image.save(screen, "scratch/interactive_capture.png")
                    print("Saved scratch/interactive_capture.png")
                elif event.key == pygame.K_TAB:
                    mouse_locked = not mouse_locked
                    pygame.event.set_grab(mouse_locked)
                    pygame.mouse.set_visible(not mouse_locked)
            elif event.type == pygame.MOUSEMOTION and mouse_locked:
                dx, dy = event.rel
                cam_ang = (cam_ang - dx * 0.2) % 360.0

        # Handle movement keys
        keys = pygame.key.get_pressed()
        speed = MOVE_SPEED * (2.5 if keys[pygame.K_LSHIFT] else 1.0) * dt
        rot_speed = ROT_SPEED * dt
        
        rad = math.radians(cam_ang)
        cos_f = math.cos(rad)
        sin_f = math.sin(rad)
        
        if keys[pygame.K_w] or keys[pygame.K_UP]:
            cam_x += cos_f * speed
            cam_y += sin_f * speed
        if keys[pygame.K_s] or keys[pygame.K_DOWN]:
            cam_x -= cos_f * speed
            cam_y -= sin_f * speed
        if keys[pygame.K_a]:
            cam_x -= sin_f * speed
            cam_y += cos_f * speed
        if keys[pygame.K_d]:
            cam_x += sin_f * speed
            cam_y -= cos_f * speed
            
        if keys[pygame.K_LEFT]:
            cam_ang = (cam_ang + rot_speed) % 360.0
        if keys[pygame.K_RIGHT]:
            cam_ang = (cam_ang - rot_speed) % 360.0
            
        if keys[pygame.K_PAGEUP] or keys[pygame.K_SPACE]:
            cam_z += speed
        if keys[pygame.K_PAGEDOWN] or keys[pygame.K_LCTRL]:
            cam_z -= speed

        # Render frame
        t0 = time.time()
        renderer.set_camera(cam_x, cam_y, cam_z, cam_ang)
        renderer.render_frame()
        render_time = (time.time() - t0) * 1000.0
        
        # Display image
        # rasterizer.fb is (H, W, 3) BGR numpy array
        # Pygame surface requires (W, H, 3) RGB
        rgb_fb = rasterizer.fb[:, :, ::-1] # BGR to RGB
        surf_sim = pygame.surfarray.make_surface(np.transpose(rgb_fb, (1, 0, 2)))
        surf_scaled = pygame.transform.scale(surf_sim, (WIN_W, WIN_H))
        screen.blit(surf_scaled, (0, 0))
        
        # Draw HUD
        hud_lines = [
            f"FPS: {clock.get_fps():.1f} | Render: {render_time:.1f}ms",
            f"Pose: X={cam_x:6.1f} Y={cam_y:6.1f} Z={cam_z:5.1f} Ang={cam_ang:5.1f}",
            f"Quads: Walls={renderer.stat_wall_quads:3d} Flats={renderer.stat_flat_quads:3d} (Total={renderer.stat_wall_quads+renderer.stat_flat_quads})",
            f"Mode: {rasterizer.mode.upper()} [P] | Wireframe: {'ON' if rasterizer.show_wireframe else 'OFF'} [F1]",
            f"Occlusion: {'ON' if renderer.enable_occlusion else 'OFF'} [O] | Presets: [1, 2, 3] | Screenshot: [C]"
        ]
        
        y_hud = 8
        for line in hud_lines:
            txt_bg = font.render(line, True, (0, 0, 0))
            txt_fg = font.render(line, True, (255, 255, 0))
            screen.blit(txt_bg, (11, y_hud + 1))
            screen.blit(txt_fg, (10, y_hud))
            y_hud += 18
            
        pygame.display.flip()
        
    pygame.quit()

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Interactive Saturn VDP1 Hardware Simulator")
    parser.add_argument("map", nargs="?", default="E1M1", help="Map name (default: E1M1)")
    parser.add_argument("--wad", default="cd/DOOM.WAD", help="Path to WAD file")
    parser.add_argument("--flt-dir", default="cd", help="Directory for .FLT files")
    args = parser.parse_args()
    
    run_interactive(args.map, wad_path=args.wad, flt_dir=args.flt_dir)
