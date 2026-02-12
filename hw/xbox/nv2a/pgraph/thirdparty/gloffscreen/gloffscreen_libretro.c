/*
 *  Offscreen OpenGL abstraction layer -- libretro/WGL based
 *
 *  In libretro mode, RetroArch owns the primary GL context.
 *  This implementation creates shared WGL contexts for worker threads
 *  (like the PFIFO/PGRAPH thread) that need their own GL context.
 *
 *  Copyright (c) 2025
 *  SPDX-License-Identifier: MIT
 */

#ifdef LIBRETRO

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "gloffscreen.h"

#ifdef _WIN32
#include <windows.h>
#include <wingdi.h>

/* WGL function types */
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTPROC)(HDC);
typedef BOOL  (WINAPI *PFNWGLDELETECONTEXTPROC)(HGLRC);
typedef BOOL  (WINAPI *PFNWGLMAKECURRENTPROC)(HDC, HGLRC);
typedef HGLRC (WINAPI *PFNWGLGETCURRENTCONTEXTPROC)(void);
typedef HDC   (WINAPI *PFNWGLGETCURRENTDCPROC)(void);
typedef BOOL  (WINAPI *PFNWGLSHARELISTSPROC)(HGLRC, HGLRC);

struct _GloContext {
    HWND   hwnd;
    HDC    hdc;
    HGLRC  hglrc;
};

/* WGL_ARB_create_context defines */
#define WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB  0x00000001

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int *);

/* Stored reference to RetroArch's GL context for sharing */
static HGLRC g_retroarch_hglrc = NULL;
static HDC   g_retroarch_hdc = NULL;
static volatile bool g_libretro_gl_ready = false;
static PFNWGLCREATECONTEXTATTRIBSARBPROC p_wglCreateContextAttribsARB = NULL;
static int g_ra_pixel_format = 0;
static bool g_wndclass_registered = false;
static bool g_standalone_gl_mode = false;
static GloContext *g_standalone_root_ctx = NULL;

/* Event for PFIFO thread to wait on */
static HANDLE g_gl_ready_event = NULL;

void libretro_gl_init_wait_event(void)
{
    if (!g_gl_ready_event) {
        g_gl_ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    }
}

void libretro_gl_prepare(void)
{
    g_retroarch_hglrc = wglGetCurrentContext();
    g_retroarch_hdc = wglGetCurrentDC();

    if (!p_wglCreateContextAttribsARB) {
        p_wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)
            wglGetProcAddress("wglCreateContextAttribsARB");
    }

    if (g_retroarch_hdc) {
        g_ra_pixel_format = GetPixelFormat(g_retroarch_hdc);
    }

    g_libretro_gl_ready = true;
}

void libretro_gl_wake_pfifo(void)
{
    if (g_gl_ready_event) {
        SetEvent(g_gl_ready_event);
    }
}

void libretro_gl_wait_for_contexts(void)
{
    libretro_gl_init_wait_event();
    WaitForSingleObject(g_gl_ready_event, 30000);
}

void libretro_gl_signal_ready(void)
{
    libretro_gl_prepare();
    libretro_gl_wake_pfifo();
}

void libretro_gl_set_standalone_mode(void)
{
    g_standalone_gl_mode = true;
    g_libretro_gl_ready = true;
}

void libretro_gl_wait_ready(void)
{
    if (g_libretro_gl_ready) return;

    libretro_gl_init_wait_event();
    WaitForSingleObject(g_gl_ready_event, 30000);
}

bool libretro_gl_is_ready(void)
{
    return g_libretro_gl_ready;
}

/* Create an OpenGL core profile context, shared with RetroArch or standalone */
GloContext *glo_context_create(void)
{
    GloContext *context = (GloContext *)calloc(1, sizeof(GloContext));
    assert(context != NULL);

    libretro_gl_wait_ready();

    if (!g_wndclass_registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "XemuGloOffscreen";
        RegisterClassA(&wc);
        g_wndclass_registered = true;
    }

    context->hwnd = CreateWindowA("XemuGloOffscreen", "Offscreen",
                                   0, 0, 0, 1, 1,
                                   NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!context->hwnd) {
        free(context);
        return NULL;
    }

    context->hdc = GetDC(context->hwnd);

    if (g_standalone_gl_mode) {
        PIXELFORMATDESCRIPTOR pfd = {0};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        int pf = ChoosePixelFormat(context->hdc, &pfd);
        SetPixelFormat(context->hdc, pf, &pfd);

        if (!p_wglCreateContextAttribsARB) {
            HGLRC tmp = wglCreateContext(context->hdc);
            wglMakeCurrent(context->hdc, tmp);
            p_wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)
                wglGetProcAddress("wglCreateContextAttribsARB");
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(tmp);
        }

        if (!p_wglCreateContextAttribsARB) {
            ReleaseDC(context->hwnd, context->hdc);
            DestroyWindow(context->hwnd);
            free(context);
            return NULL;
        }

        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
            WGL_CONTEXT_MINOR_VERSION_ARB, 0,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };

        HGLRC share = g_standalone_root_ctx ? g_standalone_root_ctx->hglrc : NULL;
        context->hglrc = p_wglCreateContextAttribsARB(context->hdc, share, attribs);
        if (!context->hglrc) {
            ReleaseDC(context->hwnd, context->hdc);
            DestroyWindow(context->hwnd);
            free(context);
            return NULL;
        }

        if (!g_standalone_root_ctx) {
            g_standalone_root_ctx = context;
        }

        return context;
    }

    if (!g_retroarch_hglrc) {
        return context;
    }

    if (!p_wglCreateContextAttribsARB) {
        free(context);
        return NULL;
    }

    if (g_ra_pixel_format > 0) {
        PIXELFORMATDESCRIPTOR pfd = {0};
        pfd.nSize = sizeof(pfd);
        DescribePixelFormat(g_retroarch_hdc, g_ra_pixel_format, sizeof(pfd), &pfd);
        SetPixelFormat(context->hdc, g_ra_pixel_format, &pfd);
    } else {
        PIXELFORMATDESCRIPTOR pfd = {0};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        int pf = ChoosePixelFormat(context->hdc, &pfd);
        SetPixelFormat(context->hdc, pf, &pfd);
    }

    int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 0,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    context->hglrc = p_wglCreateContextAttribsARB(context->hdc, g_retroarch_hglrc, attribs);
    if (!context->hglrc) {
        ReleaseDC(context->hwnd, context->hdc);
        DestroyWindow(context->hwnd);
        free(context);
        return NULL;
    }

    return context;
}

void glo_set_current(GloContext *context)
{
    if (context == NULL || context->hglrc == NULL) {
        wglMakeCurrent(NULL, NULL);
    } else {
        wglMakeCurrent(context->hdc, context->hglrc);
    }
}

void glo_context_destroy(GloContext *context)
{
    if (!context) return;
    if (context->hglrc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(context->hglrc);
    }
    if (context->hwnd) {
        ReleaseDC(context->hwnd, context->hdc);
        DestroyWindow(context->hwnd);
    }
    free(context);
}

#endif /* _WIN32 */
#endif /* LIBRETRO */
