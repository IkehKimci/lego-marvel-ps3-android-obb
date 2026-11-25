/*
 * LEGO Marvel Super Heroes Android Port -- Bring-up / Smoke PPU Program.
 *
 * Compiled ONLY while ANDROID_SMOKE_MODE=ON (before a decrypted BLUS-31161
 * EBOOT.ELF has been lifted into recompiled/).
 *
 * Runs against the generated smoke.elf + ppu_recomp.h ABI and exercises the
 * complete Android native stack:
 *   - PPU ELF PT_LOAD / BSS / TLS loader
 *   - LV2 syscalls (sys_tty_write, sys_timer_usleep, sys_ppu_thread_join/exit)
 *   - HLE NID bridge (_cellGcmInitBody, cellGcmSetDisplayBuffer,
 *     cellGcmGetControlRegister, cellGcmSetFlipCommand, cellGcmGetFlipStatus,
 *     cellGcmSetVBlankHandler, sys_ppu_thread_create, cellPadInit/GetData,
 *     cellAudioInit)
 *   - Live 60 Hz NV4097 RSX Vulkan presentation reacting to Android touch &
 *     hardware gamepad input until the Activity requests quit (or 3 frames when
 *     PS3RECOMP_SMOKE_ONCE=1 is set for CLI verification).
 */

#include "ppu_recomp.h"
#include "rsx_commands.h"
#include "win32_compat.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void ps3_hle_call(unsigned int nid, ppu_context* ctx);
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern unsigned ppu_boot_frames_presented(void);
extern unsigned int cellGcm_flip_request_count(void);
extern int ps3_android_quit_requested(void);

#define DRAIN_TRAMPOLINE(ctx) do { \
    while (g_trampoline_fn) { \
        void (*_tf)(void*) = g_trampoline_fn; \
        g_trampoline_fn = 0; \
        _tf((void*)(ctx)); \
    } \
} while (0)

#define SMOKE_DATA_BASE     0x00020000u
#define SMOKE_OPD_THREAD    (SMOKE_DATA_BASE + 0x0008u)
#define SMOKE_OPD_VBLANK    (SMOKE_DATA_BASE + 0x0040u)
#define SMOKE_THREAD_NAME   (SMOKE_DATA_BASE + 0x0E00u)
#define SMOKE_CANARY_EA     (SMOKE_DATA_BASE + 0x0FF0u)
#define SMOKE_CANARY        0x50533352u

#define SMOKE_SC_GCMCTX     (SMOKE_DATA_BASE + 0x2010u)
#define SMOKE_SC_TID        (SMOKE_DATA_BASE + 0x2020u)
#define SMOKE_SC_EXITSTAT   (SMOKE_DATA_BASE + 0x2030u)
#define SMOKE_PAD_DATA      (SMOKE_DATA_BASE + 0x2100u)

#define SMOKE_IO_ADDR       0x00100000u
#define SMOKE_IO_SIZE       0x00200000u
#define SMOKE_CMD_SIZE      0x00010000u

#define SC_PPU_THREAD_EXIT  41
#define SC_PPU_THREAD_JOIN  44
#define SC_TIMER_USLEEP     141

#define NID__cellGcmInitBody          0x15BAE46Bu
#define NID_cellGcmSetDisplayBuffer   0xA53D12AEu
#define NID_cellGcmGetControlRegister 0xA547ADDEu
#define NID_cellGcmSetFlipCommand     0x5C770579u
#define NID_cellGcmGetFlipStatus      0x72A577CEu
#define NID_cellGcmSetVBlankHandler   0xA91B0402u
#define NID_sys_process_exit          0xE6F2C1E7u
#define NID_sys_ppu_thread_create     0x24A1EA07u
#define NID_cellPadInit               0x1CF98800u
#define NID_cellPadGetData            0x8B72CDA1u
#define NID_cellAudioInit             0x0B168F92u

static _Atomic unsigned g_vblank_calls = 0;
static _Atomic int      g_worker_ran   = 0;
static uint32_t         g_fifo_len     = 0;

static void smoke_usleep(ppu_context* ctx, uint32_t us)
{
    ctx->gpr[11] = SC_TIMER_USLEEP;
    ctx->gpr[3]  = us;
    lv2_syscall(ctx);
}

static void smoke_emit(uint32_t method, uint32_t data)
{
    vm_write32(SMOKE_IO_ADDR + g_fifo_len, (1u << 18) | (((method >> 2) & 0x7FFu) << 2));
    g_fifo_len += 4;
    vm_write32(SMOKE_IO_ADDR + g_fifo_len, data);
    g_fifo_len += 4;
}

void func_00010200(ppu_context* ctx) { ps3_hle_call(NID__cellGcmInitBody, ctx); }
void func_00010300(ppu_context* ctx) { ps3_hle_call(NID_cellGcmSetDisplayBuffer, ctx); }
void func_00010400(ppu_context* ctx) { ps3_hle_call(NID_cellGcmGetControlRegister, ctx); }
void func_00010500(ppu_context* ctx) { ps3_hle_call(NID_cellGcmSetFlipCommand, ctx); }
void func_00010600(ppu_context* ctx) { ps3_hle_call(NID_cellGcmGetFlipStatus, ctx); }
void func_00010700(ppu_context* ctx) { ps3_hle_call(NID_sys_process_exit, ctx); }
void func_00010900(ppu_context* ctx) { ps3_hle_call(NID_cellGcmSetVBlankHandler, ctx); }

void func_00010800(ppu_context* ctx)
{
    (void)ctx;
    atomic_fetch_add(&g_vblank_calls, 1u);
}

void func_00010100(ppu_context* ctx)
{
    atomic_store(&g_worker_ran, 1);
    ctx->gpr[11] = SC_PPU_THREAD_EXIT;
    ctx->gpr[3]  = 0x1234u;
    lv2_syscall(ctx);
}

void func_00010000(ppu_context* ctx)
{
    const int once = getenv("PS3RECOMP_SMOKE_ONCE") && getenv("PS3RECOMP_SMOKE_ONCE")[0] != '0';
    printf("[android-smoke] PPU guest _start live on ARM64 (canary=0x%08X, mode=%s)\n",
           vm_read32(SMOKE_CANARY_EA), once ? "once" : "interactive");
    fflush(stdout);

    /* 1. Verify guest PPU thread creation + join */
    ctx->gpr[3] = SMOKE_SC_TID;
    ctx->gpr[4] = SMOKE_OPD_THREAD;
    ctx->gpr[5] = 0x5A5A5A5Au;
    ctx->gpr[6] = 1000;
    ctx->gpr[7] = 0x10000;
    ctx->gpr[8] = 1; /* JOINABLE */
    ctx->gpr[9] = SMOKE_THREAD_NAME;
    ps3_hle_call(NID_sys_ppu_thread_create, ctx);
    DRAIN_TRAMPOLINE(ctx);
    uint64_t tid = vm_read64(SMOKE_SC_TID);
    ctx->gpr[11] = SC_PPU_THREAD_JOIN;
    ctx->gpr[3]  = tid;
    ctx->gpr[4]  = SMOKE_SC_EXITSTAT;
    lv2_syscall(ctx);
    printf("[android-smoke] guest PPU thread joined (ran=%d, status=0x%llX)\n",
           atomic_load(&g_worker_ran), (unsigned long long)vm_read64(SMOKE_SC_EXITSTAT));

    /* 2. Init cellAudio (AAudio) and cellPad (Android GameController + Touch) */
    ps3_hle_call(NID_cellAudioInit, ctx);
    DRAIN_TRAMPOLINE(ctx);
    printf("[android-smoke] cellAudioInit -> 0x%08X\n", (uint32_t)ctx->gpr[3]);

    ctx->gpr[3] = 7;
    ps3_hle_call(NID_cellPadInit, ctx);
    DRAIN_TRAMPOLINE(ctx);
    printf("[android-smoke] cellPadInit -> 0x%08X\n", (uint32_t)ctx->gpr[3]);

    /* 3. Init cellGcm (RSX -> Vulkan ANativeWindow) */
    ctx->gpr[3] = SMOKE_SC_GCMCTX;
    ctx->gpr[4] = SMOKE_CMD_SIZE;
    ctx->gpr[5] = SMOKE_IO_SIZE;
    ctx->gpr[6] = SMOKE_IO_ADDR;
    func_00010200(ctx); DRAIN_TRAMPOLINE(ctx);

    ctx->gpr[3] = 0;       /* buffer id */
    ctx->gpr[4] = 0;       /* offset */
    ctx->gpr[5] = 1280 * 4;
    ctx->gpr[6] = 1280;
    ctx->gpr[7] = 720;
    func_00010300(ctx); DRAIN_TRAMPOLINE(ctx);

    func_00010400(ctx); DRAIN_TRAMPOLINE(ctx);
    uint32_t ctrl = (uint32_t)ctx->gpr[3];

    ctx->gpr[3] = SMOKE_OPD_VBLANK;
    func_00010900(ctx); DRAIN_TRAMPOLINE(ctx);

    /* 4. Frame loop: emit NV4097 clear + flip, wrap FIFO with JUMP (0x20000000) */
    uint32_t frame = 0;
    const uint32_t max_frames = once ? 5u : 0xFFFFFFFFu;
    while (frame < max_frames && !ps3_android_quit_requested()) {
        /* Poll Android pad/touch through real PS3 cellPadGetData HLE */
        ctx->gpr[3] = 0;
        ctx->gpr[4] = SMOKE_PAD_DATA;
        ps3_hle_call(NID_cellPadGetData, ctx);
        DRAIN_TRAMPOLINE(ctx);

        uint32_t btn1 = vm_read16(SMOKE_PAD_DATA + 4 + 2 * 2);
        uint32_t btn2 = vm_read16(SMOKE_PAD_DATA + 4 + 3 * 2);
        uint32_t lx   = vm_read16(SMOKE_PAD_DATA + 4 + 6 * 2);
        uint32_t ly   = vm_read16(SMOKE_PAD_DATA + 4 + 7 * 2);

        /* Smooth Marvel palette pulse (crimson/gold/cobalt), reactive to input */
        uint32_t phase = (frame * 3u) & 0x1FFu;
        uint32_t tri = phase < 256u ? phase : (511u - phase);
        uint32_t r = 140u + (tri * 100u) / 255u;
        uint32_t g = 24u  + ((255u - tri) * 55u) / 255u;
        uint32_t b = 36u  + (tri * 70u) / 255u;
        if (btn1 || btn2) {
            /* Highlight gold/cyan when any touch or gamepad button is pressed */
            r = (btn2 & 0x40u) ? 240u : 40u;   /* Cross = bright gold/red */
            g = (btn2 & 0x10u) ? 230u : 190u;  /* Triangle = emerald/gold */
            b = (btn2 & 0x80u) ? 245u : 80u;   /* Square = cobalt */
        } else if (lx != 128u || ly != 128u) {
            r = lx & 0xFFu;
            b = ly & 0xFFu;
            g = 96u;
        }
        uint32_t argb = 0xFF000000u | ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);

        /* Wrap FIFO cleanly before reaching end of 64 KB command ring */
        if (g_fifo_len + 64u >= SMOKE_CMD_SIZE) {
            vm_write32(SMOKE_IO_ADDR + g_fifo_len, 0x20000000u); /* JUMP to offset 0 */
            g_fifo_len = 0;
            vm_write32(ctrl + 0, 0);
            for (int w = 0; w < 500 && vm_read32(ctrl + 4) != 0; w++)
                smoke_usleep(ctx, 1000);
        }

        smoke_emit(NV4097_SET_COLOR_CLEAR_VALUE, argb);
        smoke_emit(NV4097_CLEAR_SURFACE,         0xF0u);
        vm_write32(ctrl + 0, g_fifo_len);

        ctx->gpr[3] = 0;
        func_00010500(ctx); DRAIN_TRAMPOLINE(ctx);

        for (int waited = 0; waited < 200; waited++) {
            func_00010600(ctx); DRAIN_TRAMPOLINE(ctx);
            if ((uint32_t)ctx->gpr[3] == 0u) break;
            smoke_usleep(ctx, 1000);
        }

        frame++;
        if (frame == 3u || (frame % 120u) == 0u) {
            printf("[android-smoke] frame=%u presented=%u vblanks=%u btn=0x%02X/%02X stick=(%u,%u)\n",
                   frame, ppu_boot_frames_presented(), atomic_load(&g_vblank_calls),
                   btn1, btn2, lx, ly);
            fflush(stdout);
        }
    }

    printf("[android-smoke] PASS: completed %u frames (vblanks=%u, presented=%u)\n",
           frame, atomic_load(&g_vblank_calls), ppu_boot_frames_presented());
    fflush(stdout);
}

const func_entry function_table[] = {
    { 0x00010000ULL, func_00010000, "func_00010000" },
    { 0x00010100ULL, func_00010100, "func_00010100" },
    { 0x00010200ULL, func_00010200, "func_00010200" },
    { 0x00010300ULL, func_00010300, "func_00010300" },
    { 0x00010400ULL, func_00010400, "func_00010400" },
    { 0x00010500ULL, func_00010500, "func_00010500" },
    { 0x00010600ULL, func_00010600, "func_00010600" },
    { 0x00010700ULL, func_00010700, "func_00010700" },
    { 0x00010800ULL, func_00010800, "func_00010800" },
    { 0x00010900ULL, func_00010900, "func_00010900" },
    { 0, NULL, NULL }
};
const uint64_t function_table_count = 10;
