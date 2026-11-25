/*
 * LEGO Marvel Super Heroes (BLUS-31161) -- Native Android ARM64 Host
 *
 * Bridges Android's ANativeActivity lifecycle, ANativeWindow (Vulkan WSI),
 * AInputQueue (Bluetooth/USB GameController + touchscreen virtual controls),
 * AAssetManager, and Android logcat onto the ps3recomp PPU/SPU/RSX/HLE runtime.
 *
 * Also builds as a standalone CLI runner when compiled with -DPS3RECOMP_ANDROID_CLI=1.
 */

#include "ppu_recomp.h"
#include "win32_compat.h"
#include "port_extension.h"
#include "guest_arena.h"
#include "rsx_vulkan_backend.h"
#include "cellPad.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#if defined(__ANDROID__) && !defined(PS3RECOMP_ANDROID_CLI)
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#ifdef HAVE_ADRENOTOOLS
#include <adrenotools/driver.h>
#endif
#define PS3_ANDROID_APK 1
#else
#define PS3_ANDROID_APK 0
#endif

/* ---------------------------------------------------------------------------
 * ps3recomp PPU boot scaffold symbols
 * -------------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
int      ppu_fs_mount_obb(const char* obb_path);
int      ppu_fs_obb_mounted(void);
int      ppu_fs_obb_stat(int64_t* out_size, int64_t* out_mtime);
int      ppu_fs_obb_extract_file(const char* rel_path, const char* dst_path);
int      ppu_fs_obb_extract_prefix(const char* rel_prefix, const char* dst_root);
void     lv2_init_syscalls(void);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;
void     ps3_host_quit(const char* why);

extern uint32_t g_ppu_main_stack_addr;
extern uint32_t g_ppu_main_stack_size;
extern int      g_sys_process_exit_called;
extern int32_t  g_sys_process_exit_code;

uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
int      cellGcm_take_flip_pending_synced(void);
void     cellGcm_rsx_process_fifo(void);
void     cellGcm_fifo_kick_wait(unsigned ms);
unsigned cellGcm_flip_request_count(void);
}

#define VM_SIZE       0x100010000ull
#define STACK_TOP     0x0FF00000u
#define STACK_SIZE    0x00100000u
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

struct AndroidLauncherSettings {
    int render_width        = WINDOW_WIDTH;
    int render_height       = WINDOW_HEIGHT;
    int fps_limit           = 60;   /* 60 = unlocked 60 FPS, 30 = 30 FPS cap, 0 = uncapped */
    int frame_tick_ms       = 16;   /* 60 Hz PS3 VBlank reference */
    int touch_controls      = 1;
    int show_fps            = 1;
    int adreno_turbo        = 0;
    std::string scaling_mode = "fsr";   /* "fsr", "cas", "integer", "linear" */
    std::string driver_mode = "system"; /* "system" or "custom" */
    std::string driver_name = "System Vulkan (Stock)";
    std::string driver_dir;
    std::string driver_lib;
    std::string hook_lib_dir;
    std::string tmp_lib_dir;
};

static AndroidLauncherSettings g_settings;
static volatile LONG      g_frames_presented = 0;
static std::atomic<int>   g_quit_requested{0};
static std::atomic<int>   g_clock_stop{0};

static std::string trim_ws(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

static void load_android_settings(const char* internal_dir)
{
    g_settings = AndroidLauncherSettings();
    if (internal_dir && *internal_dir) {
        g_settings.tmp_lib_dir = std::string(internal_dir) + "/adrenotools_tmp";
        mkdir(g_settings.tmp_lib_dir.c_str(), 0755);
    }

    const std::string paths[] = {
        "/storage/emulated/0/PS3/BLUS31161/settings.ini",
        (internal_dir && *internal_dir) ? (std::string(internal_dir) + "/settings.ini") : std::string(),
    };

    FILE* f = nullptr;
    std::string loaded_from;
    for (const auto& p : paths) {
        if (p.empty()) continue;
        f = fopen(p.c_str(), "r");
        if (f) { loaded_from = p; break; }
    }
    if (!f) {
        setenv("PS3RECOMP_GCM_FLIP_PERIOD_US", "15500", 1);
        setenv("PS3RECOMP_VK_SCALING", g_settings.scaling_mode.c_str(), 1);
        fprintf(stderr, "[settings] no settings.ini found; using defaults (720p, 60fps, fsr, system Vulkan)\n");
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim_ws(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_ws(s.substr(0, eq));
        std::string v = trim_ws(s.substr(eq + 1));

        if (k == "resolution") {
            if (v == "480p")      { g_settings.render_width = 854;  g_settings.render_height = 480; }
            else if (v == "540p") { g_settings.render_width = 960;  g_settings.render_height = 540; }
            else if (v == "1080p"){ g_settings.render_width = 1920; g_settings.render_height = 1080; }
            else                  { g_settings.render_width = 1280; g_settings.render_height = 720; }
        } else if (k == "fps_limit") {
            int fps = atoi(v.c_str());
            g_settings.fps_limit = fps;
            /* Keep PS3 VBlank steady at 60 Hz for audio/cutscene timers while
             * adaptive GCM flip pacing + BLUS-31161 vblank_interval override
             * enforce 30 / 60 / Uncapped without 16ms vs 16.67ms beat-aliasing. */
            g_settings.frame_tick_ms = 16;
            if (fps == 30) {
                setenv("PS3RECOMP_GCM_FLIP_PERIOD_US", "32500", 1);
                setenv("PS3RECOMP_VK_PRESENT_MODE", "mailbox", 1);
            } else if (fps <= 0) {
                setenv("PS3RECOMP_GCM_FLIP_PERIOD_US", "0", 1);
                setenv("PS3RECOMP_VK_PRESENT_MODE", "immediate", 1);
            } else {
                int us = (fps == 60) ? 15500 : (1000000 / fps);
                char pbuf[32];
                snprintf(pbuf, sizeof(pbuf), "%d", us);
                setenv("PS3RECOMP_GCM_FLIP_PERIOD_US", pbuf, 1);
                setenv("PS3RECOMP_VK_PRESENT_MODE", "mailbox", 1);
            }
        } else if (k == "scaling_mode") {
            if (v == "fsr" || v == "cas" || v == "integer" || v == "linear")
                g_settings.scaling_mode = v;
        } else if (k == "guest_shaders") {
            setenv("PS3RECOMP_VK_GUEST_PROGRAMS", (v == "0") ? "0" : "1", 1);
        } else if (k == "direct_present") {
            setenv("PS3RECOMP_VK_DIRECT_PRESENT", (v == "0") ? "0" : "1", 1);
        } else if (k == "spu_interp_fallback") {
            setenv("PS3RECOMP_SPU_INTERP_FALLBACK", (v == "0") ? "0" : "1", 1);
        } else if (k == "skip_fmv") {
            if (v == "1") setenv("PS3_NO_SAIL", "1", 1);
            else          unsetenv("PS3_NO_SAIL");
        } else if (k == "verbose_log") {
            setenv("PS3_VERBOSE", (v == "1") ? "1" : "0", 1);
        } else if (k == "touch_controls") {
            g_settings.touch_controls = (v == "0") ? 0 : 1;
        } else if (k == "show_fps") {
            g_settings.show_fps = (v == "0") ? 0 : 1;
        } else if (k == "adreno_turbo") {
            g_settings.adreno_turbo = (v == "1") ? 1 : 0;
        } else if (k == "driver_mode") {
            g_settings.driver_mode = v;
        } else if (k == "driver_name") {
            g_settings.driver_name = v;
        } else if (k == "driver_dir") {
            g_settings.driver_dir = v;
        } else if (k == "driver_lib") {
            g_settings.driver_lib = v;
        } else if (k == "hook_lib_dir") {
            g_settings.hook_lib_dir = v;
        }
    }
    fclose(f);
    setenv("PS3RECOMP_VK_SCALING", g_settings.scaling_mode.c_str(), 1);

    fprintf(stderr,
            "[settings] loaded %s: %dx%d fps_limit=%d scaling=%s driver=%s(%s) turbo=%d touch=%d fps_hud=%d\n",
            loaded_from.c_str(), g_settings.render_width, g_settings.render_height,
            g_settings.fps_limit, g_settings.scaling_mode.c_str(),
            g_settings.driver_mode.c_str(),
            g_settings.driver_name.c_str(), g_settings.adreno_turbo,
            g_settings.touch_controls, g_settings.show_fps);
    fflush(stderr);
}

#if PS3_ANDROID_APK
extern "C" void* ps3_android_open_custom_vulkan(void)
{
#ifdef HAVE_ADRENOTOOLS
    if (g_settings.adreno_turbo) {
        fprintf(stderr, "[adrenotools] enabling Adreno Turbo Mode (max GPU clocks)\n");
        adrenotools_set_turbo(true);
    }
    if (g_settings.driver_mode == "custom" &&
        !g_settings.driver_dir.empty() && !g_settings.driver_lib.empty()) {
        std::string hdir = g_settings.hook_lib_dir;
        Dl_info dli{};
        if (dladdr((const void*)&ps3_android_open_custom_vulkan, &dli) && dli.dli_fname) {
            std::string self_path(dli.dli_fname);
            size_t slash = self_path.rfind('/');
            if (slash != std::string::npos) {
                hdir = self_path.substr(0, slash + 1);
            }
        }
        if (hdir.empty()) {
            fprintf(stderr, "[adrenotools] WARNING: empty hook_lib_dir -- falling back to system libvulkan.so\n");
            fflush(stderr);
            return nullptr;
        }
        std::string ddir = g_settings.driver_dir;
        if (ddir.back() != '/') ddir.push_back('/');
        if (hdir.back() != '/') hdir.push_back('/');
        std::string tdir = g_settings.tmp_lib_dir;
        if (!tdir.empty() && tdir.back() != '/') tdir.push_back('/');

        fprintf(stderr, "[adrenotools] loading custom Vulkan driver '%s': dir=%s lib=%s hooks=%s\n",
                g_settings.driver_name.c_str(), ddir.c_str(),
                g_settings.driver_lib.c_str(), hdir.c_str());
        fflush(stderr);

        void* h = adrenotools_open_libvulkan(
            RTLD_NOW,
            ADRENOTOOLS_DRIVER_CUSTOM,
            tdir.empty() ? nullptr : tdir.c_str(),
            hdir.c_str(),
            ddir.c_str(),
            g_settings.driver_lib.c_str(),
            nullptr,
            nullptr);
        if (h) {
            fprintf(stderr, "[adrenotools] custom Vulkan driver '%s' loaded OK (handle=%p)\n",
                    g_settings.driver_name.c_str(), h);
            fflush(stderr);
            return h;
        }
        fprintf(stderr, "[adrenotools] WARNING: failed to load '%s' -- falling back to system libvulkan.so\n",
                g_settings.driver_name.c_str());
        fflush(stderr);
    }
#endif
    return nullptr;
}
#endif

extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

extern "C" int ps3_android_quit_requested(void)
{
    return g_quit_requested.load(std::memory_order_acquire);
}

static std::atomic<uint64_t> s_vpad_visual_state{
    (128ull << 16) | (128ull << 24) | (128ull << 32) | (128ull << 40)
};

#if PS3_ANDROID_APK
extern "C" JNIEXPORT jlong JNICALL __attribute__((visibility("default")))
Java_com_ps3recomp_legomarvel_GameActivity_nativeGetFrameStats(JNIEnv*, jclass)
{
    const uint64_t frames = (uint64_t)(uint32_t)ppu_boot_frames_presented();
    const uint64_t draws  = (uint64_t)(uint32_t)rsx_vulkan_backend_guest_draws();
    return (jlong)((frames << 32) | draws);
}

extern "C" JNIEXPORT jint JNICALL __attribute__((visibility("default")))
Java_com_ps3recomp_legomarvel_GameActivity_nativeGetRenderHeight(JNIEnv*, jclass)
{
    return (jint)g_settings.render_height;
}

extern "C" JNIEXPORT jlong JNICALL __attribute__((visibility("default")))
Java_com_ps3recomp_legomarvel_GameActivity_nativeGetVirtualPadState(JNIEnv*, jclass)
{
    return (jlong)s_vpad_visual_state.load(std::memory_order_relaxed);
}
#endif

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

static void present_guest_frame(void)
{
    rsx_vulkan_backend_present();
    InterlockedIncrement(&g_frames_presented);
}

static inline uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* BLUS-31161 NuRenderSetVBlankInterval global (0x015B369C):
 * PS3 release sets this to 2 (30 FPS = 2 VBlanks per frame) via func_0044C540.
 * Patching it to 1 (60 FPS) or 0 (Uncapped) unlocks full frame rate while
 * keeping game physics/audio synchronized to wall-clock time. */
static void enforce_blus31161_fps_limit(void)
{
    if (!vm_base) return;
    static const uint32_t kVBlankIntervalEA = 0x015B369Cu;
    uint32_t want = (g_settings.fps_limit == 30) ? 2u
                  : (g_settings.fps_limit <= 0)  ? 0u
                  : 1u;
    uint32_t cur_be = 0;
    memcpy(&cur_be, vm_base + kVBlankIntervalEA, 4);
    uint32_t cur = __builtin_bswap32(cur_be);
    if (cur != want) {
        uint32_t want_be = __builtin_bswap32(want);
        memcpy(vm_base + kVBlankIntervalEA, &want_be, 4);
        static int s_logged = 0;
        if (s_logged < 4) {
            s_logged++;
            fprintf(stderr, "[fps-unlock] patched 0x%08X vblank_interval %u -> %u (fps_limit=%d)\n",
                    kVBlankIntervalEA, cur, want, g_settings.fps_limit);
        }
    }
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "LEGO Marvel Super Heroes (BLUS31161)";

    int rsx_ok = (rsx_vulkan_backend_init(g_settings.render_width, g_settings.render_height, title) == 0);
    fprintf(stderr, "[rsx-android] Vulkan backend init %s (%dx%d)\n",
            rsx_ok ? "OK" : "FAILED", g_settings.render_width, g_settings.render_height);
    fflush(stderr);

    unsigned last_flip = 0;
    const uint64_t vblank_ns = 16666667ull; /* Exact 60 Hz PS3 VBlank period */
    uint64_t next_tick_ns = monotonic_ns() + vblank_ns;

    while (!g_clock_stop.load(std::memory_order_acquire)) {
        cellGcm_fifo_kick_wait(2);
        const uint64_t now_ns = monotonic_ns();

        if ((int64_t)(now_ns - next_tick_ns) >= 0) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            if (cellGcm_flip_request_count() > 0) {
                enforce_blus31161_fps_limit();
            }
            if ((int64_t)(now_ns - next_tick_ns) > (int64_t)(vblank_ns * 2ull)) {
                /* Avoid VBlank burst catch-up spirals after a heavy frame or shader compile. */
                next_tick_ns = now_ns + vblank_ns;
            } else {
                next_tick_ns += vblank_ns;
            }
            if (auto poll = port_extension()->frame_poll) poll();
        }

        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();
            cellGcmTickFlip();

            if (rsx_vulkan_backend_pump_messages() != 0) {
                g_quit_requested.store(1, std::memory_order_release);
                break;
            }
            const int fp = cellGcm_take_flip_pending();
            const unsigned fc = cellGcm_flip_request_count();
            if (fp || fc != last_flip) {
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                rsx_vulkan_backend_present();
            }
        }
    }

    if (rsx_ok) rsx_vulkan_backend_shutdown();
    return 0;
}

static bool alloc_guest_vm(void)
{
    vm_base = guest_arena_map(VM_SIZE);
    ppu_vm_size = 0;
    if (vm_base && ((uintptr_t)vm_base & 0xFFFFFFFFu)) {
        fprintf(stderr, "[boot] guest arena at %p is not 4 GB aligned\n", (void*)vm_base);
        guest_arena_unmap(vm_base, VM_SIZE);
        vm_base = nullptr;
    }
    return vm_base != nullptr;
}

static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot, const char* explicit_vfs)
{
    if (explicit_vfs && *explicit_vfs) {
        strncpy(s_vfs_root, explicit_vfs, sizeof(s_vfs_root) - 1);
        s_vfs_root[sizeof(s_vfs_root) - 1] = 0;
        ppu_vfs_root = s_vfs_root;
        return;
    }
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof(s_vfs_root) - 1);
    s_vfs_root[sizeof(s_vfs_root) - 1] = 0;
    for (int i = 0; i < 3; i++) {
        char* s = strrchr(s_vfs_root, '/');
        if (s) *s = 0;
    }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

static std::string g_fallback_ext_dir;
static std::string g_fallback_int_dir;
static void mkdir_p(const std::string& dir);

static bool probe_writable_dir(const std::string& dir)
{
    mkdir_p(dir);
    std::string probe = dir + "/.ps3recomp_write_test";
    int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const char ok = '1';
    ssize_t w = write(fd, &ok, 1);
    close(fd);
    unlink(probe.c_str());
    return w == 1;
}

static void configure_spirv_cache(const std::string& gamedata_dir)
{
    const char* existing = getenv("PS3RECOMP_SPIRV_CACHE");
    if (existing && *existing) {
        mkdir_p(existing);
        return;
    }
    /* Prefer fast app-internal ext4/f2fs storage over Android FUSE shared storage
     * for the hundreds of small SPIR-V cache files; fall back to gamedata on CLI. */
    std::string dir = !g_fallback_int_dir.empty()
                    ? (g_fallback_int_dir + "/spirv_cache")
                    : (gamedata_dir + "/spirv_cache");
    if (!dir.empty() && probe_writable_dir(dir)) {
        setenv("PS3RECOMP_SPIRV_CACHE", dir.c_str(), 1);
        printf("[boot] SPIR-V shader cache: %s\n", dir.c_str());
        fflush(stdout);
    }
}

static void configure_gamedata_root(void)
{
    /* Respect an explicit override if already set by caller/CLI. */
    const char* existing = getenv("PS3_GAMEDATA_ROOT");
    if (existing && *existing) {
        mkdir_p(std::string(existing) + "/dev_hdd0/home/00000001/savedata");
        mkdir_p(std::string(existing) + "/trophies");
        configure_spirv_cache(existing);
        printf("[boot] Save/Trophy root (env): %s\n", existing);
        fflush(stdout);
        return;
    }

    const std::string vfs_str = (ppu_vfs_root && *ppu_vfs_root) ? std::string(ppu_vfs_root) : std::string();
    const std::string bases[] = {
        ppu_fs_obb_mounted() ? std::string("/storage/emulated/0/PS3/BLUS31161") : vfs_str,
        g_fallback_ext_dir,
        g_fallback_int_dir,
        vfs_str,
    };

    for (const auto& base : bases) {
        if (base.empty() || base == "/") continue;
        std::string gd = base + "/gamedata";
        std::string savedata = gd + "/dev_hdd0/home/00000001/savedata";
        std::string trophies = gd + "/trophies";
        if (probe_writable_dir(savedata)) {
            mkdir_p(trophies);
            setenv("PS3_GAMEDATA_ROOT", gd.c_str(), 1);
            configure_spirv_cache(gd);
            /* Android launches native processes with cwd="/", which is read-only.
             * Switch cwd to the writable base so any relative ./gamedata path works too. */
            if (chdir(base.c_str()) != 0) {
                fprintf(stderr, "[boot] WARNING: chdir(%s) failed (errno=%d)\n",
                        base.c_str(), errno);
            }
            printf("[boot] Save/Trophy root: %s (cwd=%s)\n", gd.c_str(), base.c_str());
            fflush(stdout);
            return;
        }
    }
    configure_spirv_cache(g_fallback_int_dir);
    fprintf(stderr, "[boot] WARNING: no writable gamedata directory found; saves/trophies may fail\n");
    fflush(stderr);
}

static int run_ps3_session(const char* elf_path, const char* explicit_vfs)
{
    printf("=== ps3recomp Android ARM64 Runner (BLUS31161) ===\n");
    printf("[boot] ELF: %s\n", elf_path);
    fflush(stdout);

    if (auto prepare = port_extension()->prepare) prepare(elf_path);

    if (!alloc_guest_vm()) {
        fprintf(stderr, "[boot] ERROR: could not allocate 4 GB guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(elf_path);
    if (!entry) {
        fprintf(stderr, "[boot] ERROR: could not load ELF %s\n", elf_path);
        return 1;
    }

    derive_vfs_root(elf_path, explicit_vfs);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);
    fflush(stdout);
    configure_gamedata_root();

    ppu_recomp_register();
    ps3_load_prx_modules();
    ppu_hle_init();
    ppu_sysprx_register();
    ppu_fs_register();
    lv2_init_syscalls();

    g_ps3_guest_caller = harness_guest_caller;
    if (auto start = port_extension()->start) start(elf_path);

    g_clock_stop.store(0, std::memory_order_release);
    HANDLE clk = CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    g_ppu_main_stack_addr = STACK_TOP - STACK_SIZE;
    g_ppu_main_stack_size = STACK_SIZE;

    printf("[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n", entry, STACK_TOP);
    fflush(stdout);

    int rc = ppu_run(entry, STACK_TOP);
    printf("[boot] ppu_run returned %d\n", rc);
    fflush(stdout);

    g_clock_stop.store(1, std::memory_order_release);
    if (clk) {
        WaitForSingleObject(clk, 3000);
        CloseHandle(clk);
    }
    if (auto stop = port_extension()->stop) stop();
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : rc;
}

/* ---------------------------------------------------------------------------
 * Automatic shared-storage logger + async-signal-safe crash reporter
 *
 * Writes live runtime logs to:
 *   /storage/emulated/0/PS3/BLUS31161/logs/latest.log   (current session)
 *   /storage/emulated/0/PS3/BLUS31161/logs/prev.log     (previous session)
 *   /storage/emulated/0/PS3/BLUS31161/logs/crash_last.log (fatal crash dump)
 * Falls back to <externalDataPath>/logs/ if shared storage is not writable.
 * -------------------------------------------------------------------------*/
static constexpr const char* kSharedLogDir = "/storage/emulated/0/PS3/BLUS31161/logs";

static char        s_active_log_dir[512]   = {0};
static char        s_crash_log_path[560]   = {0};
static char        s_active_elf_path[512]  = {0};
static int         s_log_fd                = -1;
static std::atomic<int> s_in_crash{0};
static uint8_t     s_alt_stack_mem[65536];

static void mkdir_p(const std::string& dir)
{
    std::string cur;
    for (size_t i = 0; i < dir.size(); i++) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0755);
    }
    if (!cur.empty()) mkdir(cur.c_str(), 0755);
}

static void crash_write_all(int fd, const char* s, size_t n)
{
    if (fd < 0 || !s || !n) return;
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) break;
        s += w;
        n -= (size_t)w;
    }
}

static void crash_puts(int fd, const char* s)
{
    if (!s) return;
    size_t n = 0;
    while (s[n]) n++;
    crash_write_all(fd, s, n);
}

static void crash_put_hex(int fd, uint64_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[18];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xFu];
    }
    crash_write_all(fd, buf, 18);
}

static void crash_put_dec(int fd, int64_t v)
{
    char buf[32];
    int pos = 0;
    uint64_t u;
    if (v < 0) {
        buf[pos++] = '-';
        u = (uint64_t)(-(v + 1)) + 1u;
    } else {
        u = (uint64_t)v;
    }
    char rev[24];
    int rlen = 0;
    do {
        rev[rlen++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u > 0 && rlen < (int)sizeof(rev));
    while (rlen > 0) buf[pos++] = rev[--rlen];
    crash_write_all(fd, buf, (size_t)pos);
}

static const char* signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    default:      return "SIGNAL";
    }
}

static void crash_put_sym(int fd, uintptr_t addr)
{
    crash_put_hex(fd, (uint64_t)addr);
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (addr && dladdr((void*)addr, &info)) {
        crash_puts(fd, " (");
        if (info.dli_fname) {
            const char* base = info.dli_fname;
            for (const char* p = base; *p; ++p)
                if (*p == '/') base = p + 1;
            crash_puts(fd, base);
            crash_puts(fd, "+");
            crash_put_hex(fd, (uint64_t)(addr - (uintptr_t)info.dli_fbase));
        }
        if (info.dli_sname) {
            crash_puts(fd, " ");
            crash_puts(fd, info.dli_sname);
            crash_puts(fd, "+");
            crash_put_hex(fd, (uint64_t)(addr - (uintptr_t)info.dli_saddr));
        }
        crash_puts(fd, ")");
    }
    crash_puts(fd, "\n");
}

struct CrashUnwindState {
    int fd;
    int depth;
};

static _Unwind_Reason_Code crash_unwind_cb(struct _Unwind_Context* uctx, void* arg)
{
    auto* st = (CrashUnwindState*)arg;
    if (st->depth >= 24) return _URC_END_OF_STACK;
    uintptr_t ip = _Unwind_GetIP(uctx);
    if (!ip) return _URC_END_OF_STACK;
    crash_puts(st->fd, "  #");
    crash_put_dec(st->fd, st->depth);
    crash_puts(st->fd, " ");
    crash_put_sym(st->fd, ip);
    st->depth++;
    return _URC_NO_REASON;
}

static void write_crash_report(int fd, int sig, siginfo_t* si, void* uctx_void)
{
    crash_puts(fd, "\n================ PS3RECOMP FATAL CRASH REPORT ================\n");
    crash_puts(fd, "Signal    : ");
    crash_puts(fd, signal_name(sig));
    crash_puts(fd, " (");
    crash_put_dec(fd, sig);
    crash_puts(fd, ") si_code=");
    crash_put_dec(fd, si ? si->si_code : 0);
    crash_puts(fd, "\nFaultAddr : ");
    uintptr_t fault = si ? (uintptr_t)si->si_addr : 0;
    crash_put_hex(fd, (uint64_t)fault);
    crash_puts(fd, "\n");

    uintptr_t vmb = (uintptr_t)vm_base;
    crash_puts(fd, "GuestBase : ");
    crash_put_hex(fd, (uint64_t)vmb);
    if (vmb && fault >= vmb && (uint64_t)(fault - vmb) < VM_SIZE) {
        crash_puts(fd, "  --> GuestEA=");
        crash_put_hex(fd, (uint64_t)(fault - vmb));
    }
    crash_puts(fd, "\nFrames    : ");
    crash_put_dec(fd, (int64_t)g_frames_presented);
    crash_puts(fd, "\nELF       : ");
    crash_puts(fd, s_active_elf_path[0] ? s_active_elf_path : "(unset)");
    crash_puts(fd, "\nVFS Root  : ");
    crash_puts(fd, ppu_vfs_root ? ppu_vfs_root : "(unset)");
    crash_puts(fd, "\n");

#if defined(__aarch64__)
    if (uctx_void) {
        auto* uc = (ucontext_t*)uctx_void;
        crash_puts(fd, "PC        : ");
        crash_put_sym(fd, (uintptr_t)uc->uc_mcontext.pc);
        crash_puts(fd, "LR (x30)  : ");
        crash_put_sym(fd, (uintptr_t)uc->uc_mcontext.regs[30]);
        crash_puts(fd, "SP        : ");
        crash_put_hex(fd, (uint64_t)uc->uc_mcontext.sp);
        crash_puts(fd, "  FP (x29): ");
        crash_put_hex(fd, (uint64_t)uc->uc_mcontext.regs[29]);
        crash_puts(fd, "  PSTATE: ");
        crash_put_hex(fd, (uint64_t)uc->uc_mcontext.pstate);
        crash_puts(fd, "\n");
        for (int i = 0; i < 8; i++) {
            crash_puts(fd, "  x");
            crash_put_dec(fd, i);
            crash_puts(fd, "=");
            crash_put_hex(fd, (uint64_t)uc->uc_mcontext.regs[i]);
            if ((i & 1) == 1) crash_puts(fd, "\n");
        }
    }
#else
    (void)uctx_void;
#endif

    crash_puts(fd, "Backtrace :\n");
    CrashUnwindState ust{fd, 0};
    _Unwind_Backtrace(crash_unwind_cb, &ust);
    crash_puts(fd, "==============================================================\n");
}

static void crash_signal_handler(int sig, siginfo_t* si, void* uctx)
{
    if (s_in_crash.exchange(1) != 0) {
        _exit(128 + sig);
    }

    if (s_crash_log_path[0]) {
        int cfd = open(s_crash_log_path, O_CREAT | O_WRONLY | O_TRUNC, 0664);
        if (cfd >= 0) {
            write_crash_report(cfd, sig, si, uctx);
            fsync(cfd);
            close(cfd);
        }
    }
    if (s_log_fd >= 0) {
        write_crash_report(s_log_fd, sig, si, uctx);
        fsync(s_log_fd);
    }
#if PS3_ANDROID_APK
    __android_log_print(ANDROID_LOG_FATAL, "ps3recomp",
                        "FATAL CRASH %s (%d) fault=%p -- saved to %s",
                        signal_name(sig), sig, si ? si->si_addr : nullptr,
                        s_crash_log_path[0] ? s_crash_log_path : "(none)");
#endif

    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler(void)
{
    stack_t ss{};
    ss.ss_sp = s_alt_stack_mem;
    ss.ss_size = sizeof(s_alt_stack_mem);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;

    const int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTRAP };
    for (int sig : sigs) sigaction(sig, &sa, nullptr);
}

static void init_log_paths(const char* fallback_dir)
{
    std::string chosen;
    mkdir_p(kSharedLogDir);
    std::string probe = std::string(kSharedLogDir) + "/.write_test";
    int pfd = open(probe.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0664);
    if (pfd >= 0) {
        close(pfd);
        unlink(probe.c_str());
        chosen = kSharedLogDir;
    } else if (fallback_dir && *fallback_dir) {
        chosen = std::string(fallback_dir) + "/logs";
        mkdir_p(chosen);
    }

    if (!chosen.empty()) {
        strncpy(s_active_log_dir, chosen.c_str(), sizeof(s_active_log_dir) - 1);
        snprintf(s_crash_log_path, sizeof(s_crash_log_path), "%s/crash_last.log", s_active_log_dir);
    }
}

/* ---------------------------------------------------------------------------
 * Android OBB (uncompressed ZIP64) discovery & zero-copy boot staging
 *
 * Mounts the 7.6 GB .obb directly in ppu_fs.cpp via pread() and stages only
 * tiny boot/metadata files (EBOOT.ELF, PARAM.SFO, TROPDIR, LIC.DAT) into a
 * small cache dir so GAME*.DAT files are never duplicated on disk.
 * -------------------------------------------------------------------------*/
static bool obb_is_regular_file(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool ends_with_icase(const std::string& s, const char* suf)
{
    size_t sl = strlen(suf);
    if (s.size() < sl) return false;
    size_t off = s.size() - sl;
    for (size_t i = 0; i < sl; i++) {
        char a = s[off + i];
        char b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static std::string scan_dir_for_obb(const std::string& dir)
{
    if (dir.empty()) return std::string();
    DIR* d = opendir(dir.c_str());
    if (!d) return std::string();
    std::string preferred;
    std::string fallback;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name(e->d_name);
        if (!ends_with_icase(name, ".obb")) continue;
        std::string full = dir + "/" + name;
        if (!obb_is_regular_file(full)) continue;
        if (strncmp(name.c_str(), "main.", 5) == 0) {
            if (preferred.empty() || name > preferred) preferred = full;
        } else if (fallback.empty()) {
            fallback = full;
        }
    }
    closedir(d);
    return !preferred.empty() ? preferred : fallback;
}

static std::string find_installed_obb(const std::string& obb_dir)
{
    static const char* kDefaultObbName = "main.2.com.ps3recomp.legomarvel.obb";
    static const char* kStdObbDir      = "/storage/emulated/0/Android/obb/com.ps3recomp.legomarvel";

    if (!obb_dir.empty()) {
        std::string p = obb_dir + "/" + kDefaultObbName;
        if (obb_is_regular_file(p)) return p;
        p = scan_dir_for_obb(obb_dir);
        if (!p.empty()) return p;
    }
    {
        std::string p = std::string(kStdObbDir) + "/" + kDefaultObbName;
        if (obb_is_regular_file(p)) return p;
        p = scan_dir_for_obb(kStdObbDir);
        if (!p.empty()) return p;
    }
    return std::string();
}

static std::string find_fallback_obb(void)
{
    static const char* kDefaultObbName = "main.2.com.ps3recomp.legomarvel.obb";
    const std::string extra[] = {
        std::string("/storage/emulated/0/Download/") + kDefaultObbName,
        std::string("/storage/emulated/0/PS3/BLUS31161/") + kDefaultObbName,
    };
    for (const auto& c : extra) {
        if (obb_is_regular_file(c)) return c;
    }
    return std::string();
}

static bool prepare_obb_boot_dir(const std::string& obb_path,
                                 const std::string& cache_root,
                                 std::string& out_elf)
{
    if (ppu_fs_mount_obb(obb_path.c_str()) != 0) {
        fprintf(stderr, "[boot] ERROR: failed to mount OBB '%s'\n", obb_path.c_str());
        return false;
    }
    int64_t osz = 0, omt = 0;
    ppu_fs_obb_stat(&osz, &omt);

    mkdir_p(cache_root);
    std::string elf_dst   = cache_root + "/PS3_GAME/USRDIR/EBOOT.ELF";
    std::string stamp_path = cache_root + "/.obb_stamp";
    char want_stamp[128];
    snprintf(want_stamp, sizeof want_stamp, "%lld %lld\n", (long long)osz, (long long)omt);

    bool need_stage = !obb_is_regular_file(elf_dst);
    if (!need_stage) {
        FILE* sf = fopen(stamp_path.c_str(), "rb");
        if (!sf) {
            need_stage = true;
        } else {
            char got[128] = {0};
            size_t n = fread(got, 1, sizeof(got) - 1, sf);
            fclose(sf);
            got[n] = '\0';
            if (strcmp(got, want_stamp) != 0) need_stage = true;
        }
    }

    if (need_stage) {
        fprintf(stderr, "[boot] Staging tiny boot/metadata files from OBB into %s ...\n",
                cache_root.c_str());
        fflush(stderr);
        if (ppu_fs_obb_extract_file("PS3_GAME/USRDIR/EBOOT.ELF", elf_dst.c_str()) != 0) {
            fprintf(stderr, "[boot] ERROR: OBB is missing PS3_GAME/USRDIR/EBOOT.ELF\n");
            return false;
        }
        ppu_fs_obb_extract_file("PS3_GAME/PARAM.SFO",
                                (cache_root + "/PS3_GAME/PARAM.SFO").c_str());
        ppu_fs_obb_extract_file("PS3_GAME/LICDIR/LIC.DAT",
                                (cache_root + "/PS3_GAME/LICDIR/LIC.DAT").c_str());
        ppu_fs_obb_extract_prefix("PS3_GAME/TROPDIR", cache_root.c_str());
        if (FILE* sf = fopen(stamp_path.c_str(), "wb")) {
            fputs(want_stamp, sf);
            fclose(sf);
        }
    }

    out_elf = elf_dst;
    fprintf(stderr, "[boot] OBB ready: '%s' -> staged ELF '%s' (GAME*.DAT zero-copy mounted)\n",
            obb_path.c_str(), out_elf.c_str());
    fflush(stderr);
    return true;
}

#if PS3_ANDROID_APK
/* ---------------------------------------------------------------------------
 * Android logcat + shared-storage file stdout/stderr bridge
 * -------------------------------------------------------------------------*/
static int         s_log_pipe[2] = { -1, -1 };
static pthread_t   s_log_thread;
static int         s_log_started = 0;

static void* android_log_thread(void*)
{
    char buf[2048];
    ssize_t n;
    while ((n = read(s_log_pipe[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        __android_log_write(ANDROID_LOG_INFO, "ps3recomp", buf);
        if (s_log_fd >= 0) {
            crash_write_all(s_log_fd, buf, (size_t)n);
        }
    }
    return nullptr;
}

static void start_android_logging(const char* fallback_dir)
{
    if (s_log_started) return;
    s_log_started = 1;

    init_log_paths(fallback_dir);
    if (s_active_log_dir[0]) {
        std::string latest = std::string(s_active_log_dir) + "/latest.log";
        std::string prev   = std::string(s_active_log_dir) + "/prev.log";
        rename(latest.c_str(), prev.c_str());
        s_log_fd = open(latest.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0664);
    }

    install_crash_handler();

    if (pipe(s_log_pipe) == 0) {
        dup2(s_log_pipe[1], STDOUT_FILENO);
        dup2(s_log_pipe[1], STDERR_FILENO);
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IOLBF, 0);
        pthread_create(&s_log_thread, nullptr, android_log_thread, nullptr);
        pthread_detach(s_log_thread);
    }

    time_t now = time(nullptr);
    char ts[64] = {0};
    struct tm tm_buf{};
    localtime_r(&now, &tm_buf);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(stderr,
            "=== LEGO Marvel Super Heroes (BLUS31161) Native Android Log ===\n"
            "[log] started : %s\n"
            "[log] dir     : %s\n"
            "[log] latest  : %s/latest.log\n"
            "[log] crash   : %s\n",
            ts,
            s_active_log_dir[0] ? s_active_log_dir : "(logcat only)",
            s_active_log_dir[0] ? s_active_log_dir : ".",
            s_crash_log_path[0] ? s_crash_log_path : "(none)");
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * Asset extraction helper (APK assets/ -> internalDataPath)
 * -------------------------------------------------------------------------*/

static bool extract_apk_asset(AAssetManager* mgr, const char* asset_rel, const std::string& dest)
{
    if (!mgr) return false;
    AAsset* a = AAssetManager_open(mgr, asset_rel, AASSET_MODE_STREAMING);
    if (!a) return false;
    size_t slash = dest.find_last_of('/');
    if (slash != std::string::npos) mkdir_p(dest.substr(0, slash));
    FILE* out = fopen(dest.c_str(), "wb");
    if (!out) { AAsset_close(a); return false; }
    char buf[16384];
    int r;
    while ((r = AAsset_read(a, buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)r, out);
    fclose(out);
    AAsset_close(a);
    return true;
}

static bool file_exists(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/* ---------------------------------------------------------------------------
 * Android AInputQueue -> cellPad bridge (hardware gamepad + touch overlay)
 * -------------------------------------------------------------------------*/
static ANativeWindow* s_cur_window = nullptr;

static u8 axis_to_u8_raw(float v)
{
    if (v < -1.0f) v = -1.0f;
    if (v >  1.0f) v =  1.0f;
    int out = (v >= 0.0f) ? (128 + (int)lroundf(v * 127.0f))
                          : (128 + (int)lroundf(v * 128.0f));
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return (u8)out;
}

static u8 axis_to_u8(float v)
{
    if (fabsf(v) < 0.12f) v = 0.0f;
    return axis_to_u8_raw(v);
}

static u16 android_keycode_to_ps3(int32_t kc)
{
    switch (kc) {
    case AKEYCODE_BUTTON_A:      return CELL_PAD_CTRL_CROSS;
    case AKEYCODE_BUTTON_B:      return CELL_PAD_CTRL_CIRCLE;
    case AKEYCODE_BUTTON_X:      return CELL_PAD_CTRL_SQUARE;
    case AKEYCODE_BUTTON_Y:      return CELL_PAD_CTRL_TRIANGLE;
    case AKEYCODE_BUTTON_L1:     return CELL_PAD_CTRL_L1;
    case AKEYCODE_BUTTON_R1:     return CELL_PAD_CTRL_R1;
    case AKEYCODE_BUTTON_L2:     return CELL_PAD_CTRL_L2;
    case AKEYCODE_BUTTON_R2:     return CELL_PAD_CTRL_R2;
    case AKEYCODE_BUTTON_THUMBL: return CELL_PAD_CTRL_L3;
    case AKEYCODE_BUTTON_THUMBR: return CELL_PAD_CTRL_R3;
    case AKEYCODE_BUTTON_START:  return CELL_PAD_CTRL_START;
    case AKEYCODE_BUTTON_SELECT: return CELL_PAD_CTRL_SELECT;
    case AKEYCODE_DPAD_UP:       return CELL_PAD_CTRL_UP;
    case AKEYCODE_DPAD_DOWN:     return CELL_PAD_CTRL_DOWN;
    case AKEYCODE_DPAD_LEFT:     return CELL_PAD_CTRL_LEFT;
    case AKEYCODE_DPAD_RIGHT:    return CELL_PAD_CTRL_RIGHT;
    default:                     return 0;
    }
}

static int32_t s_left_stick_pid  = -1;
static int32_t s_right_stick_pid = -1;

static inline bool hit_pill(float x, float y, float cx, float cy, float hw, float hh)
{
    return fabsf(x - cx) <= hw && fabsf(y - cy) <= hh;
}

static inline bool hit_circle(float x, float y, float cx, float cy, float r)
{
    float dx = x - cx, dy = y - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

static void eval_stick(float x, float y, float cx, float cy, float range, u8* out_x, u8* out_y)
{
    float dx = (x - cx) / range;
    float dy = (y - cy) / range;
    float len = sqrtf(dx * dx + dy * dy);
    const float dz = 0.10f;
    if (len <= dz) {
        *out_x = 128;
        *out_y = 128;
        return;
    }
    float ux = dx / len;
    float uy = dy / len;
    float mag = (len - dz) / (1.0f - dz);
    if (mag > 1.0f) mag = 1.0f;
    /* DualShock 3 square outer-gate projection: ensures dragging to/beyond the
     * stick ring in ANY direction (cardinal or diagonal, no matter how far)
     * always outputs full 100% run speed instead of dropping to 70% walk speed. */
    float dom = fmaxf(fabsf(ux), fabsf(uy));
    float sq = (dom > 1e-4f) ? (1.0f / dom) : 1.0f;
    float scale = mag * (1.0f + (sq - 1.0f) * mag);
    *out_x = axis_to_u8_raw(ux * scale);
    *out_y = axis_to_u8_raw(uy * scale);
}

static void handle_touch_motion(const AInputEvent* ev)
{
    float w = (float)(s_cur_window ? ANativeWindow_getWidth(s_cur_window) : 1280);
    float h = (float)(s_cur_window ? ANativeWindow_getHeight(s_cur_window) : 720);
    if (w <= 0.0f) w = 1280.0f;
    if (h <= 0.0f) h = 720.0f;
    const float unit = (w < h) ? w : h;

    int32_t action = AMotionEvent_getAction(ev);
    int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    int32_t act_idx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    int32_t up_idx = (masked == AMOTION_EVENT_ACTION_POINTER_UP || masked == AMOTION_EVENT_ACTION_UP ||
                      masked == AMOTION_EVENT_ACTION_CANCEL)
                   ? act_idx : -1;

    /* Layout geometry — matches GameActivity.java HudOverlayView.onDraw() 1:1 */
    const float shW = unit * 0.085f * 1.22f;
    const float shH = unit * 0.042f * 1.30f;

    const float dpadCx  = w * 0.115f;
    const float dpadCy  = h * 0.52f;
    const float dpadArm = unit * 0.125f;

    const float stickR  = unit * 0.135f;
    const float stickRange = stickR * 0.72f;
    const float lStickX = w * 0.21f, lStickY = h * 0.80f;
    const float rStickX = w * 0.79f, rStickY = h * 0.80f;

    const float fx = w * 0.885f, fy = h * 0.52f;
    const float spread = unit * 0.115f;
    const float btnHitR = unit * 0.058f * 1.32f;

    const float botY = h * 0.91f;
    const float sysH = unit * 0.036f * 1.35f;

    size_t count = AMotionEvent_getPointerCount(ev);

    if (masked == AMOTION_EVENT_ACTION_CANCEL) {
        s_left_stick_pid  = -1;
        s_right_stick_pid = -1;
    } else if (up_idx >= 0 && (size_t)up_idx < count) {
        int32_t up_pid = AMotionEvent_getPointerId(ev, (size_t)up_idx);
        if (up_pid == s_left_stick_pid)  s_left_stick_pid  = -1;
        if (up_pid == s_right_stick_pid) s_right_stick_pid = -1;
    } else if ((masked == AMOTION_EVENT_ACTION_DOWN || masked == AMOTION_EVENT_ACTION_POINTER_DOWN) &&
               act_idx >= 0 && (size_t)act_idx < count) {
        float px = AMotionEvent_getX(ev, (size_t)act_idx);
        float py = AMotionEvent_getY(ev, (size_t)act_idx);
        int32_t pid = AMotionEvent_getPointerId(ev, (size_t)act_idx);
        if (s_left_stick_pid < 0 && hit_circle(px, py, lStickX, lStickY, stickR * 1.38f)) {
            s_left_stick_pid = pid;
        } else if (s_right_stick_pid < 0 && hit_circle(px, py, rStickX, rStickY, stickR * 1.38f)) {
            s_right_stick_pid = pid;
        }
    }

    u16 btns = 0;
    u8 lx = 128, ly = 128, rx = 128, ry = 128;
    u8 l2 = 0, r2 = 0;
    bool left_seen = false, right_seen = false;

    for (size_t i = 0; i < count; i++) {
        if (masked == AMOTION_EVENT_ACTION_CANCEL || (int32_t)i == up_idx) continue;
        float x = AMotionEvent_getX(ev, i);
        float y = AMotionEvent_getY(ev, i);
        int32_t pid = AMotionEvent_getPointerId(ev, i);

        if (pid == s_left_stick_pid) {
            eval_stick(x, y, lStickX, lStickY, stickRange, &lx, &ly);
            left_seen = true;
            continue;
        }
        if (pid == s_right_stick_pid) {
            eval_stick(x, y, rStickX, rStickY, stickRange, &rx, &ry);
            right_seen = true;
            continue;
        }

        /* Shoulder buttons / triggers */
        if (hit_pill(x, y, w * 0.075f, h * 0.14f, shW, shH)) { btns |= CELL_PAD_CTRL_L2; l2 = 255; continue; }
        if (hit_pill(x, y, w * 0.075f, h * 0.25f, shW, shH)) { btns |= CELL_PAD_CTRL_L1; continue; }
        if (hit_pill(x, y, w * 0.925f, h * 0.14f, shW, shH)) { btns |= CELL_PAD_CTRL_R2; r2 = 255; continue; }
        if (hit_pill(x, y, w * 0.925f, h * 0.25f, shW, shH)) { btns |= CELL_PAD_CTRL_R1; continue; }

        /* Bottom utility bar: L3, SELECT, START, R3 */
        if (hit_pill(x, y, w * 0.355f, botY, unit * 0.062f * 1.18f, sysH)) { btns |= CELL_PAD_CTRL_L3; continue; }
        if (hit_pill(x, y, w * 0.450f, botY, unit * 0.080f * 1.15f, sysH)) { btns |= CELL_PAD_CTRL_SELECT; continue; }
        if (hit_pill(x, y, w * 0.550f, botY, unit * 0.080f * 1.15f, sysH)) { btns |= CELL_PAD_CTRL_START; continue; }
        if (hit_pill(x, y, w * 0.645f, botY, unit * 0.062f * 1.18f, sysH)) { btns |= CELL_PAD_CTRL_R3; continue; }

        /* 8-way D-Pad */
        if (hit_circle(x, y, dpadCx, dpadCy, dpadArm * 1.28f)) {
            float dx = x - dpadCx;
            float dy = y - dpadCy;
            float dead = dpadArm * 0.22f;
            if (dx < -dead) btns |= CELL_PAD_CTRL_LEFT;
            if (dx >  dead) btns |= CELL_PAD_CTRL_RIGHT;
            if (dy < -dead) btns |= CELL_PAD_CTRL_UP;
            if (dy >  dead) btns |= CELL_PAD_CTRL_DOWN;
            continue;
        }

        /* Face buttons diamond (allow simultaneous adjacent presses like □+✕) */
        bool hit_face = false;
        if (hit_circle(x, y, fx, fy - spread, btnHitR)) { btns |= CELL_PAD_CTRL_TRIANGLE; hit_face = true; }
        if (hit_circle(x, y, fx, fy + spread, btnHitR)) { btns |= CELL_PAD_CTRL_CROSS;    hit_face = true; }
        if (hit_circle(x, y, fx - spread, fy, btnHitR)) { btns |= CELL_PAD_CTRL_SQUARE;   hit_face = true; }
        if (hit_circle(x, y, fx + spread, fy, btnHitR)) { btns |= CELL_PAD_CTRL_CIRCLE;   hit_face = true; }
        if (hit_face) continue;

        /* Late capture if a finger moved into an idle stick ring */
        if (s_left_stick_pid < 0 && hit_circle(x, y, lStickX, lStickY, stickR * 1.15f)) {
            s_left_stick_pid = pid;
            eval_stick(x, y, lStickX, lStickY, stickRange, &lx, &ly);
            left_seen = true;
            continue;
        }
        if (s_right_stick_pid < 0 && hit_circle(x, y, rStickX, rStickY, stickR * 1.15f)) {
            s_right_stick_pid = pid;
            eval_stick(x, y, rStickX, rStickY, stickRange, &rx, &ry);
            right_seen = true;
            continue;
        }
    }

    if (!left_seen)  s_left_stick_pid  = -1;
    if (!right_seen) s_right_stick_pid = -1;

    cellPad_android_set_buttons_mask(0, btns);
    cellPad_android_set_sticks(0, lx, ly, rx, ry);
    cellPad_android_set_triggers(0, l2, r2);

    const uint64_t vis = ((uint64_t)btns)
                       | ((uint64_t)lx << 16)
                       | ((uint64_t)ly << 24)
                       | ((uint64_t)rx << 32)
                       | ((uint64_t)ry << 40)
                       | ((uint64_t)(left_seen  ? 1u : 0u) << 48)
                       | ((uint64_t)(right_seen ? 1u : 0u) << 49);
    s_vpad_visual_state.store(vis, std::memory_order_relaxed);
}

static int on_input_queue_cb(int fd, int events, void* data)
{
    (void)fd; (void)events;
    AInputQueue* q = (AInputQueue*)data;
    AInputEvent* ev = nullptr;
    while (AInputQueue_getEvent(q, &ev) >= 0) {
        if (AInputQueue_preDispatchEvent(q, ev)) continue;
        int32_t handled = 0;
        int32_t type = AInputEvent_getType(ev);
        int32_t src  = AInputEvent_getSource(ev);

        if (type == AINPUT_EVENT_TYPE_KEY) {
            int32_t kc = AKeyEvent_getKeyCode(ev);
            u16 mask = android_keycode_to_ps3(kc);
            if (mask) {
                int down = (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN);
                cellPad_android_set_button(0, mask, down);
                handled = 1;
            }
        } else if (type == AINPUT_EVENT_TYPE_MOTION) {
            if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
                u8 lx = axis_to_u8(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0));
                u8 ly = axis_to_u8(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0));
                u8 rx = axis_to_u8(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Z, 0));
                u8 ry = axis_to_u8(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RZ, 0));
                float lt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_LTRIGGER, 0);
                float rt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RTRIGGER, 0);
                float hx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0);
                float hy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0);
                cellPad_android_set_sticks(0, lx, ly, rx, ry);
                cellPad_android_set_triggers(0, (u8)(lt * 255.0f), (u8)(rt * 255.0f));
                cellPad_android_set_button(0, CELL_PAD_CTRL_LEFT,  hx < -0.5f);
                cellPad_android_set_button(0, CELL_PAD_CTRL_RIGHT, hx >  0.5f);
                cellPad_android_set_button(0, CELL_PAD_CTRL_UP,    hy < -0.5f);
                cellPad_android_set_button(0, CELL_PAD_CTRL_DOWN,  hy >  0.5f);
                handled = 1;
            } else if ((src & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN &&
                       g_settings.touch_controls) {
                handle_touch_motion(ev);
                handled = 1;
            }
        }
        AInputQueue_finishEvent(q, ev, handled);
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Android NativeActivity runner thread & lifecycle callbacks
 * -------------------------------------------------------------------------*/
struct AndroidRunnerCtx {
    std::string internal_dir;
    std::string external_dir;
    std::string obb_dir;
    AAssetManager* assets;
    std::atomic<int> started{0};
    pthread_t thread{};
};

static AndroidRunnerCtx* s_ctx = nullptr;

static void* android_game_thread(void* arg)
{
    auto* ctx = (AndroidRunnerCtx*)arg;
    /* Stage metadata from APK assets/ into internalDataPath */
    std::string staged_sfo = ctx->internal_dir + "/PS3_GAME/PARAM.SFO";
    if (!file_exists(staged_sfo))
        extract_apk_asset(ctx->assets, "PS3_GAME/PARAM.SFO", staged_sfo);

#ifdef ANDROID_SMOKE_MODE
    std::string elf = ctx->internal_dir + "/smoke.elf";
    extract_apk_asset(ctx->assets, "smoke.elf", elf);
    run_ps3_session(elf.c_str(), ctx->internal_dir.c_str());
#else
    /* Strictly boot from Android OBB archive (no automatic loose-folder fallback) */
    std::string elf;
    std::string explicit_vfs;
    std::string obb = find_installed_obb(ctx->obb_dir);
    if (obb.empty()) obb = find_fallback_obb();
    if (!obb.empty()) {
        std::string obb_cache = ctx->internal_dir + "/obb_boot";
        if (prepare_obb_boot_dir(obb, obb_cache, elf)) {
            explicit_vfs = obb_cache;
        }
    }
    if (elf.empty()) {
        fprintf(stderr,
                "[boot] ERROR: OBB archive not found. Place main.2.com.ps3recomp.legomarvel.obb at:\n"
                "  /storage/emulated/0/Android/obb/com.ps3recomp.legomarvel/main.2.com.ps3recomp.legomarvel.obb\n"
                "  or /storage/emulated/0/Download/main.2.com.ps3recomp.legomarvel.obb\n");
        return nullptr;
    }
    strncpy(s_active_elf_path, elf.c_str(), sizeof(s_active_elf_path) - 1);
    int rc = run_ps3_session(elf.c_str(), explicit_vfs.empty() ? nullptr : explicit_vfs.c_str());
    fprintf(stderr, "[boot] Android game session exited rc=%d frames=%u\n",
            rc, ppu_boot_frames_presented());
    fflush(stderr);
    if (s_log_fd >= 0) fsync(s_log_fd);
#endif
    return nullptr;
}

static void on_window_created(ANativeActivity* act, ANativeWindow* win)
{
    (void)act;
    s_cur_window = win;
    rsx_vulkan_android_set_window(win);
    if (s_ctx && s_ctx->started.exchange(1) == 0) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64u * 1024u * 1024u);
        if (pthread_create(&s_ctx->thread, &attr, android_game_thread, s_ctx) != 0) {
            pthread_create(&s_ctx->thread, nullptr, android_game_thread, s_ctx);
        }
        pthread_attr_destroy(&attr);
    }
}

static void on_window_resized(ANativeActivity* act, ANativeWindow* win)
{
    (void)act; (void)win;
    rsx_vulkan_android_notify_resize();
}

static void on_window_destroyed(ANativeActivity* act, ANativeWindow* win)
{
    (void)act; (void)win;
    s_cur_window = nullptr;
    rsx_vulkan_android_set_window(nullptr);
}

static void on_input_queue_created(ANativeActivity* act, AInputQueue* queue)
{
    (void)act;
    AInputQueue_attachLooper(queue, ALooper_forThread(), 1, on_input_queue_cb, queue);
}

static void on_input_queue_destroyed(ANativeActivity* act, AInputQueue* queue)
{
    (void)act;
    AInputQueue_detachLooper(queue);
}

static void on_destroy(ANativeActivity* act)
{
    (void)act;
    g_quit_requested.store(1, std::memory_order_release);
    rsx_vulkan_android_request_quit();
    if (s_ctx && s_ctx->started.load()) {
        /* Do NOT block the Android UI thread in pthread_join: the recompiled PS3
         * main loop may not exit on host quit signals, and GameActivity runs in
         * an isolated :game process that terminates cleanly right after onDestroy. */
        pthread_detach(s_ctx->thread);
    }
    fflush(stdout);
    fflush(stderr);
    if (s_log_fd >= 0) fsync(s_log_fd);
}

extern "C" __attribute__((visibility("default")))
void ANativeActivity_onCreate(ANativeActivity* activity, void* savedState, size_t savedStateSize)
{
    (void)savedState; (void)savedStateSize;
    const char* ext = activity->externalDataPath ? activity->externalDataPath : activity->internalDataPath;
    start_android_logging(ext);
    load_android_settings(activity->internalDataPath);

    __android_log_print(ANDROID_LOG_INFO, "ps3recomp",
                        "ANativeActivity_onCreate: LEGO Marvel Super Heroes (BLUS31161) ps3recomp");

    s_ctx = new AndroidRunnerCtx();
    s_ctx->internal_dir = activity->internalDataPath ? activity->internalDataPath : ".";
    s_ctx->external_dir = ext ? ext : s_ctx->internal_dir;
    s_ctx->obb_dir      = activity->obbPath ? activity->obbPath : "";
    s_ctx->assets       = activity->assetManager;
    g_fallback_int_dir  = s_ctx->internal_dir;
    g_fallback_ext_dir  = s_ctx->external_dir;
    g_quit_requested.store(0, std::memory_order_release);

    activity->callbacks->onNativeWindowCreated  = on_window_created;
    activity->callbacks->onNativeWindowResized  = on_window_resized;
    activity->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    activity->callbacks->onInputQueueCreated    = on_input_queue_created;
    activity->callbacks->onInputQueueDestroyed  = on_input_queue_destroyed;
    activity->callbacks->onDestroy              = on_destroy;
}
#else
int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <PPU ELF | .obb archive> [vfs_root_or_obb_cache]\n", argv[0]);
        return 2;
    }
    init_log_paths(nullptr);
    install_crash_handler();
    load_android_settings(nullptr);
    std::string input(argv[1]);
    if (ends_with_icase(input, ".obb")) {
        std::string cache_dir = (argc >= 3 && argv[2][0]) ? std::string(argv[2])
                                                          : std::string("./obb_boot_cache");
        std::string elf;
        if (!prepare_obb_boot_dir(input, cache_dir, elf)) {
            return 1;
        }
        strncpy(s_active_elf_path, elf.c_str(), sizeof(s_active_elf_path) - 1);
        return run_ps3_session(elf.c_str(), cache_dir.c_str());
    }
    strncpy(s_active_elf_path, argv[1], sizeof(s_active_elf_path) - 1);
    return run_ps3_session(argv[1], argc >= 3 ? argv[2] : nullptr);
}
#endif
