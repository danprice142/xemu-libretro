/*
 * xemu libretro proxy DLL
 *
 * This tiny DLL is loaded by RetroArch. It then loads the real core
 * (xemu_libretro_impl.dll) from a subdirectory using LoadLibraryEx
 * with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR so that all MSYS2 dependency
 * DLLs are resolved from the subdirectory, avoiding conflicts with
 * RetroArch's own DLLs in its root directory.
 *
 * Build: gcc -shared -static -o xemu_libretro.dll libretro_proxy.c
 */

#include <windows.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

/* ---------- Debug file logging ---------- */

static FILE *g_proxy_log = NULL;

static void proxy_log_open(void)
{
    if (!g_proxy_log) {
        char log_path[MAX_PATH];
        HMODULE self;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)proxy_log_open, &self);
        GetModuleFileNameA(self, log_path, MAX_PATH);
        char *last = strrchr(log_path, '\\');
        if (last) *(last + 1) = '\0';
        strncat(log_path, "xemu_proxy_debug.log", MAX_PATH - strlen(log_path) - 1);
        g_proxy_log = fopen(log_path, "w");
    }
}

static void proxy_log(const char *fmt, ...)
{
    proxy_log_open();
    if (!g_proxy_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_proxy_log, fmt, ap);
    va_end(ap);
    fflush(g_proxy_log);
}

/* ---------- libretro types (minimal, matching libretro.h) ---------- */

#define RETRO_API __declspec(dllexport)

struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;
    bool need_fullpath;
    bool block_extract;
};

struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
};

/* Function pointer types for all retro_* APIs */
typedef void     (*pf_retro_init)(void);
typedef void     (*pf_retro_deinit)(void);
typedef unsigned (*pf_retro_api_version)(void);
typedef void     (*pf_retro_get_system_info)(struct retro_system_info *info);
typedef void     (*pf_retro_get_system_av_info)(void *info);
typedef void     (*pf_retro_set_controller_port_device)(unsigned port, unsigned device);
typedef void     (*pf_retro_reset)(void);
typedef void     (*pf_retro_run)(void);
typedef size_t   (*pf_retro_serialize_size)(void);
typedef bool     (*pf_retro_serialize)(void *data, size_t size);
typedef bool     (*pf_retro_unserialize)(const void *data, size_t size);
typedef void     (*pf_retro_cheat_reset)(void);
typedef void     (*pf_retro_cheat_set)(unsigned index, bool enabled, const char *code);
typedef bool     (*pf_retro_load_game)(const struct retro_game_info *game);
typedef bool     (*pf_retro_load_game_special)(unsigned type, const struct retro_game_info *info, size_t num);
typedef void     (*pf_retro_unload_game)(void);
typedef unsigned (*pf_retro_get_region)(void);
typedef void*    (*pf_retro_get_memory_data)(unsigned id);
typedef size_t   (*pf_retro_get_memory_size)(unsigned id);
typedef void     (*pf_retro_set_environment)(void *cb);
typedef void     (*pf_retro_set_video_refresh)(void *cb);
typedef void     (*pf_retro_set_audio_sample)(void *cb);
typedef void     (*pf_retro_set_audio_sample_batch)(void *cb);
typedef void     (*pf_retro_set_input_poll)(void *cb);
typedef void     (*pf_retro_set_input_state)(void *cb);

/* ---------- Global state ---------- */

static HMODULE g_real_dll = NULL;
static bool    g_load_failed = false;
static char    g_error_msg[1024] = "";

static pf_retro_init                    p_retro_init;
static pf_retro_deinit                  p_retro_deinit;
static pf_retro_api_version            p_retro_api_version;
static pf_retro_get_system_info        p_retro_get_system_info;
static pf_retro_get_system_av_info     p_retro_get_system_av_info;
static pf_retro_set_controller_port_device p_retro_set_controller_port_device;
static pf_retro_reset                  p_retro_reset;
static pf_retro_run                    p_retro_run;
static pf_retro_serialize_size         p_retro_serialize_size;
static pf_retro_serialize              p_retro_serialize;
static pf_retro_unserialize            p_retro_unserialize;
static pf_retro_cheat_reset            p_retro_cheat_reset;
static pf_retro_cheat_set             p_retro_cheat_set;
static pf_retro_load_game             p_retro_load_game;
static pf_retro_load_game_special     p_retro_load_game_special;
static pf_retro_unload_game           p_retro_unload_game;
static pf_retro_get_region            p_retro_get_region;
static pf_retro_get_memory_data       p_retro_get_memory_data;
static pf_retro_get_memory_size       p_retro_get_memory_size;
static pf_retro_set_environment       p_retro_set_environment;
static pf_retro_set_video_refresh     p_retro_set_video_refresh;
static pf_retro_set_audio_sample      p_retro_set_audio_sample;
static pf_retro_set_audio_sample_batch p_retro_set_audio_sample_batch;
static pf_retro_set_input_poll        p_retro_set_input_poll;
static pf_retro_set_input_state       p_retro_set_input_state;

#define LOAD_SYM(name) do { \
    p_##name = (pf_##name)GetProcAddress(g_real_dll, #name); \
    if (!p_##name) { \
        snprintf(g_error_msg, sizeof(g_error_msg), \
                 "xemu proxy: GetProcAddress failed for " #name); \
        FreeLibrary(g_real_dll); \
        g_real_dll = NULL; \
        g_load_failed = true; \
        return false; \
    } \
} while(0)

static bool ensure_loaded(void)
{
    if (g_real_dll)
        return true;
    if (g_load_failed)
        return false;

    proxy_log("ensure_loaded: attempting to load real DLL\n");

    /* Get path to this proxy DLL */
    char proxy_path[MAX_PATH];
    HMODULE self;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)ensure_loaded, &self);
    GetModuleFileNameA(self, proxy_path, MAX_PATH);
    proxy_log("ensure_loaded: proxy_path = %s\n", proxy_path);

    /* Build path to real DLL: same dir + /xemu/xemu_libretro_impl.dll */
    char *last_sep = strrchr(proxy_path, '\\');
    if (!last_sep) last_sep = strrchr(proxy_path, '/');
    if (!last_sep) {
        snprintf(g_error_msg, sizeof(g_error_msg),
                 "xemu proxy: cannot determine proxy DLL directory");
        g_load_failed = true;
        return false;
    }

    char real_path[MAX_PATH];
    size_t dir_len = (size_t)(last_sep - proxy_path + 1);
    memcpy(real_path, proxy_path, dir_len);
    real_path[dir_len] = '\0';
    strncat(real_path, "xemu\\xemu_libretro_impl.dll",
            MAX_PATH - dir_len - 1);

    /* Load with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR so deps are found
     * in the xemu/ subdirectory, not in RetroArch's root. */
    proxy_log("ensure_loaded: loading real DLL from: %s\n", real_path);

    /* Use DLL_LOAD_DIR + SYSTEM32 only, NOT DEFAULT_DIRS which includes
     * APPLICATION_DIR (RetroArch root) where old conflicting DLLs live. */
    g_real_dll = LoadLibraryExA(real_path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
        LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!g_real_dll) {
        DWORD err = GetLastError();
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0,
                       g_error_msg, sizeof(g_error_msg), NULL);
        proxy_log("ensure_loaded: LoadLibraryEx FAILED err=%lu: %s\n", err, g_error_msg);
        g_load_failed = true;
        return false;
    }
    proxy_log("ensure_loaded: LoadLibraryEx SUCCESS handle=%p\n", (void*)g_real_dll);

    /* Resolve all function pointers */
    LOAD_SYM(retro_init);
    LOAD_SYM(retro_deinit);
    LOAD_SYM(retro_api_version);
    LOAD_SYM(retro_get_system_info);
    LOAD_SYM(retro_get_system_av_info);
    LOAD_SYM(retro_set_controller_port_device);
    LOAD_SYM(retro_reset);
    LOAD_SYM(retro_run);
    LOAD_SYM(retro_serialize_size);
    LOAD_SYM(retro_serialize);
    LOAD_SYM(retro_unserialize);
    LOAD_SYM(retro_cheat_reset);
    LOAD_SYM(retro_cheat_set);
    LOAD_SYM(retro_load_game);
    LOAD_SYM(retro_load_game_special);
    LOAD_SYM(retro_unload_game);
    LOAD_SYM(retro_get_region);
    LOAD_SYM(retro_get_memory_data);
    LOAD_SYM(retro_get_memory_size);
    LOAD_SYM(retro_set_environment);
    LOAD_SYM(retro_set_video_refresh);
    LOAD_SYM(retro_set_audio_sample);
    LOAD_SYM(retro_set_audio_sample_batch);
    LOAD_SYM(retro_set_input_poll);
    LOAD_SYM(retro_set_input_state);

    proxy_log("ensure_loaded: all symbols resolved OK\n");
    return true;
}

/* ---------- Exported retro_* functions ---------- */

RETRO_API void retro_init(void)
{
    proxy_log("retro_init called\n");
    if (ensure_loaded()) p_retro_init();
    proxy_log("retro_init done\n");
}

RETRO_API void retro_deinit(void)
{
    if (ensure_loaded()) p_retro_deinit();
    if (g_real_dll) {
        FreeLibrary(g_real_dll);
        g_real_dll = NULL;
        g_load_failed = false;
    }
}

RETRO_API unsigned retro_api_version(void)
{
    if (ensure_loaded()) return p_retro_api_version();
    return 1;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    proxy_log("retro_get_system_info called\n");
    if (ensure_loaded()) {
        p_retro_get_system_info(info);
        proxy_log("retro_get_system_info: name=%s ver=%s ext=%s\n",
                  info->library_name, info->library_version, info->valid_extensions);
        return;
    }
    /* Fallback if real DLL not loaded */
    if (info) {
        info->library_name = "xemu";
        info->library_version = "0.0.0 (proxy-load-failed)";
        info->valid_extensions = "iso|xiso";
        info->need_fullpath = true;
        info->block_extract = true;
    }
}

RETRO_API void retro_get_system_av_info(void *info)
{
    if (ensure_loaded()) p_retro_get_system_av_info(info);
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (ensure_loaded()) p_retro_set_controller_port_device(port, device);
}

RETRO_API void retro_reset(void)
{
    if (ensure_loaded()) p_retro_reset();
}

RETRO_API void retro_run(void)
{
    if (ensure_loaded()) p_retro_run();
}

RETRO_API size_t retro_serialize_size(void)
{
    if (ensure_loaded()) return p_retro_serialize_size();
    return 0;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
    if (ensure_loaded()) return p_retro_serialize(data, size);
    return false;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
    if (ensure_loaded()) return p_retro_unserialize(data, size);
    return false;
}

RETRO_API void retro_cheat_reset(void)
{
    if (ensure_loaded()) p_retro_cheat_reset();
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    if (ensure_loaded()) p_retro_cheat_set(index, enabled, code);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    proxy_log("retro_load_game called game=%p path=%s\n", (void*)game, game ? game->path : "(null)");
    if (ensure_loaded()) {
        bool ret = p_retro_load_game(game);
        proxy_log("retro_load_game returned %d\n", ret);
        return ret;
    }
    proxy_log("retro_load_game: ensure_loaded FAILED\n");
    return false;
}

RETRO_API bool retro_load_game_special(unsigned type,
                                       const struct retro_game_info *info,
                                       size_t num)
{
    if (ensure_loaded()) return p_retro_load_game_special(type, info, num);
    return false;
}

RETRO_API void retro_unload_game(void)
{
    if (ensure_loaded()) p_retro_unload_game();
}

RETRO_API unsigned retro_get_region(void)
{
    if (ensure_loaded()) return p_retro_get_region();
    return 0;
}

RETRO_API void *retro_get_memory_data(unsigned id)
{
    if (ensure_loaded()) return p_retro_get_memory_data(id);
    return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
    if (ensure_loaded()) return p_retro_get_memory_size(id);
    return 0;
}

RETRO_API void retro_set_environment(void *cb)
{
    proxy_log("retro_set_environment called cb=%p\n", cb);
    if (ensure_loaded()) {
        proxy_log("retro_set_environment forwarding to real DLL\n");
        p_retro_set_environment(cb);
        proxy_log("retro_set_environment forwarded OK\n");
    } else {
        proxy_log("retro_set_environment: ensure_loaded FAILED: %s\n", g_error_msg);
    }
}

RETRO_API void retro_set_video_refresh(void *cb)
{
    if (ensure_loaded()) p_retro_set_video_refresh(cb);
}

RETRO_API void retro_set_audio_sample(void *cb)
{
    if (ensure_loaded()) p_retro_set_audio_sample(cb);
}

RETRO_API void retro_set_audio_sample_batch(void *cb)
{
    if (ensure_loaded()) p_retro_set_audio_sample_batch(cb);
}

RETRO_API void retro_set_input_poll(void *cb)
{
    if (ensure_loaded()) p_retro_set_input_poll(cb);
}

RETRO_API void retro_set_input_state(void *cb)
{
    if (ensure_loaded()) p_retro_set_input_state(cb);
}
