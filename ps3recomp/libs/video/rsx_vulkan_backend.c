/*
 * ps3recomp - Vulkan RSX Backend. See rsx_vulkan_backend.h.
 *
 * The file, in order:
 *   loader and device      libvulkan by dlopen, Vulkan 1.0 entry points only;
 *                          device choice, BC textures when the device has them
 *   memory, images, submit helpers
 *   vtable fallback path   the null backend's fixed-function contract:
 *                          position = attribute 0, flat colour = attribute 3
 *                          of each triangle's first vertex, texcoord0 =
 *                          attribute 8, fetched and expanded through the
 *                          shared rsx_fetch_attrib(); unit 0 decoded through
 *                          rsx_texture_decode(); depth test, no blend/stencil
 *   vtable guest programs  the same path running the decompilers' HLSL, with
 *                          whole mip chains and per-unit samplers
 *   optional window        SDL2 + swapchain; a present blits the frame into it
 *   draw engine backend    rsx_draw_backend: targets, clears, present and
 *                          readback (E1), pipelines and draws (E2), textures,
 *                          surface views and samplers (E3), depth snapshots
 *                          (E4) -- the stage names in the commit history
 *   public entry points    init picks the path, shutdown undoes everything
 *
 * Deliberately synchronous: every clear, draw and present is one one-shot
 * command buffer, submitted and waited on. Slow and trivially correct; it is
 * what lets the engine's submit_and_wait and staging reuse cost nothing, and
 * batching is the first thing to change once correctness is settled.
 *
 * Images stay in VK_IMAGE_LAYOUT_GENERAL for their whole life: legal for
 * transfers, sampling and as attachments, so there is no layout bookkeeping.
 *
 * Shaders: libs/video/vulkan/rsx_vk_fallback.{vert,frag}, embedded as SPIR-V.
 * Regenerate after editing them with:
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_vert_spv \
 *       -o rsx_vk_fallback_vert.spv.h rsx_vk_fallback.vert
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_frag_spv \
 *       -o rsx_vk_fallback_frag.spv.h rsx_vk_fallback.frag
 * then `expand -t 4` both outputs: glslang indents with tabs, the project
 * with spaces.
 */

#if !defined(_WIN32) || defined(PS3RECOMP_RSX_VULKAN)

#define VK_NO_PROTOTYPES
#ifdef _WIN32
/* Windows (opt-in): a Win32 window with VK_KHR_win32_surface instead of SDL,
 * and vulkan-1.dll loaded at run time like libvulkan elsewhere. */
#define VK_USE_PLATFORM_WIN32_KHR
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__ANDROID__)
/* Android: ANativeWindow with VK_KHR_android_surface, libvulkan.so &
 * libandroid.so loaded at run time so headless CLI harnesses need no JNI. */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#endif
#include <vulkan/vulkan.h>

#include "rsx_vulkan_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_spirv.h"
#include "rsx_draw_engine.h"
#include "metal_frame_rate.h"   /* portable bounded present-rate statistics */
#include "rsx_present_thread.h"
#include "vulkan/rsx_vk_fallback_vert.spv.h"
#include "vulkan/rsx_vk_fallback_frag.spv.h"

/* SDL2 is already required off Windows/Android (cellPad, cellAudio); here it
 * only opens the optional window. Windows opens a Win32 window and Android
 * uses ANativeWindow supplied by the host Activity. */
#if defined(__ANDROID__)
#include <dlfcn.h>
#elif !defined(_WIN32)
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <dlfcn.h>
#endif
#include "rsx_vk_thread.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif

static u64 vk_system_ram_mb(void)
{
    static u64 cached_mb;
    if (cached_mb) return cached_mb;
#if !defined(_WIN32) && defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long psz   = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0)
        cached_mb = ((u64)pages * (u64)psz) >> 20;
#endif
    if (!cached_mb) cached_mb = 4096u;
    return cached_mb;
}

/* ---------------------------------------------------------------------------
 * Run-time loaded entry points (Vulkan 1.0 core only)
 * -------------------------------------------------------------------------*/
#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkGetFenceStatus) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkResetDescriptorPool) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdSetStencilReference) X(vkCmdSetDepthBounds) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) \
    X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBlitImage)
/* Window-system entry points, loaded only when a window was requested, so a
 * headless run never depends on a driver that can present. */
#define VK_WSI_INSTANCE_FNS(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_WSI_DEVICE_FNS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define VK_DECLARE(name) static PFN_##name p##name;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
VK_WSI_INSTANCE_FNS(VK_DECLARE)
VK_WSI_DEVICE_FNS(VK_DECLARE)

/* ---------------------------------------------------------------------------
 * Backend state
 * -------------------------------------------------------------------------*/
typedef struct vk_vertex {
    float pos[4];
    float col[4];
    float uv[2];
} vk_vertex;

typedef struct vk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void*          ptr;
    VkDeviceSize   size;
    int            coherent;
} vk_buffer;

typedef struct vk_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
} vk_image;

#define VK_SAMPLER_CACHE 32
#define VK_PIPELINE_SLOTS (2 * 2 * 8)   /* ztest x zwrite x compare op */

typedef struct vk_state {
    void*            lib;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    u32              queue_family;
    VkPhysicalDeviceMemoryProperties memprops;

    u32              width, height;
    vk_image         color;           /* R8G8B8A8_UNORM                 */
    vk_image         depth;           /* D32_SFLOAT or fallback         */
    VkFormat         depth_format;
    VkImageAspectFlags depth_aspect;  /* DEPTH, plus STENCIL if it has one  */

    VkRenderPass     render_pass;
    VkFramebuffer    framebuffer;
    VkShaderModule   vs, fs;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  desc_set;
    VkPipelineLayout pipe_layout;
    VkPipelineCache  pipe_cache;
    char             pipe_cache_path[512];
    u32              pipe_cache_built;
    u32              pipe_cache_saved;
    double           pipe_cache_last_save;
    int              pipe_cache_saving;
    VkPipeline       pipelines[VK_PIPELINE_SLOTS];
    VkSampler        sampler;

    vk_image         dummy;           /* 1x1 magenta: "nothing bound"    */
    vk_image         tex[RSX_MAX_TEXTURES];   /* decoded guest textures  */
    int              tex_ready[RSX_MAX_TEXTURES];
    /* Guest path only: each unit's sampler, built from its wrap, filter and
     * LOD registers (the fallback path keeps `sampler`, the null backend's
     * nearest + repeat). Cached by register value; VK_NULL_HANDLE = use
     * `sampler`. */
    VkSampler        tex_sampler[RSX_MAX_TEXTURES];
    struct { u64 key; VkSampler s; } samp_cache[VK_SAMPLER_CACHE];
    u32              samp_count;

    vk_buffer        vertices;        /* expanded triangle list          */
    vk_vertex*       cpu_verts;
    size_t           cpu_cap, cpu_count;

    vk_buffer        readback;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;

    /* A present records and submits on its own pool, command buffer and
     * fences, so its second half can run on a host thread of its own while
     * the FIFO walker records the next frame (rsx_vulkan_backend.h,
     * present_begin/_finish). The queue is shared, and Vulkan wants every
     * vkQueueSubmit / vkQueuePresentKHR / vkDeviceWaitIdle on it externally
     * synchronised: queue_mu. present_mu keeps the present's own objects to
     * one present at a time, which matters when a flip in the command stream
     * presents on the walker's thread while a host thread finishes the last. */
    VkCommandPool    present_pool;
    /* A small ring, so a direct present (PS3RECOMP_VK_DIRECT_PRESENT=1) never
     * waits for the one before it: a slot is reused only once its fence has
     * signalled. A read-back present uses the same ring. */
#define VK_PRESENT_SLOTS 3
    VkCommandBuffer  present_cmds[VK_PRESENT_SLOTS];
    VkFence          present_fences[VK_PRESENT_SLOTS];
    VkSemaphore      present_acquire[VK_PRESENT_SLOTS]; /* direct window presents */
    int              present_busy[VK_PRESENT_SLOTS];    /* submitted, not yet waited */
    u32              present_slot;    /* the slot the last present used        */
    VkCommandBuffer  present_cmd;     /* = present_cmds[present_slot]          */
    VkFence          present_fence;   /* = present_fences[present_slot]        */
    VkFence          window_fence;    /* the blit into the swapchain image     */
    int              present_inflight;/* a read-back present awaiting completion */
    u64              present_direct, present_readback_count;
    int              present_split;   /* inside rsx_vulkan_backend_present_begin */
    int              present_order;   /* the walker's next command buffer waits */
    pthread_mutex_t  queue_mu;
    pthread_mutex_t  present_mu;

    /* Optional window (PS3RECOMP_VK_WINDOW=1). Rendering stays offscreen
     * either way -- readback and the tests are unchanged -- and a present
     * additionally blits the frame into the window's swapchain. */
    int              eng_active;      /* the register-file engine drives us */
    int              bc_ok;           /* textureCompressionBC enabled       */
    int              depth_bounds_ok; /* depthBounds enabled                */
    int              depth_clamp_ok;  /* depthClamp enabled                 */
    int              indep_blend_ok;  /* independentBlend: per-target MRT state */
    u32              max_dim;         /* maxImageDimension2D of the device  */
    int              windowed;
#ifdef _WIN32
    HWND             window;
    int              window_closed;   /* WM_CLOSE or Esc */
    int              fullscreen;      /* borderless, F11 or Alt+Enter */
    WINDOWPLACEMENT  restore;         /* the windowed placement to go back to */
#elif defined(__ANDROID__)
    ANativeWindow*   window;
#else
    SDL_Window*      window;
#endif
    const char*      inst_exts[16];
    u32              inst_ext_count;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkExtent2D       sc_extent;
    VkFormat         sc_format;
    int              sc_can_attach;
    u32              sc_count;
    VkImage          sc_images[8];
    VkSemaphore      sc_done[8];      /* per image: the present waits on it */
    VkSemaphore      sem_acquire;
    double           hold_seconds;

    const rsx_state* state;           /* live RSX state, for draws       */
    u32              clear_argb;
    u32              last_center;
    u32              presented;
    const char*      dump_path;
    u32              dump_every;       /* write every nth presented frame */
    /* PS3RECOMP_VK_FPS=1: once a second, guest presents and window present
     * calls. Window times are host clock readings when vkQueuePresentKHR
     * returns, not compositor presentation timestamps. present_mu held. */
    int              fps_on;
    MetalFrameRate   fps;
} vk_state;

static vk_state s_vk;

#define VK_LOG(...)  fprintf(stderr, "[RSX vulkan] " __VA_ARGS__)
#define VK_CHECK(call, what)                                               \
    do { VkResult r_ = (call);                                             \
         if (r_ != VK_SUCCESS) {                                           \
             VK_LOG("%s failed (VkResult %d)\n", (what), (int)r_);         \
             return -1; } } while (0)

/* ---------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------*/
#ifdef _WIN32
static int vk_load_library(void)
{
    s_vk.lib = (void*)LoadLibraryA("vulkan-1.dll");
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (LoadLibrary vulkan-1.dll: error %lu)\n", GetLastError());
        return -1;
    }
    FARPROC entry = GetProcAddress((HMODULE)s_vk.lib, "vkGetInstanceProcAddr");
    memcpy(&pvkGetInstanceProcAddr, &entry, sizeof entry);
    if (!pvkGetInstanceProcAddr) { VK_LOG("vulkan-1.dll has no vkGetInstanceProcAddr\n"); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
#undef LOAD_G
    return 0;
}
#else
#ifdef __ANDROID__
extern void* ps3_android_open_custom_vulkan(void) __attribute__((weak));
#endif

static int vk_load_library(void)
{
#ifdef __ANDROID__
    if (ps3_android_open_custom_vulkan) {
        s_vk.lib = ps3_android_open_custom_vulkan();
        if (s_vk.lib) {
            VK_LOG("using custom Android Vulkan driver handle\n");
        }
    }
    static const char* names[] = { "libvulkan.so", "libvulkan.so.1" };
#else
    static const char* names[] = { "libvulkan.so.1", "libvulkan.so" };
#endif
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !s_vk.lib; i++)
        s_vk.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (dlopen libvulkan.so.1: %s)\n", dlerror());
        return -1;
    }
    pvkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(s_vk.lib, "vkGetInstanceProcAddr");
    if (!pvkGetInstanceProcAddr) { VK_LOG("libvulkan has no vkGetInstanceProcAddr\n"); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
    return 0;
}
#endif

static int vk_load_instance_functions(void)
{
#define LOAD_I(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(s_vk.instance, #name))) \
        { VK_LOG("missing instance function " #name "\n"); return -1; }
    VK_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_wsi_instance_functions(void)
{
    VK_WSI_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_device_functions(void)
{
#define LOAD_D(name) \
    if (!(p##name = (PFN_##name)pvkGetDeviceProcAddr(s_vk.device, #name))) \
        { VK_LOG("missing device function " #name "\n"); return -1; }
    VK_DEVICE_FNS(LOAD_D)
    return 0;
}

static int vk_load_wsi_device_functions(void)
{
    VK_WSI_DEVICE_FNS(LOAD_D)
    return 0;
}

/* ---------------------------------------------------------------------------
 * Device selection: PS3RECOMP_VK_DEVICE if set, else the first discrete GPU,
 * else integrated, else anything that is not a CPU rasteriser, else whatever
 * is there (llvmpipe/lavapipe on a GPU-less CI runner).
 * -------------------------------------------------------------------------*/
static int vk_device_rank(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
    default:                                     return 1;
    }
}

static int vk_find_graphics_queue(VkPhysicalDevice pd, u32* family)
{
    u32 n = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (n == 0 || n > 64) return -1;
    VkQueueFamilyProperties props[64];
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    for (u32 i = 0; i < n; i++) {
        if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (s_vk.windowed) {           /* the same queue must also present */
            VkBool32 ok = VK_FALSE;
            if (pvkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s_vk.surface, &ok) != VK_SUCCESS || !ok)
                continue;
        }
        *family = i; return 0;
    }
    return -1;
}

static int vk_has_device_extension(VkPhysicalDevice pd, const char* name)
{
    u32 n = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties* e = (VkExtensionProperties*)malloc(n * sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, name) == 0;
    free(e);
    return found;
}

static int vk_pick_device(void)
{
    u32 n = 0;
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, NULL), "vkEnumeratePhysicalDevices");
    if (n == 0) { VK_LOG("no Vulkan physical devices\n"); return -1; }
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, devs), "vkEnumeratePhysicalDevices");

    int best = -1, best_rank = -1;
    const char* forced = getenv("PS3RECOMP_VK_DEVICE");
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(devs[i], &p);
        u32 fam;
        int usable = vk_find_graphics_queue(devs[i], &fam) == 0 &&
                     (!s_vk.windowed ||
                      vk_has_device_extension(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        VK_LOG("device %u: %s (type %d, api %u.%u.%u)%s\n", i, p.deviceName,
               (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               usable ? "" : (s_vk.windowed ? " -- cannot present to the window"
                                            : " -- no graphics queue"));
        if (!usable) continue;
        if (forced) {
            if ((u32)atoi(forced) == i) { best = (int)i; break; }
            continue;
        }
        int rank = vk_device_rank(p.deviceType);
        if (rank > best_rank) { best = (int)i; best_rank = rank; }
    }
    if (best < 0) {
        if (forced) VK_LOG("PS3RECOMP_VK_DEVICE=%s is not a usable device\n", forced);
        else        VK_LOG("no device with a graphics queue\n");
        return -1;
    }
    s_vk.phys = devs[best];
    vk_find_graphics_queue(s_vk.phys, &s_vk.queue_family);
    pvkGetPhysicalDeviceMemoryProperties(s_vk.phys, &s_vk.memprops);
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    /* Vulkan guarantees 4096; the engine's targets and textures may use what
     * the device really has (a title's 4608-wide target on RADV). */
    s_vk.max_dim = p.limits.maxImageDimension2D >= 4096u ? p.limits.maxImageDimension2D : 4096u;
    VK_LOG("using device %d: %s\n", best, p.deviceName);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Memory, images, buffers
 * -------------------------------------------------------------------------*/
static int vk_find_memory_type(u32 type_bits, VkMemoryPropertyFlags want, u32* out)
{
    for (u32 i = 0; i < s_vk.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (s_vk.memprops.memoryTypes[i].propertyFlags & want) == want) {
            *out = i; return 0;
        }
    return -1;
}

static int vk_create_image_levels(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 levels,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    if (!levels) levels = 1;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(pvkCreateImage(s_vk.device, &ici, NULL, &im->img), "vkCreateImage");
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) {
        VK_LOG("no memory type for an image\n"); return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem), "vkAllocateMemory(image)");
    VK_CHECK(pvkBindImageMemory(s_vk.device, im->img, im->mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, levels, 0, 1 },
    };
    VK_CHECK(pvkCreateImageView(s_vk.device, &vci, NULL, &im->view), "vkCreateImageView");
    return 0;
}

static int vk_create_image(vk_image* im, VkFormat fmt, u32 w, u32 h,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return vk_create_image_levels(im, fmt, w, h, 1, usage, aspect);
}

static void vk_present_wait_gpu(void);
static double vk_now_seconds(void);

/* VK_DRAW_BATCH (vk_eng_draw): draws recorded into s_vk.cmd and not yet
 * submitted. Anything that records other work, destroys an object or reads
 * results flushes them first, so everything else keeps the invariant that
 * all earlier work has completed. */
enum {
    VK_FLUSH_MAX = 0,
    VK_FLUSH_RING = 1,
    VK_FLUSH_PRESENT = 2,
    VK_FLUSH_SYNC = 3,
    VK_FLUSH_EARLY = 4,
    VK_FLUSH_REASON_COUNT = 5
};
enum {
    VK_SYNC_OTHER = 0,
    VK_SYNC_SUBMIT = 1,
    VK_SYNC_READBACK = 2,
    VK_SYNC_FB = 3,
    VK_SYNC_SNAP = 4,
    VK_SYNC_TAG_COUNT = 5
};
enum {
    VK_PB_FB    = 0,
    VK_PB_CLR   = 1,
    VK_PB_SELF  = 2,
    VK_PB_CSNAP = 3,
    VK_PB_DSNAP = 4,
    VK_PB_UPL   = 5,
    VK_PB_BLIT  = 6,
    VK_PB_COUNT = 7,
};
static u32 s_eb_pending;
static void vk_eng_flush(void);
static void vk_eng_flush_sync(void);
static void vk_eng_flush_sync_tag(int tag);
static void vk_eng_flush_async(int reason);
static int  vk_eb_defer_image(vk_image* im);
static int  vk_eb_defer_fb(VkFramebuffer* fb);
static void vk_eb_perf_present(void);
static void vk_eb_end_pass(void);
static void vk_eb_end_pass_reason(int reason);
static int vk_eb_on(void);
static int vk_eb_active(void);       /* batching decided on and on */
static u32 vk_eb_max(void);
static void vk_eb_recorded(void);   /* one more command in the open batch */
static void vk_eb_release(void);    /* engine shutdown: rings and pool */
static int vk_eb_upload(VkImage img, VkBufferImageCopy* region, const void* src, size_t tight,
                        u32 rows, u32 row_bytes);   /* -2: not batched, do it the old way */

/* ---------------------------------------------------------------------------
 * Disk-backed VkPipelineCache (eliminates Adreno / driver shader-compile hitching)
 * -------------------------------------------------------------------------*/
static void vk_pipe_cache_init(void)
{
    if (!s_vk.device || s_vk.pipe_cache) return;
    const char* env = getenv("PS3RECOMP_VK_PIPELINE_CACHE");
    if (env && *env) {
        if (!strcmp(env, "0")) return;
        snprintf(s_vk.pipe_cache_path, sizeof s_vk.pipe_cache_path, "%s", env);
    } else {
        const char* gd = getenv("PS3_GAMEDATA_ROOT");
        if (gd && *gd)
            snprintf(s_vk.pipe_cache_path, sizeof s_vk.pipe_cache_path, "%s/vk_pipeline_cache.bin", gd);
    }
    void* blob = NULL;
    size_t blob_sz = 0;
    if (s_vk.pipe_cache_path[0]) {
        FILE* f = fopen(s_vk.pipe_cache_path, "rb");
        if (f) {
            if (fseek(f, 0, SEEK_END) == 0) {
                long sz = ftell(f);
                if (sz > 0 && sz <= (64L << 20) && fseek(f, 0, SEEK_SET) == 0) {
                    blob = malloc((size_t)sz);
                    if (blob && fread(blob, 1, (size_t)sz, f) == (size_t)sz) blob_sz = (size_t)sz;
                    else { free(blob); blob = NULL; }
                }
            }
            fclose(f);
        }
    }
    VkPipelineCacheCreateInfo pcci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = blob_sz,
        .pInitialData = blob,
    };
    VkResult r = pvkCreatePipelineCache(s_vk.device, &pcci, NULL, &s_vk.pipe_cache);
    if (r != VK_SUCCESS && blob_sz) {
        pcci.initialDataSize = 0;
        pcci.pInitialData = NULL;
        blob_sz = 0;
        r = pvkCreatePipelineCache(s_vk.device, &pcci, NULL, &s_vk.pipe_cache);
    }
    free(blob);
    if (r == VK_SUCCESS) {
        s_vk.pipe_cache_last_save = vk_now_seconds();
        VK_LOG("pipeline cache ready (%zu bytes loaded%s%s)\n", blob_sz,
               s_vk.pipe_cache_path[0] ? ", path=" : "", s_vk.pipe_cache_path);
    } else {
        s_vk.pipe_cache = VK_NULL_HANDLE;
    }
}

#ifndef _WIN32
static pthread_rwlock_t s_pipe_cache_rw = PTHREAD_RWLOCK_INITIALIZER;
#endif

typedef struct vk_pipe_save_job {
    void*  buf;
    size_t sz;
    u32    built;
    char   path[512];
} vk_pipe_save_job;

static void vk_pipe_save_write(vk_pipe_save_job* j)
{
    if (!j->buf) {
        size_t sz = 0;
        VkResult r = VK_ERROR_UNKNOWN;
#ifndef _WIN32
        pthread_rwlock_rdlock(&s_pipe_cache_rw);
#endif
        if (s_vk.device && s_vk.pipe_cache) {
            r = pvkGetPipelineCacheData(s_vk.device, s_vk.pipe_cache, &sz, NULL);
            if (r == VK_SUCCESS && sz && sz <= (64u << 20)) {
                j->buf = malloc(sz);
                if (j->buf) {
                    r = pvkGetPipelineCacheData(s_vk.device, s_vk.pipe_cache, &sz, j->buf);
                    if (r == VK_SUCCESS && sz) j->sz = sz;
                    else { free(j->buf); j->buf = NULL; j->sz = 0; }
                }
            }
        }
#ifndef _WIN32
        pthread_rwlock_unlock(&s_pipe_cache_rw);
#endif
    }
    if (j->buf && j->sz) {
        char tmp[544];
        snprintf(tmp, sizeof tmp, "%s.tmp", j->path);
        FILE* f = fopen(tmp, "wb");
        if (f) {
            const size_t w = fwrite(j->buf, 1, j->sz, f);
            fclose(f);
            if (w == j->sz && rename(tmp, j->path) == 0) {
                VK_LOG("pipeline cache saved (%zu bytes, %u pipelines)\n", j->sz, j->built);
            } else {
                remove(tmp);
            }
        }
    }
    free(j->buf);
    free(j);
}

#ifndef _WIN32
static void* vk_pipe_save_thread(void* arg)
{
    (void)setpriority(PRIO_PROCESS, 0, 6);
    vk_pipe_save_write((vk_pipe_save_job*)arg);
    __atomic_store_n(&s_vk.pipe_cache_saving, 0, __ATOMIC_RELEASE);
    return NULL;
}
#endif

static void vk_pipe_cache_save_ex(int async_write)
{
    if (!s_vk.device || !s_vk.pipe_cache || !s_vk.pipe_cache_path[0]) return;
    const u32 built = __atomic_load_n(&s_vk.pipe_cache_built, __ATOMIC_RELAXED);
    if (built <= s_vk.pipe_cache_saved) return;
#ifndef _WIN32
    if (async_write) {
        if (__atomic_exchange_n(&s_vk.pipe_cache_saving, 1, __ATOMIC_ACQ_REL)) return;
    } else {
        /* Wait briefly for any in-flight background save before a synchronous shutdown save. */
        for (int i = 0; i < 400 && __atomic_load_n(&s_vk.pipe_cache_saving, __ATOMIC_ACQUIRE); i++) {
            struct timespec ts = { 0, 5000000L };
            nanosleep(&ts, NULL);
        }
    }
#else
    (void)async_write;
#endif
    vk_pipe_save_job* j = (vk_pipe_save_job*)calloc(1, sizeof *j);
    if (!j) {
#ifndef _WIN32
        if (async_write) __atomic_store_n(&s_vk.pipe_cache_saving, 0, __ATOMIC_RELEASE);
#endif
        return;
    }
    j->built = built;
    snprintf(j->path, sizeof j->path, "%s", s_vk.pipe_cache_path);
    s_vk.pipe_cache_saved = built;
    s_vk.pipe_cache_last_save = vk_now_seconds();
#ifndef _WIN32
    if (async_write) {
        pthread_t th;
        if (pthread_create(&th, NULL, vk_pipe_save_thread, j) == 0) {
            pthread_detach(th);
            return;
        }
        __atomic_store_n(&s_vk.pipe_cache_saving, 0, __ATOMIC_RELEASE);
    }
#endif
    vk_pipe_save_write(j);
}

static void vk_pipe_cache_save(void)
{
    vk_pipe_cache_save_ex(0);
}

static void vk_pipe_cache_destroy(void)
{
    if (!s_vk.device || !s_vk.pipe_cache) return;
    vk_pipe_cache_save();
#ifndef _WIN32
    for (int i = 0; i < 400 && __atomic_load_n(&s_vk.pipe_cache_saving, __ATOMIC_ACQUIRE); i++) {
        struct timespec ts = { 0, 5000000L };
        nanosleep(&ts, NULL);
    }
    pthread_rwlock_wrlock(&s_pipe_cache_rw);
#endif
    VkPipelineCache pc = s_vk.pipe_cache;
    s_vk.pipe_cache = VK_NULL_HANDLE;
#ifndef _WIN32
    pthread_rwlock_unlock(&s_pipe_cache_rw);
#endif
    if (pc) pvkDestroyPipelineCache(s_vk.device, pc, NULL);
}

static void vk_destroy_image(vk_image* im)
{
    vk_eng_flush_sync();
    /* Never under a present's copy that may still read it. */
    if (im->img) vk_present_wait_gpu();
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static int vk_create_host_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    /* A buffer the GPU only copies into is one the CPU reads back, and the
     * first coherent type is usually write-combined and uncached (RADV's
     * GTT): there the readback memcpy was 82% of the FIFO thread's time in
     * Twisted Metal. Take a cached type for it when there is one; reads
     * invalidate, so a non-coherent one is fine.
     * On Android UMA (Adreno/Mali), HOST_COHERENT|HOST_CACHED is hardware-
     * coherent in unified LPDDR5X and avoids write-combining stalls on
     * vertex/index/UBO/staging ring writes too. */
    const VkMemoryPropertyFlags cached = vis | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const int readback = usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT;
#ifdef __ANDROID__
    if (vk_find_memory_type(req.memoryTypeBits, cached | coh, &type) == 0) b->coherent = 1;
    else if (readback && vk_find_memory_type(req.memoryTypeBits, cached, &type) == 0) b->coherent = 0;
#else
    if (readback && vk_find_memory_type(req.memoryTypeBits, cached | coh, &type) == 0) b->coherent = 1;
    else if (readback && vk_find_memory_type(req.memoryTypeBits, cached, &type) == 0) b->coherent = 0;
#endif
    else if (vk_find_memory_type(req.memoryTypeBits, coh, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, vis, &type) == 0) b->coherent = 0;
    else { VK_LOG("no host-visible memory for a buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr), "vkMapMemory");
    b->size = size;
    return 0;
}

/* A buffer only the GPU touches (copies through it stay in VRAM). */
static int vk_create_device_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) { VK_LOG("no memory for a device buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(device buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    b->ptr = NULL; b->coherent = 0; b->size = size;
    return 0;
}

static void vk_destroy_buffer(vk_buffer* b)
{
    vk_eng_flush_sync();
    if (b->ptr) pvkUnmapMemory(s_vk.device, b->mem);
    if (b->buf) pvkDestroyBuffer(s_vk.device, b->buf, NULL);
    if (b->mem) pvkFreeMemory(s_vk.device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* Host writes to non-coherent memory need a flush; there is no
 * vkFlushMappedMemoryRanges loaded, so writes only ever go to coherent
 * buffers (every desktop/L4T driver exposes one). Reads invalidate. */
static void vk_invalidate(vk_buffer* b)
{
    if (b->coherent) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = b->mem, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkInvalidateMappedMemoryRanges(s_vk.device, 1, &r);
}

/* ---------------------------------------------------------------------------
 * One-shot command submission
 *
 * The queue is shared with the present (see present_mu in vk_state), so every
 * operation on it goes through these.
 * -------------------------------------------------------------------------*/
static VkResult vk_queue_submit(const VkSubmitInfo* si, VkFence fence)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueueSubmit(s_vk.queue, 1, si, fence);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

static VkResult vk_queue_present(const VkPresentInfoKHR* pi)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueuePresentKHR(s_vk.queue, pi);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

static void vk_device_idle(void)
{
    if (!s_vk.device || !pvkDeviceWaitIdle) return;
    pthread_mutex_lock(&s_vk.queue_mu);
    pvkDeviceWaitIdle(s_vk.device);
    pthread_mutex_unlock(&s_vk.queue_mu);
}

/* A full execution and memory dependency on everything submitted before
 * this command buffer, the present's copy included. */
static void vk_cmd_after_everything(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* Tighter intra-batch dependency between render passes / uploads: avoids
 * ALL_COMMANDS -> ALL_COMMANDS pipeline bubbles on Adreno TBDR while keeping
 * full colour/depth/transfer -> shader/attachment/transfer coherence. */
static void vk_cmd_pass_barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_TRANSFER_READ_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    pvkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &mb, 0, NULL, 0, NULL);
}

static int vk_begin(void)
{
    if (s_eb_pending) vk_eng_flush();
    VK_CHECK(pvkResetCommandBuffer(s_vk.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(s_vk.cmd, &bi), "vkBeginCommandBuffer");
    /* A present's copy may still be reading the surface this frame is about
     * to draw over: order everything here after it. Set only by a present,
     * which never runs alongside the walker's recording. */
    if (s_vk.present_order) { vk_cmd_after_everything(s_vk.cmd); s_vk.present_order = 0; }
    return 0;
}

static VkResult vk_wait_fence_logged(VkFence fence, const char* where)
{
    for (u32 n = 1u; ; n++) {
        VkResult r = pvkWaitForFences(s_vk.device, 1, &fence, VK_TRUE, 2000000000ull /* 2s */);
        if (r == VK_SUCCESS) return VK_SUCCESS;
        if (r == VK_TIMEOUT) {
            if (n <= 5u || (n % 5u) == 0u)
                VK_LOG("WARNING: vkWaitForFences(%s) stalled >%us\n", where, n * 2u);
            if (n >= 5u) return VK_TIMEOUT;
            continue;
        }
        VK_LOG("ERROR: vkWaitForFences(%s) failed (VkResult %d)\n", where, (int)r);
        return r;
    }
}

static int vk_submit_and_wait(void)
{
    VK_CHECK(pvkEndCommandBuffer(s_vk.cmd), "vkEndCommandBuffer");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
    VK_CHECK(pvkResetFences(s_vk.device, 1, &s_vk.fence), "vkResetFences");
    VK_CHECK(vk_queue_submit(&si, s_vk.fence), "vkQueueSubmit");
    VK_CHECK(vk_wait_fence_logged(s_vk.fence, "submit_and_wait"), "vkWaitForFences");
    return 0;
}

/* Order this command buffer's use of an image after everything submitted
 * before it; from UNDEFINED it is also the one-time move to GENERAL. */
static void vk_barrier_image_on(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                                VkImageLayout old_layout)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void vk_barrier_image(VkImage img, VkImageAspectFlags aspect, VkImageLayout old_layout)
{
    vk_barrier_image_on(s_vk.cmd, img, aspect, old_layout);
}

/* ---------------------------------------------------------------------------
 * Texture unit 0
 * -------------------------------------------------------------------------*/
static void vk_write_texture_descriptor(void)
{
    VkDescriptorImageInfo ii = { .sampler = s_vk.sampler,
                                 .imageView = s_vk.tex_ready[0] ? s_vk.tex[0].view
                                                                : s_vk.dummy.view,
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = s_vk.desc_set, .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &w, 0, NULL);
}

/* Replace *dst with an RGBA8 image of `nlv` levels: level m is lw[m] x lh[m]
 * texels, tightly packed, the levels one after another in `rgba`. */
static int vk_upload_texture_levels(vk_image* dst, const u8* rgba,
                                    const u32* lw, const u32* lh, u32 nlv)
{
    VkBufferImageCopy regions[RSX_MAX_TEXTURE_LEVELS];
    if (!nlv || nlv > RSX_MAX_TEXTURE_LEVELS) return -1;
    VkDeviceSize total = 0;
    for (u32 m = 0; m < nlv; m++) {
        regions[m] = (VkBufferImageCopy){
            .bufferOffset = total,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
            .imageExtent = { lw[m], lh[m], 1 },
        };
        total += (VkDeviceSize)lw[m] * lh[m] * 4u;
    }
    vk_buffer staging = {0};
    if (vk_create_host_buffer(&staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !staging.coherent) {
        vk_destroy_buffer(&staging); return -1;
    }
    memcpy(staging.ptr, rgba, (size_t)total);

    vk_image fresh = {0};
    if (vk_create_image_levels(&fresh, VK_FORMAT_R8G8B8A8_UNORM, lw[0], lh[0], nlv,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&fresh); vk_destroy_buffer(&staging); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(fresh.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdCopyBufferToImage(s_vk.cmd, staging.buf, fresh.img, VK_IMAGE_LAYOUT_GENERAL,
                                nlv, regions);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&staging);
    if (rc) { vk_destroy_image(&fresh); return -1; }

    vk_destroy_image(dst);          /* idle: every submission is waited on */
    *dst = fresh;
    return 0;
}

static int vk_upload_texture(vk_image* dst, const u8* rgba, u32 w, u32 h)
{
    return vk_upload_texture_levels(dst, rgba, &w, &h, 1);
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field, one per axis -- the table the Metal
 * backend and the live draw engine use: 1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE,
 * 4 BORDER, 5 CLAMP, 6..8 MIRROR_ONCE. Vulkan 1.0 has MIRROR_CLAMP_TO_EDGE
 * only behind an extension, so the MIRROR_ONCE family clamps to edge here --
 * exact for texcoords in [0,1], the mirrored half is what is lost. */
static VkSamplerAddressMode vk_gcm_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      /* 3, 5..8, unset */
    }
}

/* A unit's sampler from its registers, decoded as the Metal backend's
 * samp_slot_for does: SET_TEXTURE_FILTER min at [18:16] (1 NEAREST,
 * 2 LINEAR, 3..6 the four nearest/linear x nearest/linear-mip pairs), mag at
 * [26:24]; SET_TEXTURE_CONTROL0 max LOD at [18:7], min LOD at [30:19], 4.8
 * fixed point. The LOD bias (FILTER [12:0]) is not applied, as in Metal.
 * VK_NULL_HANDLE on failure: the caller falls back to `sampler`. */
static VkSampler vk_unit_sampler(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;       /* max then min LOD */
    const u64 key  = (u64)minf | ((u64)magf << 3)
                   | ((u64)(t->address & 0x000F0F0Fu) << 6) | ((u64)lod << 26);
    for (u32 i = 0; i < s_vk.samp_count; i++)
        if (s_vk.samp_cache[i].key == key) return s_vk.samp_cache[i].s;
    if (s_vk.samp_count >= VK_SAMPLER_CACHE) return VK_NULL_HANDLE;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);
    /* A min filter with no mip term samples level 0 only: pin the range. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (magf == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (minf == 2 || minf == 4 || minf == 6) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(t->address),
        .addressModeV = vk_gcm_wrap(t->address >> 8),
        .addressModeW = vk_gcm_wrap(t->address >> 16),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    VkSampler smp = VK_NULL_HANDLE;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_vk.samp_cache[s_vk.samp_count].key = key;
    s_vk.samp_cache[s_vk.samp_count].s   = smp;
    s_vk.samp_count++;
    return smp;
}

/* One level of a guest texture into tightly packed RGBA8, with the null
 * backend's decode (rsx_null_backend.c, nullsw_bind_texture): formats that
 * decode to fewer than four bytes are splayed, alpha forced opaque. */
static int vk_decode_level_rgba8(u8* dst, const u8* src, u32 w, u32 h,
                                 const rsx_tex_layout* tl)
{
    const u32 pitch = w * 4u;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(dst, pitch, src, w, h, tl, rsx_texture_argb_is_rgba());
        return 0;
    }
    const u32 srcp = tl->row_bytes;
    u8* tmp = (u8*)malloc((size_t)srcp * h);
    if (!tmp) return -1;
    rsx_texture_decode(tmp, srcp, src, w, h, tl, 0);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl->bytes_per_texel;
            u8* dp = dst + (size_t)y * pitch + (size_t)x * 4u;
            dp[0] = sp[0];
            dp[1] = tl->bytes_per_texel > 1 ? sp[1] : sp[0];
            dp[2] = tl->bytes_per_texel > 2 ? sp[2] : sp[0];
            dp[3] = 255;
        }
    free(tmp);
    return 0;
}

/* Unit `unit` has nothing usable bound. The fallback path's descriptor only
 * ever shows unit 0, so only that one needs rewriting. */
static void vk_tex_off(u32 unit)
{
    s_vk.tex_ready[unit] = 0;
    s_vk.tex_sampler[unit] = VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* Same decode as the headless null backend (rsx_null_backend.c,
 * nullsw_bind_texture), so both backends sample identical texels. */
static void vk_cb_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit >= RSX_MAX_TEXTURES || !t || !s_vk.device) return;
    if (!(t->control0 & 0x80000000u) || !t->offset) { vk_tex_off(unit); return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { vk_tex_off(unit); return; }

    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { vk_tex_off(unit); return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    /* The whole mip chain, as the Metal backend reads it: SET_TEXTURE_FORMAT's
     * level count above bit 15 and SET_TEXTURE_CONTROL3's row pitch, laid out
     * by the shared rsx_texture_mip_chain() (which clamps the count to what
     * the dimensions allow). Cube maps are not handled yet: face 0 only. */
    const u32 levels    = (t->format >> 16) & 0xFFFFu;
    const u32 row_pitch = t->control3 & 0xFFFFFu;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, row_pitch, lv);
    if (!nlv || lv[0].tl.compressed) { vk_tex_off(unit); return; }   /* no BC path yet */

    size_t total = 0, off[RSX_MAX_TEXTURE_LEVELS];
    u32 lw[RSX_MAX_TEXTURE_LEVELS], lh[RSX_MAX_TEXTURE_LEVELS];
    for (u32 m = 0; m < nlv; m++) {
        off[m] = total;
        lw[m] = lv[m].w; lh[m] = lv[m].h;
        total += (size_t)lv[m].w * lv[m].h * 4u;
    }
    u8* buf = (u8*)malloc(total);
    if (!buf) { vk_tex_off(unit); return; }
    for (u32 m = 0; m < nlv; m++)
        if (vk_decode_level_rgba8(buf + off[m], vm_base + ea + lv[m].offset,
                                  lv[m].w, lv[m].h, &lv[m].tl)) {
            free(buf); vk_tex_off(unit); return;
        }
    s_vk.tex_ready[unit] = vk_upload_texture_levels(&s_vk.tex[unit], buf, lw, lh, nlv) == 0;
    free(buf);
    s_vk.tex_sampler[unit] = s_vk.tex_ready[unit] ? vk_unit_sampler(t) : VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* ---------------------------------------------------------------------------
 * Render pass, framebuffer, pipelines
 * -------------------------------------------------------------------------*/
static int vk_pick_depth_format(void)
{
    /* With stencil first -- titles use it for shadows, reflections and UI
     * masks -- and D32 first within each group: a D32 depth aspect copies out
     * as 32-bit floats, which is what depth snapshots are built from. */
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM
    };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    /* TRANSFER_DST is a 1.1 format feature bit; 1.0 drivers may not report
     * it although clears work. Retry on the attachment bit alone. */
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    VK_LOG("no usable depth format\n");
    return -1;
chosen:
    s_vk.depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (s_vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        s_vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT)
        s_vk.depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return 0;
}

static int vk_create_render_pass(void)
{
    VkAttachmentDescription att[2] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = s_vk.depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,     /* ignored without stencil */
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VK_CHECK(pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_vk.render_pass), "vkCreateRenderPass");

    VkImageView views[2] = { s_vk.color.view, s_vk.depth.view };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = s_vk.render_pass, .attachmentCount = 2,
                                    .pAttachments = views, .width = s_vk.width,
                                    .height = s_vk.height, .layers = 1 };
    VK_CHECK(pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_vk.framebuffer), "vkCreateFramebuffer");
    return 0;
}

static int vk_create_shader(const uint32_t* code, size_t bytes, VkShaderModule* out)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = bytes, .pCode = code };
    VK_CHECK(pvkCreateShaderModule(s_vk.device, &ci, NULL, out), "vkCreateShaderModule");
    return 0;
}

static int vk_create_pipeline_objects(void)
{
    if (vk_create_shader(rsx_vk_fallback_vert_spv, sizeof rsx_vk_fallback_vert_spv, &s_vk.vs) ||
        vk_create_shader(rsx_vk_fallback_frag_spv, sizeof rsx_vk_fallback_frag_spv, &s_vk.fs))
        return -1;

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_vk.set_layout),
             "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_vk.desc_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_vk.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_vk.desc_set), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_vk.set_layout,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_vk.pipe_layout),
             "vkCreatePipelineLayout");

    /* Nearest + repeat: the null backend's point sample with wrapping. */
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .maxLod = 0.0f };
    VK_CHECK(pvkCreateSampler(s_vk.device, &sci, NULL, &s_vk.sampler), "vkCreateSampler");

    /* A 1x1 magenta dummy keeps the descriptor valid before any bind --
     * the same "not bound" colour the null backend's sampler returns. */
    static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
    if (vk_upload_texture(&s_vk.dummy, magenta, 1, 1)) return -1;
    vk_write_texture_descriptor();
    return 0;
}

/* NV4097 depth function is the GL enum 0x200 NEVER .. 0x207 ALWAYS, which is
 * VkCompareOp 0..7 in the same order. Anything else passes, as in the null
 * and D3D12 backends (including the 1 rsx_state_init seeds). */
static u32 vk_compare_slot(u32 gl_func)
{
    return (gl_func >= 0x200u && gl_func <= 0x207u) ? gl_func - 0x200u
                                                    : (u32)VK_COMPARE_OP_ALWAYS;
}

static VkPipeline vk_get_pipeline(int ztest, int zwrite, u32 cmp)
{
    u32 slot = ((u32)ztest * 2u + (u32)zwrite) * 8u + cmp;
    if (s_vk.pipelines[slot]) return s_vk.pipelines[slot];

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_vk.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_vk.fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(vk_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, pos) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, col) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(vk_vertex, uv)  },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* No culling: the null backend fills both windings. */
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_vk.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, s_vk.pipe_cache, 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) { VK_LOG("vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    s_vk.pipe_cache_built++;
    s_vk.pipelines[slot] = p;
    return p;
}

/* ---------------------------------------------------------------------------
 * Draw path
 * -------------------------------------------------------------------------*/
static void vk_cb_track_state(void* ud, const rsx_state* state)
{
    (void)ud; s_vk.state = state;
}

static int vk_push_vertex(const float pos[4], const float col[4], const float uv[4])
{
    if (s_vk.cpu_count == s_vk.cpu_cap) {
        size_t cap = s_vk.cpu_cap ? s_vk.cpu_cap * 2 : 1024;
        vk_vertex* n = (vk_vertex*)realloc(s_vk.cpu_verts, cap * sizeof *n);
        if (!n) return -1;
        s_vk.cpu_verts = n; s_vk.cpu_cap = cap;
    }
    vk_vertex* v = &s_vk.cpu_verts[s_vk.cpu_count++];
    memcpy(v->pos, pos, sizeof v->pos);
    memcpy(v->col, col, sizeof v->col);
    v->uv[0] = uv[0]; v->uv[1] = uv[1];
    return 0;
}

/* One triangle, colour from its first vertex (the null backend's flat shade). */
static void vk_emit_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    float p[3][4], uv[3][4], col[4];
    rsx_fetch_attrib(st, 0, i0, p[0]);
    rsx_fetch_attrib(st, 0, i1, p[1]);
    rsx_fetch_attrib(st, 0, i2, p[2]);
    rsx_fetch_attrib(st, 3, i0, col);
    rsx_fetch_attrib(st, 8, i0, uv[0]);
    rsx_fetch_attrib(st, 8, i1, uv[1]);
    rsx_fetch_attrib(st, 8, i2, uv[2]);
    for (int k = 0; k < 3; k++) vk_push_vertex(p[k], col, uv[k]);
}

/* Called once per expanded triangle: the fixed-function path records the
 * null backend's flat-shaded vertex, the guest path all sixteen attributes. */
typedef void (*vk_tri_fn)(const rsx_state* st, u32 i0, u32 i1, u32 i2);

/* Primitive expansion, identical to the null backend's draw_prim(). */
static void vk_expand(const rsx_state* st, u32 prim, u32 first, u32 count, vk_tri_fn emit)
{
    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3) emit(st, first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            emit(st, first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++) emit(st, first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            emit(st, first + i, first + i + 1, first + i + 2);
            emit(st, first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            emit(st, first + i, first + i + 1, first + i + 3);
            emit(st, first + i, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines: no coverage, as in the null backend */
    }
}

static int vk_ensure_vertex_buffer(VkDeviceSize bytes)
{
    if (s_vk.vertices.buf && s_vk.vertices.size >= bytes) return 0;
    VkDeviceSize size = s_vk.vertices.size ? s_vk.vertices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_vk.vertices);   /* idle: every submission is waited on */
    if (vk_create_host_buffer(&s_vk.vertices, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return -1;
    if (!s_vk.vertices.coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest programs (stage V3)
 *
 * With the HLSL -> SPIR-V translator built and PS3RECOMP_VK_GUEST_PROGRAMS=1,
 * a draw runs the guest's own vertex and fragment programs, as the Metal
 * backend's guest path does and keyed the same way: decompiled to HLSL by the
 * shared decompilers, translated by rsx_shader_spirv.cpp, cached per program.
 * Constants use the Metal/D3D12 layout: VPConst is the 512 transform
 * constants plus the viewport epilogue, PSConstants the fragment program's
 * inline constants plus fp_alpha.
 *
 * Vertices are still fetched and expanded on the CPU through the shared
 * rsx_fetch_attrib(), now all sixteen attributes (a disabled one reads its
 * constant register, as on the hardware). Every RSX vertex format therefore
 * arrives as float4, and the pipeline has one fixed input layout: location n
 * is attribute n, which is what the vertex decompiler declares.
 *
 * A program that needs a cube map or a vertex texture falls back to the
 * fixed-function path, with a message. Not honoured on this path yet: the RSX
 * viewport and scissor (the whole target is used, as on the fallback path),
 * blending, stencil, culling, per-unit sampler state.
 * -------------------------------------------------------------------------*/
#define VK_VP_CACHE      256
#define VK_FP_CACHE      512
#define VK_GPIPE_CACHE   512
#define VK_FP_MAX_BYTES  4096u              /* bound on a fragment program, as Metal/D3D12 */
#define VK_HLSL_BYTES    (256u * 1024u)
#define VK_SPV_WORDS     (256u * 1024u)
#define VK_VPCONST_BYTES ((RSX_MAX_VERTEX_CONSTANTS + 2u) * 16u)
#define VK_GUEST_ATTRS   16u
#define VK_GUEST_FLOATS  (VK_GUEST_ATTRS * 4u)   /* per vertex */

typedef struct { u32 hash; VkShaderModule mod; } vk_vp_entry;
typedef struct { u64 key; VkShaderModule mod; u32 nconst; } vk_fp_entry;
typedef struct { int vs, fs; u32 depth_key; VkPipeline pipe; } vk_gpipe_entry;

static struct {
    int                   on;
    char*                 hlsl;
    u32*                  spv;
    char                  log[4096];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    vk_buffer             ubo;              /* VPConst at 0, PSConstants at ps_off */
    VkDeviceSize          ps_off;
    VkDeviceSize          max_ubo_range;
    vk_buffer             vertices;
    float*                cpu;
    size_t                cpu_cap, cpu_count;   /* in vertices */
    vk_vp_entry           vp[VK_VP_CACHE];     u32 vp_count;
    vk_fp_entry           fp[VK_FP_CACHE];     u32 fp_count;
    vk_gpipe_entry        pipes[VK_GPIPE_CACHE]; u32 pipe_count;
    rsx_fp_constant_block consts;
    u32                   draws, last_draws;
    u32                   warned;
} s_g;

int rsx_vulkan_backend_guest_programs(void)
{
    const char* v = getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    return rsx_hlsl_to_spirv_available() && v && v[0] && v[0] != '0';
}

static u32 vk_fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 vk_fnv1a64(const void* d, size_t n, u64 h)
{
    const u8* p = (const u8*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* Built from the registers, as the Metal and D3D12 backends do. */
static u32 vk_cube_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
        if ((t->control0 & 0x80000000u) && (t->format & 4u) && w && w == h) m |= 1u << u;
    }
    return m;
}

static u32 vk_vtex_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u) m |= 1u << u;
    return m;
}

static VkShaderModule vk_guest_module(const char* what, int stage)
{
    u32 n = 0;
    if (rsx_hlsl_to_spirv(s_g.hlsl, stage, s_g.spv, VK_SPV_WORDS, &n,
                          s_g.log, sizeof s_g.log) != 0) {
        VK_LOG("%s: %s\n", what, s_g.log);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = s_g.spv };
    VkShaderModule m = VK_NULL_HANDLE;
    if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) != VK_SUCCESS) {
        VK_LOG("%s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    return m;
}

/* The vertex program, keyed on its own bytes to its end bit (as Metal). */
static int vk_vp_slot(const rsx_state* st)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    const u32 hash = vk_fnv1a32(uc, len);
    for (u32 i = 0; i < s_g.vp_count; i++)
        if (s_g.vp[i].hash == hash) return s_g.vp[i].mod ? (int)i : -1;
    if (s_g.vp_count >= VK_VP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "vertex program %08X", hash);
    VkShaderModule m = VK_NULL_HANDLE;
    const int ni = rsx_vp_decompile_ex(uc, len, 0, s_g.hlsl, VK_HLSL_BYTES);
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_VERTEX);
    if (s_g.vp_count < 32)
        VK_LOG("%s: %d instrs -> %s\n", what, ni, m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.vp_count++;
    s_g.vp[slot].hash = hash;
    s_g.vp[slot].mod  = m;
    return m ? slot : -1;
}

/* The fragment program, resolved and keyed as the Metal backend's
 * fp_slot_for: structure (inline constants live in PSConstants), export
 * width, and the alpha test patched into the source. No cube units here --
 * the caller has already sent those to the fixed-function path. */
static int vk_fp_slot(const rsx_state* st, const u8** uc)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;
    u64 key = rsx_fp_structural_hash(*uc, VK_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                       /* malformed or unterminated */
    const u32 ctrl     = st->shader_control;
    const u32 ctrl_key = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn = alpha_en ? st->alpha_func : 0u;
    key = vk_fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = vk_fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = vk_fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    for (u32 i = 0; i < s_g.fp_count; i++)
        if (s_g.fp[i].key == key) return s_g.fp[i].mod ? (int)i : -1;
    if (s_g.fp_count >= VK_FP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "fragment program %016llX", (unsigned long long)key);
    VkShaderModule m = VK_NULL_HANDLE;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, VK_FP_MAX_BYTES, ctrl, 0,
                                          s_g.hlsl, VK_HLSL_BYTES, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_g.hlsl, VK_HLSL_BYTES, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_FRAGMENT);
    if (s_g.fp_count < 32)
        VK_LOG("%s: %d instrs, %u constants -> %s\n", what, ni, nconst,
               m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.fp_count++;
    s_g.fp[slot].key    = key;
    s_g.fp[slot].mod    = m;
    s_g.fp[slot].nconst = nconst;
    return m ? slot : -1;
}

static VkPipeline vk_guest_pipeline(int vs, int fs, int ztest, int zwrite, u32 cmp)
{
    const u32 dkey = (u32)ztest | ((u32)zwrite << 1) | (cmp << 2);
    for (u32 i = 0; i < s_g.pipe_count; i++)
        if (s_g.pipes[i].vs == vs && s_g.pipes[i].fs == fs && s_g.pipes[i].depth_key == dkey)
            return s_g.pipes[i].pipe;
    if (s_g.pipe_count >= VK_GPIPE_CACHE) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_g.vp[vs].mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_g.fp[fs].mod, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, VK_GUEST_FLOATS * 4u, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) {
        va[a].location = a;
        va[a].binding  = 0;
        va[a].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[a].offset   = a * 16u;
    }
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = VK_GUEST_ATTRS, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_g.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, s_vk.pipe_cache, 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) {
        VK_LOG("guest pipeline: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
        return VK_NULL_HANDLE;
    }
    s_vk.pipe_cache_built++;
    vk_gpipe_entry* e = &s_g.pipes[s_g.pipe_count++];
    e->vs = vs; e->fs = fs; e->depth_key = dkey; e->pipe = p;
    return p;
}

/* One vertex: all sixteen attributes as float4, attribute n at float 4n. */
static int vk_guest_push(const rsx_state* st, u32 idx)
{
    if (s_g.cpu_count == s_g.cpu_cap) {
        size_t cap = s_g.cpu_cap ? s_g.cpu_cap * 2 : 1024;
        float* n = (float*)realloc(s_g.cpu, cap * VK_GUEST_FLOATS * sizeof(float));
        if (!n) return -1;
        s_g.cpu = n; s_g.cpu_cap = cap;
    }
    float* v = s_g.cpu + s_g.cpu_count * VK_GUEST_FLOATS;
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) rsx_fetch_attrib(st, (int)a, idx, v + a * 4u);
    s_g.cpu_count++;
    return 0;
}

static void vk_guest_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    vk_guest_push(st, i0);
    vk_guest_push(st, i1);
    vk_guest_push(st, i2);
}

static int vk_guest_init(void)
{
    if (!rsx_vulkan_backend_guest_programs()) return 0;
    s_g.hlsl = (char*)malloc(VK_HLSL_BYTES);
    s_g.spv  = (u32*)malloc(VK_SPV_WORDS * sizeof(u32));
    if (!s_g.hlsl || !s_g.spv) { VK_LOG("guest programs: out of memory\n"); return -1; }

    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_g.max_ubo_range = p.limits.maxUniformBufferRange;
    VkDeviceSize align = p.limits.minUniformBufferOffsetAlignment;
    if (align == 0) align = 256;
    s_g.ps_off = (VK_VPCONST_BYTES + align - 1u) / align * align;
    if (VK_VPCONST_BYTES > s_g.max_ubo_range) {
        VK_LOG("guest programs: maxUniformBufferRange %llu < VPConst\n",
               (unsigned long long)s_g.max_ubo_range);
        return -1;
    }

    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[4 + 2 * RSX_BE_MAX_VERTEX_TEXTURES];
    b[0] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, NULL };
    b[1] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, NULL };
    b[2] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                           RSX_MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[3] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER,
                                           RSX_MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (u32 v = 0; v < RSX_BE_MAX_VERTEX_TEXTURES; v++) {
        b[4 + v] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_VTEX_BINDING + v,
                                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                   VK_SHADER_STAGE_VERTEX_BIT, NULL };
        b[4 + RSX_BE_MAX_VERTEX_TEXTURES + v] = (VkDescriptorSetLayoutBinding){
                                                   RSX_SPIRV_VSAMPLER_BINDING + v,
                                                   VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_VERTEX_BIT, NULL };
    }
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4 + 2 * RSX_BE_MAX_VERTEX_TEXTURES, .pBindings = b };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_g.set_layout),
             "vkCreateDescriptorSetLayout(guest)");
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES + RSX_BE_MAX_VERTEX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES + RSX_BE_MAX_VERTEX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_g.pool),
             "vkCreateDescriptorPool(guest)");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_g.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_g.set), "vkAllocateDescriptorSets(guest)");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_g.pipe_layout),
             "vkCreatePipelineLayout(guest)");
    if (vk_create_host_buffer(&s_g.ubo,
                              s_g.ps_off + (RSX_FP_MAX_INLINE_CONSTANTS + 4u) * 16u,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_g.ubo.coherent) {
        VK_LOG("guest programs: no host-coherent uniform memory\n");
        return -1;
    }
    s_g.on = 1;
    VK_LOG("guest programs: on (HLSL -> SPIR-V through glslang)\n");
    return 0;
}

static void vk_guest_shutdown(void)
{
    if (s_vk.device) {
        for (u32 i = 0; i < s_g.pipe_count; i++)
            if (s_g.pipes[i].pipe) pvkDestroyPipeline(s_vk.device, s_g.pipes[i].pipe, NULL);
        for (u32 i = 0; i < s_g.vp_count; i++)
            if (s_g.vp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.vp[i].mod, NULL);
        for (u32 i = 0; i < s_g.fp_count; i++)
            if (s_g.fp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.fp[i].mod, NULL);
        if (s_g.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_g.pipe_layout, NULL);
        if (s_g.pool)        pvkDestroyDescriptorPool(s_vk.device, s_g.pool, NULL);
        if (s_g.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_g.set_layout, NULL);
        vk_destroy_buffer(&s_g.ubo);
        vk_destroy_buffer(&s_g.vertices);
    }
    free(s_g.hlsl);
    free(s_g.spv);
    free(s_g.cpu);
    memset(&s_g, 0, sizeof s_g);
}

static int vk_ensure_buffer(vk_buffer* b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= bytes) return 0;
    VkDeviceSize size = b->size ? b->size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(b);                 /* idle: every submission is waited on */
    if (vk_create_host_buffer(b, size, usage)) return -1;
    if (!b->coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}


/* Returns 1 when the draw went through the guest programs (or had nothing to
 * draw), 0 when the caller must use the fixed-function path instead. */
static int vk_guest_draw(const rsx_state* st, u32 prim, u32 first, u32 count)
{
    if (!s_g.on) return 0;
    if (vk_cube_mask(st) || vk_vtex_mask(st)) {
        if (!(s_g.warned & 1u)) {
            VK_LOG("guest programs: cube maps and vertex textures are not supported "
                   "yet; those draws use the fixed-function path\n");
            s_g.warned |= 1u;
        }
        return 0;
    }
    const int vs = vk_vp_slot(st);
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = vk_fp_slot(st, &fp_uc);
    if (fs < 0) return 0;
    /* The constant count must be what the program was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, VK_FP_MAX_BYTES, &s_g.consts) < 0 ||
        s_g.consts.count != s_g.fp[fs].nconst)
        return 0;
    const u32 nslots = s_g.consts.count ? s_g.consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;
    if (fp_len > s_g.max_ubo_range) return 0;

    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_guest_pipeline(vs, fs, ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return 0;

    s_g.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_guest_tri);
    if (!s_g.cpu_count) return 1;
    const VkDeviceSize bytes = (VkDeviceSize)s_g.cpu_count * VK_GUEST_FLOATS * sizeof(float);
    if (vk_ensure_buffer(&s_g.vertices, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    memcpy(s_g.vertices.ptr, s_g.cpu, (size_t)bytes);

    /* VPConst: the 512 transform constants, then the viewport epilogue as the
     * Metal and D3D12 backends write it -- x/y identity, and z remapped from
     * GL clip space to [0, 1] when the guest programmed a z scale. */
    u8* vpc = (u8*)s_g.ubo.ptr;
    memcpy(vpc, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16u);
    float* vpx = (float*)(vpc + RSX_MAX_VERTEX_CONSTANTS * 16u);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }
    /* PSConstants: inline constants as host-order bit patterns, then fp_alpha. */
    u8* fpc = (u8*)s_g.ubo.ptr + s_g.ps_off;
    memset(fpc, 0, fp_len);
    if (s_g.consts.count) memcpy(fpc, s_g.consts.values, s_g.consts.count * 16u);
    float* alpha = (float*)(fpc + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);

    /* Descriptors: rewritten per draw, which is safe because every submission
     * is waited on before the next one is recorded. */
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, VK_VPCONST_BYTES },
        { s_g.ubo.buf, s_g.ps_off, fp_len },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         s_vk.tex_ready[u] ? s_vk.tex[u].view : s_vk.dummy.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ s_vk.tex_sampler[u] ? s_vk.tex_sampler[u] : s_vk.sampler,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);

    if (vk_begin()) return 0;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vpt = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vpt);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    const u32 dyn_zero[2] = { 0u, 0u };
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                             0, 1, &s_g.set, 2, dyn_zero);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_g.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_g.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) return 0;
    s_g.draws++;
    return 1;
}

static void vk_draw(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_vk.state;
    if (!st || !s_vk.device || count < 3) return;
    if (vk_guest_draw(st, prim, first, count)) return;

    s_vk.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_emit_tri);
    if (!s_vk.cpu_count) return;

    const VkDeviceSize bytes = (VkDeviceSize)s_vk.cpu_count * sizeof(vk_vertex);
    if (vk_ensure_vertex_buffer(bytes)) return;
    memcpy(s_vk.vertices.ptr, s_vk.cpu_verts, (size_t)bytes);

    /* Depth writes need the test enabled, as on the hardware and in both
     * reference backends. */
    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_get_pipeline(ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return;
    const int32_t textured = (s_vk.tex_ready[0] && st->vertex_attribs[8].enabled) ? 1 : 0;

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    /* Whole target, as the null backend's ndc_to_px maps it. */
    VkViewport vp = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipe_layout,
                             0, 1, &s_vk.desc_set, 0, NULL);
    pvkCmdPushConstants(s_vk.cmd, s_vk.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof textured, &textured);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_vk.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

static void vk_cb_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    vk_draw(primitive, first, count);
}

static void vk_cb_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* Not read yet, as in the null backend: the harness geometry is
     * non-indexed. Real titles need this; it lands with the index fetch. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

/* ---------------------------------------------------------------------------
 * Clear and present
 * -------------------------------------------------------------------------*/
static void vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)stencil;                       /* no stencil buffer yet */
    s_vk.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the colour channels
     * (all or nothing, as in the null backend). */
    const int do_color = (flags & 0xF0u) != 0;
    const int do_depth = (flags & 0x01u) != 0;
    if (!s_vk.device || (!do_color && !do_depth)) return;

    if (vk_begin()) return;
    if (do_color) {
        VkClearColorValue v;
        v.float32[0] = (float)((color >> 16) & 0xFFu) / 255.0f;   /* R */
        v.float32[1] = (float)((color >>  8) & 0xFFu) / 255.0f;   /* G */
        v.float32[2] = (float)( color        & 0xFFu) / 255.0f;   /* B */
        v.float32[3] = (float)((color >> 24) & 0xFFu) / 255.0f;   /* A */
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    if (do_depth) {
        VkClearDepthStencilValue d = { depth, 0 };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    }
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Optional window: PS3RECOMP_VK_WINDOW=1
 *
 * An SDL2 window with a Vulkan swapchain. Rendering still targets the
 * offscreen image, so readback and every test are unchanged; a present then
 * also blits that image into the next swapchain image. Any failure on this
 * path drops back to headless with a message instead of failing the run.
 * -------------------------------------------------------------------------*/
static int vk_env_on(const char* name)
{
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}

#ifdef __ANDROID__
typedef void    (*pfn_ANativeWindow_acquire)(ANativeWindow*);
typedef void    (*pfn_ANativeWindow_release)(ANativeWindow*);
typedef int32_t (*pfn_ANativeWindow_getWidth)(ANativeWindow*);
typedef int32_t (*pfn_ANativeWindow_getHeight)(ANativeWindow*);

static void*                        s_android_lib = NULL;
static pfn_ANativeWindow_acquire    p_anw_acquire = NULL;
static pfn_ANativeWindow_release    p_anw_release = NULL;
static pfn_ANativeWindow_getWidth   p_anw_width   = NULL;
static pfn_ANativeWindow_getHeight  p_anw_height  = NULL;

static pthread_mutex_t s_android_win_mu        = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow*  s_android_pending_win   = NULL;
static int             s_android_surface_dirty = 0;
static volatile int    s_android_quit          = 0;
static int             s_vk_resized            = 0;

static void vk_android_ensure_lib(void)
{
    if (s_android_lib) return;
    s_android_lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!s_android_lib) return;
    p_anw_acquire = (pfn_ANativeWindow_acquire)dlsym(s_android_lib, "ANativeWindow_acquire");
    p_anw_release = (pfn_ANativeWindow_release)dlsym(s_android_lib, "ANativeWindow_release");
    p_anw_width   = (pfn_ANativeWindow_getWidth)dlsym(s_android_lib, "ANativeWindow_getWidth");
    p_anw_height  = (pfn_ANativeWindow_getHeight)dlsym(s_android_lib, "ANativeWindow_getHeight");
}

void rsx_vulkan_android_set_window(void* anative_window)
{
    vk_android_ensure_lib();
    ANativeWindow* nw = (ANativeWindow*)anative_window;
    if (nw && p_anw_acquire) p_anw_acquire(nw);
    pthread_mutex_lock(&s_android_win_mu);
    ANativeWindow* old = s_android_pending_win;
    s_android_pending_win = nw;
    s_android_surface_dirty = 1;
    s_vk_resized = 1;
    pthread_mutex_unlock(&s_android_win_mu);
    if (old && p_anw_release) p_anw_release(old);
}

void rsx_vulkan_android_notify_resize(void)
{
    pthread_mutex_lock(&s_android_win_mu);
    s_vk_resized = 1;
    pthread_mutex_unlock(&s_android_win_mu);
}

void rsx_vulkan_android_request_quit(void)
{
    __atomic_store_n(&s_android_quit, 1, __ATOMIC_RELEASE);
}
#endif

static void vk_window_drop(const char* why)
{
    if (why) VK_LOG("window disabled, continuing headless: %s\n", why);
    if (s_vk.device && pvkDeviceWaitIdle) {
        vk_device_idle();
        for (u32 i = 0; i < 8; i++)
            if (s_vk.sc_done[i]) {
                pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
                s_vk.sc_done[i] = VK_NULL_HANDLE;
            }
        if (s_vk.sem_acquire) pvkDestroySemaphore(s_vk.device, s_vk.sem_acquire, NULL);
        if (s_vk.swapchain && pvkDestroySwapchainKHR)
            pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
    }
    if (s_vk.surface && s_vk.instance && pvkDestroySurfaceKHR)
        pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
    if (s_vk.window) {
#ifdef _WIN32
        if (s_vk.fullscreen) ShowCursor(TRUE);
        s_vk.fullscreen = 0;
        DestroyWindow(s_vk.window);
#elif defined(__ANDROID__)
        if (p_anw_release) p_anw_release(s_vk.window);
#else
        SDL_DestroyWindow(s_vk.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif
    }
    s_vk.window = NULL;
    s_vk.surface = VK_NULL_HANDLE;
    s_vk.swapchain = VK_NULL_HANDLE;
    s_vk.sem_acquire = VK_NULL_HANDLE;
    s_vk.sc_count = 0;
    s_vk.windowed = 0;
}

#ifdef _WIN32
static int s_vk_resized;   /* defined here on Windows: the window procedure sets it */

/* Borderless fullscreen on the window's monitor, and back to the placement
 * the window had. */
static void vk_window_set_fullscreen(int on)
{
    if (!s_vk.window || on == s_vk.fullscreen) return;
    if (on) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        s_vk.restore.length = sizeof s_vk.restore;
        if (!GetWindowPlacement(s_vk.window, &s_vk.restore) ||
            !GetMonitorInfoA(MonitorFromWindow(s_vk.window, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        SetWindowLongPtrA(s_vk.window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(s_vk.window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ShowCursor(FALSE);
    } else {
        SetWindowLongPtrA(s_vk.window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(s_vk.window, &s_vk.restore);
        SetWindowPos(s_vk.window, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ShowCursor(TRUE);
    }
    s_vk.fullscreen = on;
    s_vk_resized = 1;
}

static LRESULT CALLBACK vk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        s_vk.window_closed = 1;
        return 0;   /* vk_window_drop destroys it on the backend's own terms */
    case WM_SIZE:
        s_vk_resized = 1;
        return 0;
    case WM_GETMINMAXINFO: {
        RECT r = { 0, 0, 320, 180 };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        ((MINMAXINFO*)lp)->ptMinTrackSize.x = r.right - r.left;
        ((MINMAXINFO*)lp)->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { s_vk.window_closed = 1; return 0; }
        if (wp == VK_F11 && !(lp & (1 << 30))) { vk_window_set_fullscreen(!s_vk.fullscreen); return 0; }
        break;
    case WM_SYSKEYDOWN:   /* Alt+Enter */
        if (wp == VK_RETURN && (lp & (1 << 29)) && !(lp & (1 << 30))) {
            vk_window_set_fullscreen(!s_vk.fullscreen);
            return 0;
        }
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void vk_window_open(const char* title)
{
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = vk_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ps3recomp_vulkan";
    RegisterClassExA(&wc);   /* a second init finds the class already registered */
    RECT wr = { 0, 0, (LONG)s_vk.width, (LONG)s_vk.height };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    /* Keep the display awake, as the D3D12 and null backends do. */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    s_vk.window_closed = 0;
    s_vk.fullscreen = 0;
    s_vk.window = CreateWindowExA(0, "ps3recomp_vulkan",
                                  title && title[0] ? title : "ps3recomp",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                  wr.right - wr.left, wr.bottom - wr.top,
                                  NULL, NULL, GetModuleHandleA(NULL), NULL);
    s_vk.windowed = 1;
    if (!s_vk.window) {
        char why[128];
        snprintf(why, sizeof why, "CreateWindowEx failed (error %lu)", GetLastError());
        vk_window_drop(why);
        return;
    }
    /* PS3RECOMP_FULLSCREEN=1 (--fullscreen), the same for every backend; PS3RECOMP_VK_FULLSCREEN is the older name. */
    if (vk_env_on("PS3RECOMP_FULLSCREEN") || vk_env_on("PS3RECOMP_VK_FULLSCREEN"))
        vk_window_set_fullscreen(1);
    s_vk.inst_exts[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    s_vk.inst_exts[1] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    s_vk.inst_ext_count = 2;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: Win32, %u surface extension(s)\n", s_vk.inst_ext_count);
}

static int vk_create_window_surface(void)
{
    PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)
        pvkGetInstanceProcAddr(s_vk.instance, "vkCreateWin32SurfaceKHR");
    if (!create) return -1;
    VkWin32SurfaceCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                         .hinstance = GetModuleHandleA(NULL), .hwnd = s_vk.window };
    return create(s_vk.instance, &info, NULL, &s_vk.surface) == VK_SUCCESS ? 0 : -1;
}

static void vk_window_drawable_size(int* w, int* h)
{
    RECT r = { 0, 0, 0, 0 };
    GetClientRect(s_vk.window, &r);
    *w = (int)(r.right - r.left); *h = (int)(r.bottom - r.top);
}

static int vk_window_minimized(void) { return IsIconic(s_vk.window) != 0; }

static const char* vk_window_error(void) { return "vkCreateWin32SurfaceKHR failed"; }
#elif defined(__ANDROID__)
static void vk_window_open(const char* title)
{
    (void)title;
    vk_android_ensure_lib();
    __atomic_store_n(&s_android_quit, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&s_android_win_mu);
    ANativeWindow* w = s_android_pending_win;
    if (w && p_anw_acquire) p_anw_acquire(w);
    s_android_surface_dirty = 0;
    s_vk_resized = 0;
    pthread_mutex_unlock(&s_android_win_mu);

    if (!w) return;   /* headless CLI run (no ANativeWindow attached) */

    s_vk.window = w;
    s_vk.windowed = 1;
    s_vk.inst_exts[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    s_vk.inst_exts[1] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    s_vk.inst_ext_count = 2;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: Android ANativeWindow (%dx%d), %u surface extension(s)\n",
           p_anw_width ? (int)p_anw_width(w) : 0,
           p_anw_height ? (int)p_anw_height(w) : 0,
           s_vk.inst_ext_count);
}

static int vk_create_window_surface(void)
{
    if (!s_vk.window) return -1;
    PFN_vkCreateAndroidSurfaceKHR create = (PFN_vkCreateAndroidSurfaceKHR)
        pvkGetInstanceProcAddr(s_vk.instance, "vkCreateAndroidSurfaceKHR");
    if (!create) return -1;
    VkAndroidSurfaceCreateInfoKHR info = {
        .sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .pNext  = NULL,
        .flags  = 0,
        .window = s_vk.window,
    };
    return create(s_vk.instance, &info, NULL, &s_vk.surface) == VK_SUCCESS ? 0 : -1;
}

static void vk_window_drawable_size(int* w, int* h)
{
    if (s_vk.window && p_anw_width && p_anw_height) {
        *w = (int)p_anw_width(s_vk.window);
        *h = (int)p_anw_height(s_vk.window);
    } else {
        *w = 0; *h = 0;
    }
}

static int vk_window_minimized(void) { return s_vk.window == NULL; }

static const char* vk_window_error(void) { return "vkCreateAndroidSurfaceKHR failed"; }
#else
static void vk_window_open(const char* title)
{
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        VK_LOG("window disabled, continuing headless: SDL video init failed: %s\n", SDL_GetError());
        return;
    }
    /* Resizable and maximizable; F11 or Alt+Enter switches to borderless fullscreen and back. The frame keeps its
     * aspect ratio inside whatever size the window has (vk_window_fit). */
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    /* PS3RECOMP_FULLSCREEN=1 (--fullscreen), the same for every backend; PS3RECOMP_VK_FULLSCREEN is the older name. */
    if (vk_env_on("PS3RECOMP_FULLSCREEN") || vk_env_on("PS3RECOMP_VK_FULLSCREEN")) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    s_vk.window = SDL_CreateWindow(title && title[0] ? title : "ps3recomp",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)s_vk.width, (int)s_vk.height, flags);
    s_vk.windowed = 1;          /* so vk_window_drop also shuts SDL video down */
    if (!s_vk.window) {
        char why[256];
        snprintf(why, sizeof why, "SDL_CreateWindow failed: %s", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    SDL_SetWindowMinimumSize(s_vk.window, 320, 180);
    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) SDL_ShowCursor(SDL_DISABLE);
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, NULL) || n == 0 || n > 16 ||
        !SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, s_vk.inst_exts)) {
        char why[256];
        snprintf(why, sizeof why, "no Vulkan surface extensions (%s)", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    s_vk.inst_ext_count = n;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: SDL video driver '%s', %u surface extension(s)\n",
           SDL_GetCurrentVideoDriver(), n);
}

static int vk_create_window_surface(void)
{
    return SDL_Vulkan_CreateSurface(s_vk.window, s_vk.instance, &s_vk.surface) ? 0 : -1;
}

static void vk_window_drawable_size(int* w, int* h)
{
    SDL_Vulkan_GetDrawableSize(s_vk.window, w, h);
}

static int vk_window_minimized(void)
{
    return (SDL_GetWindowFlags(s_vk.window) & SDL_WINDOW_MINIMIZED) != 0;
}

static const char* vk_window_error(void) { return SDL_GetError(); }
#endif

static void vk_scale_destroy_fbs(void);
static void vk_fg_destroy_fbs(void);

/* 0 on success, 1 when the window has no drawable area right now (the old swapchain is kept), -1 on failure. */
static int vk_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VK_LOG("swapchain images cannot be blit destinations\n"); return -1;
    }

    u32 nf = 0;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, NULL),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (nf == 0) { VK_LOG("surface reports no formats\n"); return -1; }
    if (nf > 64) nf = 64;
    VkSurfaceFormatKHR fmts[64];
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, fmts),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    /* UNORM, not SRGB: the offscreen target holds the guest's values as-is. */
    VkSurfaceFormatKHR fmt = fmts[0];
    if (nf == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        static const VkFormat pref[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        int found = 0;
        for (size_t k = 0; k < 2 && !found; k++)
            for (u32 i = 0; i < nf; i++)
                if (fmts[i].format == pref[k]) { fmt = fmts[i]; found = 1; break; }
    }
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VK_LOG("swapchain format %d cannot be a blit destination\n", (int)fmt.format); return -1;
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, hh = 0;
        vk_window_drawable_size(&w, &hh);
        ext.width = (u32)w; ext.height = (u32)hh;
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (!ext.width || !ext.height) return 1;   /* minimized: no drawable area, try again once it is shown */

    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    static const VkCompositeAlphaFlagBitsKHR alphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (size_t k = 0; k < 4; k++)
        if (caps.supportedCompositeAlpha & alphas[k]) { alpha = alphas[k]; break; }

    VkSurfaceTransformFlagBitsKHR pre_transform = caps.currentTransform;
#ifdef __ANDROID__
    /* The offscreen RSX blit writes an upright landscape frame without GPU
     * pre-rotation; pass IDENTITY so SurfaceFlinger/HWC rotates the buffer to
     * match the physical panel instead of applying an inverse 90-degree transform. */
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
#endif

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    {
        const char* pm_env = getenv("PS3RECOMP_VK_PRESENT_MODE");
        const int force_fifo = pm_env && !strcmp(pm_env, "fifo");
        const int want_imm   = pm_env && !strcmp(pm_env, "immediate");
        u32 npm = 0;
        if (!force_fifo && pvkGetPhysicalDeviceSurfacePresentModesKHR &&
            pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &npm, NULL) == VK_SUCCESS && npm > 0) {
            if (npm > 16) npm = 16;
            VkPresentModeKHR pms[16];
            if (pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &npm, pms) == VK_SUCCESS) {
                for (u32 i = 0; i < npm; i++) {
                    if (want_imm && pms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                        present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
                        break;
                    }
                    if (pms[i] == VK_PRESENT_MODE_MAILBOX_KHR)
                        present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
                }
            }
        }
    }

    vk_scale_destroy_fbs();
    vk_fg_destroy_fbs();
    const int can_attach = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                           (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    VkSwapchainKHR old = s_vk.swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s_vk.surface, .minImageCount = count,
        .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      (can_attach ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = pre_transform, .compositeAlpha = alpha,
        .presentMode = present_mode,
        .clipped = VK_TRUE, .oldSwapchain = old,
    };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = pvkCreateSwapchainKHR(s_vk.device, &sci, NULL, &sc);
    if (old) pvkDestroySwapchainKHR(s_vk.device, old, NULL);
    s_vk.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) { VK_LOG("vkCreateSwapchainKHR failed (VkResult %d)\n", (int)r); return -1; }
    s_vk.swapchain = sc;

    u32 n = 0;
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, NULL), "vkGetSwapchainImagesKHR");
    if (n == 0 || n > 8) { VK_LOG("unsupported swapchain image count %u\n", n); return -1; }
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, s_vk.sc_images), "vkGetSwapchainImagesKHR");

    for (u32 i = 0; i < 8; i++)
        if (s_vk.sc_done[i]) {
            pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
            s_vk.sc_done[i] = VK_NULL_HANDLE;
        }
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < n; i++)
        VK_CHECK(pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sc_done[i]), "vkCreateSemaphore");

    s_vk.sc_extent = ext;
    s_vk.sc_format = fmt.format;
    s_vk.sc_can_attach = can_attach;
    s_vk.sc_count = n;
    VK_LOG("window swapchain %ux%u, %u images, format %d, presentMode %d, transform 0x%x (panel 0x%x)\n",
           ext.width, ext.height, n, (int)fmt.format, (int)present_mode,
           (unsigned)pre_transform, (unsigned)caps.currentTransform);
    return 0;
}

/* The window changed size (or fullscreen state) since the swapchain was made: it is recreated before the next acquire.
 * Also set while the window is minimized, when there is no drawable area to make one for. (On Windows it is defined
 * with the window procedure, which sets it.) */
#if !defined(_WIN32) && !defined(__ANDROID__)
static int s_vk_resized;
#endif

/* Minimized, or no drawable area: nothing can be presented, and a swapchain cannot be created for it. */
static int vk_window_hidden(void)
{
    int w = 0, h = 0;
    if (vk_window_minimized()) return 1;
    vk_window_drawable_size(&w, &h);
    return w <= 0 || h <= 0;
}

static void vk_swapchain_lost(void)
{
#ifdef __ANDROID__
    pthread_mutex_lock(&s_android_win_mu);
    const int dirty = s_android_surface_dirty;
    ANativeWindow* next_win = NULL;
    if (dirty) {
        next_win = s_android_pending_win;
        if (next_win && p_anw_acquire) p_anw_acquire(next_win);
        s_android_surface_dirty = 0;
    }
    pthread_mutex_unlock(&s_android_win_mu);

    if (dirty) {
        vk_device_idle();
        vk_scale_destroy_fbs();
    vk_fg_destroy_fbs();
        if (s_vk.swapchain && pvkDestroySwapchainKHR) {
            pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
            s_vk.swapchain = VK_NULL_HANDLE;
            s_vk.sc_count = 0;
        }
        if (s_vk.surface && pvkDestroySurfaceKHR) {
            pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
            s_vk.surface = VK_NULL_HANDLE;
        }
        if (s_vk.window && p_anw_release) p_anw_release(s_vk.window);
        s_vk.window = next_win;
        if (!s_vk.window) {
            s_vk_resized = 1;
            return;   /* app backgrounded: wait until a fresh ANativeWindow arrives */
        }
        if (vk_create_window_surface() != 0) {
            vk_window_drop("vkCreateAndroidSurfaceKHR failed on resume");
            return;
        }
    }
#endif
    if (vk_window_hidden()) { s_vk_resized = 1; return; }   /* recreated once it is shown again */
    vk_device_idle();
    const int r = vk_create_swapchain();
    s_vk_resized = r > 0;     /* the surface is 0x0 before SDL reports the window minimized */
    if (r < 0) vk_window_drop("the swapchain could not be recreated");
}

/* ---------------------------------------------------------------------------
 * GPU Lossless Scaling Present Pass (PS3RECOMP_VK_SCALING = fsr|cas|integer|linear)
 *
 * Runs directly into the swapchain image in a single fullscreen-triangle pass:
 *   - fsr     : 12-tap edge-directed FSR1-style EASU spatial upscaler + RCAS
 *               contrast-adaptive sharpening (clamped against ringing/halos).
 *   - cas     : 5-tap AMD FidelityFX Contrast Adaptive Sharpening over bilinear.
 *   - integer : Pixel-exact integer nearest-neighbor scaling (zero blur).
 *   - linear  : Standard hardware bilinear blit.
 * -------------------------------------------------------------------------*/
enum {
    VK_SCALE_LINEAR  = 0,
    VK_SCALE_INTEGER = 1,
    VK_SCALE_CAS     = 2,
    VK_SCALE_FSR     = 3,
    VK_SCALE_SGSR    = 4,
};

static struct {
    int                   mode;       /* -1 = unread */
    int                   pipe_tried;
    int                   pipe_ready;
    VkFormat              rp_fmt;
    VkRenderPass          rp;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout      layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    VkSampler             sampler;
    vk_buffer             ubo;
    VkShaderModule        vs, fs;
    VkPipeline            pipe;
    VkImageView           sc_views[8];
    VkFramebuffer         sc_fbs[8];
    VkExtent2D            fb_ext;
    u32                   fb_count;
} s_scale = { -1 };

static int vk_scale_mode(void)
{
    if (s_scale.mode < 0) {
        const char* e = getenv("PS3RECOMP_VK_SCALING");
        if (!e || !*e || !strcmp(e, "linear"))        s_scale.mode = VK_SCALE_LINEAR;
        else if (!strcmp(e, "integer") || !strcmp(e, "nearest")) s_scale.mode = VK_SCALE_INTEGER;
        else if (!strcmp(e, "cas"))                   s_scale.mode = VK_SCALE_CAS;
        else if (!strcmp(e, "fsr"))                   s_scale.mode = VK_SCALE_FSR;
        else if (!strcmp(e, "sgsr"))                  s_scale.mode = VK_SCALE_SGSR;
        else                                          s_scale.mode = VK_SCALE_LINEAR;
        VK_LOG("present scaling mode: %s\n",
               s_scale.mode == VK_SCALE_SGSR ? "Qualcomm SGSR (Snapdragon Super Resolution)" :
               s_scale.mode == VK_SCALE_FSR  ? "Lossless Scaling (FSR1 + RCAS)" :
               s_scale.mode == VK_SCALE_CAS  ? "FidelityFX CAS" :
               s_scale.mode == VK_SCALE_INTEGER ? "Integer Nearest" : "Bilinear");
    }
    return s_scale.mode;
}

static void vk_scale_destroy_fbs(void)
{
    if (!s_vk.device) return;
    for (u32 i = 0; i < 8; i++) {
        if (s_scale.sc_fbs[i])   { pvkDestroyFramebuffer(s_vk.device, s_scale.sc_fbs[i], NULL); s_scale.sc_fbs[i] = VK_NULL_HANDLE; }
        if (s_scale.sc_views[i]) { pvkDestroyImageView(s_vk.device, s_scale.sc_views[i], NULL); s_scale.sc_views[i] = VK_NULL_HANDLE; }
    }
    s_scale.fb_count = 0;
    s_scale.fb_ext.width = s_scale.fb_ext.height = 0;
}

static void vk_scale_shutdown(void)
{
    if (!s_vk.device) return;
    vk_scale_destroy_fbs();
    if (s_scale.pipe)    pvkDestroyPipeline(s_vk.device, s_scale.pipe, NULL);
    if (s_scale.vs)      pvkDestroyShaderModule(s_vk.device, s_scale.vs, NULL);
    if (s_scale.fs)      pvkDestroyShaderModule(s_vk.device, s_scale.fs, NULL);
    if (s_scale.rp)      pvkDestroyRenderPass(s_vk.device, s_scale.rp, NULL);
    if (s_scale.pool)    pvkDestroyDescriptorPool(s_vk.device, s_scale.pool, NULL);
    if (s_scale.layout)  pvkDestroyPipelineLayout(s_vk.device, s_scale.layout, NULL);
    if (s_scale.dsl)     pvkDestroyDescriptorSetLayout(s_vk.device, s_scale.dsl, NULL);
    if (s_scale.sampler) pvkDestroySampler(s_vk.device, s_scale.sampler, NULL);
    vk_destroy_buffer(&s_scale.ubo);
    memset(&s_scale, 0, sizeof s_scale);
    s_scale.mode = -1;
}

static const char* const s_scale_vs_hlsl =
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut main(uint vid : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    float2 uv = float2((vid == 1u) ? 2.0 : 0.0, (vid == 2u) ? 2.0 : 0.0);\n"
    "    o.uv = uv;\n"
    "    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

static const char* const s_scale_fs_hlsl =
    "cbuffer ScaleParams : register(b0) {\n"
    "    float4 src_info; /* (src_w, src_h, inv_w, inv_h) */\n"
    "    float4 cfg;      /* (sharpness, mode, 0, 0) */\n"
    "};\n"
    "Texture2D src_tex : register(t0);\n"
    "SamplerState src_samp : register(s0);\n"
    "float luma3(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float3 sample_c(float2 uv) { return src_tex.SampleLevel(src_samp, uv, 0.0).rgb; }\n"
    "float sgsr_lanczos2(float x) {\n"
    "    x = x - 4.0;\n"
    "    float x2 = x * x;\n"
    "    float y = (x2 - 1.0) * (x2 * (2.0 / 5.0) - 1.0);\n"
    "    return y * ((25.0 / 16.0) * x2 - (25.0 / 16.0 - 1.0));\n"
    "}\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    float2 inv = src_info.zw;\n"
    "    float sharp = cfg.x;\n"
    "    if (cfg.y > 2.5) {\n"
    "        /* Mode 4: Qualcomm Snapdragon Game Super Resolution (SGSR) single-pass */\n"
    "        float2 pp = uv * src_info.xy - 0.5;\n"
    "        float2 fp = frac(pp);\n"
    "        float2 tc = (floor(pp) + 0.5) * inv;\n"
    "        float3 tl = sample_c(tc);\n"
    "        float3 tr = sample_c(tc + float2(inv.x, 0.0));\n"
    "        float3 bl = sample_c(tc + float2(0.0, inv.y));\n"
    "        float3 br = sample_c(tc + inv);\n"
    "        float3 bil = lerp(lerp(tl, tr, fp.x), lerp(bl, br, fp.x), fp.y);\n"
    "        float l_tl = luma3(tl), l_tr = luma3(tr), l_bl = luma3(bl), l_br = luma3(br);\n"
    "        float edge_h = abs((l_tr - l_tl) + (l_br - l_bl));\n"
    "        float edge_v = abs((l_bl - l_tl) + (l_br - l_tr));\n"
    "        float edge = edge_h + edge_v;\n"
    "        if (edge < 0.015) return float4(bil, 1.0);\n"
    "        float3 lft = sample_c(tc + float2(-inv.x, 0.0));\n"
    "        float3 rgt = sample_c(tc + float2(2.0 * inv.x, 0.0));\n"
    "        float3 top = sample_c(tc + float2(0.0, -inv.y));\n"
    "        float3 bot = sample_c(tc + float2(0.0, 2.0 * inv.y));\n"
    "        float2 dir = float2(edge_v - edge_h, edge_h - edge_v);\n"
    "        float dir_len = max(abs(dir.x), abs(dir.y)) + 1e-4;\n"
    "        float2 aniso = saturate(float2(edge_h, edge_v) / (edge + 1e-4));\n"
    "        float3 ring_min = min(min(tl, tr), min(bl, br));\n"
    "        float3 ring_max = max(max(tl, tr), max(bl, br));\n"
    "        float3 avg_nb = 0.25 * (lft + rgt + top + bot);\n"
    "        float3 detail = (bil - avg_nb) * (0.45 + 0.55 * sharp) * saturate(edge * 4.0);\n"
    "        float3 steer = lerp(0.5 * (tl + br), 0.5 * (tr + bl), step(edge_h, edge_v));\n"
    "        float3 out_c = clamp(bil + detail + (steer - bil) * (0.12 * aniso.x * aniso.y), ring_min * 0.92, ring_max * 1.08);\n"
    "        return float4(saturate(out_c), 1.0);\n"
    "    }\n"
    "    if (cfg.y < 1.5) {\n"
    "        /* Mode 2: AMD FidelityFX CAS (5-tap contrast adaptive sharpening) */\n"
    "        float3 e = sample_c(uv);\n"
    "        float3 b = sample_c(uv + float2(0.0, -inv.y));\n"
    "        float3 d = sample_c(uv + float2(-inv.x, 0.0));\n"
    "        float3 f = sample_c(uv + float2( inv.x, 0.0));\n"
    "        float3 h = sample_c(uv + float2(0.0,  inv.y));\n"
    "        float3 mn = min(e, min(min(b, d), min(f, h)));\n"
    "        float3 mx = max(e, max(max(b, d), max(f, h)));\n"
    "        float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));\n"
    "        float peak = -1.0 / lerp(8.0, 5.0, sharp);\n"
    "        float3 w = amp * peak;\n"
    "        float3 out_c = saturate((e + w * (b + d + f + h)) / (1.0 + 4.0 * w));\n"
    "        return float4(out_c, 1.0);\n"
    "    }\n"
    "    /* Mode 3: Lossless Scaling FSR1-style 12-tap edge-directed EASU + RCAS */\n"
    "    float2 pp = uv * src_info.xy - 0.5;\n"
    "    float2 fp = frac(pp);\n"
    "    float2 tc = (floor(pp) + 0.5) * inv;\n"
    "    float3 f = sample_c(tc);\n"
    "    float3 g = sample_c(tc + float2(inv.x, 0.0));\n"
    "    float3 j = sample_c(tc + float2(0.0, inv.y));\n"
    "    float3 k = sample_c(tc + inv);\n"
    "    float3 b = sample_c(tc + float2(0.0, -inv.y));\n"
    "    float3 c = sample_c(tc + float2(inv.x, -inv.y));\n"
    "    float3 e = sample_c(tc + float2(-inv.x, 0.0));\n"
    "    float3 h = sample_c(tc + float2(2.0 * inv.x, 0.0));\n"
    "    float3 i = sample_c(tc + float2(-inv.x, inv.y));\n"
    "    float3 l = sample_c(tc + float2(2.0 * inv.x, inv.y));\n"
    "    float3 n = sample_c(tc + float2(0.0, 2.0 * inv.y));\n"
    "    float3 o = sample_c(tc + float2(inv.x, 2.0 * inv.y));\n"
    "    float lf = luma3(f), lg = luma3(g), lj = luma3(j), lk = luma3(k);\n"
    "    float gx = abs((lg - lf) + (lk - lj)) + 0.25 * abs((luma3(h) - lf) + (lg - luma3(e)));\n"
    "    float gy = abs((lj - lf) + (lk - lg)) + 0.25 * abs((luma3(n) - lf) + (lj - luma3(b)));\n"
    "    float aniso = saturate(abs(gx - gy) / (gx + gy + 1e-4));\n"
    "    float2 dir = (gx > gy) ? float2(1.0 + 0.65 * aniso, 1.0 - 0.35 * aniso)\n"
    "                           : float2(1.0 - 0.35 * aniso, 1.0 + 0.65 * aniso);\n"
    "    /* Anti-ringing Lanczos-like edge-steered reconstruction over 12-tap kernel */\n"
    "    float2 f2 = fp * fp;\n"
    "    float2 f3 = f2 * fp;\n"
    "    float2 s_x = float2(1.0 - fp.x, fp.x);\n"
    "    float2 s_y = float2(1.0 - fp.y, fp.y);\n"
    "    float lobe = -0.32 * (0.65 + 0.35 * aniso);\n"
    "    float wx0 = lobe * fp.x * (1.0 - fp.x) * (1.0 - fp.x) * dir.x;\n"
    "    float wx3 = lobe * fp.x * fp.x * (1.0 - fp.x) * dir.x;\n"
    "    float wy0 = lobe * fp.y * (1.0 - fp.y) * (1.0 - fp.y) * dir.y;\n"
    "    float wy3 = lobe * fp.y * fp.y * (1.0 - fp.y) * dir.y;\n"
    "    float3 r1 = e * wx0 + f * (s_x.x - wx0) + g * (s_x.y - wx3) + h * wx3;\n"
    "    float3 r2 = i * wx0 + j * (s_x.x - wx0) + k * (s_x.y - wx3) + l * wx3;\n"
    "    float3 r0 = b * s_x.x + c * s_x.y;\n"
    "    float3 r3 = n * s_x.x + o * s_x.y;\n"
    "    float3 easu = r0 * wy0 + r1 * (s_y.x - wy0) + r2 * (s_y.y - wy3) + r3 * wy3;\n"
    "    float3 mn4 = min(min(f, g), min(j, k));\n"
    "    float3 mx4 = max(max(f, g), max(j, k));\n"
    "    easu = clamp(easu, mn4, mx4);\n"
    "    /* Single-pass RCAS contrast-adaptive sharpening */\n"
    "    float3 nb_avg = 0.25 * (f + g + j + k);\n"
    "    float3 hi_det = easu - nb_avg;\n"
    "    float3 ring_mn = min(mn4, min(min(b, e), min(h, n)));\n"
    "    float3 ring_mx = max(mx4, max(max(c, l), max(i, o)));\n"
    "    float3 amp = sqrt(saturate(min(ring_mn, 1.0 - ring_mx) / max(ring_mx, 1e-5)));\n"
    "    float3 out_c = saturate(easu + hi_det * (amp * (1.35 * sharp)) +\n"
    "                            (easu - 0.25 * (r0 + r3 + e * s_y.x + i * s_y.y + h * s_y.x + l * s_y.y)) * (amp * 0.28 * sharp));\n"
    "    return float4(out_c, 1.0);\n"
    "}\n";

static int vk_scale_ensure(void)
{
    const int mode = vk_scale_mode();
    if (mode != VK_SCALE_FSR && mode != VK_SCALE_CAS && mode != VK_SCALE_SGSR) return 0;
    if (!s_vk.sc_can_attach || !s_vk.color.view || !s_vk.sc_count) return 0;

    /* Rebuild render pass + pipeline if swapchain format changed or first init. */
    if (s_scale.pipe_ready && s_scale.rp_fmt != s_vk.sc_format) {
        vk_scale_destroy_fbs();
        if (s_scale.pipe) { pvkDestroyPipeline(s_vk.device, s_scale.pipe, NULL); s_scale.pipe = VK_NULL_HANDLE; }
        if (s_scale.rp)   { pvkDestroyRenderPass(s_vk.device, s_scale.rp, NULL); s_scale.rp = VK_NULL_HANDLE; }
        s_scale.pipe_ready = 0;
        s_scale.pipe_tried = 0;
    }

    if (!s_scale.pipe_ready) {
        if (s_scale.pipe_tried) return 0;
        s_scale.pipe_tried = 1;
        if (!rsx_hlsl_to_spirv_available()) return 0;

        if (!s_scale.vs || !s_scale.fs) {
            u32 spv[8192];
            u32 nw = 0;
            char err[256] = {0};
            if (rsx_hlsl_to_spirv(s_scale_vs_hlsl, RSX_SHADER_STAGE_VERTEX, spv, 8192, &nw, err, sizeof err) != 0) {
                VK_LOG("Lossless Scaling VS compile failed: %s\n", err);
                return 0;
            }
            VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                             .codeSize = nw * 4u, .pCode = spv };
            if (pvkCreateShaderModule(s_vk.device, &smi, NULL, &s_scale.vs) != VK_SUCCESS) return 0;

            nw = 0; err[0] = 0;
            if (rsx_hlsl_to_spirv(s_scale_fs_hlsl, RSX_SHADER_STAGE_FRAGMENT, spv, 8192, &nw, err, sizeof err) != 0) {
                VK_LOG("Lossless Scaling FS compile failed: %s\n", err);
                return 0;
            }
            smi.codeSize = nw * 4u; smi.pCode = spv;
            if (pvkCreateShaderModule(s_vk.device, &smi, NULL, &s_scale.fs) != VK_SUCCESS) return 0;
        }

        if (!s_scale.sampler) {
            VkSamplerCreateInfo sci = {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .maxLod = 1.0f,
            };
            if (pvkCreateSampler(s_vk.device, &sci, NULL, &s_scale.sampler) != VK_SUCCESS) return 0;
        }

        if (!s_scale.ubo.buf) {
            if (vk_create_host_buffer(&s_scale.ubo, 64u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0) return 0;
        }

        if (!s_scale.dsl) {
            VkDescriptorSetLayoutBinding b[3] = {
                { RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                { RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                { RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
            };
            VkDescriptorSetLayoutCreateInfo dci = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 3, .pBindings = b,
            };
            if (pvkCreateDescriptorSetLayout(s_vk.device, &dci, NULL, &s_scale.dsl) != VK_SUCCESS) return 0;

            VkPipelineLayoutCreateInfo lci = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1, .pSetLayouts = &s_scale.dsl,
            };
            if (pvkCreatePipelineLayout(s_vk.device, &lci, NULL, &s_scale.layout) != VK_SUCCESS) return 0;

            VkDescriptorPoolSize ps[3] = {
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
                { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1 },
                { VK_DESCRIPTOR_TYPE_SAMPLER,        1 },
            };
            VkDescriptorPoolCreateInfo pci = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps,
            };
            if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_scale.pool) != VK_SUCCESS) return 0;

            VkDescriptorSetAllocateInfo dai = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = s_scale.pool, .descriptorSetCount = 1, .pSetLayouts = &s_scale.dsl,
            };
            if (pvkAllocateDescriptorSets(s_vk.device, &dai, &s_scale.set) != VK_SUCCESS) return 0;
        }

        VkAttachmentDescription att = {
            .format = s_vk.sc_format, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        };
        VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1, .pColorAttachments = &cref,
        };
        VkSubpassDependency dep = {
            .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        };
        VkRenderPassCreateInfo rci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &att,
            .subpassCount = 1, .pSubpasses = &sub,
            .dependencyCount = 1, .pDependencies = &dep,
        };
        if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_scale.rp) != VK_SUCCESS) return 0;

        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_scale.vs, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_scale.fs, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1,
        };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xFu };
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &ba,
        };
        static const VkDynamicState dyn_st[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn_st,
        };
        VkGraphicsPipelineCreateInfo gci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2, .pStages = st,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia,
            .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb,
            .pDynamicState = &dyn, .layout = s_scale.layout, .renderPass = s_scale.rp,
        };
        if (pvkCreateGraphicsPipelines(s_vk.device, s_vk.pipe_cache, 1, &gci, NULL, &s_scale.pipe) != VK_SUCCESS)
            return 0;

        float params[8] = {
            (float)s_vk.width, (float)s_vk.height,
            1.0f / (float)s_vk.width, 1.0f / (float)s_vk.height,
            0.75f, (mode == VK_SCALE_SGSR) ? 3.0f : (mode == VK_SCALE_FSR) ? 2.0f : 1.0f, 0.0f, 0.0f,
        };
        memcpy(s_scale.ubo.ptr, params, sizeof params);

        VkDescriptorBufferInfo bi = { s_scale.ubo.buf, 0, sizeof params };
        VkDescriptorImageInfo ii = { VK_NULL_HANDLE, s_vk.color.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo si = { s_scale.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet w[3] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_scale.set,
              .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_scale.set,
              .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_scale.set,
              .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &si },
        };
        pvkUpdateDescriptorSets(s_vk.device, 3, w, 0, NULL);

        s_scale.rp_fmt = s_vk.sc_format;
        s_scale.pipe_ready = 1;
        VK_LOG("Lossless Scaling GPU pipeline ready (%ux%u -> %ux%u, mode=%s)\n",
               s_vk.width, s_vk.height, s_vk.sc_extent.width, s_vk.sc_extent.height,
               mode == VK_SCALE_SGSR ? "SGSR" : mode == VK_SCALE_FSR ? "FSR1+RCAS" : "CAS");
    }

    if (s_scale.fb_count != s_vk.sc_count ||
        s_scale.fb_ext.width != s_vk.sc_extent.width ||
        s_scale.fb_ext.height != s_vk.sc_extent.height) {
        vk_scale_destroy_fbs();
        for (u32 i = 0; i < s_vk.sc_count; i++) {
            VkImageViewCreateInfo vci = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = s_vk.sc_images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = s_vk.sc_format,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            if (pvkCreateImageView(s_vk.device, &vci, NULL, &s_scale.sc_views[i]) != VK_SUCCESS) {
                vk_scale_destroy_fbs(); return 0;
            }
            VkFramebufferCreateInfo fci = {
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = s_scale.rp, .attachmentCount = 1, .pAttachments = &s_scale.sc_views[i],
                .width = s_vk.sc_extent.width, .height = s_vk.sc_extent.height, .layers = 1,
            };
            if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_scale.sc_fbs[i]) != VK_SUCCESS) {
                vk_scale_destroy_fbs(); return 0;
            }
        }
        s_scale.fb_count = s_vk.sc_count;
        s_scale.fb_ext = s_vk.sc_extent;
    }
    return s_scale.fb_count > 0;
}

/* Where the frame goes in the swapchain image: the largest rectangle of the frame's aspect ratio, centred. The rest
 * of the image is black (letterbox or pillarbox). */
typedef struct { u32 x, y, w, h; } vk_rect;

static vk_rect vk_window_fit(void)
{
    const u32 dw = s_vk.sc_extent.width, dh = s_vk.sc_extent.height;
    vk_rect r = { 0, 0, dw, dh };
    if (!s_vk.width || !s_vk.height) return r;
    if (vk_scale_mode() == VK_SCALE_INTEGER) {
        const u32 kx = dw / s_vk.width;
        const u32 ky = dh / s_vk.height;
        const u32 k = kx < ky ? kx : ky;
        if (k >= 1u) {
            r.w = s_vk.width * k;
            r.h = s_vk.height * k;
            r.x = (dw - r.w) / 2u;
            r.y = (dh - r.h) / 2u;
            return r;
        }
    }
    if ((u64)dw * s_vk.height > (u64)dh * s_vk.width) {
        r.w = (u32)(((u64)dh * s_vk.width + s_vk.height / 2) / s_vk.height);
        r.x = (dw - r.w) / 2;
    } else {
        r.h = (u32)(((u64)dw * s_vk.height + s_vk.width / 2) / s_vk.width);
        r.y = (dh - r.h) / 2;
    }
    return r;
}

/* Record the blit (or GPU Lossless Scaling upscaler pass) of the offscreen target
 * into swapchain image `idx` (letterboxed to aspect ratio), leaving it in PRESENT_SRC. */
static void vk_record_window_blit(VkCommandBuffer cb, u32 idx)
{
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    const vk_rect fit = vk_window_fit();
    if (vk_scale_ensure() && idx < s_scale.fb_count) {
        VkClearValue clear_black = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
        VkRenderPassBeginInfo rbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = s_scale.rp, .framebuffer = s_scale.sc_fbs[idx],
            .renderArea = { { 0, 0 }, s_vk.sc_extent },
            .clearValueCount = 1, .pClearValues = &clear_black,
        };
        pvkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_scale.pipe);
        pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_scale.layout,
                                 0, 1, &s_scale.set, 0, NULL);
        VkViewport vp = { (float)fit.x, (float)fit.y, (float)fit.w, (float)fit.h, 0.0f, 1.0f };
        VkRect2D sc = { { (int32_t)fit.x, (int32_t)fit.y }, { fit.w, fit.h } };
        pvkCmdSetViewport(cb, 0, 1, &vp);
        pvkCmdSetScissor(cb, 0, 1, &sc);
        pvkCmdDraw(cb, 3, 1, 0, 0);
        pvkCmdEndRenderPass(cb);
        return;
    }
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s_vk.sc_images[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    if (fit.w != s_vk.sc_extent.width || fit.h != s_vk.sc_extent.height) {
        /* The bars. The clear and the blit write disjoint pixels, so they need no barrier between them. */
        static const VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        pvkCmdClearColorImage(cb, s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                              &b.subresourceRange);
    }
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { (int32_t)fit.x, (int32_t)fit.y, 0 },
                        { (int32_t)(fit.x + fit.w), (int32_t)(fit.y + fit.h), 1 } },
    };
    const VkFilter flt = (vk_scale_mode() == VK_SCALE_INTEGER) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    pvkCmdBlitImage(cb, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, flt);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
}

/* Blit the offscreen target into the next swapchain image and present it. */
static void vk_present_window(void)
{
    if (!s_vk.windowed) return;
    if (s_vk_resized) {
        vk_swapchain_lost();
        if (!s_vk.windowed || !s_vk.swapchain || s_vk_resized) return;
    }
    if (!s_vk.swapchain || vk_window_hidden()) return;

    u32 idx = 0;
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, 500000000ull /* 500 ms */,
                                        s_vk.sem_acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_TIMEOUT) {
        if (r == VK_TIMEOUT) VK_LOG("WARNING: vkAcquireNextImageKHR (window) timed out -> recreating swapchain\n");
        vk_swapchain_lost(); return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); return;
    }

    /* The present's own command buffer: free, since every caller has waited
     * for its read-back copy (present_fence) first. */
    VkCommandBuffer cb = s_vk.present_cmd;
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS) { VK_LOG("vkResetCommandBuffer (window) failed\n"); return; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { VK_LOG("vkBeginCommandBuffer (window) failed\n"); return; }
    vk_record_window_blit(cb, idx);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer failed\n"); return; }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sem_acquire,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s_vk.sc_done[idx],
    };
    pvkResetFences(s_vk.device, 1, &s_vk.window_fence);
    if (vk_queue_submit(&si, s_vk.window_fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (window) failed\n"); return;
    }
    vk_wait_fence_logged(s_vk.window_fence, "present_window");

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    r = vk_queue_present(&pi);
    if (s_vk.fps_on && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR))
        metal_frame_rate_presented(&s_vk.fps, vk_now_seconds());
#ifdef __ANDROID__
    /* Android's Vulkan swapchain layer returns VK_SUBOPTIMAL_KHR every frame when
     * preTransform is IDENTITY on a rotated panel; real resizes set s_vk_resized. */
    if (r == VK_ERROR_OUT_OF_DATE_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
#else
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
#endif
}

static double vk_now_seconds(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* present_mu held. Same line shape as the Metal backend's, plus the clock.
 * `final` reports the partial last interval at shutdown. */
static void vk_fps_report(int final)
{
    const double now = vk_now_seconds();
    if (final ? (s_vk.fps.guest == 0 && s_vk.fps.displayed == 0) : now - s_vk.fps.epoch < 1.0) return;
    MetalFrameRateSample r = metal_frame_rate_sample(&s_vk.fps, now);
    fprintf(stderr, "[RSX FPS] display=%.2f guest=%.2f avg_ms=%.2f p95_ms=%.2f max_ms=%.2f age_s=%.2f clock=host_present_call window=%d\n",
            r.display_fps, r.guest_fps, r.average_ms, r.p95_ms, r.worst_ms, r.age_seconds, s_vk.windowed && s_vk.swapchain ? 1 : 0);
}

static void vk_dump_ppm(const unsigned char* rgba)
{
    FILE* f = fopen(s_vk.dump_path, "wb");
    if (!f) { VK_LOG("cannot open %s for the frame dump\n", s_vk.dump_path); return; }
    fprintf(f, "P6\n%u %u\n255\n", s_vk.width, s_vk.height);
    for (size_t i = 0; i < (size_t)s_vk.width * s_vk.height; i++)
        fwrite(rgba + i * 4, 1, 3, f);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * Present
 *
 * A read-back present (the default) comes in two halves. The first records,
 * on the present's own command buffer,
 * whatever puts the frame into the offscreen target (the engine's blit of
 * the flipped surface) and the copy of that target into `readback`, and
 * submits it without waiting. The second waits for it, reads the centre
 * pixel and the frame dump back, and blits the target into the window.
 *
 * The first half reads the frame, so it must not run while the walker is
 * recording; the second touches only the target, `readback` and the
 * present's own objects, none of which the engine path draws into, so a host
 * can run it on a thread of its own beside the walker
 * (rsx_vulkan_backend_present_begin / _finish). On the fixed-function path
 * the walker draws into the target itself, so there the second half always
 * follows the first at once.
 *
 * Each such present waits on the CPU twice: for the copy (and so for all of
 * the frame's GPU work) and for the window blit. With
 * PS3RECOMP_VK_DIRECT_PRESENT=1 an engine-path present is direct instead, as
 * on Metal: the flipped surface's blit and the window blit go into one
 * command buffer that waits for the swapchain image on the GPU and signals
 * its present, and the CPU waits for nothing. A ring of VK_PRESENT_SLOTS
 * command buffers and fences keeps frames in flight. Direct presents read
 * nothing back, so the presents PS3RECOMP_VK_DUMP writes, and every present
 * on the fixed-function path, stay read-back presents.
 * -------------------------------------------------------------------------*/

static int vk_present_direct_wanted(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = vk_env_on("PS3RECOMP_VK_DIRECT_PRESENT");
    if (!enabled || !s_vk.eng_active) return 0;
    /* A dump reads back only the presents it writes. */
    return !(s_vk.dump_path && ((s_vk.presented + 1u) % s_vk.dump_every) == 0);
}

/* Wait until the GPU is done with every present in flight (an image is about
 * to be destroyed). Safe from any thread: a slot's fence is reset only when a
 * present starts, which never runs alongside the walker. */
static void vk_pt_wait_stages(void);
static void vk_coop_wait_gpu(void);
static void vk_present_wait_gpu(void)
{
    for (u32 i = 0; i < VK_PRESENT_SLOTS; i++)
        if (__atomic_load_n(&s_vk.present_busy[i], __ATOMIC_ACQUIRE) && s_vk.present_fences[i])
            vk_wait_fence_logged(s_vk.present_fences[i], "present_wait_gpu");
    vk_pt_wait_stages();   /* a present thread's copies out of a surface */
    vk_coop_wait_gpu();    /* async Co-op P2 copy out of a surface */
}

/* present_mu held: finish a read-back present -- wait for its copy, keep the
 * centre pixel and the dump, then show it in the window. */
static void vk_present_complete_locked(void)
{
    if (!s_vk.present_inflight) return;
    vk_wait_fence_logged(s_vk.present_fence, "present_complete");
    __atomic_store_n(&s_vk.present_busy[s_vk.present_slot], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_vk.present_inflight, 0, __ATOMIC_RELEASE);

    vk_invalidate(&s_vk.readback);
    const unsigned char* px = (const unsigned char*)s_vk.readback.ptr;
    size_t c = ((size_t)(s_vk.height / 2) * s_vk.width + s_vk.width / 2) * 4;
    s_vk.last_center = ((u32)px[c] << 16) | ((u32)px[c + 1] << 8) | (u32)px[c + 2];
    s_vk.presented++;
    s_vk.present_readback_count++;
    if (s_vk.dump_path && (s_vk.presented % s_vk.dump_every) == 0) vk_dump_ppm(px);
    if (s_vk.fps_on) metal_frame_rate_guest(&s_vk.fps);
    vk_present_window();
    if (s_vk.fps_on) vk_fps_report(0);
}

/* present_mu held: a previous read-back present finished, the next ring
 * slot's earlier present complete, and its command buffer begun after
 * everything submitted so far. */
static VkCommandBuffer vk_present_record_begin(void)
{
    vk_present_complete_locked();
    const u32 slot = (s_vk.present_slot + 1u) % VK_PRESENT_SLOTS;
    if (s_vk.present_busy[slot]) {
        vk_wait_fence_logged(s_vk.present_fences[slot], "present_record_begin");
        __atomic_store_n(&s_vk.present_busy[slot], 0, __ATOMIC_RELEASE);
    }
    s_vk.present_slot = slot;
    s_vk.present_cmd = s_vk.present_cmds[slot];
    s_vk.present_fence = s_vk.present_fences[slot];
    VkCommandBuffer cb = s_vk.present_cmd;
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_cmd_after_everything(cb);
    return cb;
}

/* present_mu held: a direct present -- the window blit recorded after the
 * frame's, the submission waiting for the swapchain image on the GPU and
 * signalling its present. Nothing waits on the CPU. */
static void vk_present_direct_end(VkCommandBuffer cb)
{
    const u32 slot = s_vk.present_slot;
    u32 idx = 0;
    if (s_vk.windowed && s_vk_resized) {
        vk_swapchain_lost();
    }
    int window = s_vk.windowed && s_vk.swapchain && !vk_window_hidden() && !s_vk_resized;
    VkSemaphore acquire = VK_NULL_HANDLE;
    if (window) {
        if (!s_vk.present_acquire[slot]) {
            VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.present_acquire[slot]) != VK_SUCCESS)
                s_vk.present_acquire[slot] = VK_NULL_HANDLE;
        }
        acquire = s_vk.present_acquire[slot];
        VkResult r = acquire ? pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, 500000000ull /* 500 ms */,
                                                      acquire, VK_NULL_HANDLE, &idx)
                             : VK_ERROR_INITIALIZATION_FAILED;
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_TIMEOUT) {
            if (r == VK_TIMEOUT) VK_LOG("WARNING: vkAcquireNextImageKHR (direct) timed out -> recreating swapchain\n");
            vk_swapchain_lost(); window = 0;
        } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); window = 0;
        }
        if (window) vk_record_window_blit(cb, idx);
    }
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = window ? 1u : 0u, .pWaitSemaphores = &acquire,
                        .pWaitDstStageMask = &wait_stage,
                        .commandBufferCount = 1, .pCommandBuffers = &cb,
                        .signalSemaphoreCount = window ? 1u : 0u,
                        .pSignalSemaphores = window ? &s_vk.sc_done[idx] : NULL };
    pvkResetFences(s_vk.device, 1, &s_vk.present_fence);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS || vk_queue_submit(&si, s_vk.present_fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (direct present) failed\n");
        if (window) {
            /* The acquire signalled a semaphore nothing will wait on: retire it. */
            vk_device_idle();
            pvkDestroySemaphore(s_vk.device, acquire, NULL);
            s_vk.present_acquire[slot] = VK_NULL_HANDLE;
        }
        return;
    }
    __atomic_store_n(&s_vk.present_busy[slot], 1, __ATOMIC_RELEASE);
    s_vk.present_order = 1;
    s_vk.presented++;
    s_vk.present_direct++;
    if (window) {
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
            .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
        };
        VkResult r = vk_queue_present(&pi);
#ifdef __ANDROID__
        if (r == VK_ERROR_OUT_OF_DATE_KHR) vk_swapchain_lost();
        else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
#else
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
        else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
#endif
    }
}

/* present_mu held: end the present's recording and submit it -- direct when
 * asked for (above), else copy the target out, submit, and finish at once
 * unless a host asked for the halves separately on the engine path. */
static void vk_present_record_end(VkCommandBuffer cb)
{
    if (vk_present_direct_wanted()) { vk_present_direct_end(cb); return; }
    const u32 slot = s_vk.present_slot;
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { s_vk.width, s_vk.height, 1 },
    };
    pvkCmdCopyImageToBuffer(cb, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.readback.buf, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vk.readback.buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &host, 0, NULL);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer (present) failed\n"); return; }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    pvkResetFences(s_vk.device, 1, &s_vk.present_fence);
    if (vk_queue_submit(&si, s_vk.present_fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (present) failed\n"); return;
    }
    __atomic_store_n(&s_vk.present_busy[slot], 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_vk.present_inflight, 1, __ATOMIC_RELEASE);
    s_vk.present_order = 1;
    if (!(s_vk.present_split && s_vk.eng_active)) vk_present_complete_locked();
}

static void vk_cb_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_vk.device) return;
    pthread_mutex_lock(&s_vk.present_mu);
    VkCommandBuffer cb = vk_present_record_begin();
    if (cb) vk_present_record_end(cb);
    pthread_mutex_unlock(&s_vk.present_mu);
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
}

/* ---------------------------------------------------------------------------
 * Register-file draw engine backend (rsx_draw_engine.h)
 *
 * The path the Metal backend takes: the shared engine walks the register
 * file, owns surfaces, texture caching, vertex compaction and pipeline keys,
 * and drives this backend through rsx_draw_backend. Selected with
 * PS3RECOMP_RSX_ENGINE=dispatch; only one of the two paths is registered at a
 * time, since the FIFO walker feeds both and would record every draw twice.
 *
 * Every operation is still one synchronous submission, so submit_and_wait has
 * nothing left to do by the time the engine calls it -- which is exactly the
 * contract it asks for (staging is free to reuse on return).
 *
 * Stage E1: colour and depth targets, clears, present and readback.
 * Pipelines, draws and textures are the next stages; until then
 * pipeline_create reports 0, which the engine caches as "cannot build".
 * -------------------------------------------------------------------------*/
#define VK_ENG_MAX_OBJ 8192          /* the engine's texture cache is up to 4096 */
#define VK_ENG_MAX_VIEW_LIST 512u

enum { VK_ENG_FREE = 0, VK_ENG_COLOR, VK_ENG_DEPTH, VK_ENG_TEXTURE, VK_ENG_VIEW };

typedef struct vk_eng_obj {
    u8            kind;
    VkFormat      fmt;
    u32           bpp;                    /* bytes per texel, 0 for depth     */
    u32           w, h;
    vk_image      im;                     /* a VIEW owns only im.view            */
    VkImageView   depth_sample_view;      /* DEPTH: depth-aspect-only sampled view */
    VkFramebuffer dac_fb;                 /* snapshot COLOR: cached depth_as_color FB */
    u32           block;                  /* bytes per 4x4 block, 0 = not BC     */
    u32           surface, remap, rsx_fmt;/* VIEW: what it is a view of          */
    u8            cube;                   /* TEXTURE: six layers, a CUBE view    */
    u8            snapshot;               /* COLOR: a feedback/dac copy, never a guest target */
    u8            pending_clear_color;    /* COLOR: deferred LOAD_OP_CLEAR */
    u8            pending_clear_depth;    /* DEPTH: bit0 = depth, bit1 = stencil */
    u8            pending_stencil_val;
    float         pending_rgba[4];
    float         pending_depth_val;
    u32           mips;                   /* TEXTURE: levels the image was created with */
} vk_eng_obj;

static vk_eng_obj s_eobj[VK_ENG_MAX_OBJ];
static u32        s_eview_list[VK_ENG_MAX_VIEW_LIST];
static u32        s_eview_count;
static void       vk_dac_shutdown(void);
static void       vk_eng_flush_pending_clear(vk_eng_obj* o);

/* Defined with the E2 pipeline state further down. */
static void vk_eng_drop_framebuffers(void);
static void vk_eng_bump_tex_gen(void);
static void vk_eng2_shutdown(void* user);

static struct {
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth;
} s_eng_bound;

static VkFormat vk_eng_format(rsx_be_format f, u32* bpp)
{
    switch (f) {
    case RSX_BE_FMT_R8:            *bpp = 1;  return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8:          *bpp = 2;  return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8:      *bpp = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16:           *bpp = 2;  return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16:        *bpp = 4;  return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F:       *bpp = 4;  return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8;  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F:          *bpp = 4;  return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RSX_BE_FMT_BC1:           *bpp = 8;  return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  /* per block */
    case RSX_BE_FMT_BC2:           *bpp = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case RSX_BE_FMT_BC3:           *bpp = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    default:                       *bpp = 0;  return VK_FORMAT_UNDEFINED;
    }
}

/* Colour formats the E2 passes render into: RGBA8 for ordinary and display
 * targets, RGBA16F for SET_SURFACE_FORMAT 0xB HDR targets, and R32F / RGBA32F
 * for the float targets a title renders small reductions into. All four are
 * required colour attachment formats in Vulkan 1.0; blending is guaranteed only
 * for the first two (see vk_eng_rt_blendable). */
#define VK_ENG_RT_FORMATS 4
static int vk_eng_rt_index(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:      return 0;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 1;
    case VK_FORMAT_R32_SFLOAT:          return 2;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 3;
    default:                            return -1;
    }
}

/* Whether the device blends into colour format `f` (guaranteed for RGBA8 and
 * RGBA16F, optional for 32-bit float formats). */
static int vk_eng_rt_blendable(VkFormat f)
{
    if (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R16G16B16A16_SFLOAT) return 1;
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, f, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
}

/* The largest image side the engine creates: the device's own limit once a
 * device is picked, the 4096 Vulkan guarantees before. */
static u32 vk_max_dim(void) { return s_vk.max_dim ? s_vk.max_dim : 4096u; }

static u32 vk_eng_alloc(void)
{
    /* Hand out slots round-robin rather than lowest-first, so a handle the
     * engine releases is not immediately reused: a late or duplicated release
     * of an old handle then finds a free slot instead of a live object. */
    static u32 cursor;
    for (u32 step = 0; step < VK_ENG_MAX_OBJ - 1; step++) {
        const u32 i = 1 + (cursor + step) % (VK_ENG_MAX_OBJ - 1);
        if (s_eobj[i].kind == VK_ENG_FREE) {
            cursor = i % (VK_ENG_MAX_OBJ - 1);
            return i;
        }
    }
    VK_LOG("engine: object table full (%d)\n", VK_ENG_MAX_OBJ);
    return 0;
}

static vk_eng_obj* vk_eng_get(u32 handle, int kind)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return NULL;
    vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == kind) ? o : NULL;
}

/* Host staging kept across readbacks and uploads. One buffer allocated,
 * mapped and freed per call is three driver round trips, and on Windows
 * those took from tens of milliseconds to seconds each under load. Each
 * grows to the largest request and is reused after its submission
 * completes; every user waits for that before returning. */
static vk_buffer s_rb_stage, s_up_stage;
static vk_buffer* vk_stage_get(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= size) return b;
    if (b->buf) vk_destroy_buffer(b);
    VkDeviceSize cap = (VkDeviceSize)1 << 20;
    while (cap < size) cap *= 2u;
    if (vk_create_host_buffer(b, cap, usage)) { vk_destroy_buffer(b); return NULL; }
    return b;
}

/* Copy `rows` rows of `w` texels from host memory into a w x h rectangle at
 * (x, y) of one level / layer of an image that is in GENERAL. */
static int vk_eng_upload_region(VkImage img, u32 layer, u32 mip, u32 x, u32 y,
                                u32 w, u32 h, size_t tight, u32 rows,
                                const void* src, u32 row_bytes)
{
    if (tight > row_bytes) tight = row_bytes;
    {   /* VK_DRAW_BATCH: staged in a ring and recorded into the open batch. */
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 },
            .imageExtent = { w, h, 1 },
        };
        const int b = vk_eb_upload(img, &region, src, tight, rows, row_bytes);
        if (b != -2) return b;
    }
    /* Unbatched: #552's reused upload staging rather than a buffer created and
     * destroyed per region. */
    vk_buffer* const st = vk_stage_get(&s_up_stage, (VkDeviceSize)tight * rows, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!st || !st->coherent) return -1;
    for (u32 y = 0; y < rows; y++)
        memcpy((u8*)st->ptr + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 },
            .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyBufferToImage(s_vk.cmd, st->buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        rc = vk_submit_and_wait();
    }
    return rc;
}

/* The same into the whole of a level, from its origin. */
static int vk_eng_upload_rows(VkImage img, u32 layer, u32 mip, u32 w, u32 h,
                              size_t tight, u32 rows, const void* src, u32 row_bytes)
{
    return vk_eng_upload_region(img, layer, mip, 0, 0, w, h, tight, rows, src, row_bytes);
}

static int vk_eng_upload(VkImage img, u32 layer, u32 mip, u32 w, u32 h, u32 bpp,
                         const void* src, u32 row_bytes)
{
    return vk_eng_upload_rows(img, layer, mip, w, h, (size_t)w * bpp, h, src, row_bytes);
}

/* NV4097 TEXTURE_CONTROL1 crossbar as a Vulkan component mapping: the shared
 * rsx_texture_component_remap() gives, in A,R,G,B order, which uploaded
 * channel (0..3 = R,G,B,A) or constant each output takes -- the table the
 * Metal backend's swizzle_sel reads. */
static VkComponentSwizzle vk_remap_sel(u8 sel)
{
    switch (sel) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}
static VkComponentMapping vk_remap(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    VkComponentMapping m = { vk_remap_sel(sel[1]), vk_remap_sel(sel[2]),
                             vk_remap_sel(sel[3]), vk_remap_sel(sel[0]) };
    return m;
}
static int vk_eng_make_view(VkImage img, VkFormat fmt, u32 levels, VkComponentMapping map,
                            int cube, VkImageView* out)
{
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .components = map,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels ? levels : 1, 0, cube ? 6u : 1u },
    };
    return pvkCreateImageView(s_vk.device, &vci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

static int s_eview_overflow;

static void vk_eview_list_add(u32 hd)
{
    if (s_eview_count < VK_ENG_MAX_VIEW_LIST)
        s_eview_list[s_eview_count++] = hd;
    else
        s_eview_overflow = 1;
}

static void vk_eview_list_remove(u32 hd)
{
    for (u32 i = 0; i < s_eview_count; i++) {
        if (s_eview_list[i] == hd) {
            s_eview_list[i] = s_eview_list[--s_eview_count];
            return;
        }
    }
}

static void vk_pt_start(void);
static void vk_pt_stop(void);
static int vk_eng_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    memset(s_eobj, 0, sizeof s_eobj);
    memset(&s_eng_bound, 0, sizeof s_eng_bound);
    s_eview_count = 0;
    s_eview_overflow = 0;
    if (!s_vk.device) return -1;
    (void)vk_eb_on();
    if (rsx_present_thread_requested()) vk_pt_start();
    return 0;
}

static void vk_eng_snapshot_recycle(vk_eng_obj* o);

static void vk_eng_release(void* user, u32 handle)
{
    (void)user;
    if (!handle || handle >= VK_ENG_MAX_OBJ || s_eobj[handle].kind == VK_ENG_FREE) return;
    vk_eng_bump_tex_gen();
    if (s_eobj[handle].kind == VK_ENG_VIEW) {
        vk_eview_list_remove(handle);
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR) {
        if (!s_eview_overflow) {
            u32 j = 0;
            while (j < s_eview_count) {
                const u32 vh = s_eview_list[j];
                if (vh && vh < VK_ENG_MAX_OBJ &&
                    s_eobj[vh].kind == VK_ENG_VIEW && s_eobj[vh].surface == handle) {
                    if (!vk_eb_defer_image(&s_eobj[vh].im))
                        vk_destroy_image(&s_eobj[vh].im);
                    memset(&s_eobj[vh], 0, sizeof s_eobj[vh]);
                    s_eview_list[j] = s_eview_list[--s_eview_count];
                } else {
                    j++;
                }
            }
        } else {
            for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
                if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == handle) {
                    vk_eview_list_remove(i);
                    if (!vk_eb_defer_image(&s_eobj[i].im))
                        vk_destroy_image(&s_eobj[i].im);
                    memset(&s_eobj[i], 0, sizeof s_eobj[i]);
                }
        }
    }
    if (s_eobj[handle].kind == VK_ENG_DEPTH && s_eobj[handle].surface) {
        const u32 snap = s_eobj[handle].surface;
        s_eobj[handle].surface = 0;
        if (vk_eng_get(snap, VK_ENG_TEXTURE)) vk_eng_release(user, snap);
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR && s_eobj[handle].snapshot) {
        /* Never a guest attachment, so no engine framebuffer names it; the
         * image (and optional depth_as_color FB) returns to the pool. */
        vk_eng_snapshot_recycle(&s_eobj[handle]);
        memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
        return;
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR || s_eobj[handle].kind == VK_ENG_DEPTH) {
        vk_eng_drop_framebuffers();
        if (s_eobj[handle].depth_sample_view) {
            vk_image dv = { .view = s_eobj[handle].depth_sample_view };
            if (!vk_eb_defer_image(&dv))
                pvkDestroyImageView(s_vk.device, s_eobj[handle].depth_sample_view, NULL);
            s_eobj[handle].depth_sample_view = VK_NULL_HANDLE;
        }
        if (!vk_eb_defer_image(&s_eobj[handle].im))
            vk_destroy_image(&s_eobj[handle].im);
    } else if (!vk_eb_defer_image(&s_eobj[handle].im)) {
        vk_destroy_image(&s_eobj[handle].im);
    }
    memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
}

/* The engine releases each object through the call for its kind: colour
 * targets through color_target_release, depth targets through
 * depth_target_release, and textures, views and colour feedback copies
 * through texture_release. Handles are slots that get reused, so a release
 * naming an object of another kind can only be a stale handle, and freeing
 * it would destroy the live object that now owns the slot. It is refused and
 * reported instead. Free slots pass through: vk_eng_release ignores them. */
static int vk_eng_release_kind_ok(u32 handle, int want_color, int want_depth)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return 1;
    const vk_eng_obj* o = &s_eobj[handle];
    if (o->kind == VK_ENG_FREE) return 1;
    const int color = o->kind == VK_ENG_COLOR && !o->snapshot, depth = o->kind == VK_ENG_DEPTH;
    const int ok = want_color ? color : want_depth ? depth : (!color && !depth);
    if (!ok) {
        static u32 logs;
        if (logs++ < 16)
            VK_LOG("engine: stale %s release of object %u (kind %d%s, %ux%u) refused\n",
                   want_color ? "colour target" : want_depth ? "depth target" : "texture", handle,
                   (int)o->kind, o->snapshot ? " snapshot" : "", o->w, o->h);
    }
    return ok;
}
static void vk_eng_texture_release(void* user, u32 handle)
{
    if (vk_eng_release_kind_ok(handle, 0, 0)) vk_eng_release(user, handle);
}
static void vk_eng_color_target_release(void* user, u32 handle)
{
    if (vk_eng_release_kind_ok(handle, 1, 0)) vk_eng_release(user, handle);
}
static void vk_eng_depth_target_release(void* user, u32 handle)
{
    if (vk_eng_release_kind_ok(handle, 0, 1)) vk_eng_release(user, handle);
}

static void vk_eng_snapshot_pool_drain(void);

static void vk_eng_shutdown(void* user)
{
    vk_pt_stop();   /* joins the show thread before anything it uses goes */
    vk_eng2_shutdown(user);
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++) vk_eng_release(user, i);
    vk_eng_snapshot_pool_drain();
    vk_dac_shutdown();
    vk_eb_release();
    vk_destroy_buffer(&s_rb_stage);
    vk_destroy_buffer(&s_up_stage);
    s_eview_count = 0;
    s_eview_overflow = 0;
}

static void vk_eng_submit_and_wait(void* user, u32 reason)
{
    (void)user; (void)reason;      /* ensure all async batched work has completed */
    vk_eng_flush_sync_tag(VK_SYNC_SUBMIT);
}

static u32 vk_eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    /* Only formats the E2 passes are built for (vk_eng_rt_index). */
    if (vk_eng_rt_index(vf) < 0 || !w || !h || w > vk_max_dim() || h > vk_max_dim()) {
        VK_LOG("engine: colour target format %d %ux%u not supported yet\n", (int)fmt, w, h);
        return 0;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image(&o->im, vf, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_COLOR; o->fmt = vf; o->bpp = bpp; o->w = w; o->h = h;
    /* UNDEFINED -> GENERAL, then the guest's own bytes when the engine could
     * resolve them, else transparent black so the contents are defined. */
    const int batch = vk_eb_active();
    if (batch ? (!s_eb_pending && vk_begin()) : vk_begin()) { vk_eng_release(user, hd); return 0; }
    if (batch) vk_eb_end_pass();
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    if (batch) vk_eb_recorded();
    else if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    if (seed && seed_row_bytes)
        vk_eng_upload(o->im.img, 0, 0, w, h, bpp, seed, seed_row_bytes);
    return hd;
}

static u32 vk_eng_depth_target_create(void* user, u32 w, u32 h)
{
    (void)user;
    if (!w || !h || w > vk_max_dim() || h > vk_max_dim()) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image(&o->im, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        s_vk.depth_aspect)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_DEPTH; o->fmt = s_vk.depth_format; o->w = w; o->h = h;
    /* Far plane, for the reason the display depth starts there. */
    const int batch = vk_eb_active();
    if (batch ? (!s_eb_pending && vk_begin()) : vk_begin()) { vk_eng_release(user, hd); return 0; }
    if (batch) vk_eb_end_pass();
    vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (batch) vk_eb_recorded();
    else if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    return hd;
}

static void vk_eng_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        s_eng_bound.rt[i] = (i < count && surfaces) ? surfaces[i] : 0;
    s_eng_bound.nrt = count;
    s_eng_bound.depth = depth;
}

static void vk_eng_flush_pending_clear(vk_eng_obj* o)
{
    if (!o) return;
    if (o->kind == VK_ENG_COLOR && o->pending_clear_color) {
        o->pending_clear_color = 0;
        const int batched = vk_eb_active();
        if (batched ? (!s_eb_pending && vk_begin()) : vk_begin()) return;
        if (batched) vk_eb_end_pass();
        VkClearColorValue v;
        memcpy(v.float32, o->pending_rgba, sizeof v.float32);
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        if (batched) vk_eb_recorded();
        else vk_submit_and_wait();
    } else if (o->kind == VK_ENG_DEPTH && o->pending_clear_depth) {
        const u8 bits = o->pending_clear_depth;
        o->pending_clear_depth = 0;
        VkImageAspectFlags aspect = 0;
        if (bits & 1u) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (bits & 2u) aspect |= s_vk.depth_aspect & VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!aspect) return;
        const int batched = vk_eb_active();
        if (batched ? (!s_eb_pending && vk_begin()) : vk_begin()) return;
        if (batched) vk_eb_end_pass();
        VkClearDepthStencilValue d = { o->pending_depth_val, o->pending_stencil_val };
        VkImageSubresourceRange r = { aspect, 0, 1, 0, 1 };
        vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
        if (batched) vk_eb_recorded();
        else vk_submit_and_wait();
    }
}

static int vk_eb_uses_color(u32 surface);
static int vk_eb_uses_depth(u32 depth);

static void vk_eng_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    /* The debug hook reports the last colour asked for, as Metal's does. */
    s_vk.clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                      ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                      ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                       (u32)(rgba[2] * 255.0f + 0.5f);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o) return;
    if (vk_eb_active()) {
        /* Defer into the next render pass's LOAD_OP_CLEAR to avoid an
         * off-tile Sysmem clear + Sysmem unresolve reload on Adreno TBDR.
         * Only end an already-open pass if it actually references this target. */
        if (vk_eb_uses_color(surface)) vk_eb_end_pass_reason(VK_PB_CLR);
        o->pending_clear_color = 1;
        memcpy(o->pending_rgba, rgba, sizeof o->pending_rgba);
        return;
    }
    o->pending_clear_color = 0;
    if (vk_begin()) return;
    VkClearColorValue v;
    memcpy(v.float32, rgba, sizeof v.float32);
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    vk_submit_and_wait();
}

static void vk_eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                       float depth_value, u8 stencil)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(depth, VK_ENG_DEPTH);
    VkImageAspectFlags aspect = 0;
    if (flags & RSX_BE_CLEAR_DEPTH) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (flags & RSX_BE_CLEAR_STENCIL) aspect |= s_vk.depth_aspect & VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!o || !aspect) return;
    if (vk_eb_active()) {
        if (vk_eb_uses_depth(depth)) vk_eb_end_pass_reason(VK_PB_CLR);
        if (flags & RSX_BE_CLEAR_DEPTH) {
            o->pending_clear_depth |= 1u;
            o->pending_depth_val = depth_value;
        }
        if ((flags & RSX_BE_CLEAR_STENCIL) && (s_vk.depth_aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
            o->pending_clear_depth |= 2u;
            o->pending_stencil_val = stencil;
        }
        return;
    }
    o->pending_clear_depth = 0;
    if (vk_begin()) return;
    VkClearDepthStencilValue d = { depth_value, stencil };
    VkImageSubresourceRange r = { aspect, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Opt-in present thread (PS3RECOMP_RSX_PRESENT_THREAD=1)
 *
 * rsx_present_thread.h owns the ring, the show thread and the counters; these
 * are the Vulkan halves, on the register-file engine path. The vtable path
 * keeps the ordinary present, with a one-time message. A periodic frame dump
 * (PS3RECOMP_VK_DUMP, every PS3RECOMP_VK_DUMP_EVERY-th frame) is copied from
 * the staging image of each dumped frame and written by the presenting
 * thread; the show thread and the window are untouched.
 *
 * Stage (the presenting thread, under present_mu like vk_eng_present): after
 * a full dependency on everything submitted before, blit the flipped surface
 * into this slot's staging image on a command buffer of the stage's own pool,
 * and submit. present_order then makes the walker's next command buffer wait
 * for that copy, so no later write reaches the surface before it is read.
 * Nothing waits for a swapchain image.
 *
 * Show (the present thread): acquire a swapchain image with a finite timeout,
 * so a shutdown is never stuck behind it; on the show's own pool blit the
 * staging image into the offscreen target and that into the image (the same
 * letterboxed window blit as every present); submit, waiting for the image on
 * the GPU, and present. The stage it shows was submitted before the slot was
 * handed over, so queue order puts the show after it, and a later stage into
 * the same slot after the show. In this mode only the show thread uses the
 * offscreen target and the swapchain.
 *
 * Off (the default) none of this is created and vk_eng_present is unchanged.
 * -------------------------------------------------------------------------*/
#define VK_PT_STAGE_CMDS 6
#define VK_PT_SHOW_SLOTS 3
static struct {
    rsx_present_thread* pt;
    int stopping;                                   /* atomic */
    VkCommandPool stage_pool;                       /* presenting thread */
    VkCommandBuffer stage_cmds[VK_PT_STAGE_CMDS];
    VkFence stage_fences[VK_PT_STAGE_CMDS];
    int stage_busy[VK_PT_STAGE_CMDS];               /* atomic: submitted, not yet waited */
    u32 stage_next;
    unsigned long long staged;                      /* presenting thread: frames staged */
    vk_image img[RSX_PRESENT_THREAD_MAX_DEPTH];     /* the staging images */
    u32 img_w[RSX_PRESENT_THREAD_MAX_DEPTH], img_h[RSX_PRESENT_THREAD_MAX_DEPTH];
    int img_fresh[RSX_PRESENT_THREAD_MAX_DEPTH];    /* still UNDEFINED */
    VkCommandPool show_pool;                        /* present thread */
    VkCommandBuffer show_cmds[VK_PT_SHOW_SLOTS];
    VkFence show_fences[VK_PT_SHOW_SLOTS];
    VkSemaphore show_acquire[VK_PT_SHOW_SLOTS];
    int show_busy[VK_PT_SHOW_SLOTS];
    u32 show_next;
} s_vpt;

/* Any thread: every stage copy submitted so far has finished (a surface is
 * about to be destroyed). */
static void vk_pt_wait_stages(void)
{
    for (u32 i = 0; i < VK_PT_STAGE_CMDS; i++)
        if (__atomic_load_n(&s_vpt.stage_busy[i], __ATOMIC_ACQUIRE) && s_vpt.stage_fences[i])
            pvkWaitForFences(s_vk.device, 1, &s_vpt.stage_fences[i], VK_TRUE, UINT64_MAX);
}

static void vk_pt_destroy_image(u32 slot)   /* the queue is idle */
{
    vk_image* im = &s_vpt.img[slot];
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
    s_vpt.img_w[slot] = s_vpt.img_h[slot] = 0;
}

static int vk_pt_stage(void* user, uint32_t slot, uint32_t surface)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || slot >= RSX_PRESENT_THREAD_MAX_DEPTH) return 0;
    /* The staging image has the offscreen target's size, which the window
     * blit reads. vk_create_image makes a view, so it needs a viewable usage
     * besides the transfers. */
    if (!s_vpt.img[slot].img) {
        if (vk_create_image(&s_vpt.img[slot], VK_FORMAT_R8G8B8A8_UNORM, s_vk.width, s_vk.height,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) {
            VK_LOG("present thread: staging image could not be created\n");
            return 0;
        }
        s_vpt.img_w[slot] = s_vk.width; s_vpt.img_h[slot] = s_vk.height;
        s_vpt.img_fresh[slot] = 1;
    }
    const u32 c = s_vpt.stage_next;
    s_vpt.stage_next = (c + 1u) % VK_PT_STAGE_CMDS;
    if (__atomic_load_n(&s_vpt.stage_busy[c], __ATOMIC_ACQUIRE)) {   /* six stages back: GPU lag only */
        pvkWaitForFences(s_vk.device, 1, &s_vpt.stage_fences[c], VK_TRUE, UINT64_MAX);
        __atomic_store_n(&s_vpt.stage_busy[c], 0, __ATOMIC_RELEASE);
    }
    pthread_mutex_lock(&s_vk.present_mu);
    VkCommandBuffer cb = s_vpt.stage_cmds[c];
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS || pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        pthread_mutex_unlock(&s_vk.present_mu);
        VK_LOG("present thread: stage recording failed\n");
        return 0;
    }
    vk_cmd_after_everything(cb);
    vk_barrier_image_on(cb, o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image_on(cb, s_vpt.img[slot].img, VK_IMAGE_ASPECT_COLOR_BIT,
                        s_vpt.img_fresh[slot] ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)o->w, (int32_t)o->h, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
    };
    pvkCmdBlitImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vpt.img[slot].img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                    (o->w == s_vk.width && o->h == s_vk.height) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    /* PS3RECOMP_VK_DUMP: every dump_every-th staged frame is also copied from
     * its staging image into `readback` (which only a read-back present uses,
     * and there is none in this mode) and written out once the stage has run,
     * on this thread, as the read-back present does. */
    const int dump = s_vk.dump_path && s_vk.readback.buf && (++s_vpt.staged % s_vk.dump_every) == 0;
    if (dump) {
        vk_barrier_image_on(cb, s_vpt.img[slot].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { s_vk.width, s_vk.height, 1 },
        };
        pvkCmdCopyImageToBuffer(cb, s_vpt.img[slot].img, VK_IMAGE_LAYOUT_GENERAL,
                                s_vk.readback.buf, 1, &region);
        VkBufferMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = s_vk.readback.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &host, 0, NULL);
    }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    pvkResetFences(s_vk.device, 1, &s_vpt.stage_fences[c]);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS || vk_queue_submit(&si, s_vpt.stage_fences[c]) != VK_SUCCESS) {
        pthread_mutex_unlock(&s_vk.present_mu);
        VK_LOG("present thread: stage submission failed\n");
        return 0;
    }
    __atomic_store_n(&s_vpt.stage_busy[c], 1, __ATOMIC_RELEASE);
    s_vpt.img_fresh[slot] = 0;
    s_vk.present_order = 1;                 /* later writes wait for this copy */
    pthread_mutex_unlock(&s_vk.present_mu);
    if (dump) {
        /* Outside present_mu, so a held stage can leave its hold. Only this
         * thread stages, so `readback` is not reused meanwhile. A dumped
         * frame waits for its copy, as a read-back present does. */
        pvkWaitForFences(s_vk.device, 1, &s_vpt.stage_fences[c], VK_TRUE, UINT64_MAX);
        vk_invalidate(&s_vk.readback);
        const unsigned char* px = (const unsigned char*)s_vk.readback.ptr;
        const size_t centre = ((size_t)(s_vk.height / 2) * s_vk.width + s_vk.width / 2) * 4;
        s_vk.last_center = ((u32)px[centre] << 16) | ((u32)px[centre + 1] << 8) | (u32)px[centre + 2];
        s_vk.present_readback_count++;
        vk_dump_ppm(px);
    }
    return 1;
}

static int vk_pt_show(void* user, uint32_t slot, rsx_present_show_timing* timing)
{
    (void)user;
    if (slot >= RSX_PRESENT_THREAD_MAX_DEPTH || !s_vpt.img[slot].img) return 0;
    const uint64_t t0 = rsx_present_thread_now();
    const u32 k = s_vpt.show_next;
    s_vpt.show_next = (k + 1u) % VK_PT_SHOW_SLOTS;
    if (s_vpt.show_busy[k]) {
        pvkWaitForFences(s_vk.device, 1, &s_vpt.show_fences[k], VK_TRUE, UINT64_MAX);
        s_vpt.show_busy[k] = 0;
    }
    u32 idx = 0;
    if (s_vk.windowed && s_vk_resized) {
        vk_swapchain_lost();
    }
    int window = s_vk.windowed && s_vk.swapchain && !vk_window_hidden() && !s_vk_resized;
    if (window) {
        VkResult r;
        for (;;) {
            r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, 100000000ull /* 100 ms */,
                                       s_vpt.show_acquire[k], VK_NULL_HANDLE, &idx);
            if ((r == VK_TIMEOUT || r == VK_NOT_READY) && !__atomic_load_n(&s_vpt.stopping, __ATOMIC_ACQUIRE))
                continue;
            break;
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); window = 0; }
        else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            if (r != VK_TIMEOUT && r != VK_NOT_READY)
                VK_LOG("present thread: vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r);
            window = 0;
        }
        if (!window) { timing->acquire_ns = rsx_present_thread_now() - t0; return 0; }
    }
    const uint64_t t1 = rsx_present_thread_now();
    timing->acquire_ns = t1 - t0;
    VkCommandBuffer cb = s_vpt.show_cmds[k];
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS || pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        VK_LOG("present thread: show recording failed\n");
        return 0;   /* an acquired image stays unpresented; a later acquire recycles it */
    }
    vk_barrier_image_on(cb, s_vpt.img[slot].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)s_vpt.img_w[slot], (int32_t)s_vpt.img_h[slot], 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
    };
    pvkCmdBlitImage(cb, s_vpt.img[slot].img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                    (s_vpt.img_w[slot] == s_vk.width && s_vpt.img_h[slot] == s_vk.height)
                        ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    if (window) vk_record_window_blit(cb, idx);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = window ? 1u : 0u, .pWaitSemaphores = &s_vpt.show_acquire[k],
                        .pWaitDstStageMask = &wait_stage,
                        .commandBufferCount = 1, .pCommandBuffers = &cb,
                        .signalSemaphoreCount = window ? 1u : 0u,
                        .pSignalSemaphores = window ? &s_vk.sc_done[idx] : NULL };
    pvkResetFences(s_vk.device, 1, &s_vpt.show_fences[k]);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS || vk_queue_submit(&si, s_vpt.show_fences[k]) != VK_SUCCESS) {
        VK_LOG("present thread: show submission failed\n");
        return 0;
    }
    s_vpt.show_busy[k] = 1;
    __atomic_add_fetch(&s_vk.presented, 1, __ATOMIC_RELAXED);
    if (window) {
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
            .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
        };
        const VkResult r = vk_queue_present(&pi);
#ifdef __ANDROID__
        if (r == VK_ERROR_OUT_OF_DATE_KHR) vk_swapchain_lost();
        else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VK_LOG("present thread: vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
#else
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
        else if (r != VK_SUCCESS) VK_LOG("present thread: vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
#endif
    }
    timing->blit_ns = rsx_present_thread_now() - t1;
    return 1;
}

static void vk_pt_discard(void* user, uint32_t slot)
{
    (void)user; (void)slot;   /* the staging image stays for the slot's next frame */
}

static void vk_pt_stop(void)
{
    __atomic_store_n(&s_vpt.stopping, 1, __ATOMIC_RELEASE);
    rsx_present_thread_destroy(s_vpt.pt);   /* joins; discards what is still staged */
    s_vpt.pt = NULL;
    if (!s_vk.device || (!s_vpt.stage_pool && !s_vpt.show_pool)) { memset(&s_vpt, 0, sizeof s_vpt); return; }
    vk_device_idle();
    for (u32 i = 0; i < RSX_PRESENT_THREAD_MAX_DEPTH; i++) vk_pt_destroy_image(i);
    for (u32 i = 0; i < VK_PT_STAGE_CMDS; i++)
        if (s_vpt.stage_fences[i]) pvkDestroyFence(s_vk.device, s_vpt.stage_fences[i], NULL);
    for (u32 i = 0; i < VK_PT_SHOW_SLOTS; i++) {
        if (s_vpt.show_fences[i]) pvkDestroyFence(s_vk.device, s_vpt.show_fences[i], NULL);
        if (s_vpt.show_acquire[i]) pvkDestroySemaphore(s_vk.device, s_vpt.show_acquire[i], NULL);
    }
    if (s_vpt.stage_pool) pvkDestroyCommandPool(s_vk.device, s_vpt.stage_pool, NULL);
    if (s_vpt.show_pool) pvkDestroyCommandPool(s_vk.device, s_vpt.show_pool, NULL);
    memset(&s_vpt, 0, sizeof s_vpt);
}

static void vk_pt_start(void)
{
    if (s_vpt.pt || !s_vk.device) return;
    memset(&s_vpt, 0, sizeof s_vpt);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    int ok = pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vpt.stage_pool) == VK_SUCCESS &&
             pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vpt.show_pool) == VK_SUCCESS;
    if (ok) {
        cai.commandPool = s_vpt.stage_pool; cai.commandBufferCount = VK_PT_STAGE_CMDS;
        ok = pvkAllocateCommandBuffers(s_vk.device, &cai, s_vpt.stage_cmds) == VK_SUCCESS;
    }
    if (ok) {
        cai.commandPool = s_vpt.show_pool; cai.commandBufferCount = VK_PT_SHOW_SLOTS;
        ok = pvkAllocateCommandBuffers(s_vk.device, &cai, s_vpt.show_cmds) == VK_SUCCESS;
    }
    for (u32 i = 0; ok && i < VK_PT_STAGE_CMDS; i++)
        ok = pvkCreateFence(s_vk.device, &fci, NULL, &s_vpt.stage_fences[i]) == VK_SUCCESS;
    for (u32 i = 0; ok && i < VK_PT_SHOW_SLOTS; i++)
        ok = pvkCreateFence(s_vk.device, &fci, NULL, &s_vpt.show_fences[i]) == VK_SUCCESS &&
             pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vpt.show_acquire[i]) == VK_SUCCESS;
    if (ok) {
        const rsx_present_hooks hooks = { NULL, vk_pt_stage, vk_pt_show, vk_pt_discard };
        s_vpt.pt = rsx_present_thread_create(&hooks,
                       rsx_present_thread_depth_from(getenv("PS3RECOMP_RSX_PRESENT_THREAD_DEPTH")));
    }
    if (!s_vpt.pt) {
        VK_LOG("present thread could not start; presents stay on the presenting thread\n");
        vk_pt_stop();
    }
}

/* ---------------------------------------------------------------------------
 * Dual-Screen 2-Phone Wi-Fi Co-op (Host Split + Async 640x360 P2 Capture)
 *
 * When enabled on Phone 1 (Host):
 *   - mode 0 (off)         : disabled (zero GPU/RAM/CPU overhead)
 *   - mode 1 (auto)        : observes left-half / right-half 3D draw viewports
 *                            & scissors each frame with hysteresis so Phone 1
 *                            shows full screen in menus/cutscenes/close co-op
 *                            and switches seamlessly to LEFT-HALF-ONLY fullscreen
 *                            for Player 1 the instant vertical split activates!
 *   - mode 2 (force_split) : always displays LEFT half on Phone 1, streams
 *                            RIGHT half to Phone 2.
 *   - mode 3 (mirror)      : full screen on both Phone 1 and Phone 2.
 *
 * Phone 2 video frames are downscaled to 640x360 asynchronously into a 2-slot
 * host-cached readback ring with dedicated fences (NEVER blocking the RSX
 * game thread). The Java MediaCodec H.264 encoder thread calls
 * rsx_vulkan_backend_coop_fill_yuv() to convert straight into Image.Plane
 * Y/U/V buffers with exact vendor row/pixel strides.
 * -------------------------------------------------------------------------*/
enum {
    VK_COOP_OFF         = 0,
    VK_COOP_AUTO        = 1,
    VK_COOP_FORCE_SPLIT = 2,
    VK_COOP_MIRROR      = 3,
};

enum {
    VK_COOP_ASPECT_BALANCED = 0, /* 78% centered width (natural wide feel + full HUD) */
    VK_COOP_ASPECT_FILL     = 1, /* 100% full width */
    VK_COOP_ASPECT_FIT      = 2, /* exact pixel aspect ratio (pillarboxed) */
};

#define VK_COOP_W      640u
#define VK_COOP_H      360u
#define VK_COOP_SLOTS  2u

static struct {
    int             mode;              /* atomic */
    int             aspect_mode;       /* atomic */
    int             client_connected;  /* atomic */
    int             split_active;      /* atomic */
    int             stop_atomic;       /* atomic */
    u32             left_draws;
    u32             right_draws;
    int             split_hist;
    int             gpu_ready;
    int             gpu_failed;
    VkCommandPool   pool;
    VkCommandBuffer cmd[VK_COOP_SLOTS];
    VkFence         fence[VK_COOP_SLOTS];
    vk_image        img;               /* 640x360 RGBA8 intermediate target */
    int             img_fresh;
    vk_buffer       rb[VK_COOP_SLOTS]; /* 640x360x4 host-cached readback ring */
    int             slot_state[VK_COOP_SLOTS]; /* atomic: 0=FREE, 1=SUBMITTED */
    int             slot_split[VK_COOP_SLOTS];
    u64             slot_seq[VK_COOP_SLOTS];
    u64             next_seq;
    u32             next_slot;
    int             active_readers;    /* atomic: encoder threads touching rb[] */
#ifndef _WIN32
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    clockid_t       clock_id;
    int             cv_init;
#endif
} s_coop = {
    .mode = VK_COOP_OFF,
    .aspect_mode = VK_COOP_ASPECT_BALANCED,
#ifndef _WIN32
    .mu = PTHREAD_MUTEX_INITIALIZER,
#endif
};

#ifndef _WIN32
static void vk_coop_cv_ensure_locked(void)
{
    if (s_coop.cv_init) return;
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) {
        s_coop.clock_id = CLOCK_MONOTONIC;
    } else {
        s_coop.clock_id = CLOCK_REALTIME;
    }
    pthread_cond_init(&s_coop.cv, &attr);
    pthread_condattr_destroy(&attr);
    s_coop.cv_init = 1;
}
#endif

static void vk_coop_configure(int mode, int aspect_mode)
{
    if (mode < VK_COOP_OFF || mode > VK_COOP_MIRROR) mode = VK_COOP_OFF;
    if (aspect_mode < VK_COOP_ASPECT_BALANCED || aspect_mode > VK_COOP_ASPECT_FIT)
        aspect_mode = VK_COOP_ASPECT_BALANCED;
    __atomic_store_n(&s_coop.mode, mode, __ATOMIC_RELEASE);
    __atomic_store_n(&s_coop.aspect_mode, aspect_mode, __ATOMIC_RELEASE);
    if (mode == VK_COOP_OFF || mode == VK_COOP_MIRROR)
        __atomic_store_n(&s_coop.split_active, 0, __ATOMIC_RELEASE);
    else if (mode == VK_COOP_FORCE_SPLIT)
        __atomic_store_n(&s_coop.split_active, 1, __ATOMIC_RELEASE);
    VK_LOG("Co-op Dual-Screen configured: mode=%d aspect=%d\n", mode, aspect_mode);
}

static void vk_coop_set_client_connected(int connected)
{
    const int v = connected ? 1 : 0;
    __atomic_store_n(&s_coop.client_connected, v, __ATOMIC_RELEASE);
    if (v) __atomic_store_n(&s_coop.stop_atomic, 0, __ATOMIC_RELEASE);
#ifndef _WIN32
    pthread_mutex_lock(&s_coop.mu);
    vk_coop_cv_ensure_locked();
    pthread_cond_broadcast(&s_coop.cv);
    pthread_mutex_unlock(&s_coop.mu);
#endif
}

static int vk_coop_get_mode(void)
{
    return __atomic_load_n(&s_coop.mode, __ATOMIC_ACQUIRE);
}

static int vk_coop_is_split_active(void)
{
    return __atomic_load_n(&s_coop.split_active, __ATOMIC_ACQUIRE);
}

static void vk_coop_observe_draw(u32 w, u32 h, const VkViewport* vp, const VkRect2D* sc)
{
    if (__atomic_load_n(&s_coop.mode, __ATOMIC_RELAXED) != VK_COOP_AUTO) return;
    if (w < 320u || h < 180u) return;
    const float vx0 = vp->x, vx1 = vp->x + vp->width;
    const float vy0 = vp->y, vy1 = vp->y + vp->height;
    const float sx0 = (float)sc->offset.x, sx1 = (float)(sc->offset.x + (int32_t)sc->extent.width);
    const float sy0 = (float)sc->offset.y, sy1 = (float)(sc->offset.y + (int32_t)sc->extent.height);
    const float ex0 = vx0 > sx0 ? vx0 : sx0;
    const float ex1 = vx1 < sx1 ? vx1 : sx1;
    const float ey0 = vy0 > sy0 ? vy0 : sy0;
    const float ey1 = vy1 < sy1 ? vy1 : sy1;
    const float ew = ex1 - ex0;
    const float eh = ey1 - ey0;
    const float fw = (float)w, fh = (float)h;
    if (ew >= fw * 0.36f && ew <= fw * 0.56f && eh >= fh * 0.58f) {
        if (ex0 <= fw * 0.08f && ex1 <= fw * 0.56f) s_coop.left_draws++;
        else if (ex0 >= fw * 0.44f && ex1 >= fw * 0.90f) s_coop.right_draws++;
    }
}

static int vk_coop_on_present_decide(void)
{
    const int mode = __atomic_load_n(&s_coop.mode, __ATOMIC_ACQUIRE);
    const u32 ld = s_coop.left_draws;
    const u32 rd = s_coop.right_draws;
    s_coop.left_draws = 0;
    s_coop.right_draws = 0;

    int split = 0;
    if (mode == VK_COOP_FORCE_SPLIT) {
        split = 1;
    } else if (mode == VK_COOP_AUTO) {
        const int cur = __atomic_load_n(&s_coop.split_active, __ATOMIC_RELAXED);
        const int det = (ld >= 6u && rd >= 6u);
        if (det) {
            if (s_coop.split_hist < 6) s_coop.split_hist++;
        } else {
            if (s_coop.split_hist > 0) s_coop.split_hist--;
        }
        if (s_coop.split_hist >= 3) split = 1;
        else if (s_coop.split_hist == 0) split = 0;
        else split = cur;
    } else {
        s_coop.split_hist = 0;
        split = 0;
    }
    __atomic_store_n(&s_coop.split_active, split, __ATOMIC_RELEASE);
    return split;
}

static void vk_coop_compute_dst_x(u32 src_w, u32 src_h, u32 dst_w, u32 dst_h,
                                  int split_now, int32_t* out_dx0, int32_t* out_dx1)
{
    if (!split_now) {
        *out_dx0 = 0;
        *out_dx1 = (int32_t)dst_w;
        return;
    }
    const int aspect = __atomic_load_n(&s_coop.aspect_mode, __ATOMIC_RELAXED);
    u32 target_w = dst_w;
    if (aspect == VK_COOP_ASPECT_FIT && src_h > 0u) {
        target_w = (u32)(((u64)dst_h * src_w + src_h / 2u) / src_h);
        if (target_w > dst_w) target_w = dst_w;
        if (target_w < 64u) target_w = 64u;
    } else if (aspect == VK_COOP_ASPECT_BALANCED) {
        target_w = (dst_w * 78u) / 100u;
    }
    const u32 pad = (dst_w > target_w) ? ((dst_w - target_w) / 2u) : 0u;
    *out_dx0 = (int32_t)pad;
    *out_dx1 = (int32_t)(pad + target_w);
}

/* Blit guest surface `o` onto Phone 1 display target `dst_img` (`dst_w x dst_h`).
 * In split mode, crops strictly the LEFT half (Player 1) so Phone 1 stays
 * fullscreen P1 with zero split divider visible. */
static void vk_coop_blit_p1(VkCommandBuffer cb, const vk_eng_obj* o,
                            VkImage dst_img, u32 dst_w, u32 dst_h, int split_now)
{
    const int32_t sx0 = 0;
    const int32_t sx1 = split_now ? (int32_t)((o->w * 496u) / 1000u) : (int32_t)o->w;
    int32_t dx0 = 0, dx1 = (int32_t)dst_w;
    vk_coop_compute_dst_x((u32)(sx1 - sx0), o->h, dst_w, dst_h, split_now, &dx0, &dx1);
    if (dx0 > 0) {
        static const VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pvkCmdClearColorImage(cb, dst_img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &r);
        vk_barrier_image_on(cb, dst_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    }
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { sx0, 0, 0 }, { sx1, (int32_t)o->h, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { dx0, 0, 0 }, { dx1, (int32_t)dst_h, 1 } },
    };
    const VkFilter flt = (!split_now && o->w == dst_w && o->h == dst_h) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    pvkCmdBlitImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    dst_img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, flt);
}

static int vk_coop_ensure_gpu(void)
{
    if (s_coop.gpu_ready) return 1;
    if (s_coop.gpu_failed || !s_vk.device) return 0;

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = s_vk.queue_family,
    };
    if (pvkCreateCommandPool(s_vk.device, &cpci, NULL, &s_coop.pool) != VK_SUCCESS) {
        s_coop.gpu_failed = 1;
        return 0;
    }
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s_coop.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = VK_COOP_SLOTS,
    };
    if (pvkAllocateCommandBuffers(s_vk.device, &cbai, s_coop.cmd) != VK_SUCCESS) {
        s_coop.gpu_failed = 1;
        return 0;
    }
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (u32 i = 0; i < VK_COOP_SLOTS; i++) {
        if (pvkCreateFence(s_vk.device, &fci, NULL, &s_coop.fence[i]) != VK_SUCCESS) {
            s_coop.gpu_failed = 1;
            return 0;
        }
    }
    if (vk_create_image(&s_coop.img, VK_FORMAT_R8G8B8A8_UNORM, VK_COOP_W, VK_COOP_H,
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
        s_coop.gpu_failed = 1;
        return 0;
    }
    s_coop.img_fresh = 1;
    const VkDeviceSize rb_sz = (VkDeviceSize)VK_COOP_W * VK_COOP_H * 4u;
    for (u32 i = 0; i < VK_COOP_SLOTS; i++) {
        if (vk_create_host_buffer(&s_coop.rb[i], rb_sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0) {
            s_coop.gpu_failed = 1;
            return 0;
        }
        __atomic_store_n(&s_coop.slot_state[i], 0, __ATOMIC_RELEASE);
    }
    s_coop.gpu_ready = 1;
    VK_LOG("Co-op P2 async stream capture ready (%ux%u x %u slots)\n",
           (unsigned)VK_COOP_W, (unsigned)VK_COOP_H, (unsigned)VK_COOP_SLOTS);
    return 1;
}

static void vk_coop_capture_p2_async(const vk_eng_obj* o, int split_now)
{
    if (!o || !s_vk.device) return;
    if (__atomic_load_n(&s_coop.mode, __ATOMIC_RELAXED) == VK_COOP_OFF) return;
    if (!__atomic_load_n(&s_coop.client_connected, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&s_coop.stop_atomic, __ATOMIC_ACQUIRE)) return;
    if (!vk_coop_ensure_gpu()) return;

    const u32 slot = s_coop.next_slot;
    if (__atomic_load_n(&s_coop.slot_state[slot], __ATOMIC_ACQUIRE) != 0) {
        /* Encoder thread still reading this slot: drop frame cleanly (zero stall). */
        return;
    }
    s_coop.next_slot = (slot + 1u) % VK_COOP_SLOTS;

    VkCommandBuffer cb = s_coop.cmd[slot];
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS || pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS)
        return;

    vk_cmd_after_everything(cb);
    vk_barrier_image_on(cb, o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image_on(cb, s_coop.img.img, VK_IMAGE_ASPECT_COLOR_BIT,
                        s_coop.img_fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL);
    s_coop.img_fresh = 0;

    /* Player 2 gets RIGHT half during split, or full screen when mirrored/menu. */
    const int32_t sx0 = split_now ? (int32_t)((o->w * 504u) / 1000u) : 0;
    const int32_t sx1 = (int32_t)o->w;
    int32_t dx0 = 0, dx1 = (int32_t)VK_COOP_W;
    vk_coop_compute_dst_x((u32)(sx1 - sx0), o->h, VK_COOP_W, VK_COOP_H, split_now, &dx0, &dx1);
    if (dx0 > 0) {
        static const VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pvkCmdClearColorImage(cb, s_coop.img.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &r);
        vk_barrier_image_on(cb, s_coop.img.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    }
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { sx0, 0, 0 }, { sx1, (int32_t)o->h, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { dx0, 0, 0 }, { dx1, (int32_t)VK_COOP_H, 1 } },
    };
    pvkCmdBlitImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    s_coop.img.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    vk_barrier_image_on(cb, s_coop.img.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);

    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { VK_COOP_W, VK_COOP_H, 1 },
    };
    pvkCmdCopyImageToBuffer(cb, s_coop.img.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_coop.rb[slot].buf, 1, &region);
    VkBufferMemoryBarrier hb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_coop.rb[slot].buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &hb, 0, NULL);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) return;

    pvkResetFences(s_vk.device, 1, &s_coop.fence[slot]);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
    };
    if (vk_queue_submit(&si, s_coop.fence[slot]) != VK_SUCCESS) return;

    s_vk.present_order = 1;
    s_coop.slot_split[slot] = split_now;
    s_coop.slot_seq[slot] = ++s_coop.next_seq;
    __atomic_store_n(&s_coop.slot_state[slot], 1, __ATOMIC_RELEASE);
#ifndef _WIN32
    pthread_mutex_lock(&s_coop.mu);
    vk_coop_cv_ensure_locked();
    pthread_cond_signal(&s_coop.cv);
    pthread_mutex_unlock(&s_coop.mu);
#endif
}

static void vk_coop_wait_gpu(void)
{
    if (!s_coop.gpu_ready || !s_vk.device) return;
    for (u32 i = 0; i < VK_COOP_SLOTS; i++) {
        if (__atomic_load_n(&s_coop.slot_state[i], __ATOMIC_ACQUIRE) && s_coop.fence[i])
            pvkWaitForFences(s_vk.device, 1, &s_coop.fence[i], VK_TRUE, UINT64_MAX);
    }
}

#ifndef _WIN32
static int vk_coop_acquire_ready_slot(u32 timeout_ms, u32* out_slot)
{
    if (__atomic_load_n(&s_coop.stop_atomic, __ATOMIC_ACQUIRE) || !s_vk.device) return -1;
    pthread_mutex_lock(&s_coop.mu);
    vk_coop_cv_ensure_locked();

    struct timespec dl;
    clock_gettime(s_coop.clock_id, &dl);
    dl.tv_sec += (time_t)(timeout_ms / 1000u);
    dl.tv_nsec += (long)((timeout_ms % 1000u) * 1000000u);
    while (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }

    int found = -1;
    for (;;) {
        if (__atomic_load_n(&s_coop.stop_atomic, __ATOMIC_ACQUIRE) || !s_vk.device) {
            pthread_mutex_unlock(&s_coop.mu);
            return -1;
        }
        /* Prefer the newest submitted slot; release any older superseded slot. */
        u64 best_seq = 0;
        for (u32 i = 0; i < VK_COOP_SLOTS; i++) {
            if (__atomic_load_n(&s_coop.slot_state[i], __ATOMIC_ACQUIRE) == 1) {
                if (s_coop.slot_seq[i] > best_seq) {
                    best_seq = s_coop.slot_seq[i];
                    found = (int)i;
                }
            }
        }
        if (found >= 0) {
            __atomic_add_fetch(&s_coop.active_readers, 1, __ATOMIC_ACQ_REL);
            /* Drop stale older slot if both were ready so latency stays lowest. */
            for (u32 i = 0; i < VK_COOP_SLOTS; i++) {
                if ((int)i != found && __atomic_load_n(&s_coop.slot_state[i], __ATOMIC_ACQUIRE) == 1)
                    __atomic_store_n(&s_coop.slot_state[i], 0, __ATOMIC_RELEASE);
            }
            break;
        }
        int rc = pthread_cond_timedwait(&s_coop.cv, &s_coop.mu, &dl);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&s_coop.mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&s_coop.mu);

    if (pvkWaitForFences(s_vk.device, 1, &s_coop.fence[found], VK_TRUE, 250000000ull) != VK_SUCCESS) {
        __atomic_store_n(&s_coop.slot_state[found], 0, __ATOMIC_RELEASE);
        __atomic_sub_fetch(&s_coop.active_readers, 1, __ATOMIC_ACQ_REL);
        return 0;
    }
    vk_invalidate(&s_coop.rb[found]);
    *out_slot = (u32)found;
    return 1;
}

static inline u8 vk_coop_clamp_u8(int v)
{
    return (u8)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* Fast BT.601 studio-range RGBA8 -> YUV420 conversion directly into Android
 * MediaCodec Image.Plane buffers (supports both NV12 uv_pstride=2 and I420 uv_pstride=1). */
static int vk_coop_fill_yuv(u8* y_dst, u32 y_stride,
                            u8* u_dst, u32 u_stride,
                            u8* v_dst, u32 v_stride,
                            u32 uv_pstride,
                            u32* out_w, u32* out_h,
                            int* out_split, u32 timeout_ms)
{
    if (!y_dst || !u_dst || !v_dst || !uv_pstride) return -1;
    u32 slot = 0;
    const int rc = vk_coop_acquire_ready_slot(timeout_ms, &slot);
    if (rc <= 0) return rc;

    const u8* rgba = (const u8*)s_coop.rb[slot].ptr;
    const u32 w = VK_COOP_W;
    const u32 h = VK_COOP_H;
    const u32 row_bytes = w * 4u;
    for (u32 y = 0; y < h; y += 2u) {
        const u8* r0 = rgba + (size_t)y * row_bytes;
        const u8* r1 = r0 + row_bytes;
        u8* dy0 = y_dst + (size_t)y * y_stride;
        u8* dy1 = dy0 + y_stride;
        u8* du  = u_dst + (size_t)(y >> 1) * u_stride;
        u8* dv  = v_dst + (size_t)(y >> 1) * v_stride;
        for (u32 x = 0; x < w; x += 2u) {
            const u32 x4 = x * 4u;
            const int r00 = r0[x4 + 0], g00 = r0[x4 + 1], b00 = r0[x4 + 2];
            const int r01 = r0[x4 + 4], g01 = r0[x4 + 5], b01 = r0[x4 + 6];
            const int r10 = r1[x4 + 0], g10 = r1[x4 + 1], b10 = r1[x4 + 2];
            const int r11 = r1[x4 + 4], g11 = r1[x4 + 5], b11 = r1[x4 + 6];

            dy0[x + 0] = vk_coop_clamp_u8(((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16);
            dy0[x + 1] = vk_coop_clamp_u8(((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16);
            dy1[x + 0] = vk_coop_clamp_u8(((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16);
            dy1[x + 1] = vk_coop_clamp_u8(((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16);

            const int ra = (r00 + r01 + r10 + r11 + 2) >> 2;
            const int ga = (g00 + g01 + g10 + g11 + 2) >> 2;
            const int ba = (b00 + b01 + b10 + b11 + 2) >> 2;
            const size_t uv_off = (size_t)(x >> 1) * uv_pstride;
            du[uv_off] = vk_coop_clamp_u8(((-38 * ra - 74 * ga + 112 * ba + 128) >> 8) + 128);
            dv[uv_off] = vk_coop_clamp_u8(((112 * ra - 94 * ga -  18 * ba + 128) >> 8) + 128);
        }
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_split) *out_split = s_coop.slot_split[slot];
    __atomic_store_n(&s_coop.slot_state[slot], 0, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&s_coop.active_readers, 1, __ATOMIC_ACQ_REL);
    return 1;
}

static int vk_coop_fill_rgba(u8* rgba_dst, u32 dst_cap,
                             u32* out_w, u32* out_h,
                             int* out_split, u32 timeout_ms)
{
    const u32 need = VK_COOP_W * VK_COOP_H * 4u;
    if (!rgba_dst || dst_cap < need) return -1;
    u32 slot = 0;
    const int rc = vk_coop_acquire_ready_slot(timeout_ms, &slot);
    if (rc <= 0) return rc;

    memcpy(rgba_dst, s_coop.rb[slot].ptr, need);
    if (out_w) *out_w = VK_COOP_W;
    if (out_h) *out_h = VK_COOP_H;
    if (out_split) *out_split = s_coop.slot_split[slot];
    __atomic_store_n(&s_coop.slot_state[slot], 0, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&s_coop.active_readers, 1, __ATOMIC_ACQ_REL);
    return 1;
}

static void vk_coop_shutdown(void)
{
    __atomic_store_n(&s_coop.stop_atomic, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_coop.client_connected, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&s_coop.mu);
    if (s_coop.cv_init) pthread_cond_broadcast(&s_coop.cv);
    pthread_mutex_unlock(&s_coop.mu);

    for (int w = 0; w < 50 && __atomic_load_n(&s_coop.active_readers, __ATOMIC_ACQUIRE) > 0; w++) {
        struct timespec ts = { 0, 5000000L };
        nanosleep(&ts, NULL);
    }
    if (s_vk.device && s_coop.gpu_ready) {
        vk_device_idle();
        for (u32 i = 0; i < VK_COOP_SLOTS; i++) {
            vk_destroy_buffer(&s_coop.rb[i]);
            if (s_coop.fence[i]) { pvkDestroyFence(s_vk.device, s_coop.fence[i], NULL); s_coop.fence[i] = VK_NULL_HANDLE; }
            __atomic_store_n(&s_coop.slot_state[i], 0, __ATOMIC_RELEASE);
        }
        vk_destroy_image(&s_coop.img);
        if (s_coop.pool) { pvkDestroyCommandPool(s_vk.device, s_coop.pool, NULL); s_coop.pool = VK_NULL_HANDLE; }
    }
    s_coop.gpu_ready = 0;
    s_coop.gpu_failed = 0;
}
#else
static int vk_coop_fill_yuv(u8* y_dst, u32 y_stride, u8* u_dst, u32 u_stride,
                            u8* v_dst, u32 v_stride, u32 uv_pstride,
                            u32* out_w, u32* out_h, int* out_split, u32 timeout_ms)
{
    (void)y_dst; (void)y_stride; (void)u_dst; (void)u_stride; (void)v_dst; (void)v_stride;
    (void)uv_pstride; (void)out_w; (void)out_h; (void)out_split; (void)timeout_ms;
    return -1;
}
static int vk_coop_fill_rgba(u8* rgba_dst, u32 dst_cap, u32* out_w, u32* out_h,
                             int* out_split, u32 timeout_ms)
{
    (void)rgba_dst; (void)dst_cap; (void)out_w; (void)out_h; (void)out_split; (void)timeout_ms;
    return -1;
}
static void vk_coop_shutdown(void) { s_coop.gpu_ready = 0; }
#endif

/* ---------------------------------------------------------------------------
 * GPU Optical-Flow 2x Frame Generation (PS3RECOMP_VK_FRAME_GEN = off|flow|blend)
 *
 * Android Vulkan GPUs (Adreno / Mali) do not expose NVIDIA Tensor Cores, so
 * this implements a Vulkan-native GPU motion-compensated 2x frame interpolator
 * (equivalent DLSS-FG / FSR3-FG concept) directly into the swapchain:
 *   - flow  : Symmetric bidirectional coarse-to-fine optical flow at t = 0.5
 *             with static-HUD protection and disocclusion / scene-cut fallback.
 *   - blend : Ultra-lightweight 2-tap temporal blend with static-HUD and
 *             scene-cut guard for low-end GPUs.
 *
 * Lock discipline: s_fg.mu and s_vk.present_mu are NEVER nested.
 *   1. Real frame arrives -> vk_fg_try_present() copies surface into hist[next]
 *      and immediately presents the interpolated midpoint frame FG(prev, cur)
 *      under s_vk.present_mu, then wakes the pacer thread under s_fg.mu.
 *   2. Half a frame period later (~0.48 * dt_avg), the pacer thread wakes,
 *      releases s_fg.mu, acquires s_vk.present_mu, verifies no newer frame
 *      superseded it, and presents the crisp REAL frame (hist[next]) through
 *      vk_record_window_blit() (preserving FSR1/CAS/Integer/Linear scaling).
 *   3. When the game is already running near 60 real FPS (dt_avg <= 17.5ms),
 *      interpolation is bypassed automatically so zero latency is added.
 * -------------------------------------------------------------------------*/
enum {
    VK_FG_OFF   = 0,
    VK_FG_BLEND = 1,
    VK_FG_FLOW  = 2,
};

static struct {
    int                   mode;           /* -1 = unread */
    int                   pipe_tried;
    int                   pipe_ready;
    VkFormat              rp_fmt;
    VkRenderPass          rp;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout      layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       sets[3];        /* pre-bound pairs: (0->1), (1->2), (2->0) */
    VkSampler             sampler;
    vk_buffer             ubo;
    VkShaderModule        vs, fs;
    VkPipeline            pipe;
    vk_image              hist[3];        /* R8G8B8A8_UNORM history ring (s_vk.width x height) */
    int                   hist_fresh[3];  /* 1 while still in UNDEFINED layout */
    VkImageView           sc_views[8];
    VkFramebuffer         sc_fbs[8];
    VkExtent2D            fb_ext;
    u32                   fb_count;
    /* Pacer thread & dedicated Vulkan objects (accessed under present_mu). */
    VkCommandPool         pacer_pool;
    VkCommandBuffer       pacer_cmd;
    VkFence               pacer_fence;
    VkSemaphore           pacer_sem;
    int                   pacer_busy;
    u32                   pacer_slot;
    int                   has_prev;
    u32                   prev_slot;
    u32                   cur_slot;
    double                last_present_s;
    double                dt_avg_s;
    u32                   gen_frames;     /* atomic: total generated frames presented */
    u64                   latest_seq;     /* atomic: monotonically incremented on real present */
    int                   stop_atomic;    /* atomic: 1 during shutdown */
#ifndef _WIN32
    pthread_t             pacer_thread;
    int                   thread_started;
    pthread_mutex_t       mu;
    pthread_cond_t        cv;
    int                   sync_init;
    clockid_t             clock_id;
    int                   stop;
    int                   pending_real;
    u32                   pending_slot;
    u64                   pending_seq;
    struct timespec       deadline;
#endif
} s_fg = { -1 };

static int vk_fg_mode(void)
{
    if (s_fg.mode < 0) {
        const char* e = getenv("PS3RECOMP_VK_FRAME_GEN");
        if (!e || !*e || !strcmp(e, "off") || !strcmp(e, "0") || !strcmp(e, "none"))
            s_fg.mode = VK_FG_OFF;
        else if (!strcmp(e, "blend") || !strcmp(e, "fast") || !strcmp(e, "1"))
            s_fg.mode = VK_FG_BLEND;
        else if (!strcmp(e, "flow") || !strcmp(e, "dlss") || !strcmp(e, "optical") || !strcmp(e, "2"))
            s_fg.mode = VK_FG_FLOW;
        else
            s_fg.mode = VK_FG_OFF;
        VK_LOG("frame generation mode: %s\n",
               s_fg.mode == VK_FG_FLOW  ? "Optical-Flow 2x (GPU Motion Interpolation)" :
               s_fg.mode == VK_FG_BLEND ? "Fast Blend 2x (Low-End)" : "Off");
    }
    return s_fg.mode;
}

static void vk_fg_destroy_fbs(void)
{
    if (!s_vk.device) return;
    for (u32 i = 0; i < 8; i++) {
        if (s_fg.sc_fbs[i])   { pvkDestroyFramebuffer(s_vk.device, s_fg.sc_fbs[i], NULL); s_fg.sc_fbs[i] = VK_NULL_HANDLE; }
        if (s_fg.sc_views[i]) { pvkDestroyImageView(s_vk.device, s_fg.sc_views[i], NULL); s_fg.sc_views[i] = VK_NULL_HANDLE; }
    }
    s_fg.fb_count = 0;
    s_fg.fb_ext.width = s_fg.fb_ext.height = 0;
    s_fg.pacer_busy = 0;
}

static const char* const s_fg_fs_hlsl =
    "cbuffer FGParams : register(b0) {\n"
    "    float4 src_info; /* (src_w, src_h, inv_w, inv_h) */\n"
    "    float4 cfg;      /* (alpha=0.5, mode: 1=blend 2=flow, 0, 0) */\n"
    "};\n"
    "Texture2D prev_tex : register(t0);\n"
    "Texture2D cur_tex  : register(t1);\n"
    "SamplerState fg_samp : register(s0);\n"
    "float3 sp(float2 uv) { return prev_tex.SampleLevel(fg_samp, uv, 0.0).rgb; }\n"
    "float3 sc(float2 uv) { return cur_tex.SampleLevel(fg_samp, uv, 0.0).rgb; }\n"
    "float cdiff(float3 a, float3 b) { return dot(abs(a - b), float3(0.35, 0.50, 0.15)); }\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    float3 p0 = sp(uv);\n"
    "    float3 c0 = sc(uv);\n"
    "    float d0 = cdiff(p0, c0);\n"
    "    /* Static HUD, UI overlays and stationary pixels pass straight through */\n"
    "    if (d0 < 0.022) return float4(c0, 1.0);\n"
    "    if (cfg.y < 1.5) {\n"
    "        /* Mode 1: Fast Blend 2x for low-end GPUs (2 texture taps total) */\n"
    "        float w = 0.5 * saturate(1.0 - (d0 - 0.20) * 3.4);\n"
    "        return float4(lerp(c0, p0, w), 1.0);\n"
    "    }\n"
    "    /* Mode 2: Symmetric Bidirectional Optical-Flow 2x (t = 0.5) */\n"
    "    float2 inv = src_info.zw;\n"
    "    float2 best_v = float2(0.0, 0.0);\n"
    "    float  best_e = d0 - 0.012;\n"
    "    float3 best_a = p0;\n"
    "    float3 best_b = c0;\n"
    "    /* Stage 1: Coarse 8-direction ring (step = 5 px -> covers +/-10 px/frame) */\n"
    "    float2 r1 = inv * 5.0;\n"
    "    float2 d1 = inv * 3.5355;\n"
    "    float2 dirs1[8] = {\n"
    "        float2( r1.x, 0.0), float2(-r1.x, 0.0), float2(0.0,  r1.y), float2(0.0, -r1.y),\n"
    "        float2( d1.x,  d1.y), float2(-d1.x,  d1.y), float2( d1.x, -d1.y), float2(-d1.x, -d1.y)\n"
    "    };\n"
    "    [unroll] for (int i = 0; i < 8; i++) {\n"
    "        float2 v = dirs1[i];\n"
    "        float3 a = sp(uv - v);\n"
    "        float3 b = sc(uv + v);\n"
    "        float e = cdiff(a, b) + 0.012;\n"
    "        if (e < best_e) { best_e = e; best_v = v; best_a = a; best_b = b; }\n"
    "    }\n"
    "    /* Stage 2: Medium 4-way refinement (+/-2.0 px around coarse vector) */\n"
    "    float2 r2 = inv * 2.0;\n"
    "    float2 dirs2[4] = {\n"
    "        float2( r2.x, 0.0), float2(-r2.x, 0.0), float2(0.0,  r2.y), float2(0.0, -r2.y)\n"
    "    };\n"
    "    float2 base2 = best_v;\n"
    "    [unroll] for (int j = 0; j < 4; j++) {\n"
    "        float2 v = base2 + dirs2[j];\n"
    "        float3 a = sp(uv - v);\n"
    "        float3 b = sc(uv + v);\n"
    "        float e = cdiff(a, b) + 0.008;\n"
    "        if (e < best_e) { best_e = e; best_v = v; best_a = a; best_b = b; }\n"
    "    }\n"
    "    /* Stage 3: Sub-pixel fine refinement (+/-0.75 px) */\n"
    "    float2 r3 = inv * 0.75;\n"
    "    float2 dirs3[4] = {\n"
    "        float2( r3.x, 0.0), float2(-r3.x, 0.0), float2(0.0,  r3.y), float2(0.0, -r3.y)\n"
    "    };\n"
    "    float2 base3 = best_v;\n"
    "    [unroll] for (int k = 0; k < 4; k++) {\n"
    "        float2 v = base3 + dirs3[k];\n"
    "        float3 a = sp(uv - v);\n"
    "        float3 b = sc(uv + v);\n"
    "        float e = cdiff(a, b) + 0.004;\n"
    "        if (e < best_e) { best_e = e; best_v = v; best_a = a; best_b = b; }\n"
    "    }\n"
    "    /* Disocclusion / scene-cut confidence guard: fall back cleanly to cur */\n"
    "    float conf = saturate(1.0 - (best_e - 0.13) * 3.2);\n"
    "    float3 interp = 0.5 * (best_a + best_b);\n"
    "    return float4(lerp(c0, interp, conf), 1.0);\n"
    "}\n";

#ifndef _WIN32
/* present_mu held: present the real frame stored in hist[hist_slot] through
 * vk_record_window_blit() so the user's selected Lossless Scaling upscaler
 * (FSR1 / CAS / Integer / Linear) runs on every real frame. */
static void vk_fg_present_real_locked(u32 hist_slot)
{
    if (!s_vk.device || hist_slot >= 3u || !s_fg.hist[hist_slot].img) return;
    if (s_fg.pacer_busy) {
        pvkWaitForFences(s_vk.device, 1, &s_fg.pacer_fence, VK_TRUE, UINT64_MAX);
        s_fg.pacer_busy = 0;
    }
    if (s_vk.windowed && s_vk_resized) vk_swapchain_lost();
    if (!s_vk.windowed || !s_vk.swapchain || vk_window_hidden() || s_vk_resized) return;
    if (!s_fg.pacer_sem) return;

    u32 idx = 0;
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, 50000000ull /* 50 ms */,
                                        s_fg.pacer_sem, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;

    VkCommandBuffer cb = s_fg.pacer_cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS || pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS)
        return;

    vk_cmd_after_everything(cb);
    vk_barrier_image_on(cb, s_fg.hist[hist_slot].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy ic = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { s_vk.width, s_vk.height, 1 },
    };
    pvkCmdCopyImage(cb, s_fg.hist[hist_slot].img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    vk_record_window_blit(cb, idx);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) return;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_fg.pacer_sem,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s_vk.sc_done[idx],
    };
    pvkResetFences(s_vk.device, 1, &s_fg.pacer_fence);
    if (vk_queue_submit(&si, s_fg.pacer_fence) != VK_SUCCESS) {
        vk_device_idle();
        pvkDestroySemaphore(s_vk.device, s_fg.pacer_sem, NULL);
        s_fg.pacer_sem = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_fg.pacer_sem);
        return;
    }
    s_fg.pacer_busy = 1;
    s_fg.pacer_slot = hist_slot;

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    r = vk_queue_present(&pi);
#ifdef __ANDROID__
    if (r == VK_ERROR_OUT_OF_DATE_KHR) vk_swapchain_lost();
#else
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
#endif
}

static void* vk_fg_pacer_thread(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&s_fg.mu);
    while (!s_fg.stop) {
        if (!s_fg.pending_real) {
            pthread_cond_wait(&s_fg.cv, &s_fg.mu);
            continue;
        }
        struct timespec dl = s_fg.deadline;
        int rc = pthread_cond_timedwait(&s_fg.cv, &s_fg.mu, &dl);
        if (s_fg.stop) break;
        if (!s_fg.pending_real) continue;
        int due = (rc == ETIMEDOUT);
        if (!due) {
            struct timespec now;
            clock_gettime(s_fg.clock_id, &now);
            if (now.tv_sec > s_fg.deadline.tv_sec ||
                (now.tv_sec == s_fg.deadline.tv_sec && now.tv_nsec >= s_fg.deadline.tv_nsec))
                due = 1;
        }
        if (!due) continue;
        const u32 slot = s_fg.pending_slot;
        const u64 seq  = s_fg.pending_seq;
        s_fg.pending_real = 0;
        pthread_mutex_unlock(&s_fg.mu);

        /* Acquire present_mu strictly without holding s_fg.mu (zero deadlock). */
        pthread_mutex_lock(&s_vk.present_mu);
        if (!__atomic_load_n(&s_fg.stop_atomic, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&s_fg.latest_seq, __ATOMIC_ACQUIRE) == seq) {
            vk_fg_present_real_locked(slot);
        }
        pthread_mutex_unlock(&s_vk.present_mu);

        pthread_mutex_lock(&s_fg.mu);
    }
    pthread_mutex_unlock(&s_fg.mu);
    return NULL;
}

static int vk_fg_ensure(void)
{
    const int mode = vk_fg_mode();
    if (mode == VK_FG_OFF) return 0;
    if (!s_vk.device || !s_vk.sc_can_attach || !s_vk.sc_count || !s_vk.width || !s_vk.height) return 0;

    if (s_fg.pipe_ready && s_fg.rp_fmt != s_vk.sc_format) {
        vk_fg_destroy_fbs();
        if (s_fg.pipe) { pvkDestroyPipeline(s_vk.device, s_fg.pipe, NULL); s_fg.pipe = VK_NULL_HANDLE; }
        if (s_fg.rp)   { pvkDestroyRenderPass(s_vk.device, s_fg.rp, NULL); s_fg.rp = VK_NULL_HANDLE; }
        s_fg.pipe_ready = 0;
        s_fg.pipe_tried = 0;
    }

    if (!s_fg.pipe_ready) {
        if (s_fg.pipe_tried) return 0;
        s_fg.pipe_tried = 1;
        if (!rsx_hlsl_to_spirv_available()) return 0;

        if (!s_fg.vs || !s_fg.fs) {
            u32 spv[12288];
            u32 nw = 0;
            char err[256] = {0};
            if (rsx_hlsl_to_spirv(s_scale_vs_hlsl, RSX_SHADER_STAGE_VERTEX, spv, 12288, &nw, err, sizeof err) != 0) {
                VK_LOG("Frame Gen VS compile failed: %s\n", err);
                return 0;
            }
            VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                             .codeSize = nw * 4u, .pCode = spv };
            if (pvkCreateShaderModule(s_vk.device, &smi, NULL, &s_fg.vs) != VK_SUCCESS) return 0;

            nw = 0; err[0] = 0;
            if (rsx_hlsl_to_spirv(s_fg_fs_hlsl, RSX_SHADER_STAGE_FRAGMENT, spv, 12288, &nw, err, sizeof err) != 0) {
                VK_LOG("Frame Gen FS compile failed: %s\n", err);
                return 0;
            }
            smi.codeSize = nw * 4u; smi.pCode = spv;
            if (pvkCreateShaderModule(s_vk.device, &smi, NULL, &s_fg.fs) != VK_SUCCESS) return 0;
        }

        for (u32 i = 0; i < 3u; i++) {
            if (!s_fg.hist[i].img) {
                if (vk_create_image(&s_fg.hist[i], VK_FORMAT_R8G8B8A8_UNORM, s_vk.width, s_vk.height,
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) != 0)
                    return 0;
                s_fg.hist_fresh[i] = 1;
            }
        }

        if (!s_fg.sampler) {
            VkSamplerCreateInfo sci = {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .maxLod = 1.0f,
            };
            if (pvkCreateSampler(s_vk.device, &sci, NULL, &s_fg.sampler) != VK_SUCCESS) return 0;
        }

        if (!s_fg.ubo.buf) {
            if (vk_create_host_buffer(&s_fg.ubo, 64u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0) return 0;
        }

        if (!s_fg.dsl) {
            VkDescriptorSetLayoutBinding b[4] = {
                { RSX_SPIRV_VPCONST_BINDING,       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                { RSX_SPIRV_TEXTURE_BINDING + 0u,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                { RSX_SPIRV_TEXTURE_BINDING + 1u,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                { RSX_SPIRV_SAMPLER_BINDING,       VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
            };
            VkDescriptorSetLayoutCreateInfo dci = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 4, .pBindings = b,
            };
            if (pvkCreateDescriptorSetLayout(s_vk.device, &dci, NULL, &s_fg.dsl) != VK_SUCCESS) return 0;

            VkPipelineLayoutCreateInfo lci = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1, .pSetLayouts = &s_fg.dsl,
            };
            if (pvkCreatePipelineLayout(s_vk.device, &lci, NULL, &s_fg.layout) != VK_SUCCESS) return 0;

            VkDescriptorPoolSize ps[3] = {
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
                { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  6 },
                { VK_DESCRIPTOR_TYPE_SAMPLER,        3 },
            };
            VkDescriptorPoolCreateInfo pci = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 3, .poolSizeCount = 3, .pPoolSizes = ps,
            };
            if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_fg.pool) != VK_SUCCESS) return 0;

            VkDescriptorSetLayout layouts[3] = { s_fg.dsl, s_fg.dsl, s_fg.dsl };
            VkDescriptorSetAllocateInfo dai = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = s_fg.pool, .descriptorSetCount = 3, .pSetLayouts = layouts,
            };
            if (pvkAllocateDescriptorSets(s_vk.device, &dai, s_fg.sets) != VK_SUCCESS) return 0;
        }

        VkAttachmentDescription att = {
            .format = s_vk.sc_format, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        };
        VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1, .pColorAttachments = &cref,
        };
        VkSubpassDependency dep = {
            .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        };
        VkRenderPassCreateInfo rci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &att,
            .subpassCount = 1, .pSubpasses = &sub,
            .dependencyCount = 1, .pDependencies = &dep,
        };
        if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_fg.rp) != VK_SUCCESS) return 0;

        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_fg.vs, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_fg.fs, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1,
        };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xFu };
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &ba,
        };
        static const VkDynamicState dyn_st[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn_st,
        };
        VkGraphicsPipelineCreateInfo gci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2, .pStages = st,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia,
            .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb,
            .pDynamicState = &dyn, .layout = s_fg.layout, .renderPass = s_fg.rp,
        };
        if (pvkCreateGraphicsPipelines(s_vk.device, s_vk.pipe_cache, 1, &gci, NULL, &s_fg.pipe) != VK_SUCCESS)
            return 0;

        float params[8] = {
            (float)s_vk.width, (float)s_vk.height,
            1.0f / (float)s_vk.width, 1.0f / (float)s_vk.height,
            0.5f, (float)mode, 0.0f, 0.0f,
        };
        memcpy(s_fg.ubo.ptr, params, sizeof params);

        /* Pre-bind all 3 ring-transition pairs: sets[i] reads (hist[i], hist[(i+1)%3]). */
        for (u32 i = 0; i < 3u; i++) {
            const u32 nxt = (i + 1u) % 3u;
            VkDescriptorBufferInfo bi = { s_fg.ubo.buf, 0, sizeof params };
            VkDescriptorImageInfo i0 = { VK_NULL_HANDLE, s_fg.hist[i].view,   VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo i1 = { VK_NULL_HANDLE, s_fg.hist[nxt].view, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo si = { s_fg.sampler,   VK_NULL_HANDLE,      VK_IMAGE_LAYOUT_UNDEFINED };
            VkWriteDescriptorSet w[4] = {
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_fg.sets[i],
                  .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_fg.sets[i],
                  .dstBinding = RSX_SPIRV_TEXTURE_BINDING + 0u, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &i0 },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_fg.sets[i],
                  .dstBinding = RSX_SPIRV_TEXTURE_BINDING + 1u, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &i1 },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_fg.sets[i],
                  .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &si },
            };
            pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);
        }

        if (!s_fg.pacer_pool) {
            VkCommandPoolCreateInfo cpci = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = s_vk.queue_family,
            };
            VkCommandBufferAllocateInfo cbai = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
            };
            VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (pvkCreateCommandPool(s_vk.device, &cpci, NULL, &s_fg.pacer_pool) != VK_SUCCESS) return 0;
            cbai.commandPool = s_fg.pacer_pool;
            if (pvkAllocateCommandBuffers(s_vk.device, &cbai, &s_fg.pacer_cmd) != VK_SUCCESS ||
                pvkCreateFence(s_vk.device, &fci, NULL, &s_fg.pacer_fence) != VK_SUCCESS ||
                pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_fg.pacer_sem) != VK_SUCCESS)
                return 0;
        }

        if (!s_fg.sync_init) {
            pthread_mutex_init(&s_fg.mu, NULL);
            pthread_condattr_t attr;
            pthread_condattr_init(&attr);
            if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) {
                s_fg.clock_id = CLOCK_MONOTONIC;
            } else {
                s_fg.clock_id = CLOCK_REALTIME;
            }
            pthread_cond_init(&s_fg.cv, &attr);
            pthread_condattr_destroy(&attr);
            s_fg.sync_init = 1;
        }

        if (!s_fg.thread_started) {
            s_fg.stop = 0;
            __atomic_store_n(&s_fg.stop_atomic, 0, __ATOMIC_RELEASE);
            if (pthread_create(&s_fg.pacer_thread, NULL, vk_fg_pacer_thread, NULL) == 0)
                s_fg.thread_started = 1;
            else
                return 0;
        }

        s_fg.rp_fmt = s_vk.sc_format;
        s_fg.pipe_ready = 1;
        VK_LOG("GPU Frame Generation ready (%ux%u -> %ux%u, mode=%s)\n",
               s_vk.width, s_vk.height, s_vk.sc_extent.width, s_vk.sc_extent.height,
               mode == VK_FG_FLOW ? "Optical-Flow 2x" : "Fast Blend 2x");
    }

    if (s_fg.fb_count != s_vk.sc_count ||
        s_fg.fb_ext.width != s_vk.sc_extent.width ||
        s_fg.fb_ext.height != s_vk.sc_extent.height) {
        vk_fg_destroy_fbs();
        for (u32 i = 0; i < s_vk.sc_count; i++) {
            VkImageViewCreateInfo vci = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = s_vk.sc_images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = s_vk.sc_format,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            if (pvkCreateImageView(s_vk.device, &vci, NULL, &s_fg.sc_views[i]) != VK_SUCCESS) {
                vk_fg_destroy_fbs(); return 0;
            }
            VkFramebufferCreateInfo fci = {
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = s_fg.rp, .attachmentCount = 1, .pAttachments = &s_fg.sc_views[i],
                .width = s_vk.sc_extent.width, .height = s_vk.sc_extent.height, .layers = 1,
            };
            if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_fg.sc_fbs[i]) != VK_SUCCESS) {
                vk_fg_destroy_fbs(); return 0;
            }
        }
        s_fg.fb_count = s_vk.sc_count;
        s_fg.fb_ext = s_vk.sc_extent;
    }
    return s_fg.fb_count > 0;
}

static int vk_fg_try_present(vk_eng_obj* o)
{
    if (!o || vk_fg_mode() == VK_FG_OFF) return 0;
    if (!s_vk.windowed || !s_vk.sc_can_attach) return 0;
    if (s_vk.dump_path && ((s_vk.presented + 1u) % s_vk.dump_every) == 0) return 0;

    const u64 seq = __atomic_add_fetch(&s_fg.latest_seq, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_lock(&s_vk.present_mu);
    if (s_vk.windowed && s_vk_resized) vk_swapchain_lost();
    if (!s_vk.windowed || !s_vk.swapchain || vk_window_hidden() || s_vk_resized || !vk_fg_ensure()) {
        pthread_mutex_unlock(&s_vk.present_mu);
        return 0;
    }

    const double now = vk_now_seconds();
    if (s_fg.last_present_s > 0.0) {
        const double dt = now - s_fg.last_present_s;
        if (dt > 0.004 && dt < 0.25) {
            s_fg.dt_avg_s = (s_fg.dt_avg_s > 0.0)
                ? (s_fg.dt_avg_s * 0.72 + dt * 0.28) : dt;
        } else if (dt >= 0.25) {
            s_fg.has_prev = 0;
            s_fg.dt_avg_s = 0.033;
        }
    }
    s_fg.last_present_s = now;

    const u32 prev_slot = s_fg.cur_slot;
    const u32 next_slot = s_fg.has_prev ? ((prev_slot + 1u) % 3u) : 0u;
    const int do_interp = s_fg.has_prev && (s_fg.dt_avg_s > 0.0175);

    if (s_fg.pacer_busy && s_fg.pacer_slot == next_slot) {
        pvkWaitForFences(s_vk.device, 1, &s_fg.pacer_fence, VK_TRUE, UINT64_MAX);
        s_fg.pacer_busy = 0;
    }

    VkCommandBuffer cb = vk_present_record_begin();
    if (!cb) {
        pthread_mutex_unlock(&s_vk.present_mu);
        return 0;
    }
    const u32 pslot = s_vk.present_slot;

    /* Copy/blit the newly rendered guest surface into hist[next_slot] (full or P1 left half in Co-op Split). */
    vk_barrier_image_on(cb, o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image_on(cb, s_fg.hist[next_slot].img, VK_IMAGE_ASPECT_COLOR_BIT,
                        s_fg.hist_fresh[next_slot] ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL);
    vk_coop_blit_p1(cb, o, s_fg.hist[next_slot].img, s_vk.width, s_vk.height, vk_coop_is_split_active());
    s_fg.hist_fresh[next_slot] = 0;

    if (!s_vk.present_acquire[pslot]) {
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.present_acquire[pslot]) != VK_SUCCESS)
            s_vk.present_acquire[pslot] = VK_NULL_HANDLE;
    }
    VkSemaphore acquire = s_vk.present_acquire[pslot];
    u32 idx = 0;
    int window = 1;
    VkResult ar = acquire ? pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                                   acquire, VK_NULL_HANDLE, &idx)
                          : VK_ERROR_INITIALIZATION_FAILED;
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); window = 0; }
    else if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { window = 0; }

    int interp_sent = 0;
    if (window) {
        if (do_interp && idx < s_fg.fb_count) {
            vk_barrier_image_on(cb, s_fg.hist[prev_slot].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            vk_barrier_image_on(cb, s_fg.hist[next_slot].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            const vk_rect fit = vk_window_fit();
            VkClearValue clear_black = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
            VkRenderPassBeginInfo rbi = {
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass = s_fg.rp, .framebuffer = s_fg.sc_fbs[idx],
                .renderArea = { { 0, 0 }, s_vk.sc_extent },
                .clearValueCount = 1, .pClearValues = &clear_black,
            };
            pvkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_fg.pipe);
            pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_fg.layout,
                                     0, 1, &s_fg.sets[prev_slot], 0, NULL);
            VkViewport vp = { (float)fit.x, (float)fit.y, (float)fit.w, (float)fit.h, 0.0f, 1.0f };
            VkRect2D sc = { { (int32_t)fit.x, (int32_t)fit.y }, { fit.w, fit.h } };
            pvkCmdSetViewport(cb, 0, 1, &vp);
            pvkCmdSetScissor(cb, 0, 1, &sc);
            pvkCmdDraw(cb, 3, 1, 0, 0);
            pvkCmdEndRenderPass(cb);
            interp_sent = 1;
        } else {
            vk_barrier_image_on(cb, s_fg.hist[next_slot].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            VkImageCopy ic = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .extent = { s_vk.width, s_vk.height, 1 },
            };
            pvkCmdCopyImage(cb, s_fg.hist[next_slot].img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
            vk_record_window_blit(cb, idx);
        }
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = window ? 1u : 0u, .pWaitSemaphores = &acquire,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
        .signalSemaphoreCount = window ? 1u : 0u,
        .pSignalSemaphores = window ? &s_vk.sc_done[idx] : NULL,
    };
    pvkResetFences(s_vk.device, 1, &s_vk.present_fence);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS || vk_queue_submit(&si, s_vk.present_fence) != VK_SUCCESS) {
        if (window) {
            vk_device_idle();
            pvkDestroySemaphore(s_vk.device, acquire, NULL);
            s_vk.present_acquire[pslot] = VK_NULL_HANDLE;
        }
        pthread_mutex_unlock(&s_vk.present_mu);
        return 1;
    }
    __atomic_store_n(&s_vk.present_busy[pslot], 1, __ATOMIC_RELEASE);
    s_vk.present_order = 1;
    s_vk.presented++;
    s_vk.present_direct++;
    if (interp_sent)
        __atomic_add_fetch(&s_fg.gen_frames, 1, __ATOMIC_RELAXED);

    if (window) {
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
            .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
        };
        VkResult pr = vk_queue_present(&pi);
#ifdef __ANDROID__
        if (pr == VK_ERROR_OUT_OF_DATE_KHR) vk_swapchain_lost();
#else
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
#endif
    }

    s_fg.prev_slot = prev_slot;
    s_fg.cur_slot  = next_slot;
    s_fg.has_prev  = 1;
    double delay_s = s_fg.dt_avg_s * 0.48;
    if (delay_s < 0.006) delay_s = 0.006;
    if (delay_s > 0.045) delay_s = 0.045;
    pthread_mutex_unlock(&s_vk.present_mu);

    /* Arm the pacer thread to show the real frame half a frame interval later. */
    if (interp_sent) {
        struct timespec dl;
        clock_gettime(s_fg.clock_id, &dl);
        long add_ns = (long)(delay_s * 1e9);
        dl.tv_nsec += add_ns;
        while (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        pthread_mutex_lock(&s_fg.mu);
        s_fg.pending_slot = next_slot;
        s_fg.pending_seq  = seq;
        s_fg.deadline     = dl;
        s_fg.pending_real = 1;
        pthread_cond_signal(&s_fg.cv);
        pthread_mutex_unlock(&s_fg.mu);
    }
    return 1;
}

static void vk_fg_shutdown(void)
{
    __atomic_store_n(&s_fg.stop_atomic, 1, __ATOMIC_RELEASE);
    if (s_fg.thread_started) {
        pthread_mutex_lock(&s_fg.mu);
        s_fg.stop = 1;
        s_fg.pending_real = 0;
        pthread_cond_signal(&s_fg.cv);
        pthread_mutex_unlock(&s_fg.mu);
        pthread_join(s_fg.pacer_thread, NULL);
        s_fg.thread_started = 0;
    }
    if (s_fg.sync_init) {
        pthread_cond_destroy(&s_fg.cv);
        pthread_mutex_destroy(&s_fg.mu);
        s_fg.sync_init = 0;
    }
    if (!s_vk.device) {
        memset(&s_fg, 0, sizeof s_fg);
        s_fg.mode = -1;
        return;
    }
    vk_device_idle();
    vk_fg_destroy_fbs();
    if (s_fg.pacer_fence) pvkDestroyFence(s_vk.device, s_fg.pacer_fence, NULL);
    if (s_fg.pacer_sem)   pvkDestroySemaphore(s_vk.device, s_fg.pacer_sem, NULL);
    if (s_fg.pacer_pool)  pvkDestroyCommandPool(s_vk.device, s_fg.pacer_pool, NULL);
    for (u32 i = 0; i < 3u; i++) vk_destroy_image(&s_fg.hist[i]);
    if (s_fg.pipe)    pvkDestroyPipeline(s_vk.device, s_fg.pipe, NULL);
    if (s_fg.vs)      pvkDestroyShaderModule(s_vk.device, s_fg.vs, NULL);
    if (s_fg.fs)      pvkDestroyShaderModule(s_vk.device, s_fg.fs, NULL);
    if (s_fg.rp)      pvkDestroyRenderPass(s_vk.device, s_fg.rp, NULL);
    if (s_fg.pool)    pvkDestroyDescriptorPool(s_vk.device, s_fg.pool, NULL);
    if (s_fg.layout)  pvkDestroyPipelineLayout(s_vk.device, s_fg.layout, NULL);
    if (s_fg.dsl)     pvkDestroyDescriptorSetLayout(s_vk.device, s_fg.dsl, NULL);
    if (s_fg.sampler) pvkDestroySampler(s_vk.device, s_fg.sampler, NULL);
    vk_destroy_buffer(&s_fg.ubo);
    memset(&s_fg, 0, sizeof s_fg);
    s_fg.mode = -1;
}
#else
static int  vk_fg_try_present(vk_eng_obj* o) { (void)o; return 0; }
static void vk_fg_shutdown(void) { memset(&s_fg, 0, sizeof s_fg); s_fg.mode = -1; }
#endif

/* The named surface becomes the frame: blit it onto the display image, then
 * the existing present does the readback, the PPM dump and the window. */
static void vk_eng_present(void* user, u32 surface)
{
    (void)user;
    if (!s_vk.device) return;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (o) vk_eng_flush_pending_clear(o);
    if (s_vpt.pt) {   /* present thread: stage the surface and return (above) */
        if (s_eb_pending) vk_eng_flush_async(VK_FLUSH_PRESENT);
        (void)rsx_present_thread_submit(s_vpt.pt, surface);
        vk_eb_perf_present();
        s_g.last_draws = s_g.draws;
        s_g.draws = 0;
        return;
    }
    if (s_eb_pending) vk_eng_flush_async(VK_FLUSH_PRESENT);   /* GPU executes in queue order before present blit */
    const int coop_split = vk_coop_on_present_decide();
    if (o) vk_coop_capture_p2_async(o, coop_split);
    if (!vk_fg_try_present(o)) {
        pthread_mutex_lock(&s_vk.present_mu);
        VkCommandBuffer cb = vk_present_record_begin();
        if (cb) {
            if (o) {
                vk_barrier_image_on(cb, o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
                vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
                vk_coop_blit_p1(cb, o, s_vk.color.img, s_vk.width, s_vk.height, coop_split);
            }
            vk_present_record_end(cb);
        }
        pthread_mutex_unlock(&s_vk.present_mu);
    }
    if (__atomic_load_n(&s_vk.pipe_cache_built, __ATOMIC_RELAXED) - s_vk.pipe_cache_saved >= 128u &&
        vk_now_seconds() - s_vk.pipe_cache_last_save >= 20.0)
        vk_pipe_cache_save_ex(1);
    vk_eb_perf_present();
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
}

/* Pixels a 2D transfer put into guest memory, pushed into the colour target
 * that rectangle belongs to. */
static void vk_eng_color_target_write(void* user, u32 surface, u32 x, u32 y,
                                      u32 w, u32 h, const void* rows, u32 row_bytes)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || o->fmt != VK_FORMAT_R8G8B8A8_UNORM) return;   /* rows are R,G,B,A */
    if (!rows || !w || !h || x + w > o->w || y + h > o->h) return;
    vk_eng_flush_pending_clear(o);
    vk_eng_upload_region(o->im.img, 0, 0, x, y, w, h, (size_t)w * o->bpp, h, rows, row_bytes);
}

/* Async GPU-to-GPU blit/copy between live colour targets (NV0039 / NV3089),
 * recorded directly inside the open draw batch so it avoids synchronous
 * GPU->CPU readback + CPU->GPU re-upload stalls. */
static int vk_eng_surface_blit(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                               u32 dst, u32 dx, u32 dy, u32 dw, u32 dh, int linear)
{
    (void)user;
    vk_eng_obj* so = vk_eng_get(src, VK_ENG_COLOR);
    vk_eng_obj* do_ = vk_eng_get(dst, VK_ENG_COLOR);
    if (!so || !do_ || !sw || !sh || !dw || !dh) return -1;
    if (sx + sw > so->w || sy + sh > so->h || dx + dw > do_->w || dy + dh > do_->h) return -1;
    if (so->im.img == do_->im.img) {
        /* Reject overlapping in-place blit so CPU fallback handles overlap accurately. */
        if (sx < dx + dw && dx < sx + sw && sy < dy + dh && dy < sy + sh)
            return -1;
    }
    vk_eng_flush_pending_clear(so);
    if (dx != 0 || dy != 0 || dw != do_->w || dh != do_->h)
        vk_eng_flush_pending_clear(do_);
    else
        do_->pending_clear_color = 0;
    const int same_size = (sw == dw && sh == dh && so->fmt == do_->fmt && !linear);
    const int batch = vk_eb_active();
    if (batch ? (!s_eb_pending && vk_begin()) : vk_begin()) return -1;
    if (batch) vk_eb_end_pass_reason(VK_PB_BLIT);
    vk_cmd_pass_barrier(s_vk.cmd);
    if (same_size) {
        VkImageCopy ic = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffset = { (int32_t)sx, (int32_t)sy, 0 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffset = { (int32_t)dx, (int32_t)dy, 0 },
            .extent = { sw, sh, 1 },
        };
        pvkCmdCopyImage(s_vk.cmd, so->im.img, VK_IMAGE_LAYOUT_GENERAL,
                        do_->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else {
        VkImageBlit bl = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { (int32_t)sx, (int32_t)sy, 0 }, { (int32_t)(sx + sw), (int32_t)(sy + sh), 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { (int32_t)dx, (int32_t)dy, 0 }, { (int32_t)(dx + dw), (int32_t)(dy + dh), 1 } },
        };
        pvkCmdBlitImage(s_vk.cmd, so->im.img, VK_IMAGE_LAYOUT_GENERAL,
                        do_->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                        linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    if (batch) vk_eb_recorded();
    else if (vk_submit_and_wait()) return -1;
    return 0;
}

static void vk_eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                            void* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || !out || !out_pitch || !w || !h || x + w > o->w || y + h > o->h) return;
    vk_eng_flush_pending_clear(o);
    const size_t tight = (size_t)w * o->bpp;
    vk_buffer* const st = vk_stage_get(&s_rb_stage, (VkDeviceSize)tight * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!st) return;
    const int batch = vk_eb_active();
    if (batch && s_eb_pending) {
        /* Append the copy directly to the open batch so flush + readback is ONE GPU round trip. */
        vk_eb_end_pass();
        vk_cmd_pass_barrier(s_vk.cmd);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, st->buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st->buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        vk_eng_flush_sync_tag(VK_SYNC_READBACK);
        vk_invalidate(st);
        for (u32 row = 0; row < h; row++)
            memcpy((u8*)out + (size_t)row * out_pitch, (const u8*)st->ptr + (size_t)row * tight, tight);
        return;
    }
    if (batch) vk_eng_flush_sync_tag(VK_SYNC_READBACK);
    if (!vk_begin()) {
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, st->buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st->buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        if (!vk_submit_and_wait()) {
            vk_invalidate(st);
            for (u32 row = 0; row < h; row++)
                memcpy((u8*)out + (size_t)row * out_pitch, (const u8*)st->ptr + (size_t)row * tight, tight);
        }
    }
}

/* ---- E3: textures ---------------------------------------------------------
 * The engine decodes every level to host rows (rsx_texture_decode) and hands
 * them over one upload each; the crossbar is a property of the view, so it
 * becomes the image view's component mapping. A cube map (faces == 6) is a
 * six-layer cube-compatible image, each face uploaded into its layer, sampled
 * through a CUBE view; the pipelines that sample one use the split set layout
 * (vk_eng_split_layout). */
static u32 vk_eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || !w || !h || w > vk_max_dim() || h > vk_max_dim() ||
        (bc && !s_vk.bc_ok))
        return 0;
    const int cube = (faces == 6);
    if ((faces != 1 && !cube) || (cube && w != h)) return 0;   /* cube faces are square */
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, .imageType = VK_IMAGE_TYPE_2D,
        .format = vf, .extent = { w, h, 1 }, .mipLevels = mips, .arrayLayers = cube ? 6u : 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS ||
        vk_eng_make_view(o->im.img, vf, mips, vk_remap(remap, rsx_fmt), cube, &o->im.view))
        goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h; o->cube = (u8)cube; o->mips = mips;
    o->bpp = bc ? 0 : bpp; o->block = bc ? bpp : 0;
    /* UNDEFINED -> GENERAL for every level and face before the uploads arrive. */
    if (vk_eb_active()) {
        if (!s_eb_pending && vk_begin()) goto fail;
        vk_eb_end_pass();
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        vk_eb_recorded();
    } else {
        if (vk_begin()) goto fail;
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        if (vk_submit_and_wait()) goto fail;
    }
    return hd;
fail:
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static void vk_eng_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!o || !src || !row_bytes || !rows || face >= (o->cube ? 6u : 1u) || mip >= o->mips) return;
    /* A BC row is a row of 4x4 blocks; the engine's `rows` already counts them. */
    const size_t tight = o->block ? (size_t)((w + 3u) / 4u) * o->block : (size_t)w * o->bpp;
    vk_eng_upload_rows(o->im.img, face, mip, w, h, tight, rows, src, row_bytes);
}

/* A colour target sampled with a unit's crossbar: an extra view of the same
 * image, cached by (surface, crossbar, format) and released with the target. */
static u32 vk_eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t) return 0;
    if (!s_eview_overflow) {
        for (u32 j = 0; j < s_eview_count; j++) {
            const u32 i = s_eview_list[j];
            if (i && i < VK_ENG_MAX_OBJ &&
                s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == surface &&
                s_eobj[i].remap == remap && s_eobj[i].rsx_fmt == rsx_format)
                return i;
        }
    } else {
        for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
            if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == surface &&
                s_eobj[i].remap == remap && s_eobj[i].rsx_fmt == rsx_format)
                return i;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_eng_make_view(t->im.img, t->fmt, 1, vk_remap(remap, rsx_format), 0, &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = t->fmt; o->w = t->w; o->h = t->h;
    o->surface = surface; o->remap = remap; o->rsx_fmt = rsx_format;
    vk_eview_list_add(hd);
    return hd;
}
/* ---- colour feedback snapshots ---------------------------------------------
 * A draw that samples a colour target it is also writing (in-place deferred
 * lighting: read the albedo, write the lit colour over it) gets a copy of
 * that target taken just before it. The engine asks for one per such draw
 * (rsx_draw_backend.color_snapshot) and releases the previous one; without
 * this hook it falls back to the guest-memory bytes at the target's address,
 * which hold whatever was last stored there -- for Twisted Metal the previous
 * frame's UI, lit and composited into the next frame.
 *
 * The copy is a COLOR object marked `snapshot` so the engine can take a
 * crossbar view of it (vk_eng_surface_view); it is never a guest attachment.
 * Released copies keep their images (and any depth_as_color framebuffer) in a
 * pool, since a lit frame asks for one per light and per depth_as_color read. */
#define VK_SNAP_POOL 64
static struct { vk_image im; VkFramebuffer fb; VkFormat fmt; u32 w, h; } s_snap_pool[VK_SNAP_POOL];
static u32 s_snap_pool_n;
static vk_buffer s_snap_st;         /* depth snapshot bounce buffer (vk_eng_depth_snapshot) */

static void vk_eng_snapshot_recycle(vk_eng_obj* o)
{
    if (s_snap_pool_n < VK_SNAP_POOL && o->im.img) {
        s_snap_pool[s_snap_pool_n].im = o->im;
        s_snap_pool[s_snap_pool_n].fb = o->dac_fb;
        s_snap_pool[s_snap_pool_n].fmt = o->fmt;
        s_snap_pool[s_snap_pool_n].w = o->w;
        s_snap_pool[s_snap_pool_n].h = o->h;
        s_snap_pool_n++;
        o->dac_fb = VK_NULL_HANDLE;
    } else {
        if (o->dac_fb) {
            if (!vk_eb_defer_fb(&o->dac_fb)) {
                vk_eng_flush_sync_tag(VK_SYNC_SNAP);
                pvkDestroyFramebuffer(s_vk.device, o->dac_fb, NULL);
                o->dac_fb = VK_NULL_HANDLE;
            }
        }
        if (!vk_eb_defer_image(&o->im)) {
            vk_destroy_image(&o->im);
        }
    }
}

static void vk_eng_snapshot_pool_drain(void)
{
    vk_eng_flush_sync_tag(VK_SYNC_SNAP);
    for (u32 i = 0; i < s_snap_pool_n; i++) {
        if (s_snap_pool[i].fb) pvkDestroyFramebuffer(s_vk.device, s_snap_pool[i].fb, NULL);
        vk_destroy_image(&s_snap_pool[i].im);
    }
    s_snap_pool_n = 0;
    vk_destroy_buffer(&s_snap_st);
}

static u32 vk_eng_color_snapshot(void* user, u32 surface)
{
    (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t || t->snapshot) return 0;
    vk_eng_flush_pending_clear(t);
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    int reused = 0;
    for (u32 i = 0; i < s_snap_pool_n; i++)
        if (s_snap_pool[i].fmt == t->fmt && s_snap_pool[i].w == t->w && s_snap_pool[i].h == t->h) {
            o->im = s_snap_pool[i].im;
            o->dac_fb = s_snap_pool[i].fb;
            s_snap_pool[i] = s_snap_pool[--s_snap_pool_n];
            reused = 1;
            break;
        }
    if (!reused && vk_create_image(&o->im, t->fmt, t->w, t->h,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im);
        memset(o, 0, sizeof *o);
        return 0;
    }
    o->kind = VK_ENG_COLOR; o->snapshot = 1;
    o->fmt = t->fmt; o->bpp = t->bpp; o->w = t->w; o->h = t->h;
    const int batch = vk_eb_active();
    if (batch ? (!s_eb_pending && vk_begin()) : vk_begin()) { vk_eng_release(user, hd); return 0; }
    if (batch) {
        vk_eb_end_pass_reason(VK_PB_CSNAP);
        vk_cmd_pass_barrier(s_vk.cmd);
    }
    if (!reused)
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkImageCopy c = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { t->w, t->h, 1 },
    };
    pvkCmdCopyImage(s_vk.cmd, t->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    o->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    if (batch) {
        vk_eb_recorded();
    } else if (vk_submit_and_wait()) {
        vk_eng_release(user, hd);
        return 0;
    }
    return hd;
}

/* ---- E4: depth snapshots --------------------------------------------------
 * A depth target read as a texture, which the engine asks for only after a
 * depth-writing draw. Metal resolves it with a fullscreen pass into R32Float;
 * Vulkan can do it with two copies on the GPU, since a D32_SFLOAT depth
 * aspect copies out as 32-bit floats: depth -> buffer -> an R32_SFLOAT
 * texture. Other depth formats would need a conversion pass: they report 0.
 *
 * Each depth target keeps ONE snapshot texture (its handle in `surface`),
 * refilled on every request, so nothing accumulates however often the engine
 * invalidates and asks again; it goes when the depth target goes. */
static u32 vk_eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z) return 0;
    if (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
        s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        static int warned_fmt;
        if (!warned_fmt) { VK_LOG("engine: depth snapshots need D32_SFLOAT, this device uses %d\n",
                                  (int)s_vk.depth_format); warned_fmt = 1; }
        return 0;
    }
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!w || !h) return 0;
    vk_eng_flush_pending_clear(z);

    u32 snap = z->surface;
    vk_eng_obj* t = vk_eng_get(snap, VK_ENG_TEXTURE);
    if (!t || t->w != w || t->h != h) {
        if (t) vk_eng_release(user, snap);
        z->surface = 0;
        snap = vk_eng_alloc();
        if (!snap) return 0;
        t = &s_eobj[snap];
        if (vk_create_image(&t->im, VK_FORMAT_R32_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
        }
        t->kind = VK_ENG_TEXTURE; t->fmt = VK_FORMAT_R32_SFLOAT; t->bpp = 4; t->w = w; t->h = h;
        z = vk_eng_get(depth, VK_ENG_DEPTH);          /* same slot; reload for clarity */
        z->surface = snap;
        const int init_batch = vk_eb_active();
        if (init_batch ? (!s_eb_pending && vk_begin()) : vk_begin()) return 0;
        if (init_batch) vk_eb_end_pass();
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        if (init_batch) vk_eb_recorded();
        else if (vk_submit_and_wait()) return 0;
    }

    /* The bounce buffer stays (device-local when the device has it, grown as
     * needed): a fresh host buffer per snapshot cost ~4.8 ms each, 375 times
     * in 20 s of God of War: Ascension's first level. With draw batching the
     * copies join the open batch instead of a submission of their own; the
     * draws that sample the snapshot come after them in the same buffer, and
     * each batched draw starts with a full memory barrier. */
    const VkDeviceSize need = (VkDeviceSize)w * h * 4u;
    if (s_snap_st.size < need) {
        vk_destroy_buffer(&s_snap_st);                   /* flushes a pending batch first */
        if (vk_create_device_buffer(&s_snap_st, need,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            vk_destroy_buffer(&s_snap_st); return 0;
        }
    }
    vk_buffer st = s_snap_st;
    const int batch = vk_eb_active();
    int rc = (batch && s_eb_pending) ? 0 : vk_begin();
    if (!rc && batch) {
        vk_eb_end_pass_reason(VK_PB_DSNAP);
        vk_cmd_pass_barrier(s_vk.cmd);
    }
    if (!rc) {
        VkBufferImageCopy out = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                  .imageExtent = { w, h, 1 } };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &out);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 1, &bb, 0, NULL);
        VkBufferImageCopy in = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
        if (batch) {
            vk_eb_recorded();
        } else
            rc = vk_submit_and_wait();
    }
    return rc ? 0 : snap;
}

/* ---- E4b: GPU depth_as_color (A8R8G8B8 / DEPTH16) -------------------------
 * Converts a depth attachment into an A8R8G8B8 (Z24S8 bytes) or DEPTH16 (R16)
 * colour snapshot asynchronously inside the open command buffer, matching the
 * Metal backend's exact packing formula without stalling the GPU or reading
 * depth back to guest memory. */
static VkDescriptorSet vk_eb_alloc_set(VkDescriptorSetLayout lay);

static struct {
    int                   state;       /* 0 = uninit, 1 = ready, -1 = unavailable */
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkDescriptorPool      sync_pool;
    VkDescriptorSet       sync_set;
    VkRenderPass          rp[2];       /* 0 = RGBA8, 1 = R16_UNORM */
    VkPipeline            pipe[2];
    u32                   calls;
} s_dac;

static const char s_dac_vs_hlsl[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2((id == 1u) ? 3.0 : -1.0, (id == 2u) ? 3.0 : -1.0);\n"
    "    return float4(p, 0.0, 1.0);\n"
    "}\n";

static const char s_dac_argb_fs_hlsl[] =
    "Texture2D<float> src_depth : register(t0);\n"
    "float4 main(float4 pos : SV_Position) : SV_Target0 {\n"
    "    float d = clamp(src_depth.Load(int3((int)pos.x, (int)pos.y, 0)), 0.0, 1.0);\n"
    "    uint v = (uint)clamp(floor(d * 16777215.0 + 0.5), 0.0, 16777215.0);\n"
    "    return float4(float((v >> 8u) & 255u), float(v & 255u), 0.0, float(v >> 16u)) / 255.0;\n"
    "}\n";

static const char s_dac_d16_fs_hlsl[] =
    "Texture2D<float> src_depth : register(t0);\n"
    "float4 main(float4 pos : SV_Position) : SV_Target0 {\n"
    "    float d = clamp(src_depth.Load(int3((int)pos.x, (int)pos.y, 0)), 0.0, 1.0);\n"
    "    uint v = (uint)clamp(floor(d * 65535.0 + 0.5), 0.0, 65535.0);\n"
    "    return float4(float(v) / 65535.0, 0.0, 0.0, 1.0);\n"
    "}\n";

static int vk_dac_compile(const char* hlsl, int stage, VkShaderModule* out)
{
    u32 words = 0;
    if (rsx_hlsl_to_spirv(hlsl, stage, s_g.spv, VK_SPV_WORDS, &words, s_g.log, sizeof s_g.log) || !words) {
        VK_LOG("engine: depth_as_color shader compile failed: %s\n", s_g.log);
        return -1;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)words * 4u, .pCode = s_g.spv };
    return pvkCreateShaderModule(s_vk.device, &ci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

static VkRenderPass vk_dac_make_rp(VkFormat fmt)
{
    VkAttachmentDescription att = {
        .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref,
    };
    VkRenderPassCreateInfo rci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub,
    };
    VkRenderPass rp = VK_NULL_HANDLE;
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &rp) != VK_SUCCESS) return VK_NULL_HANDLE;
    return rp;
}

static VkPipeline vk_dac_make_pipe(VkRenderPass rp, VkShaderModule vs, VkShaderModule fs)
{
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = s_dac.pipe_layout, .renderPass = rp, .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    if (pvkCreateGraphicsPipelines(s_vk.device, s_vk.pipe_cache, 1, &gci, NULL, &p) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return p;
}

static void vk_dac_shutdown(void)
{
    if (!s_vk.device) { memset(&s_dac, 0, sizeof s_dac); return; }
    for (int i = 0; i < 2; i++) {
        if (s_dac.pipe[i]) pvkDestroyPipeline(s_vk.device, s_dac.pipe[i], NULL);
        if (s_dac.rp[i])   pvkDestroyRenderPass(s_vk.device, s_dac.rp[i], NULL);
    }
    if (s_dac.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_dac.pipe_layout, NULL);
    if (s_dac.sync_pool)   pvkDestroyDescriptorPool(s_vk.device, s_dac.sync_pool, NULL);
    if (s_dac.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_dac.set_layout, NULL);
    memset(&s_dac, 0, sizeof s_dac);
}

static int vk_dac_init(void)
{
    if (s_dac.state != 0) return s_dac.state > 0 ? 0 : -1;
    if (!s_g.on || !s_vk.device) { s_dac.state = -1; return -1; }
    VkFormatProperties zfp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, s_vk.depth_format, &zfp);
    if (!(zfp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        VK_LOG("engine: depth_as_color disabled (depth format %d not sampleable)\n", (int)s_vk.depth_format);
        s_dac.state = -1;
        return -1;
    }

    VkDescriptorSetLayoutBinding b = {
        .binding = RSX_SPIRV_TEXTURE_BINDING,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &s_dac.set_layout };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 };
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_dac.set_layout) != VK_SUCCESS ||
        pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_dac.pipe_layout) != VK_SUCCESS ||
        pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_dac.sync_pool) != VK_SUCCESS) {
        vk_dac_shutdown(); s_dac.state = -1; return -1;
    }
    VkDescriptorSetAllocateInfo dai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = s_dac.sync_pool, .descriptorSetCount = 1, .pSetLayouts = &s_dac.set_layout };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &s_dac.sync_set) != VK_SUCCESS) {
        vk_dac_shutdown(); s_dac.state = -1; return -1;
    }

    VkShaderModule vs = VK_NULL_HANDLE, fs0 = VK_NULL_HANDLE, fs1 = VK_NULL_HANDLE;
    if (vk_dac_compile(s_dac_vs_hlsl, RSX_SHADER_STAGE_VERTEX, &vs) ||
        vk_dac_compile(s_dac_argb_fs_hlsl, RSX_SHADER_STAGE_FRAGMENT, &fs0)) {
        if (vs)  pvkDestroyShaderModule(s_vk.device, vs, NULL);
        if (fs0) pvkDestroyShaderModule(s_vk.device, fs0, NULL);
        vk_dac_shutdown(); s_dac.state = -1; return -1;
    }
    s_dac.rp[0] = vk_dac_make_rp(VK_FORMAT_R8G8B8A8_UNORM);
    if (s_dac.rp[0]) s_dac.pipe[0] = vk_dac_make_pipe(s_dac.rp[0], vs, fs0);

    VkFormatProperties fp16;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, VK_FORMAT_R16_UNORM, &fp16);
    const VkFormatFeatureFlags need16 = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((fp16.optimalTilingFeatures & need16) == need16 &&
        vk_dac_compile(s_dac_d16_fs_hlsl, RSX_SHADER_STAGE_FRAGMENT, &fs1) == 0) {
        s_dac.rp[1] = vk_dac_make_rp(VK_FORMAT_R16_UNORM);
        if (s_dac.rp[1]) s_dac.pipe[1] = vk_dac_make_pipe(s_dac.rp[1], vs, fs1);
    }
    pvkDestroyShaderModule(s_vk.device, vs, NULL);
    pvkDestroyShaderModule(s_vk.device, fs0, NULL);
    if (fs1) pvkDestroyShaderModule(s_vk.device, fs1, NULL);

    if (!s_dac.pipe[0]) {
        vk_dac_shutdown(); s_dac.state = -1; return -1;
    }
    s_dac.state = 1;
    VK_LOG("engine: GPU depth_as_color on (A8R8G8B8=ok, DEPTH16=%s)\n",
           s_dac.pipe[1] ? "ok" : "fallback");
    return 0;
}

static u32 vk_eng_depth_as_color(void* user, u32 depth, u32 w, u32 h, u32 rsx_fmt)
{
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !w || !h) return 0;
    const u32 base = rsx_fmt & 0x9Fu;
    int k = -1;
    VkFormat dst_fmt = VK_FORMAT_UNDEFINED;
    u32 dst_bpp = 0;
    if (base == RSX_TEX_FMT_A8R8G8B8)     { k = 0; dst_fmt = VK_FORMAT_R8G8B8A8_UNORM; dst_bpp = 4; }
    else if (base == RSX_TEX_FMT_DEPTH16) { k = 1; dst_fmt = VK_FORMAT_R16_UNORM;      dst_bpp = 2; }
    else return 0;

    if (vk_dac_init() != 0 || !s_dac.pipe[k] || !s_dac.rp[k]) return 0;
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!w || !h) return 0;
    vk_eng_flush_pending_clear(z);

    if (!z->depth_sample_view) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = z->im.img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = z->fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
        };
        if (pvkCreateImageView(s_vk.device, &vci, NULL, &z->depth_sample_view) != VK_SUCCESS)
            return 0;
    }

    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    int reused = 0;
    for (u32 i = 0; i < s_snap_pool_n; i++) {
        if (s_snap_pool[i].fmt == dst_fmt && s_snap_pool[i].w == w && s_snap_pool[i].h == h) {
            o->im = s_snap_pool[i].im;
            o->dac_fb = s_snap_pool[i].fb;
            s_snap_pool[i] = s_snap_pool[--s_snap_pool_n];
            reused = 1;
            break;
        }
    }
    if (!reused && vk_create_image(&o->im, dst_fmt, w, h,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im);
        memset(o, 0, sizeof *o);
        return 0;
    }
    if (!o->dac_fb) {
        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = s_dac.rp[k],
            .attachmentCount = 1, .pAttachments = &o->im.view,
            .width = w, .height = h, .layers = 1,
        };
        if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &o->dac_fb) != VK_SUCCESS) {
            o->dac_fb = VK_NULL_HANDLE;
            if (!reused) vk_destroy_image(&o->im);
            else vk_eng_snapshot_recycle(o);
            memset(o, 0, sizeof *o);
            return 0;
        }
    }
    o->kind = VK_ENG_COLOR; o->snapshot = 1;
    o->fmt = dst_fmt; o->bpp = dst_bpp; o->w = w; o->h = h;

    const int batch = vk_eb_active();
    VkDescriptorSet dset = batch ? vk_eb_alloc_set(s_dac.set_layout) : s_dac.sync_set;
    if (!dset) { vk_eng_release(user, hd); return 0; }
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, z->depth_sample_view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = dset, .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii,
    };
    pvkUpdateDescriptorSets(s_vk.device, 1, &wr, 0, NULL);

    if (batch ? (!s_eb_pending && vk_begin()) : vk_begin()) { vk_eng_release(user, hd); return 0; }
    if (batch) {
        vk_eb_end_pass_reason(VK_PB_DSNAP);
        vk_cmd_pass_barrier(s_vk.cmd);
    }
    if (!reused)
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkRenderPassBeginInfo rbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = s_dac.rp[k], .framebuffer = o->dac_fb,
        .renderArea = { { 0, 0 }, { w, h } },
    };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_dac.pipe[k]);
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             s_dac.pipe_layout, 0, 1, &dset, 0, NULL);
    pvkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    s_dac.calls++;
    if (batch) {
        vk_eb_recorded();
    } else if (vk_submit_and_wait()) {
        vk_eng_release(user, hd);
        return 0;
    }
    return hd;
}
/* The depth aspect of a D32_SFLOAT target copies out as 32-bit floats, which
 * is what the engine asks for; other depth formats report failure. */
static int vk_eng_depth_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                                 float* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !out || !w || !h || x + w > z->w || y + h > z->h ||
        out_pitch < w * 4u || (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
                                  s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT))
        return -1;
    vk_eng_flush_pending_clear(z);
    vk_buffer* const st = vk_stage_get(&s_rb_stage, (VkDeviceSize)w * h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!st) return -1;
    const int batch = vk_eb_active();
    if (batch && s_eb_pending) {
        vk_eb_end_pass();
        vk_cmd_pass_barrier(s_vk.cmd);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st->buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st->buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        vk_eng_flush_sync_tag(VK_SYNC_READBACK);
        vk_invalidate(st);
        for (u32 row = 0; row < h; row++)
            memcpy((u8*)out + (size_t)row * out_pitch,
                   (const u8*)st->ptr + (size_t)row * w * 4u, (size_t)w * 4u);
        return 0;
    }
    if (batch) vk_eng_flush_sync_tag(VK_SYNC_READBACK);
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st->buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st->buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        rc = vk_submit_and_wait();
        if (!rc) {
            vk_invalidate(st);
            for (u32 row = 0; row < h; row++)
                memcpy((u8*)out + (size_t)row * out_pitch,
                       (const u8*)st->ptr + (size_t)row * w * 4u, (size_t)w * 4u);
        }
    }
    return rc ? -1 : 0;
}
/* ---- E2: pipelines and draws ---------------------------------------------
 * A pipeline handle carries the two translated modules and the render state
 * the engine keyed it on. Topology is not part of that key (Metal sets it on
 * the encoder), but Vulkan 1.0 bakes it into the pipeline, so each handle
 * builds its VkPipeline per topology on first use.
 *
 * Bindings are the translator's contract (rsx_shader_spirv.h), and the set
 * layout, pipeline layout, descriptor set and constant UBO are the ones the
 * vtable guest path already creates. Vertex inputs are the engine's compact
 * layout: slot n is a float4 at n*16, at location n, as Metal reads it.
 * Known gaps, stated rather than guessed: vertex-texture units (their
 * samplers s0..s3 would alias the pixel samplers' binding), colour targets
 * other than RGBA8 and RGBA16F, depth-only passes, and stencil (the depth format
 * has no stencil aspect yet, so the test is inert). */
/* As many as the engine keeps (ENG_MAX_PIPELINES in rsx_draw_engine.c). God of
 * War: Ascension selects ~740 distinct fragment programs in its first level on
 * top of the menu's; with 512 slots every later pipeline failed silently and the
 * level drew nothing but its clears. */
#define VK_ENG_MAX_PIPE   8192
#define VK_ENG_TOPOLOGIES 5
#define VK_ENG_FB_CACHE   64

typedef struct vk_eng_pipe {
    int                 used;
    VkShaderModule      vs, fs;
    u8                  vs_owned, fs_owned;
    rsx_be_render_state rs;
    u32                 nslots, stride, rt_count;
    VkFormat            rt_fmt;          /* picks the render pass it is built against */
    u32                 cube_mask;       /* TextureCube units: the split set layout */
    u32                 vtex_mask;       /* Vertex-stage Texture2D units (t16..t19) */
    u16                 vp_const_lo;     /* lowest vp_c[N] index read by VS */
    u16                 vp_const_hi;     /* highest vp_c[N] index + 1 (0..RSX_MAX_VERTEX_CONSTANTS) */
    VkRenderPass        rp_base;         /* pre-ensured on FIFO thread for async workers */
    VkPipelineLayout    pipe_layout;     /* pre-ensured on FIFO thread for async workers */
    u8                  rt_blendable;    /* pre-computed on FIFO thread */
    u8                  mrt_mode;        /* pre-computed on FIFO thread */
    volatile u32        async_state;     /* 0=READY, 1=COMPILING, 2=FAILED */
    volatile u32        async_refs;      /* in-flight worker jobs referencing this slot */
    VkPipeline          variant[VK_ENG_TOPOLOGIES];
    u8                  variant_failed[VK_ENG_TOPOLOGIES];
    volatile u8         variant_queued[VK_ENG_TOPOLOGIES];
} vk_eng_pipe;

static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];

static struct {
    VkRenderPass rp_ex[VK_ENG_RT_FORMATS][RSX_BE_MAX_COLOR_TARGETS + 1][64]; /* [fmt][nrt][clear_mask] */
    struct { u32 key[RSX_BE_MAX_COLOR_TARGETS + 1]; VkFramebuffer fb; } fb[VK_ENG_FB_CACHE];
    u32          fb_count;
    u32          fb_last_idx;                            /* MRU index into fb[] */
    vk_image     scratch_depth;                          /* when no zeta is bound */
    u32          scratch_w, scratch_h;
    u32          pipeline;
    u32          vs_bytes, ps_bytes;
    u32          vs_gen, ps_gen;
    u32          tex_gen;                                /* bumped on texture/sampler/release change */
    const void*  vs_src;
    u8*          ucpu;                /* the constants, in the shared UBO's layout */
    u32          tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    u32          tex_mask;
    u32          vtex[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamp[RSX_BE_MAX_VERTEX_TEXTURES];
    u32          vtex_mask;
    float        vp[4];
    u32          sc[4];
    int          have_vp, have_sc;
    u32          stencil_ref;
    float        depth_bounds[2];     /* min, max for the next draw */
    float        depth_bias[2];       /* constant, slope for the next draw */
    float        blend_color[4];      /* r, g, b, a blend constant for the next draw */
    vk_buffer    indices;
    u32          warned;
    u32          draws_seen;
    /* The split set layout (vk_eng_split_layout) and its one set, made the
     * first time a pipeline samples a cube map; the 1x1 cube a cube unit
     * with nothing bound samples. */
    VkDescriptorSetLayout split_layout;
    VkDescriptorPool      split_pool;
    VkDescriptorSet       split_set;
    VkPipelineLayout      split_pipe_layout;
    u32                   dummy_cube;
    u32                   dummy_2d;
} s_e2;

#define VK_ENG_SAMP_CACHE 64
static struct { u64 key; VkSampler s; } s_esamp[VK_ENG_SAMP_CACHE];
static u32 s_esamp_count;

static void vk_eng_drop_framebuffers(void)
{
    if (vk_eb_active()) {
        vk_eb_end_pass();
        int any_sync = 0;
        for (u32 i = 0; i < s_e2.fb_count; i++) {
            if (!s_e2.fb[i].fb) continue;
            if (!vk_eb_defer_fb(&s_e2.fb[i].fb)) {
                if (!any_sync) { vk_eng_flush_sync_tag(VK_SYNC_FB); any_sync = 1; }
                pvkDestroyFramebuffer(s_vk.device, s_e2.fb[i].fb, NULL);
                s_e2.fb[i].fb = VK_NULL_HANDLE;
            }
        }
    } else {
        vk_eng_flush_sync_tag(VK_SYNC_FB);
        for (u32 i = 0; i < s_e2.fb_count; i++)
            if (s_e2.fb[i].fb) pvkDestroyFramebuffer(s_vk.device, s_e2.fb[i].fb, NULL);
    }
    s_e2.fb_count = 0;
    s_e2.fb_last_idx = 0;
}

static void vk_eng_bump_tex_gen(void)
{
    s_e2.tex_gen++;
}

/* The render pass for `n` colour attachments of format `cf` plus depth,
 * in GENERAL layout. `clear_mask` bits 0..3 request LOAD_OP_CLEAR on colour
 * attachment 0..3, bit 4 on depth, bit 5 on stencil; Vulkan guarantees full
 * pipeline and framebuffer compatibility across loadOp variants. */
static VkRenderPass vk_eng_render_pass_ex(VkFormat cf, u32 n, u32 clear_mask)
{
    const int fi = vk_eng_rt_index(cf);
    clear_mask &= 63u;
    if (fi < 0 || n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    if (s_e2.rp_ex[fi][n][clear_mask]) return s_e2.rp_ex[fi][n][clear_mask];
    VkAttachmentDescription att[RSX_BE_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference   cref[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i <= n; i++) {
        const VkAttachmentLoadOp lop = (i < n)
            ? (((clear_mask >> i) & 1u) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD)
            : ((clear_mask & 16u)       ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
        const VkAttachmentLoadOp slop = (i == n && (clear_mask & 32u))
            ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        att[i] = (VkAttachmentDescription){
            .format = (i < n) ? cf : s_vk.depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = lop, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            /* Stencil lives across draws like depth does (a title writes it in
             * one pass and tests it in the next); ignored for colour and for
             * depth formats without stencil. */
            .stencilLoadOp = slop,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        if (i < n) cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkAttachmentReference dref = { n, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = n, .pColorAttachments = cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = n + 1, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_e2.rp_ex[fi][n][clear_mask]) != VK_SUCCESS)
        s_e2.rp_ex[fi][n][clear_mask] = VK_NULL_HANDLE;
    return s_e2.rp_ex[fi][n][clear_mask];
}

static VkRenderPass vk_eng_render_pass(VkFormat cf, u32 n)
{
    return vk_eng_render_pass_ex(cf, n, 0u);
}

/* Tables carried from the Metal backend, itself copied from the D3D12 live
 * draw engine a title has shipped on. */
static VkBlendFactor vk_gcm_blend_factor(u32 f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    /* The blend constant (vk_eng_set_blend_color). These four used to fall
     * to ONE: a decal faded by the constant came out as a plain add, and a
     * bloom pass weighted by it went in at full strength. */
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:     return VK_BLEND_OP_ADD;                  /* FUNC_ADD 0x8006 */
    }
}
static VkCompareOp vk_gcm_compare(u32 f)
{
    return (f >= 0x0200u && f <= 0x0206u) ? (VkCompareOp)(f - 0x0200u) : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp vk_gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;               /* GL_KEEP 0x1E00 */
    }
}
static VkStencilOpState vk_gcm_stencil_face(u32 func, u32 fail, u32 zfail, u32 zpass,
                                            u32 cmp_mask, u32 write_mask)
{
    VkStencilOpState s = { .failOp = vk_gcm_stencil_op(fail), .passOp = vk_gcm_stencil_op(zpass),
                           .depthFailOp = vk_gcm_stencil_op(zfail),
                           .compareOp = vk_gcm_compare(func),
                           .compareMask = cmp_mask & 0xFFu, .writeMask = write_mask & 0xFFu,
                           .reference = 0 };                /* dynamic */
    return s;
}

#define VK_ENG_SH_CACHE 2048u
typedef struct {
    u64            h1, h2;
    u32            len;
    u8             stage;
    VkShaderModule mod;
} vk_eng_sh_entry;
static vk_eng_sh_entry s_esh[VK_ENG_SH_CACHE];
static u32 s_esh_used;
#ifndef _WIN32
static pthread_mutex_t s_esh_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

static void vk_eng_sh_cache_clear(void)
{
#ifndef _WIN32
    pthread_mutex_lock(&s_esh_mu);
#endif
    if (s_vk.device) {
        for (u32 i = 0; i < VK_ENG_SH_CACHE; i++)
            if (s_esh[i].mod) pvkDestroyShaderModule(s_vk.device, s_esh[i].mod, NULL);
    }
    memset(s_esh, 0, sizeof s_esh);
    s_esh_used = 0;
#ifndef _WIN32
    pthread_mutex_unlock(&s_esh_mu);
#endif
}

static int vk_eng_translate_ex(const char* hlsl, int stage, VkShaderModule* out, u8* out_owned,
                               u32* spv_buf, u32 spv_max, char* log_buf, u32 log_size)
{
    u64 h1 = 0xcbf29ce484222325ull;
    u64 h2 = 0x84222325cbf29ce4ull ^ (u64)(u32)stage * 0x9e3779b97f4a7c15ull;
    u32 len = 0;
    for (const unsigned char* p = (const unsigned char*)hlsl; *p; p++, len++) {
        h1 ^= *p;
        h1 *= 0x100000001b3ull;
        h2 = ((h2 << 5) + h2) ^ (u64)*p;
    }
    const u32 mask = VK_ENG_SH_CACHE - 1u;
    const u32 idx = (u32)(h1 ^ (h1 >> 32)) & mask;

#ifndef _WIN32
    pthread_mutex_lock(&s_esh_mu);
#endif
    for (u32 probe = 0; probe < 32u; probe++) {
        const u32 s = (idx + probe) & mask;
        if (!s_esh[s].mod) break;
        if (s_esh[s].stage == (u8)stage && s_esh[s].len == len &&
            s_esh[s].h1 == h1 && s_esh[s].h2 == h2) {
            *out = s_esh[s].mod;
            if (out_owned) *out_owned = 0;
#ifndef _WIN32
            pthread_mutex_unlock(&s_esh_mu);
#endif
            return 0;
        }
    }
#ifndef _WIN32
    pthread_mutex_unlock(&s_esh_mu);
#endif

    u32 words = 0;
    if (rsx_hlsl_to_spirv(hlsl, stage, spv_buf, spv_max, &words, log_buf, log_size) || !words) {
        VK_LOG("engine: %s translation failed: %s\n",
               stage == RSX_SHADER_STAGE_VERTEX ? "vertex" : "fragment", log_buf ? log_buf : "");
        return -1;
    }
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)words * 4u, .pCode = spv_buf };
    if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &mod) != VK_SUCCESS) return -1;

#ifndef _WIN32
    pthread_mutex_lock(&s_esh_mu);
#endif
    u32 empty_slot = VK_ENG_SH_CACHE;
    for (u32 probe = 0; probe < 32u; probe++) {
        const u32 s = (idx + probe) & mask;
        if (!s_esh[s].mod) {
            empty_slot = s;
            break;
        }
        if (s_esh[s].stage == (u8)stage && s_esh[s].len == len &&
            s_esh[s].h1 == h1 && s_esh[s].h2 == h2) {
            VkShaderModule cached = s_esh[s].mod;
#ifndef _WIN32
            pthread_mutex_unlock(&s_esh_mu);
#endif
            pvkDestroyShaderModule(s_vk.device, mod, NULL);
            *out = cached;
            if (out_owned) *out_owned = 0;
            return 0;
        }
    }
    if (empty_slot < VK_ENG_SH_CACHE && s_esh_used < (VK_ENG_SH_CACHE * 3u) / 4u) {
        s_esh[empty_slot].h1 = h1;
        s_esh[empty_slot].h2 = h2;
        s_esh[empty_slot].len = len;
        s_esh[empty_slot].stage = (u8)stage;
        s_esh[empty_slot].mod = mod;
        s_esh_used++;
        if (out_owned) *out_owned = 0;
    } else {
        if (out_owned) *out_owned = 1;
    }
    *out = mod;
#ifndef _WIN32
    pthread_mutex_unlock(&s_esh_mu);
#endif
    return 0;
}

static int vk_eng_translate(const char* hlsl, int stage, VkShaderModule* out, u8* out_owned)
{
    return vk_eng_translate_ex(hlsl, stage, out, out_owned,
                               s_g.spv, VK_SPV_WORDS, s_g.log, sizeof s_g.log);
}

/* A pixel program with a cube unit declares all sixteen units one by one
 * (TextureCube or Texture2D rsx_texN : register(tN)) instead of the
 * Texture2D array, and register(tN) is binding RSX_SPIRV_TEXTURE_BINDING + N
 * (rsx_shader_spirv.h): sixteen single bindings, where the shared layout has
 * one binding of sixteen. Those pipelines use this layout, made on first use
 * so a title without cube maps never builds it. */
static VkPipelineLayout vk_eng_split_layout(void)
{
    if (s_e2.split_pipe_layout) return s_e2.split_pipe_layout;
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[3 + RSX_MAX_TEXTURES + 2 * RSX_BE_MAX_VERTEX_TEXTURES];
    b[0] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, NULL };
    b[1] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, NULL };
    b[2] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER,
                                           RSX_MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
        b[3 + u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING + u,
                                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (u32 v = 0; v < RSX_BE_MAX_VERTEX_TEXTURES; v++) {
        b[3 + RSX_MAX_TEXTURES + v] = (VkDescriptorSetLayoutBinding){
                                                   RSX_SPIRV_VTEX_BINDING + v,
                                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                   VK_SHADER_STAGE_VERTEX_BIT, NULL };
        b[3 + RSX_MAX_TEXTURES + RSX_BE_MAX_VERTEX_TEXTURES + v] = (VkDescriptorSetLayoutBinding){
                                                   RSX_SPIRV_VSAMPLER_BINDING + v,
                                                   VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_VERTEX_BIT, NULL };
    }
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 3 + RSX_MAX_TEXTURES + 2 * RSX_BE_MAX_VERTEX_TEXTURES,
                                            .pBindings = b };
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES + RSX_BE_MAX_VERTEX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES + RSX_BE_MAX_VERTEX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_e2.split_layout) != VK_SUCCESS ||
        pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_e2.split_pool) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_e2.split_pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_e2.split_layout };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_e2.split_layout };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &s_e2.split_set) != VK_SUCCESS ||
        pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_e2.split_pipe_layout) != VK_SUCCESS) {
        s_e2.split_pipe_layout = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    return s_e2.split_pipe_layout;
}

/* Which units a pixel program declares as TextureCube. */
static u32 vk_eng_cube_units(const char* ps_hlsl)
{
    u32 mask = 0;
    char decl[40];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        snprintf(decl, sizeof decl, "TextureCube rsx_tex%u ", u);
        if (strstr(ps_hlsl, decl)) mask |= 1u << u;
    }
    return mask;
}

/* Which vertex-texture units a vertex program declares as Texture2D rsx_vtexN. */
static u32 vk_eng_vtex_units(const char* vs_hlsl)
{
    u32 mask = 0;
    char decl[40];
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        snprintf(decl, sizeof decl, "Texture2D rsx_vtex%u ", u);
        if (strstr(vs_hlsl, decl)) mask |= 1u << u;
    }
    return mask;
}

/* Active vp_c[lo..hi) constant range read by a decompiled vertex program.
 * Skipping untouched slots trims per-draw write-combined UBO memcpy by 8-12x;
 * indexed addressing (vp_c[(... + a0.x) & 511u]) conservatively returns full range. */
static void vk_eng_scan_vp_consts(const char* vs_hlsl, u16* out_lo, u16* out_hi)
{
    *out_lo = 0;
    *out_hi = RSX_MAX_VERTEX_CONSTANTS;
    if (!vs_hlsl) return;
    const char* p = strstr(vs_hlsl, "VSOutput main(");
    if (!p) return;
    u32 lo = RSX_MAX_VERTEX_CONSTANTS, hi = 0;
    int any = 0;
    while ((p = strstr(p, "vp_c[")) != NULL) {
        p += 5;
        if (*p < '0' || *p > '9') {
            /* Indexed constant lookup (e.g. skinning palette via a0/a1). */
            *out_lo = 0;
            *out_hi = RSX_MAX_VERTEX_CONSTANTS;
            return;
        }
        u32 idx = 0;
        while (*p >= '0' && *p <= '9') {
            idx = idx * 10u + (u32)(*p - '0');
            p++;
        }
        if (*p != ']' || idx >= RSX_MAX_VERTEX_CONSTANTS) {
            *out_lo = 0;
            *out_hi = RSX_MAX_VERTEX_CONSTANTS;
            return;
        }
        if (idx < lo) lo = idx;
        if (idx + 1u > hi) hi = idx + 1u;
        any = 1;
    }
    if (any) {
        *out_lo = (u16)lo;
        *out_hi = (u16)hi;
    } else {
        *out_lo = 0;
        *out_hi = 0;
    }
}

static int vk_eng_mrt_mode(void)
{
    static int mrt_mode = -1;
    if (mrt_mode < 0) {
        const char* e = getenv("PS3RECOMP_VK_MRT_MASK");
        mrt_mode = e ? atoi(e) : 1;
    }
    return mrt_mode;
}

/* Thread-safe pipeline variant builder: reads only immutable fields of P
 * (pre-resolved rp_base, pipe_layout, rt_blendable, mrt_mode, shader modules,
 * render state) and immutable s_vk capability flags. */
static VkPipeline vk_eng_build_variant_ts(vk_eng_pipe* P, u32 topo_idx)
{
    static const VkPrimitiveTopology topo[VK_ENG_TOPOLOGIES] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipeline existing = __atomic_load_n(&P->variant[topo_idx], __ATOMIC_ACQUIRE);
    if (existing) return existing;
    if (__atomic_load_n(&P->variant_failed[topo_idx], __ATOMIC_ACQUIRE)) return VK_NULL_HANDLE;
    if (!P->vs || !P->fs || !P->rp_base || !P->pipe_layout || P->nslots > VK_GUEST_ATTRS)
        return VK_NULL_HANDLE;

    const rsx_be_render_state* rs = &P->rs;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = P->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = P->fs, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, P->stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 s = 0; s < P->nslots; s++)
        va[s] = (VkVertexInputAttributeDescription){ s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = P->nslots, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo[topo_idx] };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = (rs->depth_clamp && s_vk.depth_clamp_ok) ? VK_TRUE : VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = !rs->cull_enable ? VK_CULL_MODE_NONE
                  : (rs->cull_face == 0x0404u || rs->cull_face == 0x0408u) ? VK_CULL_MODE_FRONT_BIT
                                                                           : VK_CULL_MODE_BACK_BIT,
        .frontFace = (rs->front_face == 0x0901u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                 : VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_TRUE,   /* the values are dynamic (vk_eng_set_depth_bias) */
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState front = vk_gcm_stencil_face(rs->s_func, rs->s_fail, rs->s_zfail, rs->s_zpass,
                                                 rs->s_func_mask, rs->s_write_mask);
    VkStencilOpState back = rs->stencil_two_sided
        ? vk_gcm_stencil_face(rs->bs_func, rs->bs_fail, rs->bs_zfail, rs->bs_zpass,
                              rs->s_func_mask, rs->s_write_mask)
        : front;
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = rs->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (rs->depth_test && rs->depth_write) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = vk_gcm_compare(rs->depth_func),
        .stencilTestEnable = rs->stencil_enable ? VK_TRUE : VK_FALSE,
        .front = front, .back = back,
        .depthBoundsTestEnable = (rs->depth_bounds_test && s_vk.depth_bounds_ok) ? VK_TRUE : VK_FALSE,
        .minDepthBounds = 0.0f, .maxDepthBounds = 1.0f };

    VkColorComponentFlags wm = 0;
    if ((rs->color_mask >> 16) & 0xFFu) wm |= VK_COLOR_COMPONENT_R_BIT;
    if ((rs->color_mask >>  8) & 0xFFu) wm |= VK_COLOR_COMPONENT_G_BIT;
    if ((rs->color_mask >>  0) & 0xFFu) wm |= VK_COLOR_COMPONENT_B_BIT;
    if ((rs->color_mask >> 24) & 0xFFu) wm |= VK_COLOR_COMPONENT_A_BIT;
    const int mrt_mode = (int)P->mrt_mode;
    const int blendable = (int)P->rt_blendable;
    VkPipelineColorBlendAttachmentState cba[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 r = 0; r < P->rt_count; r++) {
        VkColorComponentFlags twm = wm;
        int tblend = rs->blend_enable != 0;
        if (r > 0 && mrt_mode != 0 && s_vk.indep_blend_ok) {
            const u32 nib = (rs->color_mask_mrt >> (r * 4u)) & 0xFu;
            if (mrt_mode == 1 || nib == 0) {
                twm = 0;
                if (nib & 1u) twm |= VK_COLOR_COMPONENT_A_BIT;
                if (nib & 2u) twm |= VK_COLOR_COMPONENT_R_BIT;
                if (nib & 4u) twm |= VK_COLOR_COMPONENT_G_BIT;
                if (nib & 8u) twm |= VK_COLOR_COMPONENT_B_BIT;
            }
            tblend = (rs->blend_mrt >> r) & 1u;
        }
        if (tblend && !blendable) tblend = 0;
        cba[r] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = tblend ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = vk_gcm_blend_factor(rs->sf_rgb),
            .dstColorBlendFactor = vk_gcm_blend_factor(rs->df_rgb),
            .colorBlendOp = vk_gcm_blend_op(rs->eq_rgb),
            .srcAlphaBlendFactor = vk_gcm_blend_factor(rs->sf_a),
            .dstAlphaBlendFactor = vk_gcm_blend_factor(rs->df_a),
            .alphaBlendOp = vk_gcm_blend_op(rs->eq_a),
            .colorWriteMask = twm };
    }
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = P->rt_count, .pAttachments = cba };
    VkDynamicState dyn[6] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS,
                              VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                              VK_DYNAMIC_STATE_DEPTH_BOUNDS };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = s_vk.depth_bounds_ok ? 6u : 5u, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = P->pipe_layout, .renderPass = P->rp_base, .subpass = 0 };

    VkPipeline built = VK_NULL_HANDLE;
    VkResult pr = VK_ERROR_UNKNOWN;
#ifndef _WIN32
    pthread_rwlock_rdlock(&s_pipe_cache_rw);
#endif
    if (s_vk.device) {
        pr = pvkCreateGraphicsPipelines(s_vk.device, s_vk.pipe_cache, 1, &gci, NULL, &built);
    }
#ifndef _WIN32
    pthread_rwlock_unlock(&s_pipe_cache_rw);
#endif
    if (pr != VK_SUCCESS || !built) {
        VK_LOG("engine: vkCreateGraphicsPipelines failed (VkResult %d, topo=%u, nslots=%u, rt=%u, fmt=%d, cube=0x%x, vtex=0x%x)\n",
               (int)pr, topo_idx, P->nslots, P->rt_count, (int)P->rt_fmt, P->cube_mask, P->vtex_mask);
        __atomic_store_n(&P->variant_failed[topo_idx], 1u, __ATOMIC_RELEASE);
        return VK_NULL_HANDLE;
    }
    __atomic_store_n(&P->variant[topo_idx], built, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_vk.pipe_cache_built, 1u, __ATOMIC_RELAXED);
    return built;
}

/* ---- Background Async Shader & Pipeline Compiler Pool (ARMSX3/RPCS3-style) ----
 * Offloads HLSL->SPIR-V translation + VkShaderModule + Turnip IR3 VkPipeline
 * compilation from the RSX FIFO thread onto low-priority worker threads so
 * cutscene and level transitions never freeze rendering (pipe=60ms -> 0ms). */
#ifndef _WIN32
#define VK_APIPE_MAX_WORKERS 4u
#define VK_APIPE_Q_CAP       2048u
#define VK_APIPE_Q_MASK      (VK_APIPE_Q_CAP - 1u)

enum {
    VK_APIPE_JOB_PIPE    = 1u, /* translate VS+PS and pre-build TRIANGLE_LIST variant */
    VK_APIPE_JOB_VARIANT = 2u, /* build non-TRIANGLE_LIST topology variant */
};

typedef struct {
    u32   kind;
    u32   slot;
    u32   topo_idx;
    char* vs_hlsl;
    char* ps_hlsl;
} vk_apipe_job;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pthread_cond_t  done_cv;
    pthread_t       workers[VK_APIPE_MAX_WORKERS];
    u32             nworkers;
    int             started;
    int             stop;
    int             enabled; /* -1 undecided, 0 off, 1 on */
    vk_apipe_job    q[VK_APIPE_Q_CAP];
    u32             q_head;
    u32             q_tail;
    volatile u32    completed;
} s_apipe = {
    .mu      = PTHREAD_MUTEX_INITIALIZER,
    .cv      = PTHREAD_COND_INITIALIZER,
    .done_cv = PTHREAD_COND_INITIALIZER,
    .enabled = -1,
};

static int vk_apipe_enabled(void)
{
    if (s_apipe.enabled >= 0) return s_apipe.enabled;
    const char* e = getenv("PS3RECOMP_VK_ASYNC_PIPE");
#ifdef __ANDROID__
    s_apipe.enabled = (e && *e == '0') ? 0 : 1;
#else
    s_apipe.enabled = (e && *e == '1') ? 1 : 0;
#endif
    return s_apipe.enabled;
}

static void* vk_apipe_worker(void* arg)
{
    (void)arg;
    /* Run slightly below normal priority so background shader compilation never
     * preempts cellAudio (-16), SPU task_audio (-8), or the RSX FIFO thread. */
    (void)setpriority(PRIO_PROCESS, 0, 4);
    u32* spv = (u32*)malloc((size_t)VK_SPV_WORDS * sizeof(u32));
    char log_buf[1024];

    for (;;) {
        vk_apipe_job job;
        pthread_mutex_lock(&s_apipe.mu);
        while (!s_apipe.stop && s_apipe.q_head == s_apipe.q_tail)
            pthread_cond_wait(&s_apipe.cv, &s_apipe.mu);
        if (s_apipe.stop && s_apipe.q_head == s_apipe.q_tail) {
            pthread_mutex_unlock(&s_apipe.mu);
            break;
        }
        job = s_apipe.q[s_apipe.q_tail & VK_APIPE_Q_MASK];
        s_apipe.q_tail++;
        pthread_mutex_unlock(&s_apipe.mu);

        vk_eng_pipe* P = &s_epipe[job.slot];
        if (job.kind == VK_APIPE_JOB_PIPE) {
            VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
            u8 vs_own = 0, fs_own = 0;
            int ok = 0;
            if (spv && job.vs_hlsl && job.ps_hlsl &&
                vk_eng_translate_ex(job.vs_hlsl, RSX_SHADER_STAGE_VERTEX, &vs, &vs_own,
                                    spv, VK_SPV_WORDS, log_buf, sizeof log_buf) == 0) {
                if (vk_eng_translate_ex(job.ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, &fs, &fs_own,
                                        spv, VK_SPV_WORDS, log_buf, sizeof log_buf) == 0) {
                    ok = 1;
                } else if (vs_own && vs && s_vk.device) {
                    pvkDestroyShaderModule(s_vk.device, vs, NULL);
                    vs = VK_NULL_HANDLE;
                }
            }
            free(job.vs_hlsl);
            free(job.ps_hlsl);
            if (ok) {
                P->vs = vs; P->vs_owned = vs_own;
                P->fs = fs; P->fs_owned = fs_own;
                /* Pre-compile TRIANGLE_LIST (covers >98% of PS3 draws) immediately on worker. */
                (void)vk_eng_build_variant_ts(P, job.topo_idx);
                __atomic_store_n(&P->async_state, 0u, __ATOMIC_RELEASE);
                __atomic_add_fetch(&s_apipe.completed, 1u, __ATOMIC_RELAXED);
            } else {
                __atomic_store_n(&P->async_state, 2u, __ATOMIC_RELEASE);
            }
        } else if (job.kind == VK_APIPE_JOB_VARIANT) {
            if (vk_eng_build_variant_ts(P, job.topo_idx))
                __atomic_add_fetch(&s_apipe.completed, 1u, __ATOMIC_RELAXED);
        }

        if (__atomic_sub_fetch(&P->async_refs, 1u, __ATOMIC_ACQ_REL) == 0u) {
            pthread_mutex_lock(&s_apipe.mu);
            pthread_cond_broadcast(&s_apipe.done_cv);
            pthread_mutex_unlock(&s_apipe.mu);
        }
    }
    free(spv);
    return NULL;
}

static int vk_apipe_ensure_started(void)
{
    if (s_apipe.started) return s_apipe.nworkers > 0 ? 1 : 0;
    s_apipe.started = 1;
    s_apipe.stop = 0;
    u32 want = 2u;
#ifdef _SC_NPROCESSORS_ONLN
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu >= 6) want = 3u;
    else if (ncpu <= 2) want = 1u;
#endif
    const char* env_t = getenv("PS3RECOMP_VK_ASYNC_THREADS");
    if (env_t && atoi(env_t) > 0) want = (u32)atoi(env_t);
    if (want > VK_APIPE_MAX_WORKERS) want = VK_APIPE_MAX_WORKERS;
    s_apipe.nworkers = 0;
    for (u32 i = 0; i < want; i++) {
        if (pthread_create(&s_apipe.workers[s_apipe.nworkers], NULL, vk_apipe_worker, NULL) == 0)
            s_apipe.nworkers++;
    }
    if (s_apipe.nworkers)
        VK_LOG("async pipeline compiler: on (%u background workers)\n", s_apipe.nworkers);
    return s_apipe.nworkers > 0 ? 1 : 0;
}

static int vk_apipe_enqueue(u32 kind, u32 slot, u32 topo_idx, char* vs_hlsl, char* ps_hlsl)
{
    if (!vk_apipe_ensure_started()) return 0;
    pthread_mutex_lock(&s_apipe.mu);
    if (s_apipe.stop || (s_apipe.q_head - s_apipe.q_tail) >= VK_APIPE_Q_CAP) {
        pthread_mutex_unlock(&s_apipe.mu);
        return 0;
    }
    __atomic_add_fetch(&s_epipe[slot].async_refs, 1u, __ATOMIC_ACQ_REL);
    vk_apipe_job* j = &s_apipe.q[s_apipe.q_head & VK_APIPE_Q_MASK];
    j->kind = kind;
    j->slot = slot;
    j->topo_idx = topo_idx;
    j->vs_hlsl = vs_hlsl;
    j->ps_hlsl = ps_hlsl;
    s_apipe.q_head++;
    pthread_cond_signal(&s_apipe.cv);
    pthread_mutex_unlock(&s_apipe.mu);
    return 1;
}

static void vk_apipe_wait_slot(vk_eng_pipe* P)
{
    if (!__atomic_load_n(&P->async_refs, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&s_apipe.mu);
    while (__atomic_load_n(&P->async_refs, __ATOMIC_ACQUIRE) > 0u)
        pthread_cond_wait(&s_apipe.done_cv, &s_apipe.mu);
    pthread_mutex_unlock(&s_apipe.mu);
}

static void vk_apipe_shutdown(void)
{
    if (!s_apipe.started) return;
    pthread_mutex_lock(&s_apipe.mu);
    s_apipe.stop = 1;
    while (s_apipe.q_tail != s_apipe.q_head) {
        vk_apipe_job* j = &s_apipe.q[s_apipe.q_tail & VK_APIPE_Q_MASK];
        s_apipe.q_tail++;
        free(j->vs_hlsl);
        free(j->ps_hlsl);
        __atomic_sub_fetch(&s_epipe[j->slot].async_refs, 1u, __ATOMIC_ACQ_REL);
    }
    pthread_cond_broadcast(&s_apipe.cv);
    pthread_cond_broadcast(&s_apipe.done_cv);
    pthread_mutex_unlock(&s_apipe.mu);
    for (u32 i = 0; i < s_apipe.nworkers; i++)
        pthread_join(s_apipe.workers[i], NULL);
    s_apipe.nworkers = 0;
    s_apipe.started = 0;
    s_apipe.stop = 0;
    s_apipe.q_head = s_apipe.q_tail = 0;
}

static u32 vk_apipe_completed(void)
{
    return __atomic_load_n(&s_apipe.completed, __ATOMIC_RELAXED);
}
#else
static int  vk_apipe_enabled(void) { return 0; }
static void vk_apipe_wait_slot(vk_eng_pipe* P) { (void)P; }
static void vk_apipe_shutdown(void) {}
static u32  vk_apipe_completed(void) { return 0; }
#endif

static u32 vk_eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                                  const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)user;
    /* Same gate as Metal: no guest programs, no engine pipelines. */
    if (!s_g.on || !vs_hlsl || !ps_hlsl || !rs || !layout || !vertex_stride) return 0;
    u32 rt_bpp;
    const VkFormat cf = vk_eng_format(rt_fmt, &rt_bpp);
    if (vk_eng_rt_index(cf) < 0) {
        if (!(s_e2.warned & 1u)) { VK_LOG("engine: colour format %d not supported yet\n", (int)rt_fmt); s_e2.warned |= 1u; }
        return 0;
    }
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    u32 slot = VK_ENG_MAX_PIPE;
    for (u32 i = 0; i < VK_ENG_MAX_PIPE; i++) if (!s_epipe[i].used) { slot = i; break; }
    if (slot == VK_ENG_MAX_PIPE) {
        if (!(s_e2.warned & 128u)) {
            VK_LOG("engine: pipeline table full (%u), draws will drop\n", (unsigned)VK_ENG_MAX_PIPE);
            s_e2.warned |= 128u;
        }
        return 0;
    }
    const u32 cube_mask = vk_eng_cube_units(ps_hlsl);
    VkRenderPass rp_base = vk_eng_render_pass(cf, rt_count);
    VkPipelineLayout pipe_layout = cube_mask ? vk_eng_split_layout() : s_g.pipe_layout;
    if (!rp_base || !pipe_layout) return 0;

    if (rs->depth_bounds_test && !s_vk.depth_bounds_ok && !(s_e2.warned & 64u)) {
        VK_LOG("engine: the device has no depthBounds; the depth bounds test is ignored\n");
        s_e2.warned |= 64u;
    }
    const int blendable = vk_eng_rt_blendable(cf);
    if (rs->blend_enable && !blendable && !(s_e2.warned & 256u)) {
        VK_LOG("engine: blending unsupported for colour format %d on this device; drawing unblended\n",
               (int)cf);
        s_e2.warned |= 256u;
    }

    vk_eng_pipe* P = &s_epipe[slot];
    memset(P, 0, sizeof *P);
    P->rs = *rs;
    P->nslots = layout->count;
    P->stride = vertex_stride;
    P->rt_count = rt_count;
    P->rt_fmt = cf;
    P->cube_mask = cube_mask;
    P->vtex_mask = vk_eng_vtex_units(vs_hlsl);
    vk_eng_scan_vp_consts(vs_hlsl, &P->vp_const_lo, &P->vp_const_hi);
    P->rp_base = rp_base;
    P->pipe_layout = pipe_layout;
    P->rt_blendable = (u8)blendable;
    P->mrt_mode = (u8)vk_eng_mrt_mode();
    P->used = 1;

#ifndef _WIN32
    /* Offload shader translation + TRIANGLE_LIST pipeline creation once active
     * rendering has begun so cutscene/area switches never stall the RSX thread. */
    if (vk_apipe_enabled() && s_e2.draws_seen > 16u) {
        char* dup_vs = strdup(vs_hlsl);
        char* dup_ps = strdup(ps_hlsl);
        if (dup_vs && dup_ps) {
            P->async_state = 1u; /* COMPILING */
            P->variant_queued[3] = 1u; /* TRIANGLE_LIST built with shader pair */
            if (vk_apipe_enqueue(VK_APIPE_JOB_PIPE, slot, 3u, dup_vs, dup_ps))
                return slot + 1u;
            P->async_state = 0u;
            P->variant_queued[3] = 0u;
        }
        free(dup_vs);
        free(dup_ps);
    }
#endif

    if (vk_eng_translate(vs_hlsl, RSX_SHADER_STAGE_VERTEX, &P->vs, &P->vs_owned)) {
        memset(P, 0, sizeof *P);
        return 0;
    }
    if (vk_eng_translate(ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, &P->fs, &P->fs_owned)) {
        if (P->vs_owned && P->vs) pvkDestroyShaderModule(s_vk.device, P->vs, NULL);
        memset(P, 0, sizeof *P);
        return 0;
    }
    P->async_state = 0u;
    return slot + 1u;
}

static void vk_eng_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    if (s_eb_pending) vk_eng_flush();
    if (!pipeline || pipeline > VK_ENG_MAX_PIPE || !s_epipe[pipeline - 1].used) return;
    vk_eng_pipe* P = &s_epipe[pipeline - 1];
    vk_apipe_wait_slot(P);
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++)
        if (P->variant[t]) pvkDestroyPipeline(s_vk.device, P->variant[t], NULL);
    if (P->vs_owned && P->vs) pvkDestroyShaderModule(s_vk.device, P->vs, NULL);
    if (P->fs_owned && P->fs) pvkDestroyShaderModule(s_vk.device, P->fs, NULL);
    memset(P, 0, sizeof *P);
    if (s_e2.pipeline == pipeline) s_e2.pipeline = 0;
}

static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx)
{
    VkPipeline v = __atomic_load_n(&P->variant[topo_idx], __ATOMIC_ACQUIRE);
    if (v) return v;
    if (__atomic_load_n(&P->variant_failed[topo_idx], __ATOMIC_ACQUIRE)) return VK_NULL_HANDLE;
    if (!P->rp_base) P->rp_base = vk_eng_render_pass(P->rt_fmt, P->rt_count);
    if (!P->pipe_layout) P->pipe_layout = P->cube_mask ? vk_eng_split_layout() : s_g.pipe_layout;
#ifndef _WIN32
    if (vk_apipe_enabled() && s_e2.draws_seen > 16u) {
        if (!__atomic_exchange_n(&P->variant_queued[topo_idx], 1u, __ATOMIC_ACQ_REL)) {
            const u32 slot = (u32)(P - s_epipe);
            if (!vk_apipe_enqueue(VK_APIPE_JOB_VARIANT, slot, topo_idx, NULL, NULL)) {
                __atomic_store_n(&P->variant_queued[topo_idx], 0u, __ATOMIC_RELEASE);
                return vk_eng_build_variant_ts(P, topo_idx);
            }
        }
        return VK_NULL_HANDLE;
    }
#endif
    return vk_eng_build_variant_ts(P, topo_idx);
}

static void vk_eng_bind_pipeline(void* user, u32 pipeline) { (void)user; s_e2.pipeline = pipeline; }

static void vk_gpuvc_shutdown(void);

static void vk_eng2_shutdown(void* user)
{
    vk_apipe_shutdown();
    for (u32 i = 1; i <= VK_ENG_MAX_PIPE; i++) vk_eng_pipeline_release(user, i);
    vk_gpuvc_shutdown();
    vk_eng_sh_cache_clear();
    vk_eng_drop_framebuffers();
    for (u32 f = 0; f < VK_ENG_RT_FORMATS; f++)
        for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
            for (u32 m = 0; m < 64u; m++)
                if (s_e2.rp_ex[f][n][m]) pvkDestroyRenderPass(s_vk.device, s_e2.rp_ex[f][n][m], NULL);
    vk_destroy_image(&s_e2.scratch_depth);
    vk_destroy_buffer(&s_e2.indices);
    if (s_e2.split_pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_e2.split_pipe_layout, NULL);
    if (s_e2.split_pool)        pvkDestroyDescriptorPool(s_vk.device, s_e2.split_pool, NULL);
    if (s_e2.split_layout)      pvkDestroyDescriptorSetLayout(s_vk.device, s_e2.split_layout, NULL);
    free(s_e2.ucpu);
    memset(&s_e2, 0, sizeof s_e2);
    for (u32 i = 0; i < s_esamp_count; i++) pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
    s_esamp_count = 0;
}

/* Constants are kept in ordinary memory laid out like the shared UBO (vertex
 * at 0, pixel at ps_off) and copied to the GPU per draw: into the draw's ring
 * slice when batched, into the shared UBO otherwise (every such draw is waited
 * on, so overwriting it is safe). Writing them straight into the UBO meant a
 * batched draw read them back out of it, and that memory is host-coherent,
 * i.e. write-combined and uncached on most drivers. */
static u8* vk_eng_ucpu(void)
{
    if (!s_e2.ucpu && s_g.ubo.size) s_e2.ucpu = (u8*)calloc(1, (size_t)s_g.ubo.size);
    return s_e2.ucpu;
}
static void vk_eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data || !vk_eng_ucpu()) { s_e2.vs_src = NULL; s_e2.vs_bytes = 0; return; }
    /* The engine's block ends in the Metal translator's vp_pointsize row (one
     * float4), which this backend's shaders do not declare
     * (RSX_POINT_SIZE_BUILTIN is not defined for them): never read, so it is
     * dropped here. A block longer than that is still reported below. */
    if (bytes > VK_VPCONST_BYTES) {
        if (bytes > VK_VPCONST_BYTES + 16u && !(s_e2.warned & 2u)) {
            VK_LOG("engine: VS constants truncated\n");
            s_e2.warned |= 2u;
        }
        bytes = VK_VPCONST_BYTES;
    }
    const u32 cap = (u32)s_g.ps_off;
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 2u)) { VK_LOG("engine: VS constants truncated\n"); s_e2.warned |= 2u; } }
    /* The draw engine already tracks vp_const_dirty + viewport/point-size changes
     * and keeps data alive through draw(), so avoid an extra 8 KB memcmp+memcpy. */
    s_e2.vs_src = data;
    s_e2.vs_bytes = bytes;
    s_e2.vs_gen++;
}
static void vk_eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data || !vk_eng_ucpu()) { s_e2.ps_bytes = 0; s_e2.ps_gen++; return; }
    const u32 cap = (u32)(s_g.ubo.size - s_g.ps_off);
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 4u)) { VK_LOG("engine: PS constants truncated\n"); s_e2.warned |= 4u; } }
    if (s_e2.ps_gen && bytes == s_e2.ps_bytes && memcmp(s_e2.ucpu + s_g.ps_off, data, bytes) == 0)
        return;
    memcpy(s_e2.ucpu + s_g.ps_off, data, bytes);
    s_e2.ps_bytes = bytes;
    s_e2.ps_gen++;
}
static void vk_eng_bind_textures(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    mask &= (1u << RSX_BE_MAX_TEXTURES) - 1u;
    int changed = (s_e2.tex_mask != mask);
    u32 work = mask | s_e2.tex_mask;
    while (work) {
        const u32 u = (u32)__builtin_ctz(work);
        work &= work - 1u;
        const int on = (int)((mask >> u) & 1u);
        const u32 h = on ? textures[u] : 0u;
        if (s_e2.tex[u] != h) {
            s_e2.tex[u] = h;
            changed = 1;
        }
        if (on && samplers && memcmp(&s_e2.samp[u], &samplers[u], sizeof samplers[u]) != 0) {
            s_e2.samp[u] = samplers[u];
            changed = 1;
        }
    }
    s_e2.tex_mask = mask;
    if (changed) s_e2.tex_gen++;
}
static void vk_eng_bind_vertex_textures(void* user, const u32* textures,
                                        const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    mask &= (1u << RSX_BE_MAX_VERTEX_TEXTURES) - 1u;
    int changed = (s_e2.vtex_mask != mask);
    u32 work = mask | s_e2.vtex_mask;
    while (work) {
        const u32 u = (u32)__builtin_ctz(work);
        work &= work - 1u;
        const int on = (int)((mask >> u) & 1u);
        const u32 h = on ? textures[u] : 0u;
        if (s_e2.vtex[u] != h) {
            s_e2.vtex[u] = h;
            changed = 1;
        }
        if (on && samplers && memcmp(&s_e2.vsamp[u], &samplers[u], sizeof samplers[u]) != 0) {
            s_e2.vsamp[u] = samplers[u];
            changed = 1;
        }
    }
    s_e2.vtex_mask = mask;
    if (changed) s_e2.tex_gen++;
}
static void vk_eng_set_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    s_e2.vp[0] = x; s_e2.vp[1] = y; s_e2.vp[2] = w; s_e2.vp[3] = h; s_e2.have_vp = 1;
}
static void vk_eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    s_e2.sc[0] = x; s_e2.sc[1] = y; s_e2.sc[2] = w; s_e2.sc[3] = h; s_e2.have_sc = 1;
}
static void vk_eng_set_stencil_ref(void* user, u32 ref) { (void)user; s_e2.stencil_ref = ref; }
static void vk_eng_set_blend_color(void* user, const float rgba[4])
{
    (void)user;
    memcpy(s_e2.blend_color, rgba, sizeof s_e2.blend_color);
}

/* Vulkan wants the bounds inside [0, 1] without VK_EXT_depth_range_unrestricted;
 * a NaN clamps to the open end so a garbage register cannot reject a draw. */
static float vk_depth_bound(float v, float open)
{
    if (!(v == v)) return open;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static void vk_eng_set_depth_bounds(void* user, float min, float max)
{
    (void)user;
    s_e2.depth_bounds[0] = vk_depth_bound(min, 0.0f);
    s_e2.depth_bounds[1] = vk_depth_bound(max, 1.0f);
}

/* Polygon offset. glPolygonOffset's units are steps of the guest depth buffer,
 * 2^-16 for Z16 and 2^-24 for Z24S8. On the D32F targets a Vulkan unit is
 * 2^(e-23) for a primitive whose largest depth has exponent e: 2^-24 over
 * [0.5, 1), where nearly all of a perspective scene's depth sits, so Z24 units
 * carry over as they are and a Z16 unit is 256 of them. */
static void vk_eng_set_depth_bias(void* user, float slope, float units, u32 depth_bits)
{
    (void)user;
    s_e2.depth_bias[0] = depth_bits == 16u ? units * 256.0f : units;
    s_e2.depth_bias[1] = slope;
}

static VkFramebuffer vk_eng_framebuffer(VkRenderPass rp, const u32* rt, u32 n, VkImageView dview,
                                        u32 dkey, u32 w, u32 h)
{
    u32 key[RSX_BE_MAX_COLOR_TARGETS + 1] = { 0 };
    for (u32 i = 0; i < n; i++) key[i] = rt[i];
    key[RSX_BE_MAX_COLOR_TARGETS] = dkey;
    if (s_e2.fb_last_idx < s_e2.fb_count &&
        !memcmp(s_e2.fb[s_e2.fb_last_idx].key, key, sizeof key))
        return s_e2.fb[s_e2.fb_last_idx].fb;
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (!memcmp(s_e2.fb[i].key, key, sizeof key)) {
            s_e2.fb_last_idx = i;
            return s_e2.fb[i].fb;
        }
    if (s_e2.fb_count >= VK_ENG_FB_CACHE) vk_eng_drop_framebuffers();
    VkImageView views[RSX_BE_MAX_COLOR_TARGETS + 1];
    for (u32 i = 0; i < n; i++) views[i] = s_eobj[rt[i]].im.view;
    views[n] = dview;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = n + 1, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    const u32 idx = s_e2.fb_count++;
    memcpy(s_e2.fb[idx].key, key, sizeof key);
    s_e2.fb[idx].fb = fb;
    s_e2.fb_last_idx = idx;
    return fb;
}

/* Depth when the draw names no zeta: one scratch image, grown as needed and
 * cleared to the far plane when (re)created. Key 0xFFFFFFFF in the cache. */
static VkImageView vk_eng_scratch_depth(u32 w, u32 h)
{
    if (s_e2.scratch_depth.view && s_e2.scratch_w >= w && s_e2.scratch_h >= h)
        return s_e2.scratch_depth.view;
    vk_eng_drop_framebuffers();
    vk_destroy_image(&s_e2.scratch_depth);
    if (vk_create_image(&s_e2.scratch_depth, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        s_vk.depth_aspect) || vk_begin())
        return VK_NULL_HANDLE;
    vk_barrier_image(s_e2.scratch_depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_e2.scratch_depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_submit_and_wait()) return VK_NULL_HANDLE;
    s_e2.scratch_w = w; s_e2.scratch_h = h;
    return s_e2.scratch_depth.view;
}

static int vk_eng_ensure_indices(VkDeviceSize bytes)
{
    if (s_e2.indices.buf && s_e2.indices.size >= bytes) return 0;
    VkDeviceSize size = s_e2.indices.size ? s_e2.indices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_e2.indices);
    if (vk_create_host_buffer(&s_e2.indices, size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return -1;
    return s_e2.indices.coherent ? 0 : -1;
}

/* rsx_be_sampler_desc is the registers decoded the D3D12 way; the wrap codes
 * stay the guest's 1..8, which vk_gcm_wrap already translates. */
static VkSampler vk_eng_sampler(const rsx_be_sampler_desc* d)
{
    static int s_low_end = -1;
    static u32 s_last_samp = 0;
    if (s_low_end < 0) s_low_end = vk_env_on("PS3RECOMP_ENG_LOW_END");
    const u8 mip_lin = s_low_end ? 0u : d->mip_linear;
    u32 lo, hi;
    memcpy(&lo, &d->min_lod, 4); memcpy(&hi, &d->max_lod, 4);
    const u64 key = (u64)d->min_linear | ((u64)d->mag_linear << 1) | ((u64)mip_lin << 2)
                  | ((u64)d->mip_present << 3) | ((u64)(d->wrap_s & 0xF) << 4)
                  | ((u64)(d->wrap_t & 0xF) << 8) | ((u64)(d->wrap_r & 0xF) << 12)
                  | ((u64)(lo >> 16) << 16) | ((u64)(hi >> 16) << 32);
    if (s_last_samp < s_esamp_count && s_esamp[s_last_samp].key == key)
        return s_esamp[s_last_samp].s;
    for (u32 i = 0; i < s_esamp_count; i++)
        if (s_esamp[i].key == key) { s_last_samp = i; return s_esamp[i].s; }
    if (s_esamp_count >= VK_ENG_SAMP_CACHE) return s_vk.sampler;
    float min_lod = d->min_lod, max_lod = d->mip_present ? d->max_lod : d->min_lod;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = d->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = d->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_lin ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(d->wrap_s), .addressModeV = vk_gcm_wrap(d->wrap_t),
        .addressModeW = vk_gcm_wrap(d->wrap_r),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    VkSampler smp;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return s_vk.sampler;
    s_last_samp = s_esamp_count;
    s_esamp[s_esamp_count].key = key;
    s_esamp[s_esamp_count++].s = smp;
    return smp;
}

/* The view a bound engine handle samples through: an uploaded texture, a
 * colour target (bound directly when surface_view was not asked), or a view.
 * A unit gets a view of its own kind only -- a cube for a TextureCube unit, a
 * 2D one otherwise -- and NULL for anything else, which then samples the
 * dummy of the unit's kind. */
static VkImageView vk_eng_sample_view(u32 handle, int want_cube)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return VK_NULL_HANDLE;
    const vk_eng_obj* o = &s_eobj[handle];
    if (o->kind != VK_ENG_TEXTURE && o->kind != VK_ENG_COLOR && o->kind != VK_ENG_VIEW)
        return VK_NULL_HANDLE;
    return ((int)o->cube == want_cube) ? o->im.view : VK_NULL_HANDLE;
}

/* What an engine draw samples on a unit with nothing bound: transparent
 * black (0,0,0,0), the Metal backend's placeholder, as a 1x1 2D texture and
 * a 1x1 cube, each made on first use. A material that reads a unit the title
 * never binds then adds nothing, as it does on Metal; the magenta "not bound"
 * colour it read before showed through as a pink cast. The fixed-function
 * path keeps its magenta s_vk.dummy. */
static const u8 s_eng_unbound_texel[4] = { 0x00, 0x00, 0x00, 0x00 };
static VkImageView vk_eng_dummy_cube_view(void)
{
    if (!vk_eng_get(s_e2.dummy_cube, VK_ENG_TEXTURE)) {
        s_e2.dummy_cube = vk_eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 1, 1, 1, 6,
                                                0xAAE4u, 0x85u);
        if (!s_e2.dummy_cube) return VK_NULL_HANDLE;
        for (u32 f = 0; f < 6; f++)
            vk_eng_texture_upload(NULL, s_e2.dummy_cube, f, 0, 1, 1, s_eng_unbound_texel, 4, 1);
    }
    return s_eobj[s_e2.dummy_cube].im.view;
}
static VkImageView vk_eng_dummy_2d_view(void)
{
    if (!vk_eng_get(s_e2.dummy_2d, VK_ENG_TEXTURE)) {
        s_e2.dummy_2d = vk_eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 1, 1, 1, 1,
                                              0xAAE4u, 0x85u);
        if (!s_e2.dummy_2d) return VK_NULL_HANDLE;
        vk_eng_texture_upload(NULL, s_e2.dummy_2d, 0, 0, 1, 1, s_eng_unbound_texel, 4, 1);
    }
    return s_eobj[s_e2.dummy_2d].im.view;
}

/* ---------------------------------------------------------------------------
 * Draw batching (VK_DRAW_BATCH=1)
 *
 * Every vk_eng_draw used to submit its own command buffer and wait on a
 * fence, because the vertex, index and uniform buffers and the descriptor set
 * are single objects rewritten per draw. In God of War: Ascension's first level
 * that was 54,759 draws in 20 s at ~168 us each: the FIFO walker spent 46% of
 * its time blocked on the GPU, the RSX ring drained one draw at a time and the
 * SPU geometry jobs waited for ring space. Batched, a draw takes slices of
 * three ring buffers and a descriptor set from a pool, records into the open
 * command buffer, and only s_eb_pending grows. vk_eng_flush() submits, waits
 * once, and resets the rings and the pool; vk_begin, every object destroy, the
 * present and the engine's own submit point call it first, so all other work
 * keeps seeing everything before it complete. Between batched draws a full
 * memory barrier keeps render-to-texture ordering without tracking.
 * VK_DRAW_BATCH_MAX (default 512) caps the draws per submission.
 * -------------------------------------------------------------------------*/
#define VK_EB_SLOTS      3u
#define VK_EB_DEFER_MAX  256u
#define VK_EB_DS_BUCKETS 128u
#define VK_GPUVC_SETS    2048u

static VkDeviceSize vk_eb_ps_range(void)
{
    VkDeviceSize r = (VkDeviceSize)(RSX_FP_MAX_INLINE_CONSTANTS + 4u) * 16u;
    if (s_g.max_ubo_range && r > s_g.max_ubo_range) r = s_g.max_ubo_range;
    return r;
}

typedef struct {
    u64             hash;
    VkDescriptorSet set;
    u32             vtex_mask;
    u8              split;
    u8              valid;
    VkImageView     img[RSX_MAX_TEXTURES];
    VkSampler       smp[RSX_MAX_TEXTURES];
    VkImageView     vimg[RSX_BE_MAX_VERTEX_TEXTURES];
    VkSampler       vsmp[RSX_BE_MAX_VERTEX_TEXTURES];
} vk_eb_ds_entry;

typedef struct {
    VkCommandBuffer  cmd;
    VkFence          fence;
    int              busy;
    vk_buffer        vb, ib, ub, sb;
    VkDeviceSize     vb_head, ib_head, ub_head, sb_head;
    VkDescriptorPool pool;
    u32              sets;
    int              has_last_vs, has_last_ps;
    int              last_vs_copy_valid;
    u32              last_vs_gen, last_ps_gen;
    u16              last_vs_lo, last_vs_hi;
    VkDeviceSize     last_vs_off, last_ps_off;
    VkDescriptorSet  last_ds_set;
    u32              last_tex_gen, last_cube_mask, last_vtex_mask;
    vk_eb_ds_entry   ds_cache[VK_EB_DS_BUCKETS][2];
    u8               ds_lru[VK_EB_DS_BUCKETS];
    vk_image         defer[VK_EB_DEFER_MAX];
    u32              defer_n;
    VkFramebuffer    defer_fb[VK_EB_DEFER_MAX];
    u32              defer_fb_n;
} vk_eb_slot;

static struct {
    int              on;            /* -1 undecided, 0 off, 1 on */
    u32              max_draws;
    u32              max_sets;
    VkDeviceSize     vb_cap, ib_cap, ub_cap, sb_cap;
    VkDeviceSize     ub_align;
    VkCommandBuffer  saved_cmd;
    vk_eb_slot       slot[VK_EB_SLOTS];
    u32              cur;
    /* Open render-pass coalescing: keeps consecutive draws targeting the same
     * (rp_base, fb) inside one VkRenderPass so tile-based GPUs (Adreno GMEM) stay
     * on-chip instead of unresolving/resolving every single draw. */
    int              in_pass;
    VkRenderPass     open_rp_base;
    VkFramebuffer    open_fb;
    u32              open_w, open_h;
    u32              open_nrt;
    u32              open_rt[RSX_BE_MAX_COLOR_TARGETS];
    u32              open_depth;
    u32              open_rt_dirty;
    int              open_depth_dirty;
    VkPipeline       open_pipe;
    VkDescriptorSet  open_set;
    u32              open_dyn[2];
    u8               open_set_split;
    /* Cached CPU copy of the last written VS constant slice for fast memcmp dedup. */
    u8               last_vs_copy[VK_VPCONST_BYTES];
    /* Cached dynamic state inside the open render pass. */
    int              dyn_valid;
    VkViewport       open_vp;
    VkRect2D         open_sc;
    u32              open_stencil;
    float            open_blend[4];
    float            open_dbounds[2];
    float            open_dbias[2];
    unsigned long long draws, passes, flushes;
    /* 120-frame periodic [vk-perf] counters */
    u32              p_frames, p_draws, p_passes, p_flushes;
    u32              p_reason[VK_FLUSH_REASON_COUNT];
    u32              p_sync_tag[VK_SYNC_TAG_COUNT];
    u32              p_pb[VK_PB_COUNT];
    u32              p_vs_dedup, p_ps_dedup, p_ds_hit;
    u32              p_gpuvc_hit, p_clear_fold;
    u32              p_fg_last, p_acomp_last, p_async_skip;
    double           p_wait_sec;
} s_eb = { .on = -1 };

/* ---- GPU-resident persistent VBO/IBO cache (keyed by eng_vcache vc_id) ----
 * On Android UMA (Adreno LPDDR5X), HOST_COHERENT|HOST_CACHED memory is shared
 * directly by CPU and GPU, so keeping static meshes resident across frames
 * eliminates 40-60 MB/frame of per-draw vertex/index ring memcpy traffic. */
typedef struct {
    u64          id;
    VkDeviceSize voff, ioff;
    VkDeviceSize vbytes, ibytes;
    u64          lru;
    u8           valid;
} vk_gpuvc_entry;

static struct {
    int            state;    /* 0 uninit, 1 ready, -1 unavailable */
    vk_buffer      vb, ib;
    VkDeviceSize   vb_head, ib_head;
    u64            lru;
    vk_gpuvc_entry tab[VK_GPUVC_SETS][2];
} s_gpuvc;

static u64 s_eng_next_vc_id;

static void vk_eng_set_vcache_id(void* user, u64 id)
{
    (void)user;
    s_eng_next_vc_id = id;
}

static int vk_gpuvc_ensure(void)
{
    if (s_gpuvc.state != 0) return s_gpuvc.state > 0 ? 1 : 0;
    const u64 ram_mb = vk_system_ram_mb();
    const VkDeviceSize vb_sz = (ram_mb >= 6000u) ? (64u << 20)
                             : (ram_mb >= 4000u) ? (24u << 20) : (12u << 20);
    const VkDeviceSize ib_sz = (ram_mb >= 6000u) ? (24u << 20)
                             : (ram_mb >= 4000u) ? (8u  << 20) : (4u  << 20);
    if (vk_create_host_buffer(&s_gpuvc.vb, vb_sz, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) || !s_gpuvc.vb.coherent ||
        vk_create_host_buffer(&s_gpuvc.ib, ib_sz, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)  || !s_gpuvc.ib.coherent) {
        vk_destroy_buffer(&s_gpuvc.vb);
        vk_destroy_buffer(&s_gpuvc.ib);
        s_gpuvc.state = -1;
        return 0;
    }
    s_gpuvc.vb_head = s_gpuvc.ib_head = 0;
    s_gpuvc.lru = 0;
    memset(s_gpuvc.tab, 0, sizeof s_gpuvc.tab);
    s_gpuvc.state = 1;
    VK_LOG("GPU vertex cache: on (%u MB VB + %u MB IB, %u entries)\n",
           (u32)(vb_sz >> 20), (u32)(ib_sz >> 20), (u32)(VK_GPUVC_SETS * 2u));
    return 1;
}

static void vk_gpuvc_shutdown(void)
{
    if (s_gpuvc.vb.buf) vk_destroy_buffer(&s_gpuvc.vb);
    if (s_gpuvc.ib.buf) vk_destroy_buffer(&s_gpuvc.ib);
    memset(&s_gpuvc, 0, sizeof s_gpuvc);
}

/* Returns: 1 = GPU cache hit (0 memcpy), 2 = uploaded into GPU cache, 0 = bypass. */
static int vk_gpuvc_get_or_upload(u64 id, const void* verts, VkDeviceSize vbytes,
                                  const void* idxs, VkDeviceSize ibytes,
                                  VkBuffer* out_vb, VkDeviceSize* out_voff,
                                  VkBuffer* out_ib, VkDeviceSize* out_ioff)
{
    if (!id || !vbytes || vbytes > (4u << 20) || ibytes > (2u << 20)) return 0;
    if (!vk_gpuvc_ensure()) return 0;
    const u32 set = (u32)(((id ^ (id >> 16) ^ (id >> 32)) * 0x9E3779B1ull) & (VK_GPUVC_SETS - 1u));
    for (u32 w = 0; w < 2u; w++) {
        vk_gpuvc_entry* e = &s_gpuvc.tab[set][w];
        if (e->valid && e->id == id && e->vbytes == vbytes && e->ibytes == ibytes) {
            e->lru = ++s_gpuvc.lru;
            *out_vb = s_gpuvc.vb.buf;
            *out_voff = e->voff;
            if (ibytes) { *out_ib = s_gpuvc.ib.buf; *out_ioff = e->ioff; }
            return 1;
        }
    }
    VkDeviceSize vo = (s_gpuvc.vb_head + 15u) & ~(VkDeviceSize)15u;
    VkDeviceSize io = ibytes ? ((s_gpuvc.ib_head + 15u) & ~(VkDeviceSize)15u) : s_gpuvc.ib_head;
    if (vo + vbytes > s_gpuvc.vb.size || (ibytes && io + ibytes > s_gpuvc.ib.size)) {
        /* Arena wrapped (typically once per level transition): drain GPU before reset. */
        vk_eng_flush_sync_tag(VK_SYNC_OTHER);
        memset(s_gpuvc.tab, 0, sizeof s_gpuvc.tab);
        s_gpuvc.vb_head = s_gpuvc.ib_head = 0;
        vo = io = 0;
    }
    memcpy((u8*)s_gpuvc.vb.ptr + vo, verts, (size_t)vbytes);
    s_gpuvc.vb_head = vo + vbytes;
    if (ibytes) {
        memcpy((u8*)s_gpuvc.ib.ptr + io, idxs, (size_t)ibytes);
        s_gpuvc.ib_head = io + ibytes;
    }
    u32 victim = 0u;
    if (!s_gpuvc.tab[set][0].valid) victim = 0u;
    else if (!s_gpuvc.tab[set][1].valid) victim = 1u;
    else victim = (s_gpuvc.tab[set][0].lru <= s_gpuvc.tab[set][1].lru) ? 0u : 1u;
    vk_gpuvc_entry* e = &s_gpuvc.tab[set][victim];
    e->id = id;
    e->voff = vo;
    e->ioff = io;
    e->vbytes = vbytes;
    e->ibytes = ibytes;
    e->lru = ++s_gpuvc.lru;
    e->valid = 1;
    *out_vb = s_gpuvc.vb.buf;
    *out_voff = vo;
    if (ibytes) { *out_ib = s_gpuvc.ib.buf; *out_ioff = io; }
    return 2;
}

static void vk_eb_drain_defer(vk_eb_slot* sl)
{
    for (u32 i = 0; i < sl->defer_fb_n; i++) {
        if (sl->defer_fb[i]) pvkDestroyFramebuffer(s_vk.device, sl->defer_fb[i], NULL);
    }
    sl->defer_fb_n = 0;
    for (u32 i = 0; i < sl->defer_n; i++) {
        vk_image* im = &sl->defer[i];
        if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
        if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
        if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    }
    sl->defer_n = 0;
}

static void vk_eb_wait_slot(u32 idx)
{
    vk_eb_slot* sl = &s_eb.slot[idx];
    if (sl->busy) {
        const double t0 = vk_now_seconds();
        vk_wait_fence_logged(sl->fence, "eb_wait_slot");
        s_eb.p_wait_sec += vk_now_seconds() - t0;
        sl->busy = 0;
    }
    vk_eb_drain_defer(sl);
    sl->vb_head = sl->ib_head = sl->ub_head = sl->sb_head = 0;
    sl->has_last_vs = sl->has_last_ps = 0;
    sl->last_vs_copy_valid = 0;
    sl->last_ds_set = VK_NULL_HANDLE;
    if (sl->sets) {
        pvkResetDescriptorPool(s_vk.device, sl->pool, 0);
        sl->sets = 0;
        memset(sl->ds_cache, 0, sizeof sl->ds_cache);
        memset(sl->ds_lru, 0, sizeof sl->ds_lru);
    }
}

static int vk_eb_on(void)
{
    if (s_eb.on >= 0) return s_eb.on;
    const char* e = getenv("VK_DRAW_BATCH");
    s_eb.on = 0;
    if (!e || *e != '1' || !s_g.on) return 0;
    const u64 ram_mb = vk_system_ram_mb();
#ifdef __ANDROID__
    if (ram_mb >= 6000u) {
        s_eb.vb_cap   = 32u << 20;
        s_eb.ib_cap   = 10u << 20;
        s_eb.ub_cap   = 48u << 20; /* >6,200 draws/slot -> zero UBO ring overflows */
        s_eb.sb_cap   = 24u << 20;
        s_eb.max_sets = 6144u;
    } else if (ram_mb >= 4000u) {
        s_eb.vb_cap   = 12u << 20;
        s_eb.ib_cap   = 4u  << 20;
        s_eb.ub_cap   = 32u << 20;
        s_eb.sb_cap   = 16u << 20;
        s_eb.max_sets = 4096u;
    } else {
        s_eb.vb_cap   = 8u  << 20;
        s_eb.ib_cap   = 3u  << 20;
        s_eb.ub_cap   = 20u << 20;
        s_eb.sb_cap   = 12u << 20;
        s_eb.max_sets = 4096u;
    }
#else
    (void)ram_mb;
    s_eb.vb_cap   = 64u << 20;
    s_eb.ib_cap   = 16u << 20;
    s_eb.ub_cap   = 48u << 20;
    s_eb.sb_cap   = 64u << 20;
    s_eb.max_sets = 6144u;
#endif
    const char* m = getenv("VK_DRAW_BATCH_MAX");
    s_eb.max_draws = m && atoi(m) > 0 ? (u32)atoi(m) : 512u;
    if (s_eb.max_draws > s_eb.max_sets) s_eb.max_draws = s_eb.max_sets;
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_eb.ub_align = p.limits.minUniformBufferOffsetAlignment ? p.limits.minUniformBufferOffsetAlignment : 256;
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * s_eb.max_sets },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (RSX_MAX_TEXTURES + RSX_BE_MAX_VERTEX_TEXTURES) * s_eb.max_sets },
        { VK_DESCRIPTOR_TYPE_SAMPLER, (RSX_MAX_TEXTURES + RSX_BE_MAX_VERTEX_TEXTURES) * s_eb.max_sets },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = s_eb.max_sets, .poolSizeCount = 3, .pPoolSizes = ps };
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_vk.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    const VkDeviceSize ub_alloc = s_eb.ub_cap + vk_eb_ps_range();
    s_eb.saved_cmd = s_vk.cmd;
    for (u32 i = 0; i < VK_EB_SLOTS; i++) {
        vk_eb_slot* sl = &s_eb.slot[i];
        if (pvkAllocateCommandBuffers(s_vk.device, &cai, &sl->cmd) != VK_SUCCESS ||
            pvkCreateFence(s_vk.device, &fci, NULL, &sl->fence) != VK_SUCCESS ||
            vk_create_host_buffer(&sl->vb, s_eb.vb_cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) || !sl->vb.coherent ||
            vk_create_host_buffer(&sl->ib, s_eb.ib_cap, VK_BUFFER_USAGE_INDEX_BUFFER_BIT) || !sl->ib.coherent ||
            vk_create_host_buffer(&sl->ub, ub_alloc, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !sl->ub.coherent ||
            vk_create_host_buffer(&sl->sb, s_eb.sb_cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !sl->sb.coherent ||
            pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &sl->pool) != VK_SUCCESS) {
            VK_LOG("draw batching: no host-coherent ring memory, staying off\n");
            vk_eb_release();
            s_eb.on = 0;
            return 0;
        }
    }
    s_eb.cur = 0;
    s_vk.cmd = s_eb.slot[0].cmd;
    VK_LOG("draw batching: on (%u async slots, UB=%uMB VB=%uMB, up to %u draws/sub, early-submit + gpuvc)\n",
           (u32)VK_EB_SLOTS, (u32)(s_eb.ub_cap >> 20), (u32)(s_eb.vb_cap >> 20), s_eb.max_draws);
    s_eb.on = 1;
    return 1;
}

static VkDescriptorSet vk_eb_alloc_set(VkDescriptorSetLayout lay)
{
    if (s_eb.on != 1) return VK_NULL_HANDLE;
    for (int tries = 0; tries < 2; tries++) {
        vk_eb_slot* sl = &s_eb.slot[s_eb.cur];
        if (sl->sets < s_eb.max_sets) {
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDescriptorSetAllocateInfo dai = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = sl->pool, .descriptorSetCount = 1, .pSetLayouts = &lay };
            if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) == VK_SUCCESS) {
                sl->sets++;
                return set;
            }
        }
        if (tries || !s_eb_pending) break;
        vk_eng_flush_async(VK_FLUSH_RING);
    }
    return VK_NULL_HANDLE;
}

/* The rings and the pool live until the engine shuts down; a later vk_eb_on
 * decides again. Also frees what a failed vk_eb_on had already created. */
static void vk_eb_release(void)
{
    vk_eng_flush_sync();
    if (s_eb.saved_cmd) s_vk.cmd = s_eb.saved_cmd;
    s_eb.on = 0;
    for (u32 i = 0; i < VK_EB_SLOTS; i++) {
        vk_eb_slot* sl = &s_eb.slot[i];
        vk_eb_drain_defer(sl);
        vk_destroy_buffer(&sl->vb);
        vk_destroy_buffer(&sl->ib);
        vk_destroy_buffer(&sl->ub);
        vk_destroy_buffer(&sl->sb);
        if (sl->pool)  pvkDestroyDescriptorPool(s_vk.device, sl->pool, NULL);
        if (sl->fence) pvkDestroyFence(s_vk.device, sl->fence, NULL);
    }
    memset(&s_eb, 0, sizeof s_eb);
    s_eb.on = -1;
}

static void vk_eb_end_pass(void)
{
    if (!s_eb.in_pass) return;
    pvkCmdEndRenderPass(s_vk.cmd);
    s_eb.in_pass = 0;
    s_eb.open_rp_base = VK_NULL_HANDLE;
    s_eb.open_fb = VK_NULL_HANDLE;
    s_eb.open_pipe = VK_NULL_HANDLE;
    s_eb.open_set = VK_NULL_HANDLE;
    s_eb.open_rt_dirty = 0;
    s_eb.open_depth_dirty = 0;
    s_eb.dyn_valid = 0;
}

static void vk_eb_end_pass_reason(int reason)
{
    if (!s_eb.in_pass) return;
    if (reason >= 0 && reason < VK_PB_COUNT)
        s_eb.p_pb[reason]++;
    vk_eb_end_pass();
}

static int vk_eb_uses_color(u32 surface)
{
    if (!s_eb.in_pass || !surface || surface >= VK_ENG_MAX_OBJ) return 0;
    const VkImage img = s_eobj[surface].im.img;
    for (u32 i = 0; i < s_eb.open_nrt; i++) {
        const u32 rh = s_eb.open_rt[i];
        if (rh == surface || (img && rh && rh < VK_ENG_MAX_OBJ && s_eobj[rh].im.img == img))
            return 1;
    }
    return 0;
}

static int vk_eb_uses_depth(u32 depth)
{
    if (!s_eb.in_pass || !depth || depth >= VK_ENG_MAX_OBJ || !s_eb.open_depth) return 0;
    if (s_eb.open_depth == depth) return 1;
    const VkImage img = s_eobj[depth].im.img;
    return img && s_eb.open_depth < VK_ENG_MAX_OBJ && s_eobj[s_eb.open_depth].im.img == img;
}

static int vk_eb_draw_samples_open_rt(void)
{
    if (!s_eb.in_pass || (!s_eb.open_rt_dirty && !s_eb.open_depth_dirty) ||
        (!s_e2.tex_mask && !s_e2.vtex_mask))
        return 0;
    u32 tm = s_e2.tex_mask & ((1u << RSX_MAX_TEXTURES) - 1u);
    while (tm) {
        const u32 u = (u32)__builtin_ctz(tm);
        tm &= tm - 1u;
        u32 h = s_e2.tex[u];
        if (!h || h >= VK_ENG_MAX_OBJ) continue;
        if (s_eobj[h].kind == VK_ENG_VIEW) h = s_eobj[h].surface;
        if (!h || h >= VK_ENG_MAX_OBJ) continue;
        const VkImage img = s_eobj[h].im.img;
        if (s_eb.open_rt_dirty) {
            for (u32 i = 0; i < s_eb.open_nrt; i++) {
                if (!((s_eb.open_rt_dirty >> i) & 1u)) continue;
                const u32 rh = s_eb.open_rt[i];
                if (rh == h || (img && rh && rh < VK_ENG_MAX_OBJ && s_eobj[rh].im.img == img))
                    return 1;
            }
        }
        if (s_eb.open_depth_dirty && s_eb.open_depth &&
            (s_eb.open_depth == h ||
             (img && s_eb.open_depth < VK_ENG_MAX_OBJ && s_eobj[s_eb.open_depth].im.img == img)))
            return 1;
    }
    u32 vm = s_e2.vtex_mask & ((1u << RSX_BE_MAX_VERTEX_TEXTURES) - 1u);
    while (vm) {
        const u32 u = (u32)__builtin_ctz(vm);
        vm &= vm - 1u;
        u32 h = s_e2.vtex[u];
        if (!h || h >= VK_ENG_MAX_OBJ) continue;
        if (s_eobj[h].kind == VK_ENG_VIEW) h = s_eobj[h].surface;
        if (!h || h >= VK_ENG_MAX_OBJ) continue;
        const VkImage img = s_eobj[h].im.img;
        if (s_eb.open_rt_dirty) {
            for (u32 i = 0; i < s_eb.open_nrt; i++) {
                if (!((s_eb.open_rt_dirty >> i) & 1u)) continue;
                const u32 rh = s_eb.open_rt[i];
                if (rh == h || (img && rh && rh < VK_ENG_MAX_OBJ && s_eobj[rh].im.img == img))
                    return 1;
            }
        }
        if (s_eb.open_depth_dirty && s_eb.open_depth &&
            (s_eb.open_depth == h ||
             (img && s_eb.open_depth < VK_ENG_MAX_OBJ && s_eobj[s_eb.open_depth].im.img == img)))
            return 1;
    }
    return 0;
}

static int vk_eb_active(void) { return s_eb.on == 1; }
static u32 vk_eb_max(void) { return s_eb.max_draws; }
static void vk_eb_recorded(void) { if (++s_eb_pending >= s_eb.max_draws) vk_eng_flush_async(VK_FLUSH_MAX); }

/* ARMSX3-style early asynchronous submit: when >=1024 commands are recorded and
 * we are at a natural render-pass boundary (zero extra TBDR pass splits), if the
 * next triple-buffered slot is already idle on the GPU, kick this batch now so
 * Adreno starts rendering pass 1 while CPU records pass 2! */
static void vk_eb_try_early_submit(void)
{
    if (s_eb.on != 1 || s_eb_pending < 1024u) return;
    const u32 next_idx = (s_eb.cur + 1u) % VK_EB_SLOTS;
    const vk_eb_slot* next = &s_eb.slot[next_idx];
    if (!next->busy || (pvkGetFenceStatus && pvkGetFenceStatus(s_vk.device, next->fence) == VK_SUCCESS)) {
        vk_eng_flush_async(VK_FLUSH_EARLY);
    }
}

static int vk_eb_defer_image(vk_image* im)
{
    if (s_eb.on != 1) return 0;
    if (!im->img && !im->view && !im->mem) return 1;
    vk_eb_slot* sl = &s_eb.slot[s_eb.cur];
    if (sl->defer_n >= VK_EB_DEFER_MAX) {
        if (s_eb_pending) vk_eng_flush_async(VK_FLUSH_RING);
        sl = &s_eb.slot[s_eb.cur];
        if (sl->defer_n >= VK_EB_DEFER_MAX) vk_eng_flush_sync_tag(VK_SYNC_OTHER);
        sl = &s_eb.slot[s_eb.cur];
    }
    sl->defer[sl->defer_n++] = *im;
    memset(im, 0, sizeof *im);
    return 1;
}

static int vk_eb_defer_fb(VkFramebuffer* fb)
{
    if (s_eb.on != 1) return 0;
    if (!fb || !*fb) return 1;
    vk_eb_slot* sl = &s_eb.slot[s_eb.cur];
    if (sl->defer_fb_n >= VK_EB_DEFER_MAX) {
        if (s_eb_pending) vk_eng_flush_async(VK_FLUSH_RING);
        sl = &s_eb.slot[s_eb.cur];
        if (sl->defer_fb_n >= VK_EB_DEFER_MAX) vk_eng_flush_sync_tag(VK_SYNC_FB);
        sl = &s_eb.slot[s_eb.cur];
    }
    sl->defer_fb[sl->defer_fb_n++] = *fb;
    *fb = VK_NULL_HANDLE;
    return 1;
}

static VkDeviceSize vk_eb_take(VkDeviceSize* head, VkDeviceSize cap, VkDeviceSize bytes, VkDeviceSize align);

/* A texture upload as one more batched operation: rows staged in the ring, the
 * copy recorded behind a pass barrier (earlier draws of the batch may still
 * sample the old texels). Each used to allocate a host buffer and wait on its
 * own submission, ~3000 times in 20 s of God of War: Ascension's first level. */
static int vk_eb_upload(VkImage img, VkBufferImageCopy* region, const void* src, size_t tight,
                        u32 rows, u32 row_bytes)
{
    if (!vk_eb_active()) return -2;
    const VkDeviceSize bytes = (VkDeviceSize)tight * rows;
    if (!bytes || bytes > s_eb.sb_cap) return -2;
    vk_eb_slot* sl = &s_eb.slot[s_eb.cur];
    VkDeviceSize off = vk_eb_take(&sl->sb_head, s_eb.sb_cap, bytes, 16u);
    if (off == (VkDeviceSize)-1) {
        vk_eng_flush_async(VK_FLUSH_RING);
        sl = &s_eb.slot[s_eb.cur];
        off = vk_eb_take(&sl->sb_head, s_eb.sb_cap, bytes, 16u);
        if (off == (VkDeviceSize)-1) return -2;
    }
    if (tight == row_bytes) {
        memcpy((u8*)sl->sb.ptr + off, src, (size_t)bytes);
    } else {
        for (u32 y = 0; y < rows; y++)
            memcpy((u8*)sl->sb.ptr + off + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
    }
    if (!s_eb_pending && vk_begin()) return -1;
    vk_eb_end_pass_reason(VK_PB_UPL);
    vk_cmd_pass_barrier(s_vk.cmd);
    region->bufferOffset = off;
    pvkCmdCopyBufferToImage(s_vk.cmd, sl->sb.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, region);
    vk_eb_recorded();
    return 0;
}

static void vk_eng_flush_ex(int sync_all, int reason, int sync_tag)
{
    if (s_eb.on != 1) {
        s_eb_pending = 0;
        return;
    }
    if (sync_all && sync_tag >= 0 && sync_tag < VK_SYNC_TAG_COUNT)
        s_eb.p_sync_tag[sync_tag]++;
    if (s_eb_pending) {
        vk_eb_end_pass();
        s_eb_pending = 0;               /* first: the submit below must not re-enter */
        vk_eb_slot* sl = &s_eb.slot[s_eb.cur];
        if (pvkEndCommandBuffer(sl->cmd) == VK_SUCCESS) {
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .commandBufferCount = 1, .pCommandBuffers = &sl->cmd };
            pvkResetFences(s_vk.device, 1, &sl->fence);
            if (vk_queue_submit(&si, sl->fence) == VK_SUCCESS)
                sl->busy = 1;
        }
        s_eb.flushes++;
        s_eb.p_flushes++;
        if (reason >= 0 && reason < VK_FLUSH_REASON_COUNT) s_eb.p_reason[reason]++;
        if (sync_all) {
            for (u32 step = 1u; step <= VK_EB_SLOTS; step++)
                vk_eb_wait_slot((s_eb.cur + step) % VK_EB_SLOTS);
        } else {
            s_eb.cur = (s_eb.cur + 1u) % VK_EB_SLOTS;
            vk_eb_wait_slot(s_eb.cur);
        }
        s_vk.cmd = s_eb.slot[s_eb.cur].cmd;
        if ((s_eb.flushes & 0x3FFFu) == 0)
            VK_LOG("draw batching: %llu draws in %llu passes / %llu submissions\n",
                   s_eb.draws, s_eb.passes, s_eb.flushes);
        return;
    }
    if (sync_all) {
        for (u32 i = 0; i < VK_EB_SLOTS; i++)
            vk_eb_wait_slot(i);
        s_vk.cmd = s_eb.slot[s_eb.cur].cmd;
    }
}

static void vk_eng_flush(void) { vk_eng_flush_ex(1, VK_FLUSH_SYNC, VK_SYNC_OTHER); }
static void vk_eng_flush_sync(void) { vk_eng_flush_ex(1, VK_FLUSH_SYNC, VK_SYNC_OTHER); }
static void vk_eng_flush_sync_tag(int tag) { vk_eng_flush_ex(1, VK_FLUSH_SYNC, tag); }
static void vk_eng_flush_async(int reason) { vk_eng_flush_ex(0, reason, VK_SYNC_OTHER); }

static void vk_eb_perf_present(void)
{
    if (s_eb.on != 1) return;
    if (++s_eb.p_frames < 120u) return;
    const double inv = 1.0 / (double)s_eb.p_frames;
    const u32 vs_pct = s_eb.p_draws ? (s_eb.p_vs_dedup * 100u) / s_eb.p_draws : 0u;
    const u32 ps_pct = s_eb.p_draws ? (s_eb.p_ps_dedup * 100u) / s_eb.p_draws : 0u;
    const u32 ds_pct = s_eb.p_draws ? (s_eb.p_ds_hit   * 100u) / s_eb.p_draws : 0u;
    const u32 gv_pct = s_eb.p_draws ? (s_eb.p_gpuvc_hit * 100u) / s_eb.p_draws : 0u;
    const u32 fg_now = __atomic_load_n(&s_fg.gen_frames, __ATOMIC_RELAXED);
    const u32 fg_delta = fg_now - s_eb.p_fg_last;
    s_eb.p_fg_last = fg_now;
    const u32 acomp_now = vk_apipe_completed();
    const u32 acomp_delta = acomp_now - s_eb.p_acomp_last;
    s_eb.p_acomp_last = acomp_now;
    VK_LOG("[vk-perf] %uf avg: draws=%.0f/f passes=%.1f/f pb(%u/%u/%u/%u/%u/%u/%u) flushes=%.2f/f "
           "(max=%u ring=%u early=%u pres=%u sync=%u[%u/%u/%u/%u/%u]) wait=%.1fms/f "
           "dedup(vs=%u%% ps=%u%% ds=%u%% gpuvc=%u%%) clrf=%.1f/f dac=%u fg=%u acomp=%u askip=%u\n",
           s_eb.p_frames, s_eb.p_draws * inv, s_eb.p_passes * inv,
           s_eb.p_pb[VK_PB_FB], s_eb.p_pb[VK_PB_CLR], s_eb.p_pb[VK_PB_SELF],
           s_eb.p_pb[VK_PB_CSNAP], s_eb.p_pb[VK_PB_DSNAP], s_eb.p_pb[VK_PB_UPL],
           s_eb.p_pb[VK_PB_BLIT], s_eb.p_flushes * inv,
           s_eb.p_reason[VK_FLUSH_MAX], s_eb.p_reason[VK_FLUSH_RING],
           s_eb.p_reason[VK_FLUSH_EARLY], s_eb.p_reason[VK_FLUSH_PRESENT],
           s_eb.p_reason[VK_FLUSH_SYNC],
           s_eb.p_sync_tag[VK_SYNC_SUBMIT], s_eb.p_sync_tag[VK_SYNC_READBACK],
           s_eb.p_sync_tag[VK_SYNC_FB], s_eb.p_sync_tag[VK_SYNC_SNAP],
           s_eb.p_sync_tag[VK_SYNC_OTHER],
           s_eb.p_wait_sec * 1000.0 * inv, vs_pct, ps_pct, ds_pct, gv_pct,
           s_eb.p_clear_fold * inv, s_dac.calls, fg_delta, acomp_delta, s_eb.p_async_skip);
    s_eb.p_frames = s_eb.p_draws = s_eb.p_passes = s_eb.p_flushes = 0;
    s_eb.p_vs_dedup = s_eb.p_ps_dedup = s_eb.p_ds_hit = 0;
    s_eb.p_gpuvc_hit = s_eb.p_clear_fold = s_eb.p_async_skip = 0;
    s_eb.p_wait_sec = 0.0;
    s_dac.calls = 0;
    memset(s_eb.p_reason, 0, sizeof s_eb.p_reason);
    memset(s_eb.p_sync_tag, 0, sizeof s_eb.p_sync_tag);
    memset(s_eb.p_pb, 0, sizeof s_eb.p_pb);
}

static VkDeviceSize vk_eb_take(VkDeviceSize* head, VkDeviceSize cap, VkDeviceSize bytes, VkDeviceSize align)
{
    const VkDeviceSize at = (*head + align - 1u) / align * align;
    if (at + bytes > cap) return (VkDeviceSize)-1;
    *head = at + bytes;
    return at;
}

static int vk_eng_resolve_tex_key(const vk_eng_pipe* P,
                                  VkImageView img[RSX_MAX_TEXTURES],
                                  VkSampler   smp[RSX_MAX_TEXTURES],
                                  VkImageView vimg[RSX_BE_MAX_VERTEX_TEXTURES],
                                  VkSampler   vsmp[RSX_BE_MAX_VERTEX_TEXTURES],
                                  u64* out_hash)
{
    const int split = P->cube_mask != 0;
    const VkImageView dummy_cube = split ? vk_eng_dummy_cube_view() : VK_NULL_HANDLE;
    if (split && !dummy_cube) return -1;
    const VkImageView dummy_2d = vk_eng_dummy_2d_view();
    if (!dummy_2d) return -1;

    if (!split) {
        static VkImageView s_img_tpl[RSX_MAX_TEXTURES];
        static VkSampler   s_smp_tpl[RSX_MAX_TEXTURES];
        static VkImageView s_tpl_v = VK_NULL_HANDLE;
        static VkSampler   s_tpl_s = VK_NULL_HANDLE;
        if (s_tpl_v != dummy_2d || s_tpl_s != s_vk.sampler) {
            for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
                s_img_tpl[u] = dummy_2d;
                s_smp_tpl[u] = s_vk.sampler;
            }
            s_tpl_v = dummy_2d;
            s_tpl_s = s_vk.sampler;
        }
        memcpy(img, s_img_tpl, sizeof(VkImageView) * RSX_MAX_TEXTURES);
        memcpy(smp, s_smp_tpl, sizeof(VkSampler)   * RSX_MAX_TEXTURES);
        u32 tm = s_e2.tex_mask & ((1u << RSX_MAX_TEXTURES) - 1u);
        while (tm) {
            const u32 u = (u32)__builtin_ctz(tm);
            tm &= tm - 1u;
            VkImageView v = vk_eng_sample_view(s_e2.tex[u], 0);
            if (v) {
                img[u] = v;
                smp[u] = vk_eng_sampler(&s_e2.samp[u]);
            }
        }
    } else {
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
            const int cube = (P->cube_mask >> u) & 1u;
            VkImageView v = ((s_e2.tex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.tex[u], cube)
                                                         : VK_NULL_HANDLE;
            img[u] = v ? v : (cube ? dummy_cube : dummy_2d);
            smp[u] = v ? vk_eng_sampler(&s_e2.samp[u]) : s_vk.sampler;
        }
    }
    u64 h = 0xcbf29ce484222325ull ^ ((u64)split << 32) ^ (u64)P->vtex_mask;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        h ^= (u64)(uintptr_t)img[u] * 0x9e3779b97f4a7c15ull;
        h = (h ^ (h >> 27)) * 0x100000001b3ull;
        h ^= (u64)(uintptr_t)smp[u];
        h *= 0x100000001b3ull;
    }
    if (P->vtex_mask) {
        for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
            if ((P->vtex_mask >> u) & 1u) {
                VkImageView vv = ((s_e2.vtex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.vtex[u], 0)
                                                              : VK_NULL_HANDLE;
                vimg[u] = vv ? vv : dummy_2d;
                vsmp[u] = vv ? vk_eng_sampler(&s_e2.vsamp[u]) : s_vk.sampler;
                h ^= (u64)(uintptr_t)vimg[u] * 0x9e3779b97f4a7c15ull;
                h *= 0x100000001b3ull;
                h ^= (u64)(uintptr_t)vsmp[u];
                h *= 0x100000001b3ull;
            } else {
                vimg[u] = VK_NULL_HANDLE;
                vsmp[u] = VK_NULL_HANDLE;
            }
        }
    }
    *out_hash = h;
    return 0;
}

static VkDescriptorSet vk_eng_write_descriptors_resolved(const vk_eng_pipe* P, VkDescriptorSet set,
                                                         VkBuffer ubo,
                                                         const VkImageView img[RSX_MAX_TEXTURES],
                                                         const VkSampler   smp[RSX_MAX_TEXTURES],
                                                         const VkImageView vimg[RSX_BE_MAX_VERTEX_TEXTURES],
                                                         const VkSampler   vsmp[RSX_BE_MAX_VERTEX_TEXTURES])
{
    const int split = P->cube_mask != 0;
    if (!set) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo bi[2] = {
        { ubo, 0, VK_VPCONST_BYTES },
        { ubo, 0, vk_eb_ps_range() },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, img[u], VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ smp[u], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkDescriptorImageInfo vii[RSX_BE_MAX_VERTEX_TEXTURES], vsi[RSX_BE_MAX_VERTEX_TEXTURES];
    VkWriteDescriptorSet w[3 + RSX_MAX_TEXTURES + 2 * RSX_BE_MAX_VERTEX_TEXTURES];
    u32 n = 0;
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[0] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[1] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si };
    if (split) {
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
            w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  .dstSet = set, .dstBinding = RSX_SPIRV_TEXTURE_BINDING + u, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[u] };
    } else {
        w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
              .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii };
    }
    if (P->vtex_mask) {
        for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
            if (!((P->vtex_mask >> u) & 1u)) continue;
            vii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, vimg[u], VK_IMAGE_LAYOUT_GENERAL };
            vsi[u] = (VkDescriptorImageInfo){ vsmp[u], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  .dstSet = set, .dstBinding = RSX_SPIRV_VTEX_BINDING + u, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &vii[u] };
            w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  .dstSet = set, .dstBinding = RSX_SPIRV_VSAMPLER_BINDING + u, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &vsi[u] };
        }
    }
    pvkUpdateDescriptorSets(s_vk.device, n, w, 0, NULL);
    return set;
}

static void vk_eng_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                        u32 stride, const u32* indices, u32 index_count)
{
    (void)user;
    const u64 vc_id = s_eng_next_vc_id;
    s_eng_next_vc_id = 0;
    if (!s_e2.pipeline || !vertices || !vertex_count || !stride) return;
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) return;
    vk_eng_pipe* P = &s_epipe[s_e2.pipeline - 1];
    if (!P->used || stride != P->stride) return;
    if (s_e2.draws_seen < 0xFFFFFFFFu) s_e2.draws_seen++;
    {
        const u32 astate = __atomic_load_n(&P->async_state, __ATOMIC_ACQUIRE);
        if (astate != 0u) {
            if (astate == 1u) s_eb.p_async_skip++;
            return;
        }
    }
    VkPipeline pipe = vk_eng_variant(P, (u32)topology - 1u);
    if (!pipe) {
        if (__atomic_load_n(&P->variant_queued[(u32)topology - 1u], __ATOMIC_ACQUIRE))
            s_eb.p_async_skip++;
        return;
    }

    /* Targets: every member of a set is target A's size (engine contract). */
    const u32 n = s_eng_bound.nrt;
    if (n < 1 || n != P->rt_count) {
        if (!(s_e2.warned & 16u)) { VK_LOG("engine: draw with %u colour targets for a %u-target pipeline skipped\n", n, P->rt_count); s_e2.warned |= 16u; }
        return;
    }
    vk_eng_obj* rt0 = vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR);
    if (!rt0) return;
    for (u32 i = 1; i < n; i++)
        if (!vk_eng_get(s_eng_bound.rt[i], VK_ENG_COLOR)) return;
    if (rt0->fmt != P->rt_fmt) {
        if (!(s_e2.warned & 32u)) { VK_LOG("engine: draw into colour format %d with a pipeline built for %d skipped\n", (int)rt0->fmt, (int)P->rt_fmt); s_e2.warned |= 32u; }
        return;
    }
    const u32 w = rt0->w, h = rt0->h;
    vk_eng_obj* zo = vk_eng_get(s_eng_bound.depth, VK_ENG_DEPTH);
    VkImageView dview = (zo && zo->w >= w && zo->h >= h) ? zo->im.view : vk_eng_scratch_depth(w, h);
    if (!dview) return;
    const u32 dkey = (zo && dview == zo->im.view) ? s_eng_bound.depth : 0xFFFFFFFFu;

    /* Flush any deferred clear on a target that this draw is about to SAMPLE. */
    if (s_e2.tex_mask) {
        u32 tm = s_e2.tex_mask & ((1u << RSX_MAX_TEXTURES) - 1u);
        while (tm) {
            const u32 u = (u32)__builtin_ctz(tm);
            tm &= tm - 1u;
            u32 th = s_e2.tex[u];
            if (!th || th >= VK_ENG_MAX_OBJ) continue;
            if (s_eobj[th].kind == VK_ENG_VIEW) th = s_eobj[th].surface;
            if (th && th < VK_ENG_MAX_OBJ &&
                (s_eobj[th].pending_clear_color || s_eobj[th].pending_clear_depth))
                vk_eng_flush_pending_clear(&s_eobj[th]);
        }
    }
    if (s_e2.vtex_mask) {
        u32 vm = s_e2.vtex_mask & ((1u << RSX_BE_MAX_VERTEX_TEXTURES) - 1u);
        while (vm) {
            const u32 u = (u32)__builtin_ctz(vm);
            vm &= vm - 1u;
            u32 th = s_e2.vtex[u];
            if (!th || th >= VK_ENG_MAX_OBJ) continue;
            if (s_eobj[th].kind == VK_ENG_VIEW) th = s_eobj[th].surface;
            if (th && th < VK_ENG_MAX_OBJ &&
                (s_eobj[th].pending_clear_color || s_eobj[th].pending_clear_depth))
                vk_eng_flush_pending_clear(&s_eobj[th]);
        }
    }

    /* Collect deferred clears on bound attachments to fold into LOAD_OP_CLEAR. */
    u32 clear_mask = 0;
    VkClearValue clear_vals[RSX_BE_MAX_COLOR_TARGETS + 1];
    memset(clear_vals, 0, sizeof clear_vals);
    for (u32 i = 0; i < n; i++) {
        vk_eng_obj* co = &s_eobj[s_eng_bound.rt[i]];
        if (co->pending_clear_color) {
            clear_mask |= (1u << i);
            memcpy(clear_vals[i].color.float32, co->pending_rgba, sizeof co->pending_rgba);
        }
    }
    if (zo && dview == zo->im.view && zo->pending_clear_depth) {
        if (zo->pending_clear_depth & 1u) clear_mask |= 16u;
        if (zo->pending_clear_depth & 2u) clear_mask |= 32u;
        clear_vals[n].depthStencil.depth   = zo->pending_depth_val;
        clear_vals[n].depthStencil.stencil = zo->pending_stencil_val;
    }

    VkRenderPass rp_base = vk_eng_render_pass(P->rt_fmt, n);
    VkRenderPass rp_use  = clear_mask ? vk_eng_render_pass_ex(P->rt_fmt, n, clear_mask) : rp_base;
    VkFramebuffer fb = vk_eng_framebuffer(rp_base, s_eng_bound.rt, n, dview, dkey, w, h);
    if (!rp_base || !rp_use || !fb) return;

    const VkDeviceSize vbytes = (VkDeviceSize)vertex_count * stride;
    const VkDeviceSize ibytes = (indices && index_count) ? (VkDeviceSize)index_count * 4u : 0;
    VkBuffer vbuf = s_vk.vertices.buf, ibuf = s_e2.indices.buf;
    VkDeviceSize voff = 0, ioff = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
    u32 dyn_off[2] = { 0u, (u32)s_g.ps_off };
    const u32 vsb = s_e2.vs_bytes ? s_e2.vs_bytes : 16u, psb = s_e2.ps_bytes ? s_e2.ps_bytes : 16u;
    const int eb_on = vk_eb_on();
    int gpuvc_res = 0;
    VkBuffer gv_vb = VK_NULL_HANDLE, gv_ib = VK_NULL_HANDLE;
    VkDeviceSize gv_voff = 0, gv_ioff = 0;
    if (eb_on && vc_id) {
        gpuvc_res = vk_gpuvc_get_or_upload(vc_id, vertices, vbytes,
                                           indices, ibytes,
                                           &gv_vb, &gv_voff, &gv_ib, &gv_ioff);
    }
    const int use_gpuvc = (gpuvc_res != 0);
    int batched = eb_on && (use_gpuvc || (vbytes <= s_eb.vb_cap && ibytes <= s_eb.ib_cap));
    const u8 is_split = P->cube_mask != 0 ? 1u : 0u;

    /* ARMSX3-style early async submit at natural pass boundaries (before
     * allocating slices in the active slot) so Adreno executes pass 1 while
     * CPU records pass 2 with zero extra TBDR pass splits. */
    if (batched && s_eb_pending >= 1024u) {
        const int same_fb = s_eb.in_pass &&
                            s_eb.open_rp_base == rp_base && s_eb.open_fb == fb &&
                            s_eb.open_w == w && s_eb.open_h == h;
        const int self_read = (!clear_mask && same_fb) ? vk_eb_draw_samples_open_rt() : 0;
        const int would_same_pass = !clear_mask && same_fb && !self_read;
        if (!would_same_pass) {
            vk_eb_end_pass_reason(clear_mask ? VK_PB_CLR : (self_read ? VK_PB_SELF : VK_PB_FB));
            vk_eb_try_early_submit();
        }
    }

    /* Fast-path: if bound textures/samplers and pipeline texture layout have
     * not changed since the last draw in this batch slot, reuse the exact
     * VkDescriptorSet without resolving views/samplers or hashing 16 slots. */
    VkImageView d_img[RSX_MAX_TEXTURES];
    VkSampler   d_smp[RSX_MAX_TEXTURES];
    VkImageView d_vimg[RSX_BE_MAX_VERTEX_TEXTURES];
    VkSampler   d_vsmp[RSX_BE_MAX_VERTEX_TEXTURES];
    u64 d_hash = 0;
    u32 ds_bucket = 0;
    int tex_resolved = 0;

    vk_eb_slot* sl = NULL;
    if (batched) {
        const u8* vsrc = (const u8*)(s_e2.vs_src ? s_e2.vs_src : (const void*)vk_eng_ucpu());
        const u32 tail_off = RSX_MAX_VERTEX_CONSTANTS * 16u;
        /* Slices of the active slot's rings and a cached or freshly allocated
         * descriptor set; dynamic UBO offsets allow reusing descriptor sets
         * across draws with different VS/PS constants! */
        for (int tries = 0; tries < 2; tries++) {
            sl = &s_eb.slot[s_eb.cur];
            const int fast_ds = sl->last_ds_set &&
                                sl->last_tex_gen == s_e2.tex_gen &&
                                sl->last_cube_mask == P->cube_mask &&
                                sl->last_vtex_mask == P->vtex_mask;
            if (!fast_ds && !tex_resolved) {
                if (vk_eng_resolve_tex_key(P, d_img, d_smp, d_vimg, d_vsmp, &d_hash)) return;
                ds_bucket = (u32)(d_hash ^ (d_hash >> 32)) & (VK_EB_DS_BUCKETS - 1u);
                tex_resolved = 1;
            }
            VkDeviceSize vh = sl->vb_head, ih = sl->ib_head, uh = sl->ub_head;
            voff = use_gpuvc ? gv_voff : vk_eb_take(&vh, s_eb.vb_cap, vbytes, 16u);
            ioff = use_gpuvc ? gv_ioff : (ibytes ? vk_eb_take(&ih, s_eb.ib_cap, ibytes, 16u) : 0);
            int reuse_vs = sl->has_last_vs && sl->last_vs_gen == s_e2.vs_gen &&
                           P->vp_const_lo >= sl->last_vs_lo &&
                           P->vp_const_hi <= sl->last_vs_hi;
            if (!reuse_vs && sl->has_last_vs && sl->last_vs_copy_valid && vsrc &&
                vsb >= tail_off + 32u && vsb <= VK_VPCONST_BYTES &&
                P->vp_const_lo >= sl->last_vs_lo && P->vp_const_hi <= sl->last_vs_hi &&
                P->vp_const_hi <= RSX_MAX_VERTEX_CONSTANTS) {
                const u32 off_b = (u32)P->vp_const_lo * 16u;
                const u32 len_b = (P->vp_const_hi > P->vp_const_lo)
                                    ? (u32)(P->vp_const_hi - P->vp_const_lo) * 16u : 0u;
                if (len_b <= 2048u &&
                    (!len_b || memcmp(vsrc + off_b, s_eb.last_vs_copy + off_b, len_b) == 0) &&
                    memcmp(vsrc + tail_off, s_eb.last_vs_copy + tail_off, vsb - tail_off) == 0) {
                    reuse_vs = 1;
                }
            }
            const int reuse_ps = sl->has_last_ps && sl->last_ps_gen == s_e2.ps_gen;
            const VkDeviceSize uvs = reuse_vs ? sl->last_vs_off
                                              : vk_eb_take(&uh, s_eb.ub_cap, vsb, s_eb.ub_align);
            const VkDeviceSize ups = (uvs == (VkDeviceSize)-1)
                                   ? uvs
                                   : (reuse_ps ? sl->last_ps_off
                                               : vk_eb_take(&uh, s_eb.ub_cap, psb, s_eb.ub_align));
            if (voff != (VkDeviceSize)-1 && ioff != (VkDeviceSize)-1 && ups != (VkDeviceSize)-1) {
                if (fast_ds) {
                    set = sl->last_ds_set;
                    s_eb.p_ds_hit++;
                } else {
                    /* 2-way descriptor set cache lookup (independent of UBO offsets). */
                    int ds_hit = 0;
                    for (u32 wway = 0; wway < 2u; wway++) {
                        vk_eb_ds_entry* e = &sl->ds_cache[ds_bucket][wway];
                        if (e->valid && e->hash == d_hash && e->split == is_split &&
                            e->vtex_mask == P->vtex_mask &&
                            memcmp(e->img, d_img, sizeof d_img) == 0 &&
                            memcmp(e->smp, d_smp, sizeof d_smp) == 0 &&
                            (!P->vtex_mask ||
                             (memcmp(e->vimg, d_vimg, sizeof d_vimg) == 0 &&
                              memcmp(e->vsmp, d_vsmp, sizeof d_vsmp) == 0))) {
                            set = e->set;
                            sl->ds_lru[ds_bucket] = (u8)(wway ^ 1u);
                            ds_hit = 1;
                            s_eb.p_ds_hit++;
                            break;
                        }
                    }
                    if (!ds_hit && sl->sets < s_eb.max_sets) {
                        VkDescriptorSetLayout lay = is_split ? s_e2.split_layout : s_g.set_layout;
                        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                            .descriptorPool = sl->pool, .descriptorSetCount = 1,
                                                            .pSetLayouts = &lay };
                        if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) == VK_SUCCESS) {
                            sl->sets++;
                            if (!vk_eng_write_descriptors_resolved(P, set, sl->ub.buf,
                                                                   d_img, d_smp, d_vimg, d_vsmp))
                                return;
                            const u32 victim = sl->ds_lru[ds_bucket] & 1u;
                            sl->ds_lru[ds_bucket] = (u8)(victim ^ 1u);
                            vk_eb_ds_entry* e = &sl->ds_cache[ds_bucket][victim];
                            e->hash = d_hash;
                            e->set = set;
                            e->vtex_mask = P->vtex_mask;
                            e->split = is_split;
                            e->valid = 1;
                            memcpy(e->img, d_img, sizeof d_img);
                            memcpy(e->smp, d_smp, sizeof d_smp);
                            if (P->vtex_mask) {
                                memcpy(e->vimg, d_vimg, sizeof d_vimg);
                                memcpy(e->vsmp, d_vsmp, sizeof d_vsmp);
                            }
                        } else {
                            set = VK_NULL_HANDLE;
                        }
                    }
                }
                if (set) {
                    sl->vb_head = vh; sl->ib_head = ih; sl->ub_head = uh;
                    sl->last_ds_set    = set;
                    sl->last_tex_gen   = s_e2.tex_gen;
                    sl->last_cube_mask = P->cube_mask;
                    sl->last_vtex_mask = P->vtex_mask;
                    if (!reuse_vs) {
                        u8* vdst = (u8*)sl->ub.ptr + uvs;
                        if (vsb >= tail_off + 32u && P->vp_const_hi <= RSX_MAX_VERTEX_CONSTANTS) {
                            const u32 off_b = (u32)P->vp_const_lo * 16u;
                            const u32 len_b = (P->vp_const_hi > P->vp_const_lo)
                                                ? (u32)(P->vp_const_hi - P->vp_const_lo) * 16u : 0u;
                            const int save_copy = (len_b <= 2048u && vsb <= VK_VPCONST_BYTES);
                            if (len_b) {
                                memcpy(vdst + off_b, vsrc + off_b, len_b);
                                if (save_copy && off_b + len_b <= VK_VPCONST_BYTES)
                                    memcpy(s_eb.last_vs_copy + off_b, vsrc + off_b, len_b);
                            }
                            /* Always copy vp_posscale + vp_posoffset tail (32 bytes). */
                            memcpy(vdst + tail_off, vsrc + tail_off, vsb - tail_off);
                            if (save_copy)
                                memcpy(s_eb.last_vs_copy + tail_off, vsrc + tail_off, vsb - tail_off);
                            sl->last_vs_lo = P->vp_const_lo;
                            sl->last_vs_hi = P->vp_const_hi;
                            sl->last_vs_copy_valid = save_copy;
                        } else {
                            memcpy(vdst, vsrc, vsb);
                            sl->last_vs_lo = 0;
                            sl->last_vs_hi = RSX_MAX_VERTEX_CONSTANTS;
                            sl->last_vs_copy_valid = 0;
                        }
                        sl->has_last_vs = 1; sl->last_vs_gen = s_e2.vs_gen; sl->last_vs_off = uvs;
                    } else {
                        s_eb.p_vs_dedup++;
                    }
                    if (!reuse_ps) {
                        memcpy((u8*)sl->ub.ptr + ups, vk_eng_ucpu() + s_g.ps_off, psb);
                        sl->has_last_ps = 1; sl->last_ps_gen = s_e2.ps_gen; sl->last_ps_off = ups;
                    } else {
                        s_eb.p_ps_dedup++;
                    }
                    dyn_off[0] = (u32)uvs;
                    dyn_off[1] = (u32)ups;
                    break;
                }
            }
            if (tries || !s_eb_pending) { batched = 0; break; }
            vk_eng_flush_async(VK_FLUSH_RING);
        }
        if (batched && !set) batched = 0;
    }
    if (!batched && !tex_resolved) {
        if (vk_eng_resolve_tex_key(P, d_img, d_smp, d_vimg, d_vsmp, &d_hash)) return;
    }
    if (batched) {
        if (use_gpuvc) {
            vbuf = gv_vb; ibuf = gv_ib;
            if (gpuvc_res == 1) s_eb.p_gpuvc_hit++;
        } else {
            memcpy((u8*)sl->vb.ptr + voff, vertices, (size_t)vbytes);
            if (ibytes) memcpy((u8*)sl->ib.ptr + ioff, indices, (size_t)ibytes);
            vbuf = sl->vb.buf; ibuf = sl->ib.buf;
        }
        if (!s_eb_pending && vk_begin()) return;
        const int same_fb = s_eb.in_pass &&
                            s_eb.open_rp_base == rp_base && s_eb.open_fb == fb &&
                            s_eb.open_w == w && s_eb.open_h == h;
        const int self_read = (!clear_mask && same_fb) ? vk_eb_draw_samples_open_rt() : 0;
        const int same_pass = !clear_mask && same_fb && !self_read;
        if (!same_pass) {
            vk_eb_end_pass_reason(clear_mask ? VK_PB_CLR : (self_read ? VK_PB_SELF : VK_PB_FB));
            /* Tighter graphics+transfer pass barrier (no ALL_COMMANDS stall or
             * redundant per-attachment GENERAL->GENERAL image barriers). */
            vk_cmd_pass_barrier(s_vk.cmd);
            VkRenderPassBeginInfo rbi = {
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass = rp_use, .framebuffer = fb,
                .renderArea = { { 0, 0 }, { w, h } },
                .clearValueCount = clear_mask ? (n + 1u) : 0u,
                .pClearValues = clear_mask ? clear_vals : NULL,
            };
            pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            if (clear_mask) {
                for (u32 i = 0; i < n; i++)
                    if ((clear_mask >> i) & 1u)
                        s_eobj[s_eng_bound.rt[i]].pending_clear_color = 0;
                if ((clear_mask & 48u) && zo && dview == zo->im.view)
                    zo->pending_clear_depth &= (u8)~(((clear_mask & 16u) ? 1u : 0u) |
                                                     ((clear_mask & 32u) ? 2u : 0u));
                s_eb.p_clear_fold++;
            }
            s_eb.in_pass = 1;
            s_eb.open_rp_base = rp_base;
            s_eb.open_fb = fb;
            s_eb.open_w = w;
            s_eb.open_h = h;
            s_eb.open_nrt = n;
            for (u32 i = 0; i < n; i++) s_eb.open_rt[i] = s_eng_bound.rt[i];
            s_eb.open_depth = (dkey != 0xFFFFFFFFu) ? dkey : 0u;
            s_eb.open_rt_dirty = 0;
            s_eb.open_depth_dirty = 0;
            s_eb.open_pipe = VK_NULL_HANDLE;
            s_eb.open_set = VK_NULL_HANDLE;
            s_eb.dyn_valid = 0;
            s_eb.passes++;
            s_eb.p_passes++;
        }
    } else {
        vk_eng_flush_sync();
        if (vk_ensure_vertex_buffer(vbytes)) return;
        memcpy(s_vk.vertices.ptr, vertices, (size_t)vbytes);
        if (ibytes) {
            if (vk_eng_ensure_indices(ibytes)) return;
            memcpy(s_e2.indices.ptr, indices, (size_t)ibytes);
        }
        vbuf = s_vk.vertices.buf; ibuf = s_e2.indices.buf; voff = ioff = 0;
        if (!vk_eng_ucpu()) return;
        const void* vsrc = s_e2.vs_src ? s_e2.vs_src : (const void*)s_e2.ucpu;
        memcpy(s_g.ubo.ptr, vsrc, vsb);
        memcpy((u8*)s_g.ubo.ptr + s_g.ps_off, s_e2.ucpu + s_g.ps_off, psb);
        set = vk_eng_write_descriptors_resolved(P, is_split ? s_e2.split_set : s_g.set,
                                                s_g.ubo.buf, d_img, d_smp, d_vimg, d_vsmp);
        if (!set) return;
        dyn_off[0] = 0u;
        dyn_off[1] = (u32)s_g.ps_off;
        if (vk_begin()) return;
        /* Everything submitted before is complete (each submission is waited
         * on); this orders memory for the surfaces this draw touches. */
        for (u32 i = 0; i < n; i++)
            vk_barrier_image(s_eobj[s_eng_bound.rt[i]].im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkRenderPassBeginInfo rbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = rp_use, .framebuffer = fb,
            .renderArea = { { 0, 0 }, { w, h } },
            .clearValueCount = clear_mask ? (n + 1u) : 0u,
            .pClearValues = clear_mask ? clear_vals : NULL,
        };
        pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        if (clear_mask) {
            for (u32 i = 0; i < n; i++)
                if ((clear_mask >> i) & 1u)
                    s_eobj[s_eng_bound.rt[i]].pending_clear_color = 0;
            if ((clear_mask & 48u) && zo && dview == zo->im.view)
                zo->pending_clear_depth &= (u8)~(((clear_mask & 16u) ? 1u : 0u) |
                                                 ((clear_mask & 32u) ? 2u : 0u));
        }
    }
    if (!batched || s_eb.open_pipe != pipe) {
        pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        if (batched) s_eb.open_pipe = pipe;
    }
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e2.have_vp && s_e2.vp[2] > 0.0f && s_e2.vp[3] > 0.0f) {
        vp.x = s_e2.vp[0]; vp.y = s_e2.vp[1]; vp.width = s_e2.vp[2]; vp.height = s_e2.vp[3];
    }
    VkRect2D sc = { { 0, 0 }, { w, h } };
    if (s_e2.have_sc) {                       /* clamped: Vulkan rejects negative or over-size */
        u32 x0 = s_e2.sc[0] < w ? s_e2.sc[0] : w, y0 = s_e2.sc[1] < h ? s_e2.sc[1] : h;
        u32 x1 = s_e2.sc[0] + s_e2.sc[2], y1 = s_e2.sc[1] + s_e2.sc[3];
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0; sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    vk_coop_observe_draw(w, h, &vp, &sc);
    const u32 sref = s_e2.stencil_ref & 0xFFu;
    if (!batched || !s_eb.dyn_valid || memcmp(&s_eb.open_vp, &vp, sizeof vp) != 0) {
        pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
        if (batched) s_eb.open_vp = vp;
    }
    if (!batched || !s_eb.dyn_valid || memcmp(&s_eb.open_sc, &sc, sizeof sc) != 0) {
        pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
        if (batched) s_eb.open_sc = sc;
    }
    if (!batched || !s_eb.dyn_valid || s_eb.open_stencil != sref) {
        pvkCmdSetStencilReference(s_vk.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, sref);
        if (batched) s_eb.open_stencil = sref;
    }
    if (!batched || !s_eb.dyn_valid || memcmp(s_eb.open_blend, s_e2.blend_color, sizeof s_eb.open_blend) != 0) {
        pvkCmdSetBlendConstants(s_vk.cmd, s_e2.blend_color);
        if (batched) memcpy(s_eb.open_blend, s_e2.blend_color, sizeof s_eb.open_blend);
    }
    if (s_vk.depth_bounds_ok &&
        (!batched || !s_eb.dyn_valid || memcmp(s_eb.open_dbounds, s_e2.depth_bounds, sizeof s_eb.open_dbounds) != 0)) {
        pvkCmdSetDepthBounds(s_vk.cmd, s_e2.depth_bounds[0], s_e2.depth_bounds[1]);
        if (batched) memcpy(s_eb.open_dbounds, s_e2.depth_bounds, sizeof s_eb.open_dbounds);
    }
    if (!batched || !s_eb.dyn_valid || memcmp(s_eb.open_dbias, s_e2.depth_bias, sizeof s_eb.open_dbias) != 0) {
        pvkCmdSetDepthBias(s_vk.cmd, s_e2.depth_bias[0], 0.0f, s_e2.depth_bias[1]);
        if (batched) memcpy(s_eb.open_dbias, s_e2.depth_bias, sizeof s_eb.open_dbias);
    }
    if (batched) s_eb.dyn_valid = 1;
    if (!batched || s_eb.open_set != set || s_eb.open_set_split != is_split ||
        s_eb.open_dyn[0] != dyn_off[0] || s_eb.open_dyn[1] != dyn_off[1]) {
        pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 is_split ? s_e2.split_pipe_layout : s_g.pipe_layout,
                                 0, 1, &set, 2, dyn_off);
        if (batched) {
            s_eb.open_set = set;
            s_eb.open_set_split = is_split;
            s_eb.open_dyn[0] = dyn_off[0];
            s_eb.open_dyn[1] = dyn_off[1];
        }
    }
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &vbuf, &voff);
    if (ibytes) {
        pvkCmdBindIndexBuffer(s_vk.cmd, ibuf, ioff, VK_INDEX_TYPE_UINT32);
        pvkCmdDrawIndexed(s_vk.cmd, index_count, 1, 0, 0, 0);
    } else {
        pvkCmdDraw(s_vk.cmd, vertex_count, 1, 0, 0);
    }
    if (!batched) {
        pvkCmdEndRenderPass(s_vk.cmd);
        vk_submit_and_wait();
        return;
    }
    if (P->rs.color_mask) s_eb.open_rt_dirty |= 1u;
    if (!s_vk.indep_blend_ok) {
        if (P->rs.color_mask) s_eb.open_rt_dirty |= (1u << n) - 1u;
    } else {
        for (u32 i = 1; i < n; i++)
            if ((P->rs.color_mask_mrt >> (i * 4u)) & 0xFu) s_eb.open_rt_dirty |= (1u << i);
    }
    if ((P->rs.depth_test && P->rs.depth_write) ||
        (P->rs.stencil_enable && (P->rs.s_write_mask & 0xFFu)))
        s_eb.open_depth_dirty = 1;
    s_eb.draws++;
    s_eb.p_draws++;
    vk_eb_recorded();
}

static const rsx_draw_backend s_vk_engine_backend = {
    .user                 = NULL,
    .init                 = vk_eng_init,
    .shutdown             = vk_eng_shutdown,
    .submit_and_wait      = vk_eng_submit_and_wait,
    .texture_create       = vk_eng_texture_create,
    .texture_upload       = vk_eng_texture_upload,
    .texture_release      = vk_eng_texture_release,
    .color_target_create  = vk_eng_color_target_create,
    .color_target_release = vk_eng_color_target_release,
    .surface_view         = vk_eng_surface_view,
    .depth_target_create  = vk_eng_depth_target_create,
    .depth_target_release = vk_eng_depth_target_release,
    .depth_snapshot       = vk_eng_depth_snapshot,
    .depth_as_color       = vk_eng_depth_as_color,
    .color_snapshot       = vk_eng_color_snapshot,
    .pipeline_create      = vk_eng_pipeline_create,
    .pipeline_release     = vk_eng_pipeline_release,
    .bind_targets         = vk_eng_bind_targets,
    .bind_pipeline        = vk_eng_bind_pipeline,
    .bind_vs_constants    = vk_eng_bind_vs_constants,
    .bind_ps_constants    = vk_eng_bind_ps_constants,
    .bind_textures        = vk_eng_bind_textures,
    .bind_vertex_textures = vk_eng_bind_vertex_textures,
    .set_viewport         = vk_eng_set_viewport,
    .set_scissor          = vk_eng_set_scissor,
    .set_stencil_ref      = vk_eng_set_stencil_ref,
    .set_depth_bounds     = vk_eng_set_depth_bounds,
    .set_depth_bias       = vk_eng_set_depth_bias,
    .set_blend_color      = vk_eng_set_blend_color,
    .draw                 = vk_eng_draw,
    .clear_color          = vk_eng_clear_color,
    .clear_depth_stencil  = vk_eng_clear_depth_stencil,
    .present              = vk_eng_present,
    .readback             = vk_eng_readback,
    .depth_readback       = vk_eng_depth_readback,
    .color_target_write   = vk_eng_color_target_write,
    .surface_blit         = vk_eng_surface_blit,
    .set_vcache_id        = vk_eng_set_vcache_id,
    .readback_self_flushes = 1,
    /* Every draw and every upload submits and waits, so an upload runs after
     * each earlier draw that sampled the old bytes has completed. */
    .texture_upload_ordered = 1,
};

static rsx_backend s_vulkan_backend = {
    .userdata           = &s_vk,
    .set_render_target  = vk_cb_track_state,
    .set_vertex_attribs = vk_cb_track_state,
    .set_depth_stencil  = vk_cb_track_state,
    .clear              = vk_cb_clear,
    .draw_arrays        = vk_cb_draw_arrays,
    .draw_indexed       = vk_cb_draw_indexed,
    .bind_texture       = vk_cb_bind_texture,
    .present            = vk_cb_present,
};

/* ---------------------------------------------------------------------------
 * Public entry points
 * -------------------------------------------------------------------------*/
static int vk_init_all(u32 width, u32 height, const char* title)
{
    pthread_mutex_init(&s_vk.queue_mu, NULL);
    pthread_mutex_init(&s_vk.present_mu, NULL);
    s_vk.width  = width  ? width  : 1280;
    s_vk.height = height ? height : 720;
    s_vk.dump_path = getenv("PS3RECOMP_VK_DUMP");
    /* Writing a full frame to disk on every present costs more than the
     * present itself and slows a title enough to change what it does; a
     * live title wants one now and then, not all of them. */
    { const char* e = getenv("PS3RECOMP_VK_DUMP_EVERY");
      s_vk.dump_every = (e && atoi(e) > 0) ? (u32)atoi(e) : 1; }
    { const char* e = getenv("PS3RECOMP_VK_FPS");
      s_vk.fps_on = e && !strcmp(e, "1");
      metal_frame_rate_reset(&s_vk.fps, vk_now_seconds()); }

    vk_window_open(title);           /* no-op unless PS3RECOMP_VK_WINDOW is set */
    if (vk_load_library()) return -1;

    uint32_t req_api = VK_API_VERSION_1_0;
    {
        PFN_vkEnumerateInstanceVersion pEnumVer =
            (PFN_vkEnumerateInstanceVersion)pvkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
        if (pEnumVer) {
            uint32_t inst_ver = VK_API_VERSION_1_0;
            if (pEnumVer(&inst_ver) == VK_SUCCESS && inst_ver >= VK_API_VERSION_1_1) {
#ifdef VK_API_VERSION_1_3
                req_api = (inst_ver >= VK_API_VERSION_1_3) ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
#else
                req_api = VK_API_VERSION_1_1;
#endif
            }
        }
    }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ps3recomp", .pEngineName = "ps3recomp RSX",
                              .apiVersion = req_api };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
    if (s_vk.windowed) {
        ci.enabledExtensionCount   = s_vk.inst_ext_count;
        ci.ppEnabledExtensionNames = s_vk.inst_exts;
    }
    VkResult ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    if (ir != VK_SUCCESS && s_vk.windowed) {
        vk_window_drop("vkCreateInstance with the surface extensions failed");
        ci.enabledExtensionCount = 0;
        ci.ppEnabledExtensionNames = NULL;
        ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    }
    if (ir != VK_SUCCESS) { VK_LOG("vkCreateInstance failed (VkResult %d)\n", (int)ir); return -1; }
    if (vk_load_instance_functions()) return -1;
    if (s_vk.windowed) {
        if (vk_load_wsi_instance_functions()) {
            vk_window_drop("the driver has no surface functions");
        } else if (vk_create_window_surface()) {
            char why[256];
            snprintf(why, sizeof why, "window surface creation failed: %s", vk_window_error());
            vk_window_drop(why);
        }
    }
    if (vk_pick_device()) {
        if (!s_vk.windowed) return -1;
        vk_window_drop("no device can present to the window");
        if (vk_pick_device()) return -1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    static const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    /* DXT (BC1-3) is how PS3 titles ship most textures; Vulkan gates it
     * behind a feature, enabled only when the device reports it. */
    VkPhysicalDeviceFeatures have, want;
    memset(&want, 0, sizeof want);
    pvkGetPhysicalDeviceFeatures(s_vk.phys, &have);
    want.textureCompressionBC = have.textureCompressionBC;
    /* User clip planes (rsx_vp_apply_user_clip) write ClipDistance. */
    want.shaderClipDistance = have.shaderClipDistance;
    s_vk.bc_ok = have.textureCompressionBC ? 1 : 0;
    /* The depth bounds test: a deferred renderer bounds its light pass to
     * the geometry and its sky to the far plane with it. Optional in
     * Vulkan; without it the test stays off, as before. */
    want.depthBounds = have.depthBounds;
    s_vk.depth_bounds_ok = have.depthBounds ? 1 : 0;
    /* SET_ZMIN_MAX_CONTROL's depth clamp (rs->depth_clamp). Optional too;
     * without it such draws are clipped, as before. */
    want.depthClamp = have.depthClamp;
    s_vk.depth_clamp_ok = have.depthClamp ? 1 : 0;
    /* Per-target colour mask and blend enable (an MRT set's targets B..D
     * have their own); without it every target takes target A's. */
    want.independentBlend = have.independentBlend;
    s_vk.indep_blend_ok = have.independentBlend ? 1 : 0;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = s_vk.windowed ? 1u : 0u,
                               .ppEnabledExtensionNames = s_vk.windowed ? dev_exts : NULL,
                               .pEnabledFeatures = &want };
    VK_CHECK(pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device), "vkCreateDevice");
    if (vk_load_device_functions()) return -1;
    if (s_vk.windowed && vk_load_wsi_device_functions())
        vk_window_drop("the driver has no swapchain functions");
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);
    vk_pipe_cache_init();

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_vk.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, &s_vk.cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.fence), "vkCreateFence");
    /* The present's own (see vk_state). */
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.present_pool), "vkCreateCommandPool");
    cai.commandPool = s_vk.present_pool;
    cai.commandBufferCount = VK_PRESENT_SLOTS;
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, s_vk.present_cmds), "vkAllocateCommandBuffers");
    cai.commandBufferCount = 1;
    for (u32 i = 0; i < VK_PRESENT_SLOTS; i++)
        VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.present_fences[i]), "vkCreateFence");
    s_vk.present_slot = 0;
    s_vk.present_cmd = s_vk.present_cmds[0];
    s_vk.present_fence = s_vk.present_fences[0];
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.window_fence), "vkCreateFence");

    if (vk_pick_depth_format()) return -1;
    if (vk_create_image(&s_vk.color, VK_FORMAT_R8G8B8A8_UNORM, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    if (vk_create_image(&s_vk.depth, s_vk.depth_format, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, s_vk.depth_aspect)) return -1;
    if (vk_create_host_buffer(&s_vk.readback, (VkDeviceSize)s_vk.width * s_vk.height * 4u,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return -1;
    if (vk_create_render_pass()) return -1;
    if (vk_create_pipeline_objects()) return -1;
    if (vk_guest_init()) {
        VK_LOG("guest programs: setup failed, fixed-function path only\n");
        vk_guest_shutdown();
    }

    /* Both targets UNDEFINED -> GENERAL once. Colour starts opaque black so a
     * present before the first clear reads defined memory; depth starts at
     * the FAR plane, not zero -- zero is the near plane, and a guest that
     * enables LESS before its first depth clear would draw nothing (the
     * null backend makes the same choice). */
    if (vk_begin()) return -1;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange dr = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cr);
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &dr);
    if (vk_submit_and_wait()) return -1;

    if (s_vk.windowed) {
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sem_acquire) != VK_SUCCESS ||
            vk_create_swapchain())
            vk_window_drop("the swapchain could not be created");
    }
    return 0;
}

int rsx_vulkan_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_vk, 0, sizeof s_vk);
    if (vk_init_all(width, height, title)) {
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    /* One path at a time: the FIFO walker feeds both the rsx_state vtable
     * and the register-file engine, so registering both would record every
     * draw twice (the rule the Metal backend's init follows).
     *
     * With guest programs on, the engine is the default -- as it became
     * Metal's once every host mode passed through it. It builds every
     * pipeline from the guest's programs, so without them it has nothing to
     * draw with, and the vtable's fixed-function path stays the default.
     * Either way PS3RECOMP_RSX_ENGINE=dispatch|vtable overrides. */
    rsx_draw_engine_set_backend(&s_vk_engine_backend);
    rsx_draw_engine_set_default(s_g.on ? 1 : 0);
    if (rsx_draw_engine_enabled() && rsx_draw_engine_init(s_vk.width, s_vk.height) == 0)
        s_vk.eng_active = 1;
    else
        rsx_set_backend(&s_vulkan_backend);
    VK_LOG("%s path\n", s_vk.eng_active ? "register-file draw engine" : "vtable");
    /* PS3RECOMP_RSX_PRESENT_THREAD has a Vulkan half on the engine path only
     * (vk_pt_start, from vk_eng_init). */
    if (!s_vk.eng_active && rsx_present_thread_requested())
        rsx_present_thread_unsupported("vulkan", "the vtable path");
    VK_LOG("offscreen target %ux%u ready, depth format %d (%s)\n",
           s_vk.width, s_vk.height, (int)s_vk.depth_format, title ? title : "");
    return 0;
}

static double vk_hold_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

void rsx_vulkan_backend_shutdown(void)
{
    rsx_vulkan_backend_present_finish();   /* a host that began one and quit */
    if (s_vk.fps_on) {
        pthread_mutex_lock(&s_vk.present_mu);
        vk_fps_report(1);
        s_vk.fps_on = 0;
        pthread_mutex_unlock(&s_vk.present_mu);
    }
    if (rsx_get_backend() == &s_vulkan_backend) rsx_set_backend(NULL);
    if (s_vk.eng_active) { rsx_draw_engine_shutdown(); s_vk.eng_active = 0; }
    rsx_draw_engine_set_backend(NULL);
    if (s_vk.windowed && s_vk.swapchain && s_vk.presented && s_vk.hold_seconds > 0.0) {
        VK_LOG("holding the last frame on screen for %.1f s "
               "(Esc or closing the window ends it early)\n", s_vk.hold_seconds);
        const double end = vk_hold_now() + s_vk.hold_seconds;
        while (s_vk.windowed && vk_hold_now() < end) {
            if (rsx_vulkan_backend_pump_messages() != 0) break;
            vk_present_window();         /* FIFO: paced by the display */
        }
    }
    vk_coop_shutdown();
    vk_fg_shutdown();
    if (s_vk.device) {
        vk_device_idle();
        vk_scale_shutdown();
    }
    if (s_vk.windowed || s_vk.window) vk_window_drop(NULL);
    if (s_vk.device) {
        vk_device_idle();
        vk_pipe_cache_destroy();
        vk_guest_shutdown();
        for (int i = 0; i < VK_PIPELINE_SLOTS; i++)
            if (s_vk.pipelines[i]) pvkDestroyPipeline(s_vk.device, s_vk.pipelines[i], NULL);
        if (s_vk.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_vk.pipe_layout, NULL);
        if (s_vk.desc_pool)   pvkDestroyDescriptorPool(s_vk.device, s_vk.desc_pool, NULL);
        if (s_vk.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.set_layout, NULL);
        if (s_vk.sampler)     pvkDestroySampler(s_vk.device, s_vk.sampler, NULL);
        for (u32 i = 0; i < s_vk.samp_count; i++)
            pvkDestroySampler(s_vk.device, s_vk.samp_cache[i].s, NULL);
        if (s_vk.vs)          pvkDestroyShaderModule(s_vk.device, s_vk.vs, NULL);
        if (s_vk.fs)          pvkDestroyShaderModule(s_vk.device, s_vk.fs, NULL);
        if (s_vk.framebuffer) pvkDestroyFramebuffer(s_vk.device, s_vk.framebuffer, NULL);
        if (s_vk.render_pass) pvkDestroyRenderPass(s_vk.device, s_vk.render_pass, NULL);
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) vk_destroy_image(&s_vk.tex[u]);
        vk_destroy_image(&s_vk.dummy);
        vk_destroy_image(&s_vk.depth);
        vk_destroy_image(&s_vk.color);
        vk_destroy_buffer(&s_vk.vertices);
        vk_destroy_buffer(&s_vk.readback);
        if (s_vk.fence) pvkDestroyFence(s_vk.device, s_vk.fence, NULL);
        if (s_vk.pool)  pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        for (u32 i = 0; i < VK_PRESENT_SLOTS; i++) {
            if (s_vk.present_fences[i]) pvkDestroyFence(s_vk.device, s_vk.present_fences[i], NULL);
            if (s_vk.present_acquire[i]) pvkDestroySemaphore(s_vk.device, s_vk.present_acquire[i], NULL);
        }
        if (s_vk.window_fence)  pvkDestroyFence(s_vk.device, s_vk.window_fence, NULL);
        if (s_vk.present_pool)  pvkDestroyCommandPool(s_vk.device, s_vk.present_pool, NULL);
        pvkDestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance && pvkDestroyInstance) pvkDestroyInstance(s_vk.instance, NULL);
#ifdef _WIN32
    if (s_vk.lib) FreeLibrary((HMODULE)s_vk.lib);
#else
    if (s_vk.lib) dlclose(s_vk.lib);
#endif
    free(s_vk.cpu_verts);
    pthread_mutex_destroy(&s_vk.queue_mu);
    pthread_mutex_destroy(&s_vk.present_mu);
    memset(&s_vk, 0, sizeof s_vk);
}

int rsx_vulkan_backend_pump_messages(void)
{
#ifdef _WIN32
    /* The message queue belongs to the thread that created the window: the
     * host pumps from that thread, as for the D3D12 backend. */
    if (!s_vk.windowed) return 0;
    MSG msg;
    int quit = 0;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { quit = 1; continue; }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (quit || s_vk.window_closed) ? -1 : 0;
#elif defined(__ANDROID__)
    return __atomic_load_n(&s_android_quit, __ATOMIC_ACQUIRE) ? -1 : 0;
#else
    if (!s_vk.windowed) return 0;
    int quit = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = 1;
        else if (e.type == SDL_KEYDOWN && !e.key.repeat && s_vk.window &&
                 (e.key.keysym.sym == SDLK_F11 ||
                  (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))) {
            /* Borderless fullscreen on the window's display, and back. */
            const int full = (SDL_GetWindowFlags(s_vk.window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
            if (SDL_SetWindowFullscreen(s_vk.window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) == 0)
                SDL_ShowCursor(full ? SDL_ENABLE : SDL_DISABLE);
            s_vk_resized = 1;
        } else if (e.type == SDL_WINDOWEVENT &&
                   (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESTORED ||
                    e.window.event == SDL_WINDOWEVENT_MAXIMIZED)) {
            s_vk_resized = 1;
        }
    }
    return quit ? -1 : 0;
#endif
}

void rsx_vulkan_backend_present(void)
{
    rsx_vulkan_backend_present_begin();
    rsx_vulkan_backend_present_finish();
}

void rsx_vulkan_backend_present_begin(void)
{
    if (!s_vk.device) return;
    s_vk.present_split = 1;
    if (s_vk.eng_active) rsx_draw_engine_present();
    else                 vk_cb_present(&s_vk, 0);
    s_vk.present_split = 0;
}

void rsx_vulkan_backend_present_finish(void)
{
    if (!s_vk.device) return;
    pthread_mutex_lock(&s_vk.present_mu);
    vk_present_complete_locked();
    pthread_mutex_unlock(&s_vk.present_mu);
}

u32 rsx_vulkan_backend_presented_center(void)
{
    /* Only a read-back present keeps the centre pixel. */
    return s_vk.present_readback_count ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_debug_color(void) { return s_vk.clear_argb; }

u32 rsx_vulkan_backend_readback_center(void)
{
    if (s_vk.eng_active) return rsx_draw_engine_readback_center();
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_guest_draws(void)
{
    return s_vk.eng_active ? rsx_draw_engine_guest_draws() : s_g.last_draws;
}

int rsx_vulkan_backend_depth_bounds(void)
{
    return s_vk.eng_active && s_vk.depth_bounds_ok;
}

int rsx_vulkan_backend_fg_mode(void)
{
    return vk_fg_mode();
}

unsigned rsx_vulkan_backend_fg_frames(void)
{
    return __atomic_load_n(&s_fg.gen_frames, __ATOMIC_RELAXED);
}

void rsx_vulkan_backend_coop_configure(int mode, int aspect_mode)
{
    vk_coop_configure(mode, aspect_mode);
}

void rsx_vulkan_backend_coop_set_client_connected(int connected)
{
    vk_coop_set_client_connected(connected);
}

int rsx_vulkan_backend_coop_get_mode(void)
{
    return vk_coop_get_mode();
}

int rsx_vulkan_backend_coop_is_split_active(void)
{
    return vk_coop_is_split_active();
}

int rsx_vulkan_backend_coop_fill_yuv(u8* y_dst, u32 y_stride,
                                     u8* u_dst, u32 u_stride,
                                     u8* v_dst, u32 v_stride,
                                     u32 uv_pstride,
                                     u32* out_w, u32* out_h,
                                     int* out_split, u32 timeout_ms)
{
    return vk_coop_fill_yuv(y_dst, y_stride, u_dst, u_stride, v_dst, v_stride,
                            uv_pstride, out_w, out_h, out_split, timeout_ms);
}

int rsx_vulkan_backend_coop_fill_rgba(u8* rgba_dst, u32 dst_cap,
                                      u32* out_w, u32* out_h,
                                      int* out_split, u32 timeout_ms)
{
    return vk_coop_fill_rgba(rgba_dst, dst_cap, out_w, out_h, out_split, timeout_ms);
}

#endif /* !_WIN32 || PS3RECOMP_RSX_VULKAN */
