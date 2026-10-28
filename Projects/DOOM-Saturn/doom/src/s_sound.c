/* s_sound.c — Sega Saturn SCSP Hardware PCM Sound Engine & Win32 WaveOut Backend
 *
 * On Sega Saturn:
 *   - Preloads all DOOM DS* sound effect lumps once at boot (in ascending CD
 *     sector order) directly into the SCSP's dedicated 512 KB Sound RAM
 *     (0x25A00800..0x25A7FFFF) as signed 8-bit PCM (PCM8B = 1).
 *   - Uses zero bytes of mainzone / Work RAM for sample storage and performs
 *     zero CD reads during gameplay.
 *   - Drives SCSP hardware voice slots 0..7 (0x25B00000 + slot * 0x20) directly
 *     from the Master SH-2 with 3D distance attenuation and stereo panning.
 */

#include "doomdef.h"
#include "p_local.h"
#include "r_local.h"

#ifdef SATURN_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#else
#include <jo/jo.h>
#endif

extern const char * const S_sfxnames[NUMSFX];
extern sfxinfo_t          S_sfx[NUMSFX];
extern volatile unsigned int saturn_vbl_count;

#define SAT_SFX_CHANNELS   8
#define SAT_CLIP_DIST      1200   /* map units where sound attenuates to 0 */
#define SAT_CLOSE_DIST     160    /* map units where sound stays at max vol */

/* SCSP hardware base addresses (SH-2 uncached B-Bus) */
#define SCSP_RAM_BASE      0x25A00000u
#define SCSP_RAM_SAMPLE0   0x00000800u  /* leave first 2 KB for M68K vectors/idle */
#define SCSP_RAM_LIMIT     0x00080000u  /* 512 KB total Sound RAM */
#define SCSP_SLOT_BASE     0x25B00000u
#define SCSP_COMMON_CTRL   0x25B00400u

/* SCSP OCT/FNS pitch words (44100 Hz * 2^OCT * (1 + FNS/1024)):
 *   11025.00 Hz = 44100 * 2^(-2) * (1 +   0/1024) -> OCT=-2 (0xE), FNS=0   -> 0x7000
 *    8268.75 Hz = 44100 * 2^(-3) * (1 + 512/1024) -> OCT=-3 (0xD), FNS=512 -> 0x6A00
 */
#define SCSP_PITCH_11025   0x7000u
#define SCSP_PITCH_8268    0x6A00u

typedef struct {
    uint32_t sram_off;     /* byte offset in SCSP Sound RAM (even-aligned) */
    uint16_t nsamples;     /* sample count (LEA) */
    uint16_t oct_fns;      /* SCSP register 0x10 pitch word */
    uint16_t dur_vbls;     /* playback duration in 60 Hz VBlanks */
    uint16_t valid;
} sat_sfx_entry_t;

static sat_sfx_entry_t s_sfx_cache[NUMSFX];
static int             s_sound_inited = 0;

/* Active channel tracking (slots 0..7) */
static const void  *s_chan_origin[SAT_SFX_CHANNELS];
static short        s_chan_sfx[SAT_SFX_CHANNELS];
static short        s_chan_pri[SAT_SFX_CHANNELS];
static unsigned int s_chan_end_vbl[SAT_SFX_CHANNELS];

#ifdef SATURN_WIN32
typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdr;
    int16_t *pcm_buf;
    int      cap_samples;
    int      opened;
} win32_voice_t;

static uint8_t      *s_win32_sfx_pcm[NUMSFX];
static win32_voice_t s_win32_voices[SAT_SFX_CHANNELS];
#endif

static void make_ds_lump_name(char out[9], const char *sfx_name)
{
    int i = 0;
    out[0] = 'D';
    out[1] = 'S';
    while (sfx_name[i] && i < 6)
    {
        char c = sfx_name[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - ('a' - 'A'));
        out[2 + i] = c;
        i++;
    }
    out[2 + i] = '\0';
}

#ifndef SATURN_WIN32
static void scsp_hw_init(void)
{
    volatile uint16_t *sram16 = (volatile uint16_t *)SCSP_RAM_BASE;
    volatile uint16_t *slot16 = (volatile uint16_t *)SCSP_SLOT_BASE;
    volatile uint16_t *ctrl16 = (volatile uint16_t *)SCSP_COMMON_CTRL;
    int i;

    /* 1. Hold M68000 in reset while initializing vectors and SCSP slots */
    jo_smpc_begin_command();
    jo_smpc_end_command(DisableSoundCPU);

    /* 2. Clear first 2 KB of Sound RAM and plant a minimal M68000 idle loop:
     *    SSP = 0x00000400, PC = 0x00000400, code at 0x400 = NOP; BRA.S .-2 */
    for (i = 0; i < (int)(SCSP_RAM_SAMPLE0 >> 1); i++)
        sram16[i] = 0x0000u;

    sram16[0] = 0x0000u; sram16[1] = 0x0400u; /* Reset SSP = 0x00000400 */
    sram16[2] = 0x0000u; sram16[3] = 0x0400u; /* Reset PC  = 0x00000400 */
    sram16[0x400 >> 1]       = 0x4E71u;       /* NOP */
    sram16[(0x400 >> 1) + 1] = 0x60FEu;       /* BRA.S -2 (infinite idle) */

    /* 3. Configure SCSP for 4Mbit (512 KB) Sound RAM, 16-bit DAC, max volume */
    ctrl16[0] = 0x020Fu;

    /* 4. Zero all 32 hardware voice slots and execute global Key-Off */
    for (i = 0; i < 32 * 16; i++)
        slot16[i] = 0x0000u;
    slot16[0] = 0x1000u; /* KYONEX = 1 with all KYONB = 0 */

    /* 5. Release M68000 reset so emulators that gate SCSP on SNDON run audio */
    jo_smpc_begin_command();
    jo_smpc_end_command(EnableSoundCPU);

    /* Re-assert 4Mbit mode + Master Volume 15 */
    ctrl16[0] = 0x020Fu;
}
#endif

void S_Init(void)
{
    short lump_for_sfx[NUMSFX];
    short order[NUMSFX];
    int   n_valid = 0;
    int   total_11k_bytes = 0;
    int   decimate_3_4 = 0;
    uint32_t sram_cursor = SCSP_RAM_SAMPLE0;
    byte *work_buf;
    int   i, j;

    if (s_sound_inited)
        return;

    D_memset(s_sfx_cache, 0, sizeof(s_sfx_cache));
    for (i = 0; i < SAT_SFX_CHANNELS; i++)
    {
        s_chan_origin[i]  = NULL;
        s_chan_sfx[i]     = 0;
        s_chan_pri[i]     = 0;
        s_chan_end_vbl[i] = 0;
    }

#ifndef SATURN_WIN32
    scsp_hw_init();
#endif

    /* Resolve WAD lump numbers for all S_sfxnames[1..NUMSFX-1] */
    for (i = 1; i < NUMSFX; i++)
    {
        lump_for_sfx[i] = -1;
        if (!S_sfxnames[i])
            continue;
        {
            char dname[9];
            int  lump;
            make_ds_lump_name(dname, S_sfxnames[i]);
            lump = W_CheckNumForName(dname);
            if (lump >= 0)
            {
                int len = W_LumpLength(lump);
                S_sfx[i].lump = lump;
                if (len > 16 && len <= 48 * 1024)
                {
                    lump_for_sfx[i] = (short)lump;
                    order[n_valid++] = (short)i;
                    total_11k_bytes += (len - 8);
                }
            }
        }
    }

    /* Sort by ascending WAD lump index so CD streaming seeks strictly forward */
    for (i = 1; i < n_valid; i++)
    {
        short key_sfx  = order[i];
        short key_lump = lump_for_sfx[key_sfx];
        j = i - 1;
        while (j >= 0 && lump_for_sfx[order[j]] > key_lump)
        {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key_sfx;
    }

    /* If total PCM exceeds 502 KB (e.g. Ultimate DOOM's 65 SFX = ~650 KB),
     * use 3/4 decimation (8268.75 Hz, SCSP_PITCH_8268) so all SFX fit in
     * the 512 KB SCSP Sound RAM. On Shareware DOOM1.WAD (53 SFX = ~504 KB),
     * keep full 11025 Hz unless sram_cursor nears the top. */
    if (total_11k_bytes > (int)(SCSP_RAM_LIMIT - SCSP_RAM_SAMPLE0 - 8192u))
        decimate_3_4 = 1;

    work_buf = I_WorkBuffer();

    for (i = 0; i < n_valid; i++)
    {
        int      sfx_id = order[i];
        int      lump   = lump_for_sfx[sfx_id];
        int      len    = W_LumpLength(lump);
        uint16_t fmt, rate, nsamp;
        const byte *pcm_src;
        int      avail_samples;
        int      step_22k = 0;
        int      use_3_4  = decimate_3_4;
        int      out_samples;
        uint32_t aligned_cursor;

        if (len <= 16 || len > 48 * 1024)
            continue;

        W_ReadLump(lump, work_buf);

        /* DMX header: little-endian uint16 format (3), sample_rate, num_samples, pad */
        fmt   = (uint16_t)(work_buf[0] | (work_buf[1] << 8));
        rate  = (uint16_t)(work_buf[2] | (work_buf[3] << 8));
        nsamp = (uint16_t)(work_buf[4] | (work_buf[5] << 8));
        if (fmt != 3 || nsamp <= 32)
            continue;

        avail_samples = len - 8;
        if ((int)nsamp < avail_samples)
            avail_samples = (int)nsamp;

        /* Strip DMX's 16-byte leading and 16-byte trailing pad bytes */
        pcm_src = work_buf + 8 + 16;
        avail_samples -= 32;
        if (avail_samples <= 4)
            continue;

        if (rate > 16000)
            step_22k = 1;

        out_samples = step_22k ? (avail_samples >> 1) : avail_samples;
        if (use_3_4)
            out_samples = (out_samples * 3) >> 2;
        out_samples &= ~1; /* even number of bytes for 16-bit SCSP B-Bus writes */
        if (out_samples < 4)
            continue;
        if (out_samples > 65532)
            out_samples = 65532;

        aligned_cursor = (sram_cursor + 3u) & ~3u;
        if (aligned_cursor + (uint32_t)out_samples + 4u > SCSP_RAM_LIMIT)
        {
            /* Try 3/4 decimation if we were at 11025 Hz and neared the top of Sound RAM */
            if (!use_3_4)
            {
                use_3_4 = 1;
                out_samples = ((step_22k ? (avail_samples >> 1) : avail_samples) * 3) >> 2;
                out_samples &= ~1;
            }
            if (aligned_cursor + (uint32_t)out_samples + 4u > SCSP_RAM_LIMIT)
                continue;
        }

#ifndef SATURN_WIN32
        {
            volatile uint16_t *dst16 = (volatile uint16_t *)(SCSP_RAM_BASE + aligned_cursor);
            int src_idx = 0;
            int step3   = 0;
            int pairs   = out_samples >> 1;

            for (j = 0; j < pairs; j++)
            {
                uint8_t s0, s1;
                s0 = pcm_src[src_idx] ^ 0x80u;
                if (!use_3_4)
                {
                    src_idx += step_22k ? 2 : 1;
                }
                else
                {
                    step3++;
                    src_idx += (step3 == 3) ? (step_22k ? 4 : 2) : (step_22k ? 2 : 1);
                    if (step3 == 3) step3 = 0;
                }

                s1 = pcm_src[src_idx] ^ 0x80u;
                if (!use_3_4)
                {
                    src_idx += step_22k ? 2 : 1;
                }
                else
                {
                    step3++;
                    src_idx += (step3 == 3) ? (step_22k ? 4 : 2) : (step_22k ? 2 : 1);
                    if (step3 == 3) step3 = 0;
                }

                dst16[j] = (uint16_t)(((unsigned)s0 << 8) | (unsigned)s1);
            }
            dst16[pairs] = 0x0000u; /* zero guard word at sample end */
        }
#else
        {
            uint8_t *buf = (uint8_t *)malloc(out_samples);
            if (buf)
            {
                int src_idx = 0;
                int step3   = 0;
                for (j = 0; j < out_samples; j++)
                {
                    buf[j] = pcm_src[src_idx];
                    if (!use_3_4)
                    {
                        src_idx += step_22k ? 2 : 1;
                    }
                    else
                    {
                        step3++;
                        src_idx += (step3 == 3) ? (step_22k ? 4 : 2) : (step_22k ? 2 : 1);
                        if (step3 == 3) step3 = 0;
                    }
                }
                s_win32_sfx_pcm[sfx_id] = buf;
            }
        }
#endif

        s_sfx_cache[sfx_id].sram_off = aligned_cursor;
        s_sfx_cache[sfx_id].nsamples = (uint16_t)out_samples;
        s_sfx_cache[sfx_id].oct_fns  = use_3_4 ? SCSP_PITCH_8268 : SCSP_PITCH_11025;
        /* Duration in 60 Hz VBlanks (~out_samples / 184 or / 138, shift-multiply) */
        {
            unsigned int mult = use_3_4 ? 19u : 14u;
            unsigned int vbls = (((unsigned int)out_samples * mult) >> 11) + 2u;
            s_sfx_cache[sfx_id].dur_vbls = (uint16_t)vbls;
        }
        s_sfx_cache[sfx_id].valid = 1;

        sram_cursor = aligned_cursor + (uint32_t)out_samples + 2u;
    }

    s_sound_inited = 1;
}

static void scsp_reap_finished_voices(unsigned int now_vbl)
{
#ifndef SATURN_WIN32
    int c;
    for (c = 0; c < SAT_SFX_CHANNELS; c++)
    {
        if (s_chan_sfx[c] != 0 && (int)(now_vbl - s_chan_end_vbl[c]) >= 0)
        {
            volatile uint16_t *slot = (volatile uint16_t *)(SCSP_SLOT_BASE + (unsigned)c * 0x20u);
            slot[0] &= (uint16_t)~0x1800u; /* clear KYONB so future KYONEX won't retrigger */
            s_chan_sfx[c]    = 0;
            s_chan_origin[c] = NULL;
        }
    }
#else
    int c;
    for (c = 0; c < SAT_SFX_CHANNELS; c++)
    {
        if (s_chan_sfx[c] != 0 && (int)(now_vbl - s_chan_end_vbl[c]) >= 0)
        {
            s_chan_sfx[c]    = 0;
            s_chan_origin[c] = NULL;
        }
    }
#endif
}

static void scsp_trigger_voice(int ch, const sat_sfx_entry_t *e, int sfx_id, int vol, int sep)
{
#ifndef SATURN_WIN32
    volatile uint16_t *s = (volatile uint16_t *)(SCSP_SLOT_BASE + (unsigned)ch * 0x20u);
    unsigned int tl;
    unsigned int dipan;

    (void)sfx_id;

    /* 1. Key-Off this slot first so the envelope generator resets cleanly */
    s[0x00 >> 1] = 0x1000u;

    /* 2. Convert DOOM volume (1..127) to SCSP Total Level attenuation (0..95) */
    if (vol > 127) vol = 127;
    if (vol < 1)   vol = 1;
    tl = (unsigned int)(((127 - vol) * 3) >> 2);

    /* 3. Convert DOOM stereo separation (0=left, 128=center, 255=right) to SCSP DIPAN */
    if (sep < 0)   sep = 0;
    if (sep > 255) sep = 255;
    if (sep < 128)
        dipan = (unsigned int)((128 - sep) >> 3) & 0x0Fu;          /* bit 4=0: pan Left */
    else
        dipan = 0x10u | ((unsigned int)((sep - 128) >> 3) & 0x0Fu); /* bit 4=1: pan Right */

    /* 4. Program SCSP slot registers (one-shot 8-bit signed PCM) */
    s[0x02 >> 1] = (uint16_t)(e->sram_off & 0xFFFFu);
    s[0x04 >> 1] = 0x0000u;                                        /* LSA = 0 */
    s[0x06 >> 1] = e->nsamples;                                    /* LEA = sample count */
    s[0x08 >> 1] = 0x001Fu;                                        /* D2R=0, D1R=0, AR=31 */
    s[0x0A >> 1] = 0x001Fu;                                        /* DL=0, RR=31 */
    s[0x0C >> 1] = (uint16_t)(tl & 0xFFu);                         /* TL attenuation */
    s[0x0E >> 1] = 0x0000u;                                        /* MDL = 0 */
    s[0x10 >> 1] = e->oct_fns;                                     /* OCT / FNS pitch */
    s[0x12 >> 1] = 0x0000u;                                        /* LFO off */
    s[0x14 >> 1] = 0x0000u;                                        /* ISEL / IMXL = 0 */
    s[0x16 >> 1] = (uint16_t)(0xE000u | (dipan << 8));             /* DISDL=7, DIPAN */

    /* 5. Key-On (KYONEX=1, KYONB=1, PCM8B=1, SA[19:16]) */
    s[0x00 >> 1] = (uint16_t)(0x1810u | ((e->sram_off >> 16) & 0x0Fu));
#else
    win32_voice_t *v = &s_win32_voices[ch];
    const uint8_t *src = s_win32_sfx_pcm[sfx_id];
    int n = (int)e->nsamples;
    int i, gain_l, gain_r;

    if (!src || n <= 0)
        return;

    if (!v->opened)
    {
        WAVEFORMATEX wfx;
        memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 2;
        wfx.nSamplesPerSec  = (e->oct_fns == SCSP_PITCH_8268) ? 8269 : 11025;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 4;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * 4;
        if (waveOutOpen(&v->hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR)
            v->opened = 1;
    }
    if (!v->opened)
        return;

    waveOutReset(v->hwo);
    if (v->hdr.dwFlags & WHDR_PREPARED)
        waveOutUnprepareHeader(v->hwo, &v->hdr, sizeof(WAVEHDR));

    if (v->cap_samples < n)
    {
        free(v->pcm_buf);
        v->pcm_buf = (int16_t *)malloc((size_t)n * 2 * sizeof(int16_t));
        v->cap_samples = v->pcm_buf ? n : 0;
    }
    if (!v->pcm_buf)
        return;

    gain_l = (vol * (255 - sep)) >> 7;
    gain_r = (vol * sep) >> 7;

    for (i = 0; i < n; i++)
    {
        int s = (int)src[i] - 128;
        int sl = (s * gain_l * 256) >> 7;
        int sr = (s * gain_r * 256) >> 7;
        if (sl > 32767) sl = 32767; else if (sl < -32768) sl = -32768;
        if (sr > 32767) sr = 32767; else if (sr < -32768) sr = -32768;
        v->pcm_buf[i * 2 + 0] = (int16_t)sl;
        v->pcm_buf[i * 2 + 1] = (int16_t)sr;
    }

    memset(&v->hdr, 0, sizeof(WAVEHDR));
    v->hdr.lpData         = (LPSTR)v->pcm_buf;
    v->hdr.dwBufferLength = (DWORD)(n * 2 * sizeof(int16_t));
    if (waveOutPrepareHeader(v->hwo, &v->hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
        waveOutWrite(v->hwo, &v->hdr, sizeof(WAVEHDR));
#endif
}

static void saturn_start_sound_internal(const void *origin_key, int has_pos,
                                        fixed_t ox, fixed_t oy, int sfx_id)
{
    const sat_sfx_entry_t *e;
    unsigned int now_vbl;
    int vol = 127;
    int sep = 128;
    int pri;
    int ch = -1;
    int c;

    if (!s_sound_inited)
        return;
    if (sfx_id <= 0 || sfx_id >= NUMSFX)
        return;
    e = &s_sfx_cache[sfx_id];
    if (!e->valid || e->nsamples == 0)
        return;

    /* Spatialize relative to players[consoleplayer].mo */
    if (has_pos && players && players[consoleplayer].mo)
    {
        mobj_t *listener = players[consoleplayer].mo;
        if (origin_key != (const void *)listener)
        {
            fixed_t dx = D_abs(ox - listener->x);
            fixed_t dy = D_abs(oy - listener->y);
            int dist = (dx + dy - ((dx < dy ? dx : dy) >> 1)) >> FRACBITS;

            if (dist >= SAT_CLIP_DIST)
                return; /* out of earshot */

            if (dist > SAT_CLOSE_DIST)
            {
                /* Map (SAT_CLIP_DIST - dist) in [1..1040] -> [1..127] */
                vol = ((SAT_CLIP_DIST - dist) * 125) >> 10;
                if (vol < 1)   vol = 1;
                if (vol > 127) vol = 127;
            }

            {
                unsigned int ang = listener->angle >> ANGLETOFINESHIFT;
                int rx = (ox - listener->x) >> FRACBITS;
                int ry = (oy - listener->y) >> FRACBITS;
                int right_dot = (rx * (finesine(ang) >> 8) - ry * (finecosine(ang) >> 8)) >> 8;
                sep = 128 + (right_dot >> 2);
                if (sep < 16)  sep = 16;
                if (sep > 240) sep = 240;
            }
        }
    }

    now_vbl = saturn_vbl_count;
    scsp_reap_finished_voices(now_vbl);

    /* Chainsaw idle hum: don't restart if a chainsaw sound is already playing */
    if (sfx_id == sfx_sawidl)
    {
        for (c = 0; c < SAT_SFX_CHANNELS; c++)
        {
            if (s_chan_sfx[c] == sfx_sawidl || s_chan_sfx[c] == sfx_sawful ||
                s_chan_sfx[c] == sfx_sawhit || s_chan_sfx[c] == sfx_sawup)
                return;
        }
    }

    pri = S_sfx[sfx_id].voices;

    /* 1. Reuse channel if same origin or singular sound */
    for (c = 0; c < SAT_SFX_CHANNELS; c++)
    {
        if (s_chan_sfx[c] != 0)
        {
            if (S_sfx[sfx_id].max_voices && s_chan_sfx[c] == sfx_id)
            {
                ch = c;
                break;
            }
            if (origin_key != NULL && s_chan_origin[c] == origin_key)
            {
                ch = c;
                break;
            }
        }
    }

    /* 2. Otherwise pick the first free channel */
    if (ch < 0)
    {
        for (c = 0; c < SAT_SFX_CHANNELS; c++)
        {
            if (s_chan_sfx[c] == 0)
            {
                ch = c;
                break;
            }
        }
    }

    /* 3. Otherwise preempt the channel expiring soonest of equal/lower importance */
    if (ch < 0)
    {
        int best_c = 0;
        unsigned int earliest_end = s_chan_end_vbl[0];
        for (c = 1; c < SAT_SFX_CHANNELS; c++)
        {
            if ((int)(s_chan_end_vbl[c] - earliest_end) < 0 || s_chan_pri[c] >= pri)
            {
                earliest_end = s_chan_end_vbl[c];
                best_c = c;
            }
        }
        ch = best_c;
    }

    s_chan_origin[ch]  = origin_key;
    s_chan_sfx[ch]     = (short)sfx_id;
    s_chan_pri[ch]     = (short)pri;
    s_chan_end_vbl[ch] = now_vbl + (unsigned int)e->dur_vbls;

    scsp_trigger_voice(ch, e, sfx_id, vol, sep);
}

void S_StartSound(void *origin, int sfx_id)
{
    if (origin)
    {
        mobj_t *mo = (mobj_t *)origin;
        saturn_start_sound_internal(origin, 1, mo->x, mo->y, sfx_id);
    }
    else
    {
        saturn_start_sound_internal(NULL, 0, 0, 0, sfx_id);
    }
}

void S_StartFixedSound(int sound_id, fixed_t *origin)
{
    if (origin)
        saturn_start_sound_internal(NULL, 1, origin[0], origin[1], sound_id);
    else
        saturn_start_sound_internal(NULL, 0, 0, 0, sound_id);
}

void S_StopSound(void *origin)
{
    int c;
    if (!s_sound_inited || !origin)
        return;

    for (c = 0; c < SAT_SFX_CHANNELS; c++)
    {
        if (s_chan_sfx[c] != 0 && s_chan_origin[c] == origin)
        {
#ifndef SATURN_WIN32
            volatile uint16_t *slot = (volatile uint16_t *)(SCSP_SLOT_BASE + (unsigned)c * 0x20u);
            slot[0] = 0x1000u; /* KYONB=0, KYONEX=1 */
#else
            if (s_win32_voices[c].opened)
                waveOutReset(s_win32_voices[c].hwo);
#endif
            s_chan_sfx[c]    = 0;
            s_chan_origin[c] = NULL;
        }
    }
}

void S_StartSong(int musiclump, int looping, int cdtrack)
{
    (void)musiclump; (void)looping; (void)cdtrack;
}

void S_StopSong(void) { }

void S_RestartSounds(void) { }

void S_SetSoundDriver(int newdrv) { (void)newdrv; }
