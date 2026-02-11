/*
 * QEMU libretro audio backend
 *
 * Captures PCM audio from QEMU's audio subsystem and buffers it
 * for retrieval by the libretro core's retro_run() via audio_batch_cb().
 *
 * Copyright (c) 2025
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/atomic.h"

#define AUDIO_CAP "libretro"
#include "audio_int.h"

/*
 * Note: The actual libretro audio path is in hw/xbox/mcpx/apu/monitor.c
 * which writes APU frames directly to a ring buffer pulled by retro_run().
 * This QEMU audio backend stub exists only to satisfy the audio subsystem.
 */

typedef struct LibretroVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;
} LibretroVoiceOut;

typedef struct LibretroVoiceIn {
    HWVoiceIn hw;
    RateCtl rate;
} LibretroVoiceIn;

/*
 * QEMU audio backend: write callback (stub).
 * Xbox APU audio goes through mcpx/apu/monitor.c ring buffer, not here.
 */
static size_t libretro_write(HWVoiceOut *hw, void *buf, size_t len)
{
    LibretroVoiceOut *lr = (LibretroVoiceOut *)hw;
    return audio_rate_get_bytes(&lr->rate, &hw->info, len);
}

static int libretro_init_out(HWVoiceOut *hw, struct audsettings *as, void *drv_opaque)
{
    LibretroVoiceOut *lr = (LibretroVoiceOut *)hw;

    /* Request stereo int16 at 48kHz */
    struct audsettings obt = *as;
    obt.freq = 48000;
    obt.nchannels = 2;
    obt.fmt = AUDIO_FORMAT_S16;

    audio_pcm_init_info(&hw->info, &obt);
    hw->samples = 1024;
    audio_rate_start(&lr->rate);

    return 0;
}

static void libretro_fini_out(HWVoiceOut *hw)
{
    (void)hw;
}

static void libretro_enable_out(HWVoiceOut *hw, bool enable)
{
    LibretroVoiceOut *lr = (LibretroVoiceOut *)hw;

    if (enable) {
        audio_rate_start(&lr->rate);
    }
}

/* Input (microphone) - stub implementation */
static int libretro_init_in(HWVoiceIn *hw, struct audsettings *as, void *drv_opaque)
{
    LibretroVoiceIn *lr = (LibretroVoiceIn *)hw;
    audio_pcm_init_info(&hw->info, as);
    hw->samples = 1024;
    audio_rate_start(&lr->rate);
    return 0;
}

static void libretro_fini_in(HWVoiceIn *hw)
{
    (void)hw;
}

static size_t libretro_read(HWVoiceIn *hw, void *buf, size_t size)
{
    LibretroVoiceIn *lr = (LibretroVoiceIn *)hw;
    int64_t bytes = audio_rate_get_bytes(&lr->rate, &hw->info, size);
    audio_pcm_info_clear_buf(&hw->info, buf, bytes / hw->info.bytes_per_frame);
    return bytes;
}

static void libretro_enable_in(HWVoiceIn *hw, bool enable)
{
    LibretroVoiceIn *lr = (LibretroVoiceIn *)hw;
    if (enable) {
        audio_rate_start(&lr->rate);
    }
}

static void *libretro_audio_init(Audiodev *dev, Error **errp)
{
    return &libretro_audio_init;
}

static void libretro_audio_fini(void *opaque)
{
    (void)opaque;
}

static struct audio_pcm_ops libretro_pcm_ops = {
    .init_out       = libretro_init_out,
    .fini_out       = libretro_fini_out,
    .write          = libretro_write,
    .buffer_get_free = audio_generic_buffer_get_free,
    .run_buffer_out = audio_generic_run_buffer_out,
    .enable_out     = libretro_enable_out,

    .init_in        = libretro_init_in,
    .fini_in        = libretro_fini_in,
    .read           = libretro_read,
    .run_buffer_in  = audio_generic_run_buffer_in,
    .enable_in      = libretro_enable_in,
};

static struct audio_driver libretro_audio_driver = {
    .name           = "libretro",
    .init           = libretro_audio_init,
    .fini           = libretro_audio_fini,
    .pcm_ops        = &libretro_pcm_ops,
    .max_voices_out = 1,
    .max_voices_in  = 1,
    .voice_size_out = sizeof(LibretroVoiceOut),
    .voice_size_in  = sizeof(LibretroVoiceIn),
};

static void register_audio_libretro(void)
{
    audio_driver_register(&libretro_audio_driver);
}
type_init(register_audio_libretro);
