/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_int.h"

#ifdef LIBRETRO

#include "qemu/atomic.h"
#include <stdint.h>

int libretro_audio_pull(int16_t *out_buf, int max_frames);

#define LIBRETRO_APU_RING_FRAMES 16384
#define LIBRETRO_APU_RING_SAMPLES (LIBRETRO_APU_RING_FRAMES * 2)

static int16_t apu_ring[LIBRETRO_APU_RING_SAMPLES];
static volatile uint32_t apu_ring_wp = 0;
static volatile uint32_t apu_ring_rp = 0;

int libretro_audio_ring_frames(void);
int libretro_audio_ring_frames(void)
{
    uint32_t rp = qatomic_read(&apu_ring_rp);
    uint32_t wp = qatomic_read(&apu_ring_wp);
    if (wp >= rp) return (wp - rp) / 2;
    return (LIBRETRO_APU_RING_SAMPLES - rp + wp) / 2;
}

void libretro_audio_flush(void);
void libretro_audio_flush(void)
{
    qatomic_set(&apu_ring_rp, qatomic_read(&apu_ring_wp));
}

int libretro_audio_pull(int16_t *out_buf, int max_frames)
{
    uint32_t rp = qatomic_read(&apu_ring_rp);
    uint32_t wp = qatomic_read(&apu_ring_wp);
    int avail;

    if (wp >= rp) {
        avail = (wp - rp) / 2;
    } else {
        avail = (LIBRETRO_APU_RING_SAMPLES - rp + wp) / 2;
    }
    if (avail > max_frames) avail = max_frames;

    for (int i = 0; i < avail; i++) {
        out_buf[i * 2 + 0] = apu_ring[rp];
        rp = (rp + 1) % LIBRETRO_APU_RING_SAMPLES;
        out_buf[i * 2 + 1] = apu_ring[rp];
        rp = (rp + 1) % LIBRETRO_APU_RING_SAMPLES;
    }
    qatomic_set(&apu_ring_rp, rp);
    return avail;
}

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    d->monitor.stream = NULL;
    qatomic_set(&apu_ring_wp, 0);
    qatomic_set(&apu_ring_rp, 0);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    uint32_t wp = qatomic_read(&apu_ring_wp);
    int16_t (*fb)[2] = d->monitor.frame_buf;

    for (int i = 0; i < 256; i++) {
        uint32_t next = (wp + 2) % LIBRETRO_APU_RING_SAMPLES;
        if (next == qatomic_read(&apu_ring_rp)) break; /* full */

        apu_ring[wp]     = fb[i][0];
        apu_ring[wp + 1] = fb[i][1];
        wp = next;
    }
    qatomic_set(&apu_ring_wp, wp);

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}

#else /* !LIBRETRO */

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    SDL_AudioSpec spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = 2,
    };

    d->monitor.stream = NULL;

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        error_setg(errp, "SDL_Init failed: %s", SDL_GetError());
        return;
    }

    d->monitor.stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (d->monitor.stream == NULL) {
        error_setg(errp, "SDL_OpenAudioDeviceStream failed: %s",
                   SDL_GetError());
        return;
    }

    SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(d->monitor.stream));
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    if (d->monitor.stream) {
        SDL_DestroyAudioStream(d->monitor.stream);
    }
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    if (d->monitor.stream) {
        float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
        SDL_SetAudioStreamGain(d->monitor.stream, vu);
        SDL_PutAudioStreamData(d->monitor.stream, d->monitor.frame_buf,
                            sizeof(d->monitor.frame_buf));
    }

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}

#endif /* LIBRETRO */
