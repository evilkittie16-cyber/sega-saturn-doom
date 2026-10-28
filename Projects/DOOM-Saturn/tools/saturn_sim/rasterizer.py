import cv2
import numpy as np

class Rasterizer:
    def __init__(self, width=320, height=224):
        self.width = width
        self.height = height
        self.fb = np.zeros((height, width, 3), dtype=np.uint8)
        self.mode = "perspective" # "perspective", "affine", or "wireframe"
        self.show_wireframe = False
        
    def clear(self, color=(30, 30, 30)):
        self.fb[:] = color
        
    def draw_quad(self, pts_screen, texture, tex_coords=None, wire_color=None, light_scale=1.0):
        """
        pts_screen: 4x2 array of screen coordinates [(x0,y0), (x1,y1), (x2,y2), (x3,y3)]
        texture: (H, W, 3) uint8 numpy array
        tex_coords: 4x2 array of UV texel coordinates [(u0,v0), (u1,v1), (u2,v2), (u3,v3)]
                    Defaults to [[0, 0], [W, 0], [W, H], [0, H]]
        """
        th, tw = texture.shape[:2]
        if tex_coords is None:
            src_pts = np.float32([[0, 0], [tw, 0], [tw, th], [0, th]])
        else:
            src_pts = np.float32(tex_coords)
            
        dst_pts = np.float32(pts_screen)
        
        # Convexity check & area check
        min_x = max(0, int(np.min(dst_pts[:, 0])))
        max_x = min(self.width, int(np.ceil(np.max(dst_pts[:, 0]))))
        min_y = max(0, int(np.min(dst_pts[:, 1])))
        max_y = min(self.height, int(np.ceil(np.max(dst_pts[:, 1]))))
        
        if max_x <= min_x or max_y <= min_y:
            return
            
        w_box = max_x - min_x
        h_box = max_y - min_y
        local_dst = dst_pts - np.float32([min_x, min_y])
        
        # Check area
        area = cv2.contourArea(local_dst)
        if area < 1.0:
            return
            
        # Shaded texture
        if light_scale < 0.99 or light_scale > 1.01:
            tex_to_draw = np.clip(texture.astype(np.float32) * light_scale, 0, 255).astype(np.uint8)
        else:
            tex_to_draw = texture

        mask = np.zeros((h_box, w_box), dtype=np.uint8)
        cv2.fillConvexPoly(mask, local_dst.astype(np.int32), 255)
        
        if self.mode == "perspective":
            try:
                M = cv2.getPerspectiveTransform(src_pts, local_dst)
                warped = cv2.warpPerspective(tex_to_draw, M, (w_box, h_box), borderMode=cv2.BORDER_WRAP)
                fb_sub = self.fb[min_y:max_y, min_x:max_x]
                fb_sub[mask > 0] = warped[mask > 0]
            except Exception:
                pass
        else: # "affine" (VDP1 distorted sprite emulation via 2 affine triangles)
            try:
                # Triangle 1: 0, 1, 2
                M1 = cv2.getAffineTransform(src_pts[[0, 1, 2]], local_dst[[0, 1, 2]])
                w1 = cv2.warpAffine(tex_to_draw, M1, (w_box, h_box), borderMode=cv2.BORDER_WRAP)
                mask1 = np.zeros((h_box, w_box), dtype=np.uint8)
                cv2.fillConvexPoly(mask1, local_dst[[0, 1, 2]].astype(np.int32), 255)
                
                # Triangle 2: 0, 2, 3
                M2 = cv2.getAffineTransform(src_pts[[0, 2, 3]], local_dst[[0, 2, 3]])
                w2 = cv2.warpAffine(tex_to_draw, M2, (w_box, h_box), borderMode=cv2.BORDER_WRAP)
                mask2 = np.zeros((h_box, w_box), dtype=np.uint8)
                cv2.fillConvexPoly(mask2, local_dst[[0, 2, 3]].astype(np.int32), 255)
                
                fb_sub = self.fb[min_y:max_y, min_x:max_x]
                fb_sub[mask1 > 0] = w1[mask1 > 0]
                fb_sub[mask2 > 0] = w2[mask2 > 0]
            except Exception:
                pass

        if self.show_wireframe:
            color = wire_color if wire_color is not None else (0, 255, 255)
            pts_int = dst_pts.astype(np.int32).reshape((-1, 1, 2))
            cv2.polylines(self.fb, [pts_int], isClosed=True, color=color, thickness=1)

    def draw_solid_span_overlay(self, solid_mask):
        """Draw a visual bar at the top showing which horizontal spans are solid-occluded."""
        for x in range(min(self.width, len(solid_mask))):
            color = (0, 0, 255) if solid_mask[x] else (0, 255, 0)
            self.fb[0:3, x] = color
