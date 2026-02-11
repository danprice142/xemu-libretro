/*
 * xemu libretro core wrapper
 *
 * Copyright (c) 2025
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/cpus.h"
#include "migration/snapshot.h"
#include "ui/console.h"
#include "hw/xbox/nv2a/nv2a.h"

#include <epoxy/gl.h>

#include "libretro.h"
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "libretro_vulkan.h"
#include "libretro_core_options.h"
#include "xemu-settings.h"
#include "xemu-version.h"

/* ========================================================================= */
/* Libretro callback storage                                                 */
/* ========================================================================= */

static retro_environment_t        environ_cb   = NULL;
static retro_video_refresh_t      video_cb     = NULL;
static retro_audio_sample_t       audio_cb     = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_input_poll_t         input_poll_cb  = NULL;
retro_input_state_t               input_state_cb = NULL; /* non-static: used by libretro-stubs.c */
static retro_log_printf_t         log_cb       = NULL;

/* ========================================================================= */
/* Libretro log wrapper - use this instead of fprintf(stderr, ...)           */
/* ========================================================================= */

#define LRLOG_INFO(...)  do { if (log_cb) log_cb(RETRO_LOG_INFO,  __VA_ARGS__); } while(0)
#define LRLOG_WARN(...)  do { if (log_cb) log_cb(RETRO_LOG_WARN,  __VA_ARGS__); } while(0)
#define LRLOG_ERROR(...) do { if (log_cb) log_cb(RETRO_LOG_ERROR, __VA_ARGS__); } while(0)
#define LRLOG_DEBUG(...) do { if (log_cb) log_cb(RETRO_LOG_DEBUG, __VA_ARGS__); } while(0)

/* ========================================================================= */
/* VFS interface (optional)                                                  */
/* ========================================================================= */

static struct retro_vfs_interface *vfs_interface = NULL;

/* ========================================================================= */
/* Hardware rendering state                                                  */
/* ========================================================================= */

static struct retro_hw_render_callback hw_render;
static bool use_vulkan = false;
static bool context_ready = false;
static bool game_loaded = false;

/* GL blit shader state */
static GLuint blit_program = 0;
static GLuint blit_vao = 0;
static GLint  blit_tex_loc = -1;

/* Vulkan HW render interface from RetroArch */
static struct retro_hw_render_interface_vulkan *vulkan_if = NULL;

/* RA-side VK resources: xemu's display image imported into RA's VkDevice */
static VkImage ra_vk_image = VK_NULL_HANDLE;
static VkImageView ra_vk_image_view = VK_NULL_HANDLE;
static VkDeviceMemory ra_vk_memory = VK_NULL_HANDLE;
static uint32_t ra_vk_width = 0, ra_vk_height = 0;
static void *ra_last_handle = NULL;
static struct retro_vulkan_image retro_vk_image;
static VkImageViewCreateInfo ra_vk_view_ci;

/* VK function pointers from RA's device (not xemu's) */
static PFN_vkCreateImage ra_vkCreateImage;
static PFN_vkDestroyImage ra_vkDestroyImage;
static PFN_vkCreateImageView ra_vkCreateImageView;
static PFN_vkDestroyImageView ra_vkDestroyImageView;
static PFN_vkAllocateMemory ra_vkAllocateMemory;
static PFN_vkFreeMemory ra_vkFreeMemory;
static PFN_vkBindImageMemory ra_vkBindImageMemory;
static PFN_vkGetImageMemoryRequirements ra_vkGetImageMemoryRequirements;
static PFN_vkGetPhysicalDeviceMemoryProperties ra_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkCreateCommandPool ra_vkCreateCommandPool;
static PFN_vkDestroyCommandPool ra_vkDestroyCommandPool;
static PFN_vkAllocateCommandBuffers ra_vkAllocateCommandBuffers;
static PFN_vkFreeCommandBuffers ra_vkFreeCommandBuffers;
static PFN_vkBeginCommandBuffer ra_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer ra_vkEndCommandBuffer;
static PFN_vkQueueSubmit ra_vkQueueSubmit;
static PFN_vkQueueWaitIdle ra_vkQueueWaitIdle;
static PFN_vkCmdPipelineBarrier ra_vkCmdPipelineBarrier;
static PFN_vkCreateFence ra_vkCreateFence;
static PFN_vkDestroyFence ra_vkDestroyFence;
static PFN_vkWaitForFences ra_vkWaitForFences;
static PFN_vkResetFences ra_vkResetFences;
static PFN_vkResetCommandBuffer ra_vkResetCommandBuffer;
static VkCommandPool ra_vk_cmd_pool = VK_NULL_HANDLE;
static VkCommandBuffer ra_vk_cmds[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
static VkFence ra_vk_fences[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
static uint32_t ra_vk_frame_idx = 0;
static bool ra_vk_funcs_resolved = false;

/* Standalone GL for VK renderer's internal GL interop (PFIFO thread) */
typedef struct _GloContext GloContext;
extern GloContext *glo_context_create(void);
extern void glo_set_current(GloContext *context);

/* ========================================================================= */
/* Emulation state                                                           */
/* ========================================================================= */

static bool emu_initialized = false;
static QemuThread emu_thread;
static volatile bool emu_thread_running = false;

/* Snapshot dispatch: RetroArch thread requests, QEMU thread executes */
enum {
    SNAPSHOT_NONE = 0,
    SNAPSHOT_SAVE,
    SNAPSHOT_LOAD,
};
static volatile int snapshot_request = SNAPSHOT_NONE;
static volatile bool snapshot_done = false;
static volatile bool snapshot_result = false;
static HANDLE snapshot_request_event = NULL;  /* signal emu thread */
static HANDLE snapshot_done_event = NULL;     /* signal RA thread */

/* ========================================================================= */
/* Core option values                                                        */
/* ========================================================================= */

static char opt_bootrom_path[4096] = "";
static char opt_bios_path[4096] = "";
static char opt_hdd_path[4096] = "";
static char opt_eeprom_path[4096] = "";
static char opt_dvd_path[4096] = "";
static char system_dir[4096] = "";
static int  opt_memory_mb = 64;
static bool opt_skip_boot_anim = false;
static bool opt_hard_fpu = true;
static bool opt_use_dsp = false;
static int  opt_surface_scale = 1;
static int  opt_avpack = CONFIG_SYS_AVPACK_HDTV;
static bool opt_cache_shaders = true;
static int  opt_filtering = CONFIG_DISPLAY_FILTERING_LINEAR;
static int  opt_audio_volume = 100;
static int  opt_network_backend = 0; /* 0=disabled, 1=nat */

/* ========================================================================= */
/* Forward declarations                                                      */
/* ========================================================================= */

static void context_reset(void);
static void context_destroy(void);
static void update_variables(void);
static void create_blit_resources(void);
static void destroy_blit_resources(void);
static void blit_nv2a_texture(GLuint tex, unsigned width, unsigned height, uintptr_t fbo);

/* ========================================================================= */
/* GL Blit Shader (fullscreen triangle, Y-flip for libretro top-left origin) */
/* ========================================================================= */

static const char *blit_vs_src =
    "#version 330 core\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "    float x = -1.0 + float((gl_VertexID & 1) << 2);\n"
    "    float y = -1.0 + float((gl_VertexID & 2) << 1);\n"
    "    gl_Position = vec4(x, y, 0.0, 1.0);\n"
    "    vTexCoord = vec2((x + 1.0) * 0.5, (y + 1.0) * 0.5);\n"
    "}\n";

static const char *blit_fs_src =
    "#version 330 core\n"
    "in vec2 vTexCoord;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = texture(uTex, vTexCoord);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char info[512];
        glGetShaderInfoLog(shader, sizeof(info), NULL, info);
        LRLOG_ERROR("[xemu] Shader compile error: %s\n", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static void create_blit_resources(void)
{
    LRLOG_INFO("[xemu] Creating GL blit resources\n");

    GLuint vs = compile_shader(GL_VERTEX_SHADER, blit_vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, blit_fs_src);
    if (!vs || !fs) {
        LRLOG_ERROR("[xemu] Failed to compile blit shaders\n");
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }

    blit_program = glCreateProgram();
    glAttachShader(blit_program, vs);
    glAttachShader(blit_program, fs);
    glLinkProgram(blit_program);

    GLint status = 0;
    glGetProgramiv(blit_program, GL_LINK_STATUS, &status);
    if (!status) {
        char info[512];
        glGetProgramInfoLog(blit_program, sizeof(info), NULL, info);
        LRLOG_ERROR("[xemu] Shader link error: %s\n", info);
        glDeleteProgram(blit_program);
        blit_program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    blit_tex_loc = glGetUniformLocation(blit_program, "uTex");

    glGenVertexArrays(1, &blit_vao);

    LRLOG_INFO("[xemu] GL blit resources created (program=%u, vao=%u)\n",
               blit_program, blit_vao);
}

static void destroy_blit_resources(void)
{
    if (blit_program) {
        glDeleteProgram(blit_program);
        blit_program = 0;
    }
    if (blit_vao) {
        glDeleteVertexArrays(1, &blit_vao);
        blit_vao = 0;
    }
}

static void blit_nv2a_texture(GLuint tex, unsigned width, unsigned height, uintptr_t fbo)
{
    if (!blit_program || !blit_vao || !tex) return;

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    glViewport(0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(blit_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(blit_tex_loc, 0);

    glBindVertexArray(blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* Clean up state per libretro docs */
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* ========================================================================= */
/* Vulkan HW render helpers                                                  */
/* ========================================================================= */

static void ra_vk_resolve_functions(void)
{
    if (!vulkan_if || ra_vk_funcs_resolved) return;

    PFN_vkGetDeviceProcAddr gdpa = vulkan_if->get_device_proc_addr;
    PFN_vkGetInstanceProcAddr gipa = vulkan_if->get_instance_proc_addr;
    VkDevice dev = vulkan_if->device;
    VkInstance inst = vulkan_if->instance;

    ra_vkCreateImage = (PFN_vkCreateImage)gdpa(dev, "vkCreateImage");
    ra_vkDestroyImage = (PFN_vkDestroyImage)gdpa(dev, "vkDestroyImage");
    ra_vkCreateImageView = (PFN_vkCreateImageView)gdpa(dev, "vkCreateImageView");
    ra_vkDestroyImageView = (PFN_vkDestroyImageView)gdpa(dev, "vkDestroyImageView");
    ra_vkAllocateMemory = (PFN_vkAllocateMemory)gdpa(dev, "vkAllocateMemory");
    ra_vkFreeMemory = (PFN_vkFreeMemory)gdpa(dev, "vkFreeMemory");
    ra_vkBindImageMemory = (PFN_vkBindImageMemory)gdpa(dev, "vkBindImageMemory");
    ra_vkGetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)gdpa(dev, "vkGetImageMemoryRequirements");
    ra_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    ra_vkCreateCommandPool = (PFN_vkCreateCommandPool)gdpa(dev, "vkCreateCommandPool");
    ra_vkDestroyCommandPool = (PFN_vkDestroyCommandPool)gdpa(dev, "vkDestroyCommandPool");
    ra_vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)gdpa(dev, "vkAllocateCommandBuffers");
    ra_vkFreeCommandBuffers = (PFN_vkFreeCommandBuffers)gdpa(dev, "vkFreeCommandBuffers");
    ra_vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)gdpa(dev, "vkBeginCommandBuffer");
    ra_vkEndCommandBuffer = (PFN_vkEndCommandBuffer)gdpa(dev, "vkEndCommandBuffer");
    ra_vkQueueSubmit = (PFN_vkQueueSubmit)gdpa(dev, "vkQueueSubmit");
    ra_vkQueueWaitIdle = (PFN_vkQueueWaitIdle)gdpa(dev, "vkQueueWaitIdle");
    ra_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)gdpa(dev, "vkCmdPipelineBarrier");
    ra_vkCreateFence = (PFN_vkCreateFence)gdpa(dev, "vkCreateFence");
    ra_vkDestroyFence = (PFN_vkDestroyFence)gdpa(dev, "vkDestroyFence");
    ra_vkWaitForFences = (PFN_vkWaitForFences)gdpa(dev, "vkWaitForFences");
    ra_vkResetFences = (PFN_vkResetFences)gdpa(dev, "vkResetFences");
    ra_vkResetCommandBuffer = (PFN_vkResetCommandBuffer)gdpa(dev, "vkResetCommandBuffer");

    ra_vk_funcs_resolved = (ra_vkCreateImage && ra_vkDestroyImage &&
                            ra_vkCreateImageView && ra_vkDestroyImageView &&
                            ra_vkAllocateMemory && ra_vkFreeMemory &&
                            ra_vkBindImageMemory && ra_vkGetImageMemoryRequirements &&
                            ra_vkGetPhysicalDeviceMemoryProperties &&
                            ra_vkCreateCommandPool && ra_vkAllocateCommandBuffers &&
                            ra_vkBeginCommandBuffer && ra_vkEndCommandBuffer &&
                            ra_vkQueueSubmit && ra_vkCmdPipelineBarrier &&
                            ra_vkCreateFence && ra_vkWaitForFences &&
                            ra_vkResetFences && ra_vkResetCommandBuffer);

    if (ra_vk_funcs_resolved && ra_vk_cmd_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pool_ci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = vulkan_if->queue_index,
        };
        ra_vkCreateCommandPool(dev, &pool_ci, NULL, &ra_vk_cmd_pool);

        VkCommandBufferAllocateInfo cmd_ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = ra_vk_cmd_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 2,
        };
        ra_vkAllocateCommandBuffers(dev, &cmd_ai, ra_vk_cmds);

        VkFenceCreateInfo fence_ci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        ra_vkCreateFence(dev, &fence_ci, NULL, &ra_vk_fences[0]);
        ra_vkCreateFence(dev, &fence_ci, NULL, &ra_vk_fences[1]);
    }
}

static uint32_t ra_vk_find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    ra_vkGetPhysicalDeviceMemoryProperties(vulkan_if->gpu, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

static void ra_vk_transition_layout(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout)
{
    if (!vulkan_if || !ra_vk_funcs_resolved) return;

    uint32_t idx = ra_vk_frame_idx;
    VkCommandBuffer cmd = ra_vk_cmds[idx];
    VkFence fence = ra_vk_fences[idx];
    if (!cmd || !fence) return;

    VkDevice dev = vulkan_if->device;

    /* Wait for THIS slot's fence (from 2 frames ago — should be instant) */
    ra_vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    ra_vkResetFences(dev, 1, &fence);
    ra_vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    ra_vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };

    ra_vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    ra_vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };

    vulkan_if->lock_queue(vulkan_if->handle);
    ra_vkQueueSubmit(vulkan_if->queue, 1, &submit, fence);
    vulkan_if->unlock_queue(vulkan_if->handle);

    /* Flip to other slot for next frame */
    ra_vk_frame_idx = 1 - idx;
}

static void ra_vk_cleanup_display(void)
{
    if (!vulkan_if) return;
    VkDevice dev = vulkan_if->device;

    if (ra_vk_image_view != VK_NULL_HANDLE) {
        ra_vkDestroyImageView(dev, ra_vk_image_view, NULL);
        ra_vk_image_view = VK_NULL_HANDLE;
    }
    if (ra_vk_image != VK_NULL_HANDLE) {
        ra_vkDestroyImage(dev, ra_vk_image, NULL);
        ra_vk_image = VK_NULL_HANDLE;
    }
    if (ra_vk_memory != VK_NULL_HANDLE) {
        ra_vkFreeMemory(dev, ra_vk_memory, NULL);
        ra_vk_memory = VK_NULL_HANDLE;
    }
    ra_vk_width = 0;
    ra_vk_height = 0;
    ra_last_handle = NULL;
}

static bool ra_vk_import_display(void *ext_handle, int width, int height)
{
    if (!vulkan_if || !ra_vk_funcs_resolved || !ext_handle || !width || !height)
        return false;

    VkDevice dev = vulkan_if->device;

    /* Cleanup old resources if size changed */
    if (ra_vk_image != VK_NULL_HANDLE &&
        ((uint32_t)width != ra_vk_width || (uint32_t)height != ra_vk_height)) {
        ra_vk_cleanup_display();
    }

    /* Already imported this handle at this size */
    if (ra_vk_image != VK_NULL_HANDLE && ext_handle == ra_last_handle)
        return true;

    ra_vk_cleanup_display();

    fprintf(stderr, "[xemu] ra_vk_import_display: importing handle=%p %dx%d into RA device=%p\n",
            ext_handle, width, height, (void*)dev);
    fflush(stderr);

    /* Create VkImage on RA's device with external memory import */
    VkExternalMemoryImageCreateInfo ext_img_ci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
    };

    VkImageCreateInfo img_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_img_ci,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult res = ra_vkCreateImage(dev, &img_ci, NULL, &ra_vk_image);
    if (res != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_reqs;
    ra_vkGetImageMemoryRequirements(dev, ra_vk_image, &mem_reqs);

    /* Import the external memory handle */
    VkImportMemoryWin32HandleInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
        .handle = (HANDLE)ext_handle,
    };

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = ra_vk_find_memory_type(mem_reqs.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    res = ra_vkAllocateMemory(dev, &alloc_info, NULL, &ra_vk_memory);
    if (res != VK_SUCCESS) {
        ra_vkDestroyImage(dev, ra_vk_image, NULL);
        ra_vk_image = VK_NULL_HANDLE;
        return false;
    }

    res = ra_vkBindImageMemory(dev, ra_vk_image, ra_vk_memory, 0);
    if (res != VK_SUCCESS) {
        ra_vk_cleanup_display();
        return false;
    }

    /* Create VkImageView */
    ra_vk_view_ci = (VkImageViewCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = ra_vk_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    res = ra_vkCreateImageView(dev, &ra_vk_view_ci, NULL, &ra_vk_image_view);
    if (res != VK_SUCCESS) {
        ra_vk_cleanup_display();
        return false;
    }

    ra_vk_width = width;
    ra_vk_height = height;
    ra_last_handle = ext_handle;

    return true;
}

/* ========================================================================= */
/* Context callbacks                                                         */
/* ========================================================================= */

static void context_reset(void)
{
    LRLOG_INFO("[xemu] context_reset called\n");

    if (!use_vulkan) {
        LRLOG_INFO("[xemu] OpenGL context ready, creating blit resources\n");
        create_blit_resources();

        /*
         * Two-phase GL initialization:
         * 1) Prepare: capture RA context, set g_libretro_gl_ready flag
         *    (allows glo_context_create to work, but PFIFO thread stays asleep)
         * 2) nv2a_context_init: creates g_nv2a_context_render/display
         * 3) Wake PFIFO: set g_gl_ready_event so PFIFO thread's glo_context_create
         *    returns and pgraph_gl_init can safely use g_nv2a_context_render
         */
        extern void libretro_gl_prepare(void);
        extern void libretro_gl_wake_pfifo(void);

        libretro_gl_prepare();
        LRLOG_INFO("[xemu] GL prepared, calling nv2a_context_init()\n");

        nv2a_context_init();
        LRLOG_INFO("[xemu] nv2a_context_init completed, waking PFIFO thread\n");

        libretro_gl_wake_pfifo();
        LRLOG_INFO("[xemu] PFIFO thread woken\n");
    } else {
        LRLOG_INFO("[xemu] Vulkan context reset\n");
        /* Retrieve the Vulkan HW render interface */
        if (environ_cb) {
            struct retro_hw_render_interface *iface = NULL;
            if (environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) && iface &&
                iface->interface_type == RETRO_HW_RENDER_INTERFACE_VULKAN) {
                vulkan_if = (struct retro_hw_render_interface_vulkan *)iface;
                LRLOG_INFO("[xemu] Got Vulkan HW render interface (version %u, device=%p)\n",
                           vulkan_if->interface_version, (void*)vulkan_if->device);
                ra_vk_resolve_functions();
            } else {
                LRLOG_ERROR("[xemu] Failed to get Vulkan HW render interface\n");
                vulkan_if = NULL;
            }
        }

        /*
         * Enable standalone GL mode for the VK renderer's internal GL interop.
         * The VK renderer uses GL external memory on the PFIFO thread to create
         * a shared VK/GL display image. We don't need GL on the RA thread.
         */
        extern void libretro_gl_set_standalone_mode(void);
        extern void libretro_gl_wake_pfifo(void);

        libretro_gl_set_standalone_mode();
        LRLOG_INFO("[xemu] Standalone GL mode enabled for VK renderer\n");

        nv2a_context_init();
        LRLOG_INFO("[xemu] nv2a_context_init completed (VK mode)\n");

        libretro_gl_wake_pfifo();
        LRLOG_INFO("[xemu] PFIFO thread woken (VK mode)\n");
    }

    context_ready = true;
}

static void context_destroy(void)
{
    LRLOG_INFO("[xemu] context_destroy called\n");
    context_ready = false;

    if (!use_vulkan) {
        destroy_blit_resources();
    }

    if (use_vulkan && vulkan_if) {
        ra_vk_cleanup_display();
        for (int i = 0; i < 2; i++) {
            if (ra_vk_fences[i] != VK_NULL_HANDLE && ra_vkDestroyFence) {
                ra_vkWaitForFences(vulkan_if->device, 1, &ra_vk_fences[i], VK_TRUE, UINT64_MAX);
                ra_vkDestroyFence(vulkan_if->device, ra_vk_fences[i], NULL);
                ra_vk_fences[i] = VK_NULL_HANDLE;
            }
            ra_vk_cmds[i] = VK_NULL_HANDLE;
        }
        ra_vk_frame_idx = 0;
        if (ra_vk_cmd_pool != VK_NULL_HANDLE && ra_vkDestroyCommandPool) {
            ra_vkDestroyCommandPool(vulkan_if->device, ra_vk_cmd_pool, NULL);
            ra_vk_cmd_pool = VK_NULL_HANDLE;
        }
        ra_vk_funcs_resolved = false;
    }
    vulkan_if = NULL;
}

/* ========================================================================= */
/* Core option helpers                                                       */
/* ========================================================================= */

static void get_option_string(const char *key, char *buf, size_t buf_sz)
{
    struct retro_variable var = { key, NULL };
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        snprintf(buf, buf_sz, "%s", var.value);
    }
}

static void update_variables(void)
{
    struct retro_variable var;

    get_option_string("xemu_bootrom_path", opt_bootrom_path, sizeof(opt_bootrom_path));
    get_option_string("xemu_bios_path", opt_bios_path, sizeof(opt_bios_path));
    get_option_string("xemu_hdd_path", opt_hdd_path, sizeof(opt_hdd_path));
    get_option_string("xemu_eeprom_path", opt_eeprom_path, sizeof(opt_eeprom_path));

    var.key = "xemu_memory";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_memory_mb = atoi(var.value);
    }

    var.key = "xemu_skip_boot_anim";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_skip_boot_anim = !strcmp(var.value, "enabled");
    }

    var.key = "xemu_network_backend";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "nat")) opt_network_backend = 1;
        else                           opt_network_backend = 0;
    }

    var.key = "xemu_hard_fpu";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_hard_fpu = !strcmp(var.value, "enabled");
    }

    var.key = "xemu_surface_scale";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_surface_scale = atoi(var.value);
        if (opt_surface_scale < 1) opt_surface_scale = 1;
        if (opt_surface_scale > 10) opt_surface_scale = 10;
    }

    var.key = "xemu_use_dsp";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_use_dsp = !strcmp(var.value, "enabled");
    }

    var.key = "xemu_avpack";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "hdtv"))           opt_avpack = CONFIG_SYS_AVPACK_HDTV;
        else if (!strcmp(var.value, "composite")) opt_avpack = CONFIG_SYS_AVPACK_COMPOSITE;
        else if (!strcmp(var.value, "svideo"))    opt_avpack = CONFIG_SYS_AVPACK_SVIDEO;
        else if (!strcmp(var.value, "scart"))     opt_avpack = CONFIG_SYS_AVPACK_SCART;
        else if (!strcmp(var.value, "vga"))       opt_avpack = CONFIG_SYS_AVPACK_VGA;
    }

    var.key = "xemu_cache_shaders";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_cache_shaders = !strcmp(var.value, "enabled");
    }

    var.key = "xemu_display_filtering";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "nearest")) opt_filtering = CONFIG_DISPLAY_FILTERING_NEAREST;
        else                              opt_filtering = CONFIG_DISPLAY_FILTERING_LINEAR;
    }

    var.key = "xemu_audio_volume";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_audio_volume = atoi(var.value);
        if (opt_audio_volume < 0) opt_audio_volume = 0;
        if (opt_audio_volume > 100) opt_audio_volume = 100;
    }

    LRLOG_INFO("[xemu] Options: memory=%dMB, scale=%d, avpack=%d, cache=%d, filter=%d, volume=%d%%, network=%s\n",
               opt_memory_mb, opt_surface_scale,
               opt_avpack, opt_cache_shaders, opt_filtering, opt_audio_volume,
               opt_network_backend == 1 ? "nat" : "disabled");

    /* Apply runtime-safe options to g_config immediately.
     * Some options (memory, renderer, file paths) only take effect at boot,
     * but these can be applied live: */
    if (emu_initialized) {
        g_config.audio.volume_limit = opt_audio_volume / 100.0f;
        g_config.display.quality.surface_scale = opt_surface_scale;
        g_config.display.filtering = opt_filtering;
        g_config.perf.cache_shaders = opt_cache_shaders;
        g_config.audio.use_dsp = opt_use_dsp;
    }
}

/* ========================================================================= */
/* QEMU init helpers                                                         */
/* ========================================================================= */

static void populate_config(const char *dvd_path)
{
    LRLOG_INFO("[xemu] Populating g_config from core options\n");

    /* Set file paths in g_config */
    if (opt_bootrom_path[0]) {
        xemu_settings_set_string(&g_config.sys.files.bootrom_path, opt_bootrom_path);
    }
    if (opt_bios_path[0]) {
        xemu_settings_set_string(&g_config.sys.files.flashrom_path, opt_bios_path);
    }
    if (opt_hdd_path[0]) {
        xemu_settings_set_string(&g_config.sys.files.hdd_path, opt_hdd_path);
    }
    if (opt_eeprom_path[0]) {
        xemu_settings_set_string(&g_config.sys.files.eeprom_path, opt_eeprom_path);
    }
    if (dvd_path && dvd_path[0]) {
        xemu_settings_set_string(&g_config.sys.files.dvd_path, dvd_path);
    }

    /* Set renderer */
    if (use_vulkan) {
        g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
    } else {
        g_config.display.renderer = CONFIG_DISPLAY_RENDERER_OPENGL;
    }

    /* Set memory (mem_limit is an int: 0=64MB, 1=128MB) */
    g_config.sys.mem_limit = (opt_memory_mb == 128) ? 1 : 0;

    /* Critical defaults for libretro mode */
    g_config.general.show_welcome = false; /* must be false or autostart=0 */
    g_config.audio.volume_limit = opt_audio_volume / 100.0f;    /* Apply user volume setting */
    g_config.perf.cache_shaders = opt_cache_shaders;

    /* Set other options */
    g_config.general.skip_boot_anim = opt_skip_boot_anim;
    g_config.perf.hard_fpu = opt_hard_fpu;
    g_config.audio.use_dsp = opt_use_dsp;
    g_config.display.quality.surface_scale = opt_surface_scale;
    g_config.sys.avpack = opt_avpack;
    g_config.display.filtering = opt_filtering;

    /* Set network configuration */
    g_config.net.enable = (opt_network_backend == 1);
    if (opt_network_backend == 1) {
        g_config.net.backend = CONFIG_NET_BACKEND_NAT;
    }

    LRLOG_INFO("[xemu] Config populated: bootrom=%s, bios=%s, hdd=%s, dvd=%s\n",
               g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "(null)",
               g_config.sys.files.flashrom_path ? g_config.sys.files.flashrom_path : "(null)",
               g_config.sys.files.hdd_path ? g_config.sys.files.hdd_path : "(null)",
               g_config.sys.files.dvd_path ? g_config.sys.files.dvd_path : "(null)");
}

/*
 * Vblank timer callback — generates NV2A PCRTC vblank interrupts.
 * In normal xemu, the SDL display loop calls graphic_hw_update at 60Hz.
 * With -display none (libretro), no display listener exists, so we must
 * manually trigger vblank at ~60Hz using a QEMU timer.
 */
static QEMUTimer *libretro_vblank_timer;
static const int64_t VBLANK_INTERVAL_NS = 16666666LL; /* ~60 Hz */

static void libretro_vblank_cb(void *opaque)
{
    QemuConsole *con = (QemuConsole *)opaque;
    graphic_hw_update(con);
    timer_mod_ns(libretro_vblank_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + VBLANK_INTERVAL_NS);
}

/* Thread function for running QEMU main loop */
static void *emu_thread_func(void *opaque)
{
    /* Build QEMU argv */
    const char *argv[] = {
        "xemu",
        "-device", "lpc47m157",
        "-serial", "null",
        NULL,
    };
    int argc = 5;

    qemu_init(argc, (char **)argv);

    /* Create XID USB gamepad devices for all 4 ports (BQL is held) */
    extern void libretro_input_create_xid_devices(void);
    libretro_input_create_xid_devices();

    emu_initialized = true;

    /*
     * CRITICAL: qemu_init() acquires the BQL and replay mutex.
     * Since we stay on the same thread, qemu_init left BQL locked.
     * main_loop_wait(false) will unlock BQL, poll, then relock.
     */

    extern bool runstate_is_running(void);

    /*
     * Start vblank timer at 60Hz.
     * With -display none, no display listener calls gfx_update, so
     * NV2A PCRTC vblank interrupts never fire. The Xbox kernel spins
     * waiting for vblank IRQ. This timer generates them.
     */
    {
        QemuConsole *con = nv2a_get_vga_console();
        if (con) {
            libretro_vblank_timer = timer_new_ns(QEMU_CLOCK_REALTIME,
                                                  libretro_vblank_cb, con);
            timer_mod_ns(libretro_vblank_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + VBLANK_INTERVAL_NS);
        }
    }

    /* Run main loop - same as qemu_main_loop() */
    while (emu_thread_running) {
        /* Check for reset requests (like main_loop_should_exit does) */
        ShutdownCause reset_request = qemu_reset_requested_consume();
        if (reset_request) {
            pause_all_vcpus();
            qemu_system_reset(reset_request);
            resume_all_vcpus();
            /*
             * runstate can change in pause_all_vcpus()
             * as iothread mutex is unlocked
             */
            if (!runstate_check(RUN_STATE_RUNNING) &&
                    !runstate_check(RUN_STATE_INMIGRATE) &&
                    !runstate_check(RUN_STATE_FINISH_MIGRATE)) {
                runstate_set(RUN_STATE_PRELAUNCH);
            }
        }
        
        /* Check for shutdown requests */
        if (qemu_shutdown_requested_get()) {
            emu_thread_running = false;
            break;
        }
        
        main_loop_wait(false);

        /* Check for snapshot requests from RetroArch thread */
        if (snapshot_request != SNAPSHOT_NONE) {
            int req = snapshot_request;
            snapshot_request = SNAPSHOT_NONE;
            bool ok = false;
            Error *snap_err = NULL;

            if (req == SNAPSHOT_SAVE) {
                ok = save_snapshot("libretro_save", true, NULL, false, NULL, &snap_err);
                if (!ok && snap_err) {
                    error_free(snap_err);
                }
            } else if (req == SNAPSHOT_LOAD) {
                bool was_running = runstate_is_running();
                vm_stop(RUN_STATE_RESTORE_VM);
                ok = load_snapshot("libretro_save", NULL, false, NULL, &snap_err);
                if (ok && was_running) {
                    vm_start();
                } else if (!ok) {
                    if (snap_err) {
                        error_free(snap_err);
                    }
                    if (was_running) vm_start();
                }
            }

            snapshot_result = ok;
            snapshot_done = true;
            if (snapshot_done_event) SetEvent(snapshot_done_event);
        }
    }

    /* Clean up vblank timer */
    if (libretro_vblank_timer) {
        timer_del(libretro_vblank_timer);
        timer_free(libretro_vblank_timer);
        libretro_vblank_timer = NULL;
    }

    bql_unlock();
    return NULL;
}

/* ========================================================================= */
/* Libretro API implementation                                               */
/* ========================================================================= */

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    /* Set core options v2 */
    libretro_set_core_options(environ_cb);

    /* Get log interface */
    struct retro_log_callback log;
    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
        log_cb = log.log;
    }
    LRLOG_INFO("[xemu] retro_set_environment called\n");

    /* Get system directory for BIOS files */
    const char *sys_dir = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir) {
        snprintf(system_dir, sizeof(system_dir), "%s", sys_dir);
        LRLOG_INFO("[xemu] System directory: %s\n", system_dir);
    } else {
        LRLOG_WARN("[xemu] Could not get system directory\n");
    }

    /* We need content (game ISO) */
    bool no_game = false;
    environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

    /* Get VFS interface */
    struct retro_vfs_interface_info vfs_info = { 3, NULL };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info)) {
        vfs_interface = vfs_info.iface;
        LRLOG_INFO("[xemu] VFS interface obtained (version %u)\n", vfs_info.required_interface_version);
    }

    /* Set pixel format */
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    /* Set input descriptors — Xbox controller button labels */
    {
        struct retro_input_descriptor desc[] = {
#define PORT_DESCS(p) \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "X" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Y" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "White" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Black" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Left Trigger" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Right Trigger" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Left Stick" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Right Stick" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Back" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" }, \
            { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Left Stick X" }, \
            { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y" }, \
            { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" }, \
            { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" },
            PORT_DESCS(0)
            PORT_DESCS(1)
            PORT_DESCS(2)
            PORT_DESCS(3)
#undef PORT_DESCS
            { 0 },
        };
        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
    }
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)   { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)     { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)         { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)       { input_state_cb = cb; }

RETRO_API unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "xemu";
    info->library_version  = xemu_version;
    info->valid_extensions = "iso|xiso";
    info->need_fullpath    = true;
    info->block_extract    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = 640;
    info->geometry.base_height  = 480;
    info->geometry.max_width    = 1920;
    info->geometry.max_height   = 1080;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps            = 59.94;
    info->timing.sample_rate    = 48000.0;
}

RETRO_API void retro_init(void)
{
    LRLOG_INFO("[xemu] retro_init\n");

    /* Initialize Windows TLS keys for __thread replacements */
#if defined(LIBRETRO) && defined(_WIN32)
    {
        extern unsigned long tls_key_tcg_ctx;
        extern unsigned long tls_key_current_cpu;
        unsigned long __stdcall TlsAlloc(void);
        if (tls_key_tcg_ctx == 0xFFFFFFFF)
            tls_key_tcg_ctx = TlsAlloc();
        if (tls_key_current_cpu == 0xFFFFFFFF)
            tls_key_current_cpu = TlsAlloc();
        LRLOG_INFO("[xemu] TLS keys initialized: tcg_ctx=%lu current_cpu=%lu\n",
                   tls_key_tcg_ctx, tls_key_current_cpu);
    }
#endif

#ifdef _WIN32
    /* Set Windows timer resolution to 1ms for accurate APU throttle sleeps.
     * Default is ~15.6ms which causes bursty audio production. */
    {
        typedef unsigned int (__stdcall *timeBeginPeriod_t)(unsigned int);
        HMODULE winmm = LoadLibraryA("winmm.dll");
        if (winmm) {
            timeBeginPeriod_t fn = (timeBeginPeriod_t)GetProcAddress(winmm, "timeBeginPeriod");
            if (fn) {
                fn(1);
                LRLOG_INFO("[xemu] Set Windows timer resolution to 1ms\n");
            }
        }
    }
#endif

    /* Initialize the GL readiness wait event early */
    extern void libretro_gl_init_wait_event(void);
    libretro_gl_init_wait_event();
}

RETRO_API void retro_deinit(void)
{
    LRLOG_INFO("[xemu] retro_deinit\n");
    game_loaded = false;
    emu_initialized = false;
    context_ready = false;
    use_vulkan = false;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    LRLOG_INFO("[xemu] retro_load_game called\n");

    if (!game || !game->path) {
        LRLOG_ERROR("[xemu] No game path provided\n");
        return false;
    }

    LRLOG_INFO("[xemu] Loading game: %s\n", game->path);

    /* Read core options */
    update_variables();

    /* Store the DVD path */
    snprintf(opt_dvd_path, sizeof(opt_dvd_path), "%s", game->path);

    /* Set shader cache base path from system directory (like Windows xemu uses %APPDATA%\xemu\xemu\) */
    if (system_dir[0]) {
        char base_path[4096];
        snprintf(base_path, sizeof(base_path), "%s/xemu/", system_dir);
        extern void libretro_settings_set_base_path(const char *path);
        libretro_settings_set_base_path(base_path);
        LRLOG_INFO("[xemu] Shader cache base path: %s\n", base_path);
    }

    /* Auto-populate BIOS paths from system directory if not set by options */
    if (system_dir[0]) {
        if (!opt_bootrom_path[0]) {
            snprintf(opt_bootrom_path, sizeof(opt_bootrom_path),
                     "%s/xemu/mcpx_1.0.bin", system_dir);
            LRLOG_INFO("[xemu] Auto-set bootrom: %s\n", opt_bootrom_path);
        }
        if (!opt_bios_path[0]) {
            snprintf(opt_bios_path, sizeof(opt_bios_path),
                     "%s/xemu/Complex_4627v1.03.bin", system_dir);
            LRLOG_INFO("[xemu] Auto-set bios: %s\n", opt_bios_path);
        }
        if (!opt_hdd_path[0]) {
            snprintf(opt_hdd_path, sizeof(opt_hdd_path),
                     "%s/xemu/xbox_hdd.qcow2", system_dir);
            LRLOG_INFO("[xemu] Auto-set hdd: %s\n", opt_hdd_path);
        }
        if (!opt_eeprom_path[0]) {
            snprintf(opt_eeprom_path, sizeof(opt_eeprom_path),
                     "%s/xemu/xbox_eeprom.bin", system_dir);
            LRLOG_INFO("[xemu] Auto-set eeprom: %s\n", opt_eeprom_path);
        }
    }

    /* Validate required files exist */
    {
        FILE *f;
        f = fopen(opt_bootrom_path, "rb");
        if (!f) {
            LRLOG_ERROR("[xemu] MCPX Boot ROM not found: %s\n", opt_bootrom_path);
            LRLOG_ERROR("[xemu] Place mcpx_1.0.bin in RetroArch system/xemu/ directory\n");
            return false;
        }
        fclose(f);
        LRLOG_INFO("[xemu] Found bootrom: %s\n", opt_bootrom_path);

        f = fopen(opt_bios_path, "rb");
        if (!f) {
            LRLOG_ERROR("[xemu] Xbox BIOS not found: %s\n", opt_bios_path);
            LRLOG_ERROR("[xemu] Place Complex_4627v1.03.bin in RetroArch system/xemu/ directory\n");
            return false;
        }
        fclose(f);
        LRLOG_INFO("[xemu] Found bios: %s\n", opt_bios_path);

        f = fopen(opt_hdd_path, "rb");
        if (!f) {
            LRLOG_ERROR("[xemu] Xbox HDD image not found: %s\n", opt_hdd_path);
            LRLOG_ERROR("[xemu] Place xbox_hdd.qcow2 in RetroArch system/xemu/ directory\n");
            return false;
        }
        fclose(f);
        LRLOG_INFO("[xemu] Found hdd: %s\n", opt_hdd_path);
    }

    /* Query the frontend's preferred HW render context.
     * This respects the user's video_driver setting in RetroArch. */
    unsigned preferred_hw = RETRO_HW_CONTEXT_OPENGL_CORE;
    if (environ_cb) {
        environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred_hw);
    }
    LRLOG_INFO("[xemu] Frontend preferred HW render: %u\n", preferred_hw);

    bool want_vulkan = (preferred_hw == RETRO_HW_CONTEXT_VULKAN);

    /* Setup hardware rendering based on frontend preference */
    memset(&hw_render, 0, sizeof(hw_render));
    hw_render.context_reset      = context_reset;
    hw_render.context_destroy    = context_destroy;
    hw_render.depth              = true;
    hw_render.stencil            = true;
    hw_render.bottom_left_origin = true;
    hw_render.cache_context      = true;

    if (want_vulkan) {
        LRLOG_INFO("[xemu] Requesting Vulkan HW context\n");
        hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
        hw_render.version_major = 1;
        hw_render.version_minor = 3;
    } else {
        LRLOG_INFO("[xemu] Requesting OpenGL Core 4.0 HW context\n");
        hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
        hw_render.version_major = 4;
        hw_render.version_minor = 0;
    }

    if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
        /* Preferred context failed - try the other one */
        LRLOG_INFO("[xemu] Preferred context failed, trying fallback\n");
        memset(&hw_render, 0, sizeof(hw_render));
        hw_render.context_reset      = context_reset;
        hw_render.context_destroy    = context_destroy;
        hw_render.depth              = true;
        hw_render.stencil            = true;
        hw_render.bottom_left_origin = true;
        hw_render.cache_context      = true;

        if (want_vulkan) {
            hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
            hw_render.version_major = 4;
            hw_render.version_minor = 0;
            want_vulkan = false;
        } else {
            hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
            hw_render.version_major = 1;
            hw_render.version_minor = 3;
            want_vulkan = true;
        }

        if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
            LRLOG_ERROR("[xemu] Failed to set HW render (neither GL nor VK available)\n");
            return false;
        }
    }

    use_vulkan = want_vulkan;
    if (use_vulkan) {
        LRLOG_INFO("[xemu] Using Vulkan HW rendering\n");
    } else {
        LRLOG_INFO("[xemu] Using OpenGL HW rendering\n");
        environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, NULL);
    }

    /* Populate xemu config from core options */
    populate_config(opt_dvd_path);

    /* Start emulation thread */
    emu_thread_running = true;
    qemu_thread_create(&emu_thread, "xemu-emu", emu_thread_func,
                       NULL, QEMU_THREAD_JOINABLE);

    game_loaded = true;

    LRLOG_INFO("[xemu] retro_load_game completed successfully\n");
    return true;
}

RETRO_API bool retro_load_game_special(unsigned type,
                                        const struct retro_game_info *info,
                                        size_t num_info)
{
    (void)type; (void)info; (void)num_info;
    return false;
}

RETRO_API void retro_unload_game(void)
{
    LRLOG_INFO("[xemu] retro_unload_game\n");

    if (emu_thread_running) {
        emu_thread_running = false;
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
        qemu_thread_join(&emu_thread);
        LRLOG_INFO("[xemu] Emulation thread joined\n");
    }

    game_loaded = false;
    emu_initialized = false;
}

RETRO_API void retro_run(void)
{
    static int run_count = 0;
    run_count++;
    if (run_count <= 5 || (run_count % 300) == 0) {
        LRLOG_INFO("[xemu] retro_run #%d (emu_init=%d ctx_ready=%d game=%d)\n",
                   run_count, emu_initialized, context_ready, game_loaded);
    }

    /* Check if variables have been updated */
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        update_variables();
    }

    /* Poll input */
    if (input_poll_cb) {
        input_poll_cb();
    }

    /* TODO: Map input_state_cb to Xbox gamepad input injection */
    /* For now, just read some buttons for debugging */
    if (input_state_cb) {
        /* Will be implemented: inject into XID USB device */
    }

    if (!emu_initialized || !context_ready) {
        /* Emulator not ready yet, draw black frame */
        if (!use_vulkan && hw_render.get_current_framebuffer) {
            uintptr_t fbo = hw_render.get_current_framebuffer();
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            video_cb(RETRO_HW_FRAME_BUFFER_VALID, 640, 480, 0);
        } else if (use_vulkan) {
            /* VK mode: pass NULL frame (no HW framebuffer concept) */
            video_cb(NULL, 640, 480, 0);
        } else {
            video_cb(RETRO_HW_FRAME_BUFFER_VALID, 640, 480, 0);
        }

        /* Push silence for audio */
        if (audio_batch_cb) {
            int16_t silence[1600] = {0}; /* ~800 samples stereo */
            audio_batch_cb(silence, 800);
        }
        return;
    }

    /* ---- OpenGL path ---- */
    if (!use_vulkan) {
        /* Get NV2A framebuffer texture */
        GLuint tex = nv2a_get_framebuffer_surface();
        uintptr_t fbo = hw_render.get_current_framebuffer();

        unsigned width = 640;
        unsigned height = 480;

        if (tex) {
            blit_nv2a_texture(tex, width, height, fbo);
        } else {
            /* No framebuffer yet, clear to dark blue so we can see something */
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
            glClearColor(0.0f, 0.0f, 0.2f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        nv2a_release_framebuffer_surface();
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
    }
    /* ---- Vulkan path (proper VK HW rendering via external memory import) ---- */
    else if (use_vulkan) {
        unsigned width = 640;
        unsigned height = 480;

        if (!vulkan_if) {
            video_cb(NULL, width, height, 0);
            goto vk_audio;
        }

        LARGE_INTEGER vk_t0, vk_t1, vk_t2, vk_t3, vk_freq;
        QueryPerformanceFrequency(&vk_freq);
        QueryPerformanceCounter(&vk_t0);

        static bool vk_display_initialized = false;

        if (!vk_display_initialized) {
            /* First frame: blocking sync to initialize display image */
            int tex = nv2a_get_framebuffer_surface();
            nv2a_release_framebuffer_surface();
            if (tex) {
                vk_display_initialized = true;
            }
        } else {
            /* Subsequent frames: trigger async render (non-blocking) */
            nv2a_trigger_display_render();
        }
        QueryPerformanceCounter(&vk_t1);

        /* Get VK display info (external memory handle + dimensions) */
        void *ext_handle = NULL;
        int disp_w = 0, disp_h = 0;
        nv2a_get_vk_display_info(&ext_handle, &disp_w, &disp_h);

        if (ext_handle && disp_w > 0 && disp_h > 0) {
            /* Import xemu's display image into RA's VkDevice */
            static bool vk_layout_set = false;
            if (ra_vk_import_display(ext_handle, disp_w, disp_h)) {
                if (!vk_layout_set) {
                    /* One-time transition to GENERAL — RA won't touch layout,
                     * concurrent reads from core+frontend allowed (libretro VK spec) */
                    ra_vk_transition_layout(ra_vk_image,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                    vk_layout_set = true;
                }
                /* Per-frame GENERAL→GENERAL barrier: flush cross-device memory
                 * so RA sees xemu's writes. No layout change = lightweight. */
                ra_vk_transition_layout(ra_vk_image,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                QueryPerformanceCounter(&vk_t2);

                /* Set the image for RetroArch to display */
                retro_vk_image.image_view = ra_vk_image_view;
                retro_vk_image.image_layout = VK_IMAGE_LAYOUT_GENERAL;
                retro_vk_image.create_info = ra_vk_view_ci;

                vulkan_if->set_image(vulkan_if->handle, &retro_vk_image,
                                     0, NULL, VK_QUEUE_FAMILY_IGNORED);

                width = disp_w;
                height = disp_h;

                video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
            } else {
                video_cb(NULL, width, height, 0);
            }
        } else {
            video_cb(NULL, width, height, 0);
        }
    }

vk_audio:
    /* Pull audio from the APU ring buffer (hw/xbox/mcpx/apu/monitor.c).
     * Cap at ~800 frames (48000Hz / 59.94fps) per call — what RetroArch expects.
     * Time-based APU throttle produces at 48kHz; ring buffer absorbs jitter. */
    if (audio_batch_cb) {
        extern int libretro_audio_pull(int16_t *out_buf, int max_frames);
        extern void libretro_audio_flush(void);

        /* One-time flush: discard stale boot audio on first active pull */
        static bool audio_flushed = false;
        if (!audio_flushed) {
            libretro_audio_flush();
            audio_flushed = true;
        }

        /* Pull available frames (48000Hz / 59.94fps ≈ 801 expected).
         * If buffer has way more than expected, skip ahead to stay near
         * real-time. Otherwise we'd always read stale audio. */
        extern int libretro_audio_ring_frames(void);
        int avail = libretro_audio_ring_frames();

        /* If buffer is overfull (>1600 frames), discard excess to stay current */
        if (avail > 1600) {
            int16_t discard_buf[1602];
            int skip = avail - 801; /* leave ~801 frames to pull */
            while (skip > 0) {
                int chunk = skip > 801 ? 801 : skip;
                libretro_audio_pull(discard_buf, chunk);
                skip -= chunk;
            }
            avail = libretro_audio_ring_frames();
        }

        int16_t audio_buf[1602]; /* 801 stereo frames */
        int frames = libretro_audio_pull(audio_buf, 801);

        if (frames > 0) {
            audio_batch_cb(audio_buf, frames);
        }
    }
}

RETRO_API void retro_reset(void)
{
    LRLOG_INFO("[xemu] retro_reset\n");
    if (emu_initialized) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

/* ========================================================================= */
/* Save states via QEMU snapshot system                                      */
/* ========================================================================= */

/*
 * QEMU snapshots are disk-based (stored inside the qcow2 HDD image).
 * We acquire the BQL from RetroArch's thread, call save_snapshot/load_snapshot,
 * then release it. The QEMU main loop periodically releases BQL in
 * main_loop_wait(), allowing us to acquire it here.
 *
 * The libretro buffer contains a small header; actual state lives in the qcow2.
 */

/*
 * Save state header stored in RetroArch's .state file.
 * Actual VM state lives in the qcow2 HDD image via QEMU snapshots.
 * This header lets RetroArch round-trip the state file correctly.
 */
#define LIBRETRO_SAVESTATE_SIZE  4096
#define LIBRETRO_SAVESTATE_MAGIC 0x58454D55 /* 'XEMU' */

struct libretro_savestate_header {
    uint32_t magic;
    uint32_t version;
    uint64_t timestamp;
};

/*
 * Helper: dispatch a snapshot request to the QEMU emu thread and wait.
 * The emu thread picks up the request in its main_loop_wait cycle.
 */
static bool snapshot_dispatch(int request_type, int timeout_ms)
{
    if (!snapshot_request_event) {
        snapshot_request_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    }
    if (!snapshot_done_event) {
        snapshot_done_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    }

    snapshot_done = false;
    snapshot_result = false;
    snapshot_request = request_type;

    /* Wait for the emu thread to process it */
    DWORD result = WaitForSingleObject(snapshot_done_event, timeout_ms);
    if (result == WAIT_TIMEOUT) {
        LRLOG_INFO("[xemu] snapshot dispatch: timeout after %dms\n", timeout_ms);
        snapshot_request = SNAPSHOT_NONE;
        return false;
    }

    return snapshot_result;
}

RETRO_API size_t retro_serialize_size(void)
{
    if (!emu_initialized) return 0;
    return LIBRETRO_SAVESTATE_SIZE;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
    if (!emu_initialized || !data || size < LIBRETRO_SAVESTATE_SIZE)
        return false;

    memset(data, 0, LIBRETRO_SAVESTATE_SIZE);

    bool ok = snapshot_dispatch(SNAPSHOT_SAVE, 30000);

    if (!ok) {
        LRLOG_INFO("[xemu] retro_serialize: save failed\n");
        return false;
    }

    struct libretro_savestate_header *hdr = (struct libretro_savestate_header *)data;
    hdr->magic = LIBRETRO_SAVESTATE_MAGIC;
    hdr->version = 1;
    hdr->timestamp = (uint64_t)time(NULL);

    LRLOG_INFO("[xemu] retro_serialize: snapshot saved to HDD image\n");
    return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
    if (!emu_initialized || !data || size < LIBRETRO_SAVESTATE_SIZE)
        return false;

    const struct libretro_savestate_header *hdr =
        (const struct libretro_savestate_header *)data;
    if (hdr->magic != LIBRETRO_SAVESTATE_MAGIC) {
        LRLOG_INFO("[xemu] retro_unserialize: invalid magic 0x%08x\n", hdr->magic);
        return false;
    }

    bool ok = snapshot_dispatch(SNAPSHOT_LOAD, 30000);

    if (!ok) {
        LRLOG_INFO("[xemu] retro_unserialize: load failed\n");
        return false;
    }

    LRLOG_INFO("[xemu] retro_unserialize: snapshot loaded from HDD image\n");
    return true;
}

/* ========================================================================= */
/* Memory stubs                                                              */
/* ========================================================================= */

RETRO_API void  *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

/* ========================================================================= */
/* Misc                                                                      */
/* ========================================================================= */

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

/* xemu has no cheat engine or memory patching support */
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index; (void)enabled; (void)code;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    LRLOG_INFO("[xemu] Port %u device %u\n", port, device);
}
