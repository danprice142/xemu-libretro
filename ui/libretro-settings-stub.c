/*
 * Minimal xemu settings stub for libretro core build.
 * Provides g_config and stub functions without toml++/SDL dependencies.
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "xemu-config.h"
#include "xemu-settings.h"

struct config g_config;

static char settings_base_path[4096] = "";
static char settings_path[4096] = "";
static char settings_eeprom_path[4096] = "";

void libretro_settings_set_base_path(const char *path)
{
    if (path) {
        snprintf(settings_base_path, sizeof(settings_base_path), "%s", path);
    }
}

void xemu_settings_set_path(const char *path)
{
    if (path) {
        snprintf(settings_path, sizeof(settings_path), "%s", path);
    }
}

const char *xemu_settings_get_base_path(void)
{
    return settings_base_path;
}

const char *xemu_settings_get_path(void)
{
    return settings_path;
}

const char *xemu_settings_get_default_eeprom_path(void)
{
    return settings_eeprom_path;
}

const char *xemu_settings_get_error_message(void)
{
    return NULL;
}

bool xemu_settings_load(void)
{
    /* In libretro mode, config is populated directly from core options */
    return true;
}

void xemu_settings_save(void)
{
    /* No-op in libretro mode */
}

void add_net_nat_forward_ports(int host, int guest, CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL protocol)
{
    (void)host; (void)guest; (void)protocol;
}

void remove_net_nat_forward_ports(unsigned int index)
{
    (void)index;
}

bool xemu_settings_load_gamepad_mapping(const char *guid, GamepadMappings **mapping)
{
    (void)guid; (void)mapping;
    return false;
}

void xemu_settings_reset_controller_mapping(const char *guid)
{
    (void)guid;
}

void xemu_settings_reset_keyboard_mapping(void)
{
}
