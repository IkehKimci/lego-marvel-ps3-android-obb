/*
 * LEGO Marvel Super Heroes (BLUS-31161) — title-specific HLE NID overrides
 * and missing-import bridges.
 *
 * Registers handlers in port_extension.start (after ppu_hle_init()) for the
 * small set of BLUS-31161 imports not yet wired in upstream gen_hle_nids.py.
 */

#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include "port_extension.h"
#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unistd.h>

static std::recursive_mutex g_lego_music_list_mtx;

extern "C" {
void lego_music_list_lock(void)   { g_lego_music_list_mtx.lock(); }
void lego_music_list_unlock(void) { g_lego_music_list_mtx.unlock(); }

extern uint8_t* vm_base;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
uint32_t vm_read32(uint64_t a);
void     vm_write32(uint64_t a, uint32_t v);

s32 cellSailPlayerInitialize2(u32 pSelf, u32 pAllocator, u32 pCallback,
                              u32 pUserParam, u32 pAttribute, u32 pResource);
}

static inline void guest_zero(uint32_t ea, uint32_t len)
{
    if (!ea || !vm_base) return;
    memset(vm_base + ea, 0, len);
}

/* ---------------------------------------------------------------------------
 * cellGcmSys
 * -------------------------------------------------------------------------*/
/* 0x3A33C1FD: _cellGcmFunc15(CellGcmContextData* ctx) */
static void hle_cellGcmFunc15(ppu_context* ctx)
{
    ctx->gpr[3] = CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sysPrxForUser
 * -------------------------------------------------------------------------*/
/* 0x42B23552: sys_prx_register_library(void* lib) */
static void hle_sys_prx_register_library(ppu_context* ctx)
{
    ctx->gpr[3] = CELL_OK;
}

/* 0xE0DA8EFD: sys_spu_image_close(sys_spu_image_t* img) */
static void hle_sys_spu_image_close(ppu_context* ctx)
{
    ctx->gpr[3] = CELL_OK;
}

/* ---------------------------------------------------------------------------
 * cellSpurs (Taskset2 / JobChain2 helpers)
 * -------------------------------------------------------------------------*/
/* 0x1EBCF459: cellSpursDestroyTaskset2(CellSpursTaskset2* taskset) */
static void hle_cellSpursDestroyTaskset2(ppu_context* ctx)
{
    ctx->gpr[3] = CELL_OK;
}

/* 0xA7A94892: cellSpursJoinTask2(CellSpursTaskset2* ts, CellSpursTaskId id, int* exitCode) */
static void hle_cellSpursJoinTask2(ppu_context* ctx)
{
    uint32_t ts_ea = (uint32_t)ctx->gpr[3];
    uint32_t task_id = (uint32_t)ctx->gpr[4];
    uint32_t exit_ea = (uint32_t)ctx->gpr[5];

    uint32_t code = 0;
    if (ts_ea && vm_base && task_id < 128) {
        volatile uint8_t* e = vm_base + ts_ea + 0x1980u + 16u * task_id;
        /* Wait briefly if the SPU worker thread is still executing this task. */
        for (int spin = 0; spin < 4000; spin++) {
            uint8_t st = e[15] & 0x0Fu;
            if (st == 2 || st == 4) {
                code = ((uint32_t)e[8] << 24) | ((uint32_t)e[9] << 16) |
                       ((uint32_t)e[10] << 8) | (uint32_t)e[11];
                e[15] = 4;
                break;
            }
            /* Also stop waiting if the task enabled bit (+0x30) is already clear. */
            uint8_t enabled = vm_base[ts_ea + 0x30u + (task_id >> 3)] & (uint8_t)(0x80u >> (task_id & 7));
            if (!enabled) break;
            usleep(500);
        }
    }
    if (exit_ea) vm_write32(exit_ea, code);
    ctx->gpr[3] = CELL_OK;
}

/* 0x4CEB9694: cellSpursGetJobChainInfo(const CellSpursJobChain* jc, CellSpursJobChainInfo* info) */
static void hle_cellSpursGetJobChainInfo(ppu_context* ctx)
{
    uint32_t info_ea = (uint32_t)ctx->gpr[4];
    if (info_ea) guest_zero(info_ea, 0x30u);
    ctx->gpr[3] = CELL_OK;
}

/* ---------------------------------------------------------------------------
 * cellSaveData / sceNp
 * -------------------------------------------------------------------------*/
/* 0x04C06FC2: cellSaveDataGetListItem(dirName, dir, sysFileParam, bind, sizeKB) */
extern "C" s32 cellSaveDataGetListItem(const char* dirName, u32 dir_ea,
                                       u32 sysFileParam_ea, u32 bind_ea, u32 sizeKB_ea);
static void hle_cellSaveDataGetListItem(ppu_context* ctx)
{
    s32 rc = cellSaveDataGetListItem(
        (const char*)(uintptr_t)(u32)ctx->gpr[3],
        (u32)ctx->gpr[4],
        (u32)ctx->gpr[5],
        (u32)ctx->gpr[6],
        (u32)ctx->gpr[7]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* 0xCA39C4B2: sceNp unknown/lookup helper */
static void hle_sceNp_stub_ok(ppu_context* ctx)
{
    ctx->gpr[3] = CELL_OK;
}

/* ---------------------------------------------------------------------------
 * cellSail / cellPamf (FMV video playback stubs)
 * -------------------------------------------------------------------------*/
/* 0x17932B26: cellSailPlayerInitialize (v1 -> delegates to v2) */
static void hle_cellSailPlayerInitialize(ppu_context* ctx)
{
    s32 rc = cellSailPlayerInitialize2(
        (u32)ctx->gpr[3], (u32)ctx->gpr[4], (u32)ctx->gpr[5],
        (u32)ctx->gpr[6], (u32)ctx->gpr[7], 0);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

static void hle_cellSail_ok(ppu_context* ctx)
{
    ctx->gpr[3] = CELL_OK;
}

static void hle_cellSail_nodata(ppu_context* ctx)
{
    /* CELL_SAIL_ERROR_EMPTY (0x80610703) tells caller no decoded frame yet. */
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80610703u;
}

static void hle_cellSail_get_format(ppu_context* ctx)
{
    uint32_t fmt_ea = (uint32_t)ctx->gpr[4];
    if (fmt_ea) guest_zero(fmt_ea, 0x20u);
    ctx->gpr[3] = CELL_OK;
}

static void lego_marvel_register_nids(const char* /*elf_path*/)
{
    ps3_hle_register_ctx(0x3A33C1FDu, "_cellGcmFunc15", hle_cellGcmFunc15);

    ps3_hle_register_ctx(0x42B23552u, "sys_prx_register_library", hle_sys_prx_register_library);
    ps3_hle_register_ctx(0xE0DA8EFDu, "sys_spu_image_close", hle_sys_spu_image_close);

    ps3_hle_register_ctx(0x1EBCF459u, "cellSpursDestroyTaskset2", hle_cellSpursDestroyTaskset2);
    ps3_hle_register_ctx(0xA7A94892u, "cellSpursJoinTask2", hle_cellSpursJoinTask2);
    ps3_hle_register_ctx(0x4CEB9694u, "cellSpursGetJobChainInfo", hle_cellSpursGetJobChainInfo);

    ps3_hle_register_ctx(0x04C06FC2u, "cellSaveDataGetListItem", hle_cellSaveDataGetListItem);
    ps3_hle_register_ctx(0xCA39C4B2u, "sceNp_0xCA39C4B2", hle_sceNp_stub_ok);

    /* cellPamf undocumented helpers */
    ps3_hle_register_ctx(0x9AB20793u, "cellPamf_0x9AB20793", hle_cellSail_ok);
    ps3_hle_register_ctx(0xCA8181C1u, "cellPamf_0xCA8181C1", hle_cellSail_ok);

    /* cellSail v1 + graphics/sound adapter helpers */
    ps3_hle_register_ctx(0x17932B26u, "cellSailPlayerInitialize", hle_cellSailPlayerInitialize);
    ps3_hle_register_ctx(0x18B4629Du, "cellSailPlayerFinalize", hle_cellSail_ok);
    ps3_hle_register_ctx(0x0247C69Eu, "cellSailGraphicsAdapterGetFrame", hle_cellSail_nodata);
    ps3_hle_register_ctx(0x1872331Bu, "cellSailGraphicsAdapterPtsToTimePosition", hle_cellSail_ok);
    ps3_hle_register_ctx(0x44A20E79u, "cellSailGraphicsAdapterUpdateAvSync", hle_cellSail_ok);
    ps3_hle_register_ctx(0x76488BB1u, "cellSailGraphicsAdapterFinalize", hle_cellSail_ok);
    ps3_hle_register_ctx(0xFFD58AA4u, "cellSailGraphicsAdapterGetFormat", hle_cellSail_get_format);
    ps3_hle_register_ctx(0x7EB8D6B5u, "cellSailSoundAdapterGetFrame", hle_cellSail_nodata);
    ps3_hle_register_ctx(0xD1462438u, "cellSailSoundAdapterFinalize", hle_cellSail_ok);
    ps3_hle_register_ctx(0xEEC22809u, "cellSailSoundAdapterUpdateAvSync", hle_cellSail_ok);
    ps3_hle_register_ctx(0xF25F197Du, "cellSailSoundAdapterGetFormat", hle_cellSail_get_format);
    ps3_hle_register_ctx(0x0D0C2F0Cu, "cellSail_0x0D0C2F0C", hle_cellSail_ok);
    ps3_hle_register_ctx(0x15FD6A2Au, "cellSail_0x15FD6A2A", hle_cellSail_ok);
    ps3_hle_register_ctx(0x1C9D5E5Au, "cellSail_0x1C9D5E5A", hle_cellSail_ok);
    ps3_hle_register_ctx(0x6E83F5C0u, "cellSail_0x6E83F5C0", hle_cellSail_ok);
    ps3_hle_register_ctx(0xC044FAB1u, "cellSail_0xC044FAB1", hle_cellSail_ok);
    ps3_hle_register_ctx(0xDDEBD2A5u, "cellSail_0xDDEBD2A5", hle_cellSail_ok);

    fprintf(stderr, "[lego-marvel] registered 27 title-specific HLE NID bridges\n");
}

__attribute__((constructor))
static void register_lego_marvel_stubs(void)
{
    PortExtensionCallbacks cb = {};
    cb.start = lego_marvel_register_nids;
    port_extension_register(&cb);
}

extern "C" void ps3_load_prx_modules(void)
{
    /* BLUS-31161 has no standalone game .sprx modules (all game code lives in EBOOT.ELF). */
}
