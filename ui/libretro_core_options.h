/*
 * xemu libretro core - Core Options v2
 *
 * Copyright (c) 2025
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef LIBRETRO_CORE_OPTIONS_H
#define LIBRETRO_CORE_OPTIONS_H

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

struct retro_core_option_v2_category option_cats_us[] = {
    {
        "system",
        "System",
        "Configure system-level settings such as BIOS paths, memory, and boot options."
    },
    {
        "video",
        "Video",
        "Configure video rendering settings."
    },
    {
        "audio",
        "Audio",
        "Configure audio emulation settings."
    },
    { NULL, NULL, NULL },
};

struct retro_core_option_v2_definition option_defs_us[] = {
    /* NOTE: File path options (bootrom, bios, hdd, eeprom) are not exposed
     * as core options because they require text input which core options
     * don't support (dropdown only). Paths are auto-detected from
     * RetroArch's system directory instead. */
    {
        "xemu_memory",
        "System Memory (MB)",
        NULL,
        "Amount of system RAM. 64 MB is standard; 128 MB is used by debug kits.",
        NULL,
        "system",
        {
            { "64",  "64 MB" },
            { "128", "128 MB" },
            { NULL, NULL },
        },
        "64"
    },
    {
        "xemu_network_backend",
        "Network Backend",
        NULL,
        "Enable network support for Xbox games with LAN/System Link features. NAT backend provides basic connectivity.",
        NULL,
        "system",
        {
            { "disabled", "Disabled" },
            { "nat",      "NAT" },
            { NULL, NULL },
        },
        "disabled"
    },
    {
        "xemu_skip_boot_anim",
        "Skip Boot Animation",
        NULL,
        "Skip the Xbox boot animation.",
        NULL,
        "system",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL },
        },
        "disabled"
    },
    {
        "xemu_hard_fpu",
        "Hardware FPU Emulation",
        NULL,
        "Use host FPU for x87 emulation. Faster but may have minor inaccuracies.",
        NULL,
        "system",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL },
        },
        "enabled"
    },
    {
        "xemu_surface_scale",
        "Internal Resolution Scale",
        NULL,
        "Scales the internal rendering resolution. Higher values improve quality but reduce performance.",
        NULL,
        "video",
        {
            { "1", "1x (640x480)" },
            { "2", "2x (1280x960)" },
            { "3", "3x (1920x1440)" },
            { "4", "4x (2560x1920)" },
            { "5", "5x (3200x2400)" },
            { "6", "6x (3840x2880)" },
            { "7", "7x (4480x3360)" },
            { "8", "8x (5120x3840)" },
            { "9", "9x (5760x4320)" },
            { "10", "10x (6400x4800)" },
            { NULL, NULL },
        },
        "1"
    },
    {
        "xemu_avpack",
        "AV Pack",
        NULL,
        "Select the AV pack type. Affects available video modes.",
        NULL,
        "video",
        {
            { "hdtv",      "HDTV" },
            { "composite", "Composite" },
            { "svideo",    "S-Video" },
            { "scart",     "SCART" },
            { "vga",       "VGA" },
            { NULL, NULL },
        },
        "hdtv"
    },
    {
        "xemu_use_dsp",
        "APU DSP Emulation",
        NULL,
        "Enable DSP emulation for improved audio accuracy. May impact performance.",
        NULL,
        "audio",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL },
        },
        "disabled"
    },
    {
        "xemu_cache_shaders",
        "Shader Cache",
        NULL,
        "Cache compiled GPU shaders to disk. Reduces stutter on subsequent runs.",
        NULL,
        "video",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL },
        },
        "enabled"
    },
    {
        "xemu_audio_volume",
        "Audio Volume Limit (%)",
        NULL,
        "Limit Xbox audio output volume independently from RetroArch master volume.",
        NULL,
        "audio",
        {
            { "100", "100%" },
            { "80",  "80%" },
            { "60",  "60%" },
            { "40",  "40%" },
            { "20",  "20%" },
            { "0",   "Muted" },
            { NULL, NULL },
        },
        "100"
    },
    {
        "xemu_display_filtering",
        "Display Filtering",
        NULL,
        "Texture filtering mode for the display output.",
        NULL,
        "video",
        {
            { "linear",  "Bilinear" },
            { "nearest", "Nearest Neighbor" },
            { NULL, NULL },
        },
        "linear"
    },
    { NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

struct retro_core_options_v2 options_us = {
    option_cats_us,
    option_defs_us,
};

static void libretro_set_core_options(retro_environment_t environ_cb)
{
    unsigned version = 0;

    if (!environ_cb)
        return;

    if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
        version = 0;

    if (version >= 2) {
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_us);
    } else if (version >= 1) {
        /* Fallback: convert v2 to v1 format */
        /* For simplicity, just set the v2 options and let the frontend handle it */
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_us);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* LIBRETRO_CORE_OPTIONS_H */
