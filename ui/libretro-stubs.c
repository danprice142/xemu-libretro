/*
 * Stub/bridge functions for xemu symbols referenced by core code.
 * Provides libretro input bridge and stubs for UI/snapshot functions.
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

typedef struct QEMUFile QEMUFile;

#include "hw/qdev-core.h"
#include "qapi/error.h"
#include "monitor/qdev.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/config-file.h"

#include "xemu-input.h"
#include "libretro.h"

/* ========================================================================= */
/* xemu error/notification stubs                                             */
/* ========================================================================= */

void xemu_queue_error_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[xemu-libretro] ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void xemu_queue_notification(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[xemu-libretro] NOTIFY: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ========================================================================= */
/* Libretro input bridge                                                     */
/* ========================================================================= */

/* Libretro callbacks — set by libretro.c */
extern retro_input_state_t input_state_cb;

/* Static controller states for 4 Xbox ports */
static ControllerState libretro_controllers[4];
static bool libretro_controllers_inited = false;

ControllerStateList available_controllers =
    QTAILQ_HEAD_INITIALIZER(available_controllers);
ControllerState *bound_controllers[4] = { NULL, NULL, NULL, NULL };
const char *bound_drivers[4] = {
    "usb-xbox-gamepad", "usb-xbox-gamepad",
    "usb-xbox-gamepad", "usb-xbox-gamepad"
};

int *g_keyboard_scancode_map[25] = { NULL };

static void libretro_controllers_ensure_init(void)
{
    if (libretro_controllers_inited) return;
    libretro_controllers_inited = true;

    for (int i = 0; i < 4; i++) {
        memset(&libretro_controllers[i], 0, sizeof(ControllerState));
        libretro_controllers[i].type = INPUT_DEVICE_SDL_GAMEPAD;
        libretro_controllers[i].name = "RetroPad";
        libretro_controllers[i].bound = i;
        libretro_controllers[i].peripheral_types[0] = PERIPHERAL_NONE;
        libretro_controllers[i].peripheral_types[1] = PERIPHERAL_NONE;
        bound_controllers[i] = &libretro_controllers[i];
    }
}

ControllerState *xemu_input_get_bound(int index)
{
    libretro_controllers_ensure_init();
    if (index < 0 || index >= 4) return NULL;
    return bound_controllers[index];
}

int xemu_input_get_test_mode(void)
{
    return 0;
}

void xemu_input_update_controller(ControllerState *state)
{
    if (!state || !input_state_cb) return;

    int port = state->bound;
    if (port < 0 || port >= 4) return;

    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
        state->buttons |= CONTROLLER_BUTTON_A;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
        state->buttons |= CONTROLLER_BUTTON_B;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))
        state->buttons |= CONTROLLER_BUTTON_X;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))
        state->buttons |= CONTROLLER_BUTTON_Y;

    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))
        state->buttons |= CONTROLLER_BUTTON_WHITE;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))
        state->buttons |= CONTROLLER_BUTTON_BLACK;

    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
        state->buttons |= CONTROLLER_BUTTON_BACK;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
        state->buttons |= CONTROLLER_BUTTON_START;

    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3))
        state->buttons |= CONTROLLER_BUTTON_LSTICK;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3))
        state->buttons |= CONTROLLER_BUTTON_RSTICK;

    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
        state->buttons |= CONTROLLER_BUTTON_DPAD_UP;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
        state->buttons |= CONTROLLER_BUTTON_DPAD_DOWN;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
        state->buttons |= CONTROLLER_BUTTON_DPAD_LEFT;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
        state->buttons |= CONTROLLER_BUTTON_DPAD_RIGHT;

    int16_t lt = input_state_cb(port, RETRO_DEVICE_ANALOG,
                                RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                                RETRO_DEVICE_ID_JOYPAD_L2);
    if (lt == 0 && input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))
        lt = 0x7FFF;
    state->axis[CONTROLLER_AXIS_LTRIG] = lt;

    int16_t rt = input_state_cb(port, RETRO_DEVICE_ANALOG,
                                RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                                RETRO_DEVICE_ID_JOYPAD_R2);
    if (rt == 0 && input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))
        rt = 0x7FFF;
    state->axis[CONTROLLER_AXIS_RTRIG] = rt;

    state->axis[CONTROLLER_AXIS_LSTICK_X] = input_state_cb(port, RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    state->axis[CONTROLLER_AXIS_LSTICK_Y] = -input_state_cb(port, RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    state->axis[CONTROLLER_AXIS_RSTICK_X] = input_state_cb(port, RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
    state->axis[CONTROLLER_AXIS_RSTICK_Y] = -input_state_cb(port, RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
}

void xemu_input_update_rumble(ControllerState *state)
{
    /* TODO: Use RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE to send rumble */
    (void)state;
}

void libretro_input_create_xid_devices(void)
{
    static const int port_map[4] = { 3, 4, 1, 2 };

    libretro_controllers_ensure_init();

    for (int i = 0; i < 4; i++) {
        char *tmp;

        QDict *usbhub_qdict = qdict_new();
        qdict_put_str(usbhub_qdict, "driver", "usb-hub");
        tmp = g_strdup_printf("1.%d", port_map[i]);
        qdict_put_str(usbhub_qdict, "port", tmp);
        qdict_put_int(usbhub_qdict, "ports", 3);
        QemuOpts *usbhub_opts = qemu_opts_from_qdict(
            qemu_find_opts("device"), usbhub_qdict, &error_abort);
        DeviceState *usbhub_dev = qdev_device_add(usbhub_opts, &error_abort);
        g_free(tmp);

        QDict *qdict = qdict_new();
        qdict_put_str(qdict, "driver", "usb-xbox-gamepad");

        tmp = g_strdup_printf("libretro_gamepad_%d", i);
        qdict_put_str(qdict, "id", tmp);
        g_free(tmp);

        qdict_put_int(qdict, "index", i);
        tmp = g_strdup_printf("1.%d.1", port_map[i]);
        qdict_put_str(qdict, "port", tmp);
        g_free(tmp);

        QemuOpts *opts = qemu_opts_from_qdict(
            qemu_find_opts("device"), qdict, &error_abort);
        DeviceState *dev = qdev_device_add(opts, &error_abort);

        qobject_unref(usbhub_qdict);
        object_unref(OBJECT(usbhub_dev));
        qobject_unref(qdict);
        if (dev) object_unref(OBJECT(dev));

        libretro_controllers[i].device = usbhub_dev;
    }

}

void xemu_input_init(void) {}
void xemu_input_process_sdl_events(const SDL_Event *event) { (void)event; }
void xemu_input_update_controllers(void) {}
void xemu_input_update_sdl_kbd_controller_state(ControllerState *state) { (void)state; }
void xemu_input_update_sdl_controller_state(ControllerState *state) { (void)state; }
void xemu_input_bind(int index, ControllerState *state, int save)
{
    (void)index; (void)state; (void)save;
}
bool xemu_input_bind_xmu(int player_index, int peripheral_port_index,
                         const char *filename, bool is_rebind)
{
    (void)player_index; (void)peripheral_port_index;
    (void)filename; (void)is_rebind;
    return false;
}
void xemu_input_rebind_xmu(int port) { (void)port; }
void xemu_input_unbind_xmu(int player_index, int peripheral_port_index)
{
    (void)player_index; (void)peripheral_port_index;
}
int xemu_input_get_controller_default_bind_port(ControllerState *state, int start)
{
    (void)state; (void)start;
    return -1;
}
void xemu_save_peripheral_settings(int player_index, int peripheral_index,
                                   int peripheral_type,
                                   const char *peripheral_parameter)
{
    (void)player_index; (void)peripheral_index;
    (void)peripheral_type; (void)peripheral_parameter;
}
void xemu_input_set_test_mode(int enabled) { (void)enabled; }
void xemu_input_reset_input_mapping(ControllerState *state) { (void)state; }

/* ========================================================================= */
/* xemu snapshot stubs                                                       */
/* ========================================================================= */

void xemu_snapshots_mark_dirty(void)
{
}

bool xemu_snapshots_offset_extra_data(QEMUFile *f)
{
    (void)f;
    return true;
}

void xemu_snapshots_save_extra_data(QEMUFile *f)
{
    (void)f;
}
