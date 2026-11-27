/* saturn_hw_tiny.c - skip areas that are tiny on screen (2026-11-26).
 *
 * User: "E4M2/5/6/8/9 and other heavy scenes: reduce the load without
 * massively affecting image quality", "keeping the load within 30 FPS no
 * matter what", "we need to get creative". The master pays ~0.45 ms per
 * visited subsector (aperture test, walls, span occlusion, commit, floor
 * job), and in the dense E4 views most visits draw almost nothing: the
 * harness pixel census (E4M9 at 758,1670) had 131 of 207 visits owning
 * 0.4% of the picture together, 41 drawing nothing at all.
 *
 * After each frame every visit's on-screen size is estimated from what it
 * emitted (cmd_m walls/sprites and cmd_s floors of its range): the sum of
 * its quads' bounding boxes, clipped to the user clip the walls carry for
 * span occlusion. Overdraw is ignored, so the estimate only errs towards
 * "big" (drawn). A visit under hw_tiny_t px is skipped whole for the next
 * HW_TINY_HOLD times the walk reaches it, then drawn once to be measured
 * again. The governor (Saturn_HW_AdaptiveDetail) raises hw_tiny_t while
 * frames are under 30 FPS and lowers it when there is headroom; light
 * scenes stay at 0 (off).
 *
 * Included from saturn_hw_render.c (unity build), master only.
 */
#ifndef HW_TINY
/* OFF (2026-11-26 hardware play test: areas popped in late while turning,
 * distant monsters/items in skipped areas were not drawn; user: "Doom is
 * iconic because it renders everything you can see") */
#define HW_TINY 0
#endif

#if HW_TINY
#ifndef HW_TINY_T0
#define HW_TINY_T0 (-1)        /* -1: governor; >= 0: fixed threshold (px) */
#endif
#ifndef HW_TINY_HOLD
#define HW_TINY_HOLD 8         /* skips before a marked area is re-measured */
#endif
#ifndef HW_TINY_TMAX
#define HW_TINY_TMAX 400
#endif
#define HW_TINY_MAXSUB 4096    /* 4-bit counters, 2 KB */

/* Shared with the HW_WALK2 slave copy (its walk skips the same areas; a
 * lost nibble update between the CPUs only means one skip more or less). */
HW_W2_SHARED(uint8_t hw_tiny_cnt[HW_TINY_MAXSUB / 2], );
HW_W2_SHARED(int32_t hw_tiny_t, = (HW_TINY_T0 > 0) ? HW_TINY_T0 : 0);   /* px; 0 = off */
HW_W2_SHARED(unsigned int saturn_hw_tiny_skips, );
#ifndef HW_WALK2_TU
unsigned int saturn_hw_tiny_marked;

/* Level start / threshold off / teleport: forget all marks. */
static void HW_TinyReset(void)
{
    memset(hw_tiny_cnt, 0, sizeof(hw_tiny_cnt));
}
#endif

#if HW_ASM_SUBBODY
#define HW_TINY_REAL_LEAF hw_asm_subsector
#define HW_TINY_REAL_LEAF_PTR ((const void *)hw_asm_subsector)
#else
#define HW_TINY_REAL_LEAF(s, f) HW_LEAF_FN((s), (f), NULL)
#define HW_TINY_REAL_LEAF_PTR ((const void *)HW_LEAF_FN)
#endif

/* The BSP walk's leaf while hw_tiny_t > 0 (hw_asm_bsp_ctx leaf entry, or
 * the C walk directly). */
static HW_NOINLINE void HW_TinyLeaf(int subnum, int fully_inside)
{
    unsigned s = (unsigned)subnum;
    if (s < HW_TINY_MAXSUB)
    {
        uint8_t *b = &hw_tiny_cnt[s >> 1];
        int sh = (int)(s & 1u) << 2;
        if ((*b >> sh) & 15u)
        {
            *b = (uint8_t)(*b - (1u << sh));
            saturn_hw_tiny_skips++;
            return;
        }
    }
    HW_TINY_REAL_LEAF(subnum, fully_inside);
}

#ifndef HW_WALK2_TU
/* Clipped bounding-box area of n commands (stops once >= limit). Clip
 * commands (absolute x) set the clip; vertices are view-relative. */
static int HW_TinyArea(const uint32_t *c, int n, int limit, int x_off, int vw, int vh)
{
    int cx0 = 0, cy0 = 0, cx1 = vw - 1, cy1 = vh - 1, area = 0;
    for (int k = 0; k < n; k++, c += 8)
    {
        uint32_t w0 = c[0];
        int comm = (int)(w0 >> 16) & 15;
        if (w0 & 0x40000000u)
            continue;                           /* skip bit */
        if (comm == 8)
        {
            cx0 = (int16_t)(c[3] >> 16) - x_off; cy0 = (int16_t)c[3];
            cx1 = (int16_t)(c[5] >> 16) - x_off; cy1 = (int16_t)c[5];
            if (cx0 < 0) cx0 = 0;
            if (cy0 < 0) cy0 = 0;
            if (cx1 > vw - 1) cx1 = vw - 1;
            if (cy1 > vh - 1) cy1 = vh - 1;
            continue;
        }
        if (comm > 4)
            continue;
        int x0 = 32767, y0 = 32767, x1 = -32768, y1 = -32768;
        for (int v = 3; v <= 6; v++)
        {
            int x = (int16_t)(c[v] >> 16), y = (int16_t)c[v];
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        if (x0 < cx0) x0 = cx0;
        if (y0 < cy0) y0 = cy0;
        if (x1 > cx1) x1 = cx1;
        if (y1 > cy1) y1 = cy1;
        if (x1 >= x0 && y1 >= y0)
        {
            area += (x1 - x0 + 1) * (y1 - y0 + 1);
            if (area >= limit)
                return area;
        }
    }
    return area;
}

/* After the frame's lists are final (Saturn_CmdBuf_CullOccluded purged the
 * cache, the slave's floors are in): mark this frame's tiny visits. */
static void HW_TinyMeasure(void)
{
#ifdef SATURN_WIN32
    {
        static int env_done = 0;         /* harness: HW_TINY_PX = fixed threshold */
        if (!env_done)
        {
            const char *e = getenv("HW_TINY_PX");
            env_done = 1;
            if (e)
                hw_tiny_t = atoi(e);
        }
    }
#endif
    int t = hw_tiny_t;
    saturn_hw_tiny_marked = 0;
    if (t <= 0)
        return;
    {
        /* teleport / respawn: the marks describe another view */
        static int lx, ly;
        int dx = hw_cam_x - lx, dy = hw_cam_y - ly;
        lx = hw_cam_x; ly = hw_cam_y;
        if (dx < -128 || dx > 128 || dy < -128 || dy > 128)
            HW_TinyReset();
    }
    const int vw = saturn_view_w > 0 ? saturn_view_w : 320;
    const int vh = saturn_view_h > 0 ? saturn_view_h : 168;
    const int x_off = (HW_MAX_SCREEN_W - vw) >> 1;
    const uint32_t *cm = (const uint32_t *)saturn_cmd_m;
    const uint32_t *cs = (const uint32_t *)saturn_cmd_s;
    /* visits [va, vb) and [vc, vd): a split frame (HW_WALK2) has the
     * slave's at the bottom [0, hw_w2_out_v) and the master's from
     * HW_W2_VISIT_CAP; the ranges index the same buffers */
    int va = 0, vb = saturn_visit_count, vc = 0, vd = 0;
#if HW_WALK2 && !defined(SATURN_WIN32)
    {
        extern int saturn_w2_split_x, hw_w2_out_v;
        if (saturn_w2_split_x >= 0)
        {
            va = HW_W2_VISIT_CAP;
            vd = hw_w2_out_v;
        }
    }
#endif
    for (int v = va, pass = 0; pass < 2; pass++, v = vc, vb = vd)
    for (; v < vb; v++)
    {
        const sub_range_t *r = &saturn_sub_ranges[v];
        unsigned s = r->subnum;
        if (s >= HW_TINY_MAXSUB)
            continue;
        int a = 0;
        if (r->m_count)
            a = HW_TinyArea(cm + ((uint32_t)r->m_start << 3), r->m_count, t, x_off, vw, vh);
        if (a < t && r->s_count)
            a += HW_TinyArea(cs + ((uint32_t)r->s_start << 3), r->s_count, t - a, x_off, vw, vh);
        if (a < t)
        {
            uint8_t *b = &hw_tiny_cnt[s >> 1];
            int sh = (int)(s & 1u) << 2;
            *b = (uint8_t)((*b & ~(15u << sh)) | ((unsigned)HW_TINY_HOLD << sh));
            saturn_hw_tiny_marked++;
        }
    }
}

/* Saturn_HW_AdaptiveDetail, once per displayed 3D frame: avg = running
 * average frame time, slow / fast = its 30 FPS bands. */
static void HW_TinyGovern(unsigned int avg, unsigned int slow, unsigned int fast)
{
    static int run = 0;
    int t = hw_tiny_t;
#ifdef SATURN_WIN32
    return;                 /* harness: fixed HW_TINY_PX */
#endif
    if (HW_TINY_T0 >= 0)
        t = HW_TINY_T0;
    else if (avg > slow)
    {
        run = (run < 0) ? 1 : run + 1;
        if (run >= 4)
        {
            run = 0;
            t += (t >> 2) + 8;                  /* 8, 18, 30, 45, 64 ... */
            if (t > HW_TINY_TMAX)
                t = HW_TINY_TMAX;
        }
    }
    else if (avg < fast && t > 0)
    {
        run = (run > 0) ? -1 : run - 1;
        if (run <= -15)
        {
            run = 0;
            t -= (t >> 3) + 2;
            if (t < 0)
                t = 0;
        }
    }
    else
        run = 0;
    if (t == 0 && hw_tiny_t != 0)
        HW_TinyReset();
    hw_tiny_t = t;
}
#endif /* !HW_WALK2_TU */
#endif /* HW_TINY */
