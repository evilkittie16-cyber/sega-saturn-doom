import argparse
import time
import sys, os
sys.path.insert(0, os.path.abspath("."))
import cv2
from PIL import Image
from tools.saturn_sim.map_data import MapData
from tools.saturn_sim.rasterizer import Rasterizer
from tools.saturn_sim.renderer import SaturnRenderer

def main():
    parser = argparse.ArgumentParser(description="Render Doom pose with Saturn VDP1 hardware simulator")
    parser.add_argument("--wad", default="cd/DOOM.WAD", help="Path to WAD file")
    parser.add_argument("--flt-dir", default="cd", help="Directory containing .FLT files")
    parser.add_argument("--map", default="E1M1", help="Map name (default: E1M1)")
    parser.add_argument("--x", type=float, default=310.0, help="Camera X")
    parser.add_argument("--y", type=float, default=-3250.0, help="Camera Y")
    parser.add_argument("--z", type=float, default=129.0, help="Camera Z")
    parser.add_argument("--ang", type=float, default=177.0, help="Camera Angle in degrees")
    parser.add_argument("--mode", default="perspective", choices=["perspective", "affine"], help="Rasterization mode")
    parser.add_argument("--wireframe", action="store_true", help="Draw quad wireframes")
    parser.add_argument("--no-occlusion", action="store_true", help="Disable 1D occlusion culling")
    parser.add_argument("--out", default="scratch/windows_sim_doorway.png", help="Output PNG path")
    args = parser.parse_args()

    print(f"Loading map {args.map} from {args.wad}...")
    t0 = time.time()
    map_data = MapData(wad_path=args.wad, map_name=args.map, flt_dir=args.flt_dir)
    print(f"Loaded map in {time.time() - t0:.2f}s")

    rasterizer = Rasterizer(320, 224)
    rasterizer.mode = args.mode
    rasterizer.show_wireframe = args.wireframe

    renderer = SaturnRenderer(map_data, rasterizer)
    renderer.enable_occlusion = not args.no_occlusion
    renderer.set_camera(args.x, args.y, args.z, args.ang)

    print(f"Rendering pose X={args.x}, Y={args.y}, Z={args.z}, Ang={args.ang} (mode={args.mode}, occlusion={renderer.enable_occlusion})...")
    t1 = time.time()
    renderer.render_frame()
    dt = time.time() - t1
    print(f"Rendered in {dt*1000:.1f}ms:")
    print(f"  Wall Quads: {renderer.stat_wall_quads}")
    print(f"  Flat Quads: {renderer.stat_flat_quads}")
    print(f"  Subsectors: {renderer.stat_visited_subsectors}")

    # Convert BGR (OpenCV) to RGB for PIL / save
    img = Image.fromarray(rasterizer.fb)
    # Scale up 3x for clear viewing
    img_large = img.resize((960, 672), Image.Resampling.NEAREST)
    img_large.save(args.out)
    print(f"Saved output to {args.out}")

if __name__ == "__main__":
    main()
