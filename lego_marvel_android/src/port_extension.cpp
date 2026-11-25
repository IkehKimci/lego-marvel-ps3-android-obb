/*
 * LEGO Marvel Super Heroes (BLUS-31161) — Android port extension hooks.
 *
 * Implements the port_extension registry expected by the runner and sets
 * recommended Android Vulkan / RSX environment defaults before backend init.
 */

#include "port_extension.h"

#include <cstdio>
#include <cstdlib>

static PortExtensionCallbacks s_merged = {};
static void (*s_prepare[8])(const char*);
static void (*s_start[8])(const char*);
static void (*s_frame_poll[8])(void);
static void (*s_stop[8])(void);
static int s_n_prepare, s_n_start, s_n_frame, s_n_stop;

static void run_prepare(const char* elf)
{
    /* Default to guest shader translation + direct swapchain present on Android Vulkan. */
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 0);
    setenv("PS3RECOMP_VK_DIRECT_PRESENT", "1", 0);
    setenv("PS3RECOMP_RSX_SINGLE_PRESENT", "1", 0);
    setenv("VK_DRAW_BATCH", "1", 0);
    setenv("VK_DRAW_BATCH_MAX", "4096", 0);
    setenv("PS3RECOMP_ENG_FEEDBACK_PER_PASS", "1", 0);
    setenv("PS3RECOMP_GCM_FLIP_PACING", "adaptive", 0);
    setenv("PS3RECOMP_GCM_PPU_PUT_KICK", "1", 0);
    setenv("PS3RECOMP_GCM_SPU_PUT_KICK", "1", 0);
    setenv("SPU_FLOAT_RZ", "1", 0);
    setenv("SPU_INTERP_UNLIFTED", "1", 0);
    setenv("PS3RECOMP_SPU_INTERP_FALLBACK", "1", 0);
    setenv("PS3_VERBOSE", "0", 0);
    setenv("PS3_TITLE", "LEGO Marvel Super Heroes (BLUS31161)", 0);
    for (int i = 0; i < s_n_prepare; i++) s_prepare[i](elf);
}

static void run_start(const char* elf)
{
    for (int i = 0; i < s_n_start; i++) s_start[i](elf);
}

static void run_frame_poll(void)
{
    for (int i = 0; i < s_n_frame; i++) s_frame_poll[i]();
}

static void run_stop(void)
{
    for (int i = s_n_stop - 1; i >= 0; i--) s_stop[i]();
}

extern "C" int port_extension_register(const PortExtensionCallbacks* cb)
{
    if (!cb) return -1;
    if (cb->prepare    && s_n_prepare >= 8) return -1;
    if (cb->start      && s_n_start   >= 8) return -1;
    if (cb->frame_poll && s_n_frame   >= 8) return -1;
    if (cb->stop       && s_n_stop    >= 8) return -1;

    if (cb->prepare)    s_prepare[s_n_prepare++]    = cb->prepare;
    if (cb->start)      s_start[s_n_start++]        = cb->start;
    if (cb->frame_poll) s_frame_poll[s_n_frame++]   = cb->frame_poll;
    if (cb->stop)       s_stop[s_n_stop++]          = cb->stop;
    return 0;
}

extern "C" const PortExtensionCallbacks* port_extension(void)
{
    s_merged.prepare    = run_prepare;
    s_merged.start      = s_n_start ? run_start : nullptr;
    s_merged.frame_poll = s_n_frame ? run_frame_poll : nullptr;
    s_merged.stop       = s_n_stop  ? run_stop : nullptr;
    return &s_merged;
}
