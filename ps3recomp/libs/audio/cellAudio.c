/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "../../runtime/memory/vm.h"   /* vm_commit */
#include "cellAudio.h"
#include "audio_pace.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "ps3emu/log_first.h"
#include "ps3emu/thread_name.h"

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                            uint64_t source, uint64_t data1,
                                            uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size);

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
  #define AUDIO_BACKEND_AAUDIO  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
  #define AUDIO_BACKEND_AAUDIO  0
#elif defined(__ANDROID__)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  0
  #define AUDIO_BACKEND_AAUDIO  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
  #define AUDIO_BACKEND_AAUDIO  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

#if AUDIO_BACKEND_AAUDIO
  #include <aaudio/AAudio.h>
  #include <dlfcn.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
  #include <errno.h>   /* EINTR from clock_nanosleep */
  #if defined(__linux__)
    #include <sys/resource.h>
    #include <sched.h>
  #endif
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

/* ARMSX3/RPCS3 -0.0f port completion tag geometry (6 diagonal tags + 8ch front-right marks). */
#define AUDIO_PORT_TAG_COUNT          6u
#define AUDIO_NEG_ZERO_BE_BITS        0x00000080u /* IEEE-754 -0.0f stored big-endian in guest RAM */
#define AUDIO_FRONT_ONLY_SETTLE       8u

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  write_index;    /* where the game is writing */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
    /* ARMSX3 -0.0f buffer completion tag state (prevents reading empty/half-written blocks) */
    u32                  prev_touched_tag_nr;
    u32                  last_tag_bits[AUDIO_PORT_TAG_COUNT];
    u8                   periods_without_tag;
    u8                   front_only_port;
    u8                   front_only_reported;
} AudioPortSlot;

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Timestamp anchor for cellAudioGetPortTimestamp (microseconds, set at init) */
static u64            s_audio_start_us = 0;

/* Mixing thread */
static unsigned long long g_audio_dropped;   /* frames the device had no room for */
#ifdef _WIN32
static unsigned g_audio_holes[CELL_AUDIO_PORT_MAX];   /* AUDIO_RATE: empty blocks right after a block with sound */
#endif
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
/* Is s_mix_thread a thread that exists and has not been joined? The Win32
 * branch asks that of the HANDLE itself, which it nulls after closing;
 * pthread_t has no such value, so carry the flag. */
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (s_sdl_audio_dev) {
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

/* SDL queues without bound: keep about four blocks ahead of the device. */
static int audio_backend_room(void)
{
    if (!s_sdl_audio_dev) return -1;
    return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float)) <
           CELL_AUDIO_BLOCK_SAMPLES * 4 ? CELL_AUDIO_BLOCK_SAMPLES : 0;
}

static void audio_backend_wait(void) { SDL_Delay(1); }

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_AAUDIO

/* Lock-free single-producer / single-consumer stereo float ring buffer.
 * Power-of-two capacity: 32768 stereo frames (~682.7 ms at 48 kHz, 128 PS3 blocks).
 * Wide capacity guarantees burst refills never drop a single waveform block
 * (ARMSX3 cellAudio.cpp headroom rule) while the dynamic controller tracks
 * desired_buffer_duration (~64 ms = 12 blocks by default). */
#define AAUDIO_RING_FRAMES        32768u
#define AAUDIO_RING_MASK          (AAUDIO_RING_FRAMES - 1u)
#define AAUDIO_DEFAULT_BUF_MS     64u

typedef aaudio_result_t (*pfn_AAudio_createStreamBuilder)(AAudioStreamBuilder**);
typedef void            (*pfn_AAudioStreamBuilder_setDirection)(AAudioStreamBuilder*, aaudio_direction_t);
typedef void            (*pfn_AAudioStreamBuilder_setSampleRate)(AAudioStreamBuilder*, int32_t);
typedef void            (*pfn_AAudioStreamBuilder_setChannelCount)(AAudioStreamBuilder*, int32_t);
typedef void            (*pfn_AAudioStreamBuilder_setFormat)(AAudioStreamBuilder*, aaudio_format_t);
typedef void            (*pfn_AAudioStreamBuilder_setSharingMode)(AAudioStreamBuilder*, aaudio_sharing_mode_t);
typedef void            (*pfn_AAudioStreamBuilder_setPerformanceMode)(AAudioStreamBuilder*, aaudio_performance_mode_t);
typedef void            (*pfn_AAudioStreamBuilder_setUsage)(AAudioStreamBuilder*, int32_t);
typedef void            (*pfn_AAudioStreamBuilder_setBufferCapacityInFrames)(AAudioStreamBuilder*, int32_t);
typedef void            (*pfn_AAudioStreamBuilder_setDataCallback)(AAudioStreamBuilder*, AAudioStream_dataCallback, void*);
typedef void            (*pfn_AAudioStreamBuilder_setErrorCallback)(AAudioStreamBuilder*, AAudioStream_errorCallback, void*);
typedef aaudio_result_t (*pfn_AAudioStreamBuilder_openStream)(AAudioStreamBuilder*, AAudioStream**);
typedef aaudio_result_t (*pfn_AAudioStreamBuilder_delete)(AAudioStreamBuilder*);
typedef aaudio_result_t (*pfn_AAudioStream_requestStart)(AAudioStream*);
typedef aaudio_result_t (*pfn_AAudioStream_requestStop)(AAudioStream*);
typedef aaudio_result_t (*pfn_AAudioStream_close)(AAudioStream*);
typedef int32_t         (*pfn_AAudioStream_getSampleRate)(AAudioStream*);
typedef int32_t         (*pfn_AAudioStream_getState)(AAudioStream*);
typedef int32_t         (*pfn_AAudioStream_getFramesPerBurst)(AAudioStream*);
typedef int32_t         (*pfn_AAudioStream_getBufferCapacityInFrames)(AAudioStream*);
typedef aaudio_result_t (*pfn_AAudioStream_setBufferSizeInFrames)(AAudioStream*, int32_t);

static void*         s_aa_lib    = NULL;
static AAudioStream* s_aa_stream = NULL;
static pfn_AAudioStream_requestStop p_aa_stop  = NULL;
static pfn_AAudioStream_close       p_aa_close = NULL;
static pfn_AAudioStream_getState    p_aa_state = NULL;

static float         s_aa_ring[AAUDIO_RING_FRAMES * 2u];
static volatile u32  s_aa_read_pos  = 0;
static volatile u32  s_aa_write_pos = 0;
static volatile u32  s_aa_underruns = 0;
static volatile int  s_aa_reset_req = 0;
static u32           s_aa_desired_ms = AAUDIO_DEFAULT_BUF_MS;
static u32           s_aa_burst_frames = 192u;
static float         s_aa_last_l    = 0.0f;
static float         s_aa_last_r    = 0.0f;
static u32           s_aa_fade_in   = 0;

static void aaudio_error_cb(AAudioStream* stream, void* userData, aaudio_result_t error)
{
    (void)stream;
    (void)userData;
    fprintf(stderr, "[cellAudio] AAudio stream error/disconnect (%d); scheduling auto-recovery\n", (int)error);
    __atomic_store_n(&s_aa_reset_req, 1, __ATOMIC_RELEASE);
}

static aaudio_data_callback_result_t aaudio_data_cb(
    AAudioStream* stream, void* userData, void* audioData, int32_t numFrames)
{
    (void)stream;
    (void)userData;
    float* out = (float*)audioData;
    if (numFrames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;

    const u32 want = (u32)numFrames;
    u32 r = __atomic_load_n(&s_aa_read_pos, __ATOMIC_RELAXED);
    const u32 w = __atomic_load_n(&s_aa_write_pos, __ATOMIC_ACQUIRE);
    const u32 avail = w - r;
    const u32 take = avail < want ? avail : want;

    float last_l = s_aa_last_l;
    float last_r = s_aa_last_r;
    u32 fade_in = s_aa_fade_in;

    for (u32 i = 0; i < take; i++) {
        const u32 idx = (r + i) & AAUDIO_RING_MASK;
        float sl = s_aa_ring[idx * 2u + 0u];
        float sr = s_aa_ring[idx * 2u + 1u];
        if (fade_in > 0u) {
            const float t = (float)(96u - fade_in) * (1.0f / 96.0f);
            sl = last_l + (sl - last_l) * t;
            sr = last_r + (sr - last_r) * t;
            fade_in--;
        }
        out[i * 2u + 0u] = sl;
        out[i * 2u + 1u] = sr;
        last_l = sl;
        last_r = sr;
    }
    if (take < want) {
        /* ARMSX3 rule: count 1 underrun per starved callback and softly hold/decay
         * the last sample rather than dropping to a hard-zero step click. */
        __atomic_add_fetch(&s_aa_underruns, 1u, __ATOMIC_RELAXED);
        for (u32 i = take; i < want; i++) {
            last_l *= 0.985f;
            last_r *= 0.985f;
            out[i * 2u + 0u] = last_l;
            out[i * 2u + 1u] = last_r;
        }
        if (last_l > -1e-6f && last_l < 1e-6f) last_l = 0.0f;
        if (last_r > -1e-6f && last_r < 1e-6f) last_r = 0.0f;
        fade_in = 96u;
    }
    s_aa_last_l = last_l;
    s_aa_last_r = last_r;
    s_aa_fade_in = fade_in;
    __atomic_store_n(&s_aa_read_pos, r + take, __ATOMIC_RELEASE);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static int audio_backend_open_aaudio(int reset_ring)
{
    u32 desired_ms = AAUDIO_DEFAULT_BUF_MS;
    {
        const char* e = getenv("PS3_AUDIO_BUF_MS");
        if (e && *e) {
            const int v = atoi(e);
            if (v >= 24 && v <= 200) desired_ms = (u32)v;
        }
    }
    s_aa_desired_ms = desired_ms;
    const u32 desired_frames = (CELL_AUDIO_SAMPLE_RATE * desired_ms) / 1000u;

    if (reset_ring) {
        s_aa_read_pos = 0;
        s_aa_write_pos = desired_frames; /* prefill exact target duration */
        s_aa_underruns = 0;
        s_aa_last_l = 0.0f;
        s_aa_last_r = 0.0f;
        s_aa_fade_in = 0;
        memset(s_aa_ring, 0, sizeof(s_aa_ring));
    }
    __atomic_store_n(&s_aa_reset_req, 0, __ATOMIC_RELEASE);

    if (!s_aa_lib) {
        s_aa_lib = dlopen("libaaudio.so", RTLD_NOW | RTLD_LOCAL);
        if (!s_aa_lib) {
            printf("[cellAudio] AAudio dlopen(libaaudio.so) failed: %s\n", dlerror());
            return -1;
        }
    }

#define AA_SYM(var, name) \
    pfn_##name var = (pfn_##name)dlsym(s_aa_lib, #name); \
    if (!(var)) { printf("[cellAudio] AAudio missing symbol %s\n", #name); return -1; }

    AA_SYM(create_builder, AAudio_createStreamBuilder);
    AA_SYM(set_dir,        AAudioStreamBuilder_setDirection);
    AA_SYM(set_rate,       AAudioStreamBuilder_setSampleRate);
    AA_SYM(set_ch,         AAudioStreamBuilder_setChannelCount);
    AA_SYM(set_fmt,        AAudioStreamBuilder_setFormat);
    AA_SYM(set_share,      AAudioStreamBuilder_setSharingMode);
    AA_SYM(set_perf,       AAudioStreamBuilder_setPerformanceMode);
    AA_SYM(set_cb,         AAudioStreamBuilder_setDataCallback);
    AA_SYM(open_stream,    AAudioStreamBuilder_openStream);
    AA_SYM(del_builder,    AAudioStreamBuilder_delete);
    AA_SYM(req_start,      AAudioStream_requestStart);
    AA_SYM(req_stop,       AAudioStream_requestStop);
    AA_SYM(close_stream,   AAudioStream_close);
#undef AA_SYM

    pfn_AAudioStreamBuilder_setUsage set_usage =
        (pfn_AAudioStreamBuilder_setUsage)dlsym(s_aa_lib, "AAudioStreamBuilder_setUsage");
    pfn_AAudioStreamBuilder_setBufferCapacityInFrames set_cap =
        (pfn_AAudioStreamBuilder_setBufferCapacityInFrames)dlsym(s_aa_lib, "AAudioStreamBuilder_setBufferCapacityInFrames");
    pfn_AAudioStreamBuilder_setErrorCallback set_err =
        (pfn_AAudioStreamBuilder_setErrorCallback)dlsym(s_aa_lib, "AAudioStreamBuilder_setErrorCallback");
    pfn_AAudioStream_getSampleRate get_rate =
        (pfn_AAudioStream_getSampleRate)dlsym(s_aa_lib, "AAudioStream_getSampleRate");
    pfn_AAudioStream_getState get_state =
        (pfn_AAudioStream_getState)dlsym(s_aa_lib, "AAudioStream_getState");
    pfn_AAudioStream_getFramesPerBurst get_burst =
        (pfn_AAudioStream_getFramesPerBurst)dlsym(s_aa_lib, "AAudioStream_getFramesPerBurst");
    pfn_AAudioStream_getBufferCapacityInFrames get_cap =
        (pfn_AAudioStream_getBufferCapacityInFrames)dlsym(s_aa_lib, "AAudioStream_getBufferCapacityInFrames");
    pfn_AAudioStream_setBufferSizeInFrames set_buf =
        (pfn_AAudioStream_setBufferSizeInFrames)dlsym(s_aa_lib, "AAudioStream_setBufferSizeInFrames");

    p_aa_stop  = req_stop;
    p_aa_close = close_stream;
    p_aa_state = get_state;

    AAudioStreamBuilder* builder = NULL;
    if (create_builder(&builder) != AAUDIO_OK || !builder) {
        printf("[cellAudio] AAudio_createStreamBuilder failed\n");
        return -1;
    }

    /* ARMSX3 OboeBackend recipe:
     * - SHARED mode (never Exclusive, which OS disconnects under 10 FPS spikes).
     * - LOW_LATENCY by default for crisp 2-4 ms callbacks (no 40 ms deep-buffer
     *   sawtooth); PS3_AUDIO_RECORDING_COMPAT=1 selects NONE for AudioPlaybackCapture.
     * - USAGE_GAME (14) bypasses Android media post-processing latency chain.
     * - Buffer capacity & size scaled to desired_buffer_duration (whole bursts). */
    const char* rec_env = getenv("PS3_AUDIO_RECORDING_COMPAT");
    const int low_latency = !(rec_env && rec_env[0] == '1');

    set_dir(builder, AAUDIO_DIRECTION_OUTPUT);
    set_rate(builder, CELL_AUDIO_SAMPLE_RATE);
    set_ch(builder, 2);
    set_fmt(builder, AAUDIO_FORMAT_PCM_FLOAT);
    set_share(builder, AAUDIO_SHARING_MODE_SHARED);
    set_perf(builder, low_latency ? AAUDIO_PERFORMANCE_MODE_LOW_LATENCY : AAUDIO_PERFORMANCE_MODE_NONE);
    if (set_usage) set_usage(builder, 14 /* AAUDIO_USAGE_GAME */);
    if (set_cap)   set_cap(builder, (int32_t)(desired_frames < 4096u ? 4096u : desired_frames));
    set_cb(builder, aaudio_data_cb, NULL);
    if (set_err)   set_err(builder, aaudio_error_cb, NULL);

    aaudio_result_t res = open_stream(builder, &s_aa_stream);
    del_builder(builder);
    if (res != AAUDIO_OK || !s_aa_stream) {
        printf("[cellAudio] AAudioStreamBuilder_openStream failed: %d\n", (int)res);
        s_aa_stream = NULL;
        return -1;
    }

    const int32_t actual_rate = get_rate ? get_rate(s_aa_stream) : CELL_AUDIO_SAMPLE_RATE;
    if (actual_rate > 0 && actual_rate != CELL_AUDIO_SAMPLE_RATE) {
        printf("[cellAudio] WARNING: AAudio opened at %d Hz (requested %d Hz)\n",
               actual_rate, CELL_AUDIO_SAMPLE_RATE);
    }

    const int32_t burst = get_burst ? get_burst(s_aa_stream) : 0;
    const int32_t cap = get_cap ? get_cap(s_aa_stream) : 0;
    int32_t tuned_buf = 0;
    if (burst > 0) s_aa_burst_frames = (u32)burst;
    if (set_buf && burst > 0 && cap > 0) {
        const int32_t rate_use = actual_rate > 0 ? actual_rate : CELL_AUDIO_SAMPLE_RATE;
        const int32_t target = (int32_t)(((u64)rate_use * desired_ms) / 1000u);
        int32_t bursts = (target + burst - 1) / burst;
        if (bursts < 2) bursts = 2;
        int32_t want_buf = bursts * burst;
        if (want_buf > cap) want_buf = cap;
        if (want_buf > 0) tuned_buf = (int32_t)set_buf(s_aa_stream, want_buf);
    }

    res = req_start(s_aa_stream);
    if (res != AAUDIO_OK) {
        printf("[cellAudio] AAudioStream_requestStart failed: %d\n", (int)res);
        p_aa_close(s_aa_stream);
        s_aa_stream = NULL;
        return -1;
    }

    printf("[cellAudio] AAudio (ARMSX3-tuned) initialized (%d Hz stereo f32, %s, ring=%u frames, target=%u ms, burst=%d buf=%d/%d)\n",
           actual_rate, low_latency ? "LOW_LATENCY/SHARED/GAME" : "REC_COMPAT/SHARED/GAME",
           AAUDIO_RING_FRAMES, desired_ms, burst, tuned_buf, cap);
    return 0;
}

static int audio_backend_init(void)
{
    return audio_backend_open_aaudio(1);
}

static void audio_backend_shutdown(void)
{
    if (s_aa_stream) {
        if (p_aa_stop)  p_aa_stop(s_aa_stream);
        if (p_aa_close) p_aa_close(s_aa_stream);
        s_aa_stream = NULL;
    }
    /* Keep libaaudio.so loaded for process lifetime: Android's Binder/audio
     * teardown threads may still be unwinding after AAudioStream_close(). */
}

static int audio_backend_is_operational(void)
{
    if (!s_aa_stream || __atomic_load_n(&s_aa_reset_req, __ATOMIC_ACQUIRE))
        return 0;
    if (p_aa_state) {
        /* AAudio active states: OPEN(2)..FLUSHED(8). Anything else (DISCONNECTED=13,
         * CLOSED=12, STOPPED=10, UNINITIALIZED=0) triggers clean reopen. */
        const int32_t st = p_aa_state(s_aa_stream);
        if (st < 2 || st > 8) return 0;
    }
    return 1;
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_aa_stream || num_samples == 0) return;

    static int mute = -1;
    if (mute < 0) mute = getenv("PS3_MUTE") ? 1 : 0;

    u32 w = __atomic_load_n(&s_aa_write_pos, __ATOMIC_RELAXED);
    const u32 r = __atomic_load_n(&s_aa_read_pos, __ATOMIC_ACQUIRE);
    const u32 free_frames = AAUDIO_RING_FRAMES - (w - r);
    u32 put = num_samples;
    if (put > free_frames) {
        g_audio_dropped += put - free_frames;
        put = free_frames;
    }
    for (u32 i = 0; i < put; i++) {
        const u32 idx = (w + i) & AAUDIO_RING_MASK;
        s_aa_ring[idx * 2u + 0u] = mute ? 0.0f : stereo_samples[i * 2u + 0u];
        s_aa_ring[idx * 2u + 1u] = mute ? 0.0f : stereo_samples[i * 2u + 1u];
    }
    __atomic_store_n(&s_aa_write_pos, w + put, __ATOMIC_RELEASE);
}

static u32 audio_backend_queued_samples(void)
{
    if (!s_aa_stream) return 0;
    const u32 w = __atomic_load_n(&s_aa_write_pos, __ATOMIC_ACQUIRE);
    const u32 r = __atomic_load_n(&s_aa_read_pos, __ATOMIC_ACQUIRE);
    return w - r;
}

/* ---------------------------------------------------------------------------
 * Built-in Zero-Allocation Stereo WSOLA Time-Stretcher (ARMSX3-style tempo
 * scaling without pitch shift when SPU task_audio falls behind real-time).
 * - Zero latency & zero CPU when tempo >= 0.992 (direct passthrough).
 * - When ring depth drops below 75% of target, smoothly scales tempo in
 *   [0.50, 1.00] using normalized cross-correlation phase search + 4.0 ms
 *   Hann crossfade, eliminating crackling/underruns even on 10 FPS spikes.
 * - Entry seeds prev_ovl with the UNPLAYED leading edge of the first block
 *   (exact C0 sample continuity with passthrough), and exit aligns via cross-
 *   correlation so engaging/disengaging is click-free.
 * -------------------------------------------------------------------------*/
#define WSOLA_OVL      192u                         /* 4.0 ms Hann crossfade */
#define WSOLA_HOP_OUT  384u                         /* 8.0 ms output hop */
#define WSOLA_SEQ      (WSOLA_HOP_OUT + WSOLA_OVL)  /* 576 frames = 12.0 ms */
#define WSOLA_SEEK     192u                         /* 4.0 ms phase search (total 768 = 3 PS3 blocks) */
#define WSOLA_IN_CAP   4096u

static struct {
    float  in_buf[WSOLA_IN_CAP * 2u];
    u32    in_len;
    float  prev_ovl[WSOLA_OVL * 2u];
    float  out_buf[WSOLA_HOP_OUT * 2u];
    float  hann[WSOLA_OVL];
    float  tempo;
    double in_frac;
    int    active;
    int    inited;
} s_wsola;

static void cellAudio_coop_tap_push(const float* stereo_48k, u32 num_frames_48k);

static void audio_ts_init(void)
{
    if (!s_wsola.inited) {
        const double pi = 3.14159265358979323846;
        for (u32 i = 0; i < WSOLA_OVL; i++)
            s_wsola.hann[i] = (float)(0.5 * (1.0 - cos(pi * (double)i / (double)WSOLA_OVL)));
        s_wsola.inited = 1;
    }
    s_wsola.in_len = 0;
    s_wsola.tempo = 1.0f;
    s_wsola.in_frac = 0.0;
    s_wsola.active = 0;
    memset(s_wsola.prev_ovl, 0, sizeof(s_wsola.prev_ovl));
}

static u32 audio_ts_queued_out_frames(void)
{
    if (!s_wsola.active || !s_wsola.in_len) return 0u;
    const float t = s_wsola.tempo < 0.50f ? 0.50f : s_wsola.tempo;
    return (u32)((double)s_wsola.in_len / (double)t);
}

static void audio_ts_emit_raw(const float* pcm, u32 frames)
{
    audio_backend_submit(pcm, frames);
    cellAudio_coop_tap_push(pcm, frames);
}

static u32 audio_ts_best_offset(const float* prev_ovl, const float* in_buf, u32 max_seek)
{
    if (max_seek == 0u) return 0u;
    /* Compute energy of prev_ovl (decimated by 4 for coarse pass). */
    float e_prev = 0.0f;
    for (u32 i = 0; i < WSOLA_OVL; i += 4u) {
        const float m = prev_ovl[i * 2u] + prev_ovl[i * 2u + 1u];
        e_prev += m * m;
    }
    if (e_prev < 1e-8f) return 0u;

    u32 best_d = 0u;
    float best_score = -2.0f;
    for (u32 d = 0; d <= max_seek; d += 4u) {
        float num = 0.0f, ec = 0.0f;
        const float* p = in_buf + d * 2u;
        for (u32 i = 0; i < WSOLA_OVL; i += 4u) {
            const float a = prev_ovl[i * 2u] + prev_ovl[i * 2u + 1u];
            const float b = p[i * 2u] + p[i * 2u + 1u];
            num += a * b;
            ec  += b * b;
        }
        const float score = (ec > 1e-8f) ? (num / sqrtf(e_prev * ec)) : 0.0f;
        if (score > best_score) { best_score = score; best_d = d; }
    }
    /* Fine refinement +/-3 around coarse peak (decimated by 2). */
    const u32 lo = best_d > 3u ? best_d - 3u : 0u;
    const u32 hi = (best_d + 3u <= max_seek) ? (best_d + 3u) : max_seek;
    float e_prev2 = 0.0f;
    for (u32 i = 0; i < WSOLA_OVL; i += 2u) {
        const float m = prev_ovl[i * 2u] + prev_ovl[i * 2u + 1u];
        e_prev2 += m * m;
    }
    best_score = -2.0f;
    for (u32 d = lo; d <= hi; d++) {
        float num = 0.0f, ec = 0.0f;
        const float* p = in_buf + d * 2u;
        for (u32 i = 0; i < WSOLA_OVL; i += 2u) {
            const float a = prev_ovl[i * 2u] + prev_ovl[i * 2u + 1u];
            const float b = p[i * 2u] + p[i * 2u + 1u];
            num += a * b;
            ec  += b * b;
        }
        const float score = (ec > 1e-8f) ? (num / sqrtf(e_prev2 * ec)) : 0.0f;
        if (score > best_score) { best_score = score; best_d = d; }
    }
    return best_d;
}

static void audio_ts_set_tempo(float new_tempo)
{
    if (new_tempo < 0.50f) new_tempo = 0.50f;
    if (new_tempo > 1.00f) new_tempo = 1.00f;
    s_wsola.tempo = new_tempo;
}

static void audio_ts_push_and_drain(const float* block_256)
{
    if (!s_wsola.inited) audio_ts_init();

    if (!s_wsola.active) {
        if (s_wsola.tempo >= 0.992f) {
            /* Zero-latency, zero-CPU passthrough at 1.00x. */
            audio_ts_emit_raw(block_256, CELL_AUDIO_BLOCK_SAMPLES);
            return;
        }
        /* Engage WSOLA seamlessly: prev_ovl is seeded with the UNPLAYED start of
         * block_256, which is exact sample-for-sample continuation of the last
         * passthrough sample, so hop 1 starts with zero discontinuity. */
        memcpy(s_wsola.prev_ovl, block_256, WSOLA_OVL * 2u * sizeof(float));
        s_wsola.active = 1;
        s_wsola.in_len = 0;
        s_wsola.in_frac = 0.0;
    }

    /* Append incoming 256-frame block into WSOLA FIFO. */
    if (s_wsola.in_len + CELL_AUDIO_BLOCK_SAMPLES > WSOLA_IN_CAP) {
        const u32 drop = (s_wsola.in_len + CELL_AUDIO_BLOCK_SAMPLES) - WSOLA_IN_CAP;
        memmove(s_wsola.in_buf, s_wsola.in_buf + drop * 2u,
                (s_wsola.in_len - drop) * 2u * sizeof(float));
        s_wsola.in_len -= drop;
    }
    memcpy(s_wsola.in_buf + s_wsola.in_len * 2u, block_256,
           CELL_AUDIO_BLOCK_SAMPLES * 2u * sizeof(float));
    s_wsola.in_len += CELL_AUDIO_BLOCK_SAMPLES;

    /* Run WSOLA synthesis hops while enough lookahead is available. */
    while (s_wsola.in_len >= WSOLA_SEQ + WSOLA_SEEK) {
        const u32 d = audio_ts_best_offset(s_wsola.prev_ovl, s_wsola.in_buf, WSOLA_SEEK);
        const float* seg = s_wsola.in_buf + d * 2u;
        for (u32 i = 0; i < WSOLA_OVL; i++) {
            const float w = s_wsola.hann[i];
            const float iw = 1.0f - w;
            s_wsola.out_buf[i * 2u + 0u] = s_wsola.prev_ovl[i * 2u + 0u] * iw + seg[i * 2u + 0u] * w;
            s_wsola.out_buf[i * 2u + 1u] = s_wsola.prev_ovl[i * 2u + 1u] * iw + seg[i * 2u + 1u] * w;
        }
        memcpy(s_wsola.out_buf + WSOLA_OVL * 2u, seg + WSOLA_OVL * 2u,
               (WSOLA_HOP_OUT - WSOLA_OVL) * 2u * sizeof(float));
        memcpy(s_wsola.prev_ovl, seg + WSOLA_HOP_OUT * 2u, WSOLA_OVL * 2u * sizeof(float));
        audio_ts_emit_raw(s_wsola.out_buf, WSOLA_HOP_OUT);

        const double step_d = (double)WSOLA_HOP_OUT * (double)s_wsola.tempo + s_wsola.in_frac;
        u32 adv = (u32)step_d;
        s_wsola.in_frac = step_d - (double)adv;
        if (adv > s_wsola.in_len) adv = s_wsola.in_len;
        if (adv > 0u) {
            memmove(s_wsola.in_buf, s_wsola.in_buf + adv * 2u,
                    (s_wsola.in_len - adv) * 2u * sizeof(float));
            s_wsola.in_len -= adv;
        }
    }

    /* Smoothly exit back to zero-latency passthrough once tempo returns to 1.00x. */
    if (s_wsola.tempo >= 0.995f) {
        if (s_wsola.in_len > 0u) {
            u32 d = 0u;
            if (s_wsola.in_len > WSOLA_OVL) {
                const u32 avail_seek = s_wsola.in_len - WSOLA_OVL;
                const u32 max_seek = avail_seek < WSOLA_SEEK ? avail_seek : WSOLA_SEEK;
                d = audio_ts_best_offset(s_wsola.prev_ovl, s_wsola.in_buf, max_seek);
            }
            const u32 rem = s_wsola.in_len - d;
            float* tail = s_wsola.in_buf + d * 2u;
            const u32 ovl = rem < WSOLA_OVL ? rem : WSOLA_OVL;
            for (u32 i = 0; i < ovl; i++) {
                const float w = s_wsola.hann[i];
                const float iw = 1.0f - w;
                tail[i * 2u + 0u] = s_wsola.prev_ovl[i * 2u + 0u] * iw + tail[i * 2u + 0u] * w;
                tail[i * 2u + 1u] = s_wsola.prev_ovl[i * 2u + 1u] * iw + tail[i * 2u + 1u] * w;
            }
            audio_ts_emit_raw(tail, rem);
            s_wsola.in_len = 0;
        } else {
            audio_ts_emit_raw(s_wsola.prev_ovl, WSOLA_OVL);
        }
        s_wsola.active = 0;
        s_wsola.in_frac = 0.0;
    }
}

#endif /* AUDIO_BACKEND_AAUDIO */

#if AUDIO_BACKEND_WASAPI

static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;

static int audio_backend_init(void)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        return -1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* Event-driven, 60 ms by default (PS3_AUDIO_BUF_MS). The old 20 ms ran
     * dry whenever the mixing thread lost the CPU for a moment -- with every
     * core busy running SPU/PPU code that was often, and each time is an
     * audible stutter (Bink cutscenes most of all). */
    REFERENCE_TIME buf_duration = 600000; /* 60 ms in 100 ns units */
    { const char* e = getenv("PS3_AUDIO_BUF_MS"); const int ms = e ? atoi(e) : 0;
      if (ms >= 10 && ms <= 500) buf_duration = (REFERENCE_TIME)ms * 10000; }
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* Shared mode only takes the device's own mix format unless the stream
     * asks the audio engine to convert: on a device not running 48 kHz stereo
     * float (a 44.1 kHz or 5.1 output) Initialize failed with
     * AUDCLNT_E_UNSUPPORTED_FORMAT and every title ran silent. */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
    const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = s_wasapi_client->lpVtbl->Initialize(
        s_wasapi_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
        buf_duration, 0, &wfx, NULL);

    if (FAILED(hr)) {
        /* Fallback: try without event callback */
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            convert, buf_duration, 0, &wfx, NULL);
        if (FAILED(hr)) {
            printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
            s_wasapi_client->lpVtbl->Release(s_wasapi_client);
            s_wasapi_client = NULL;
            CloseHandle(s_wasapi_event);
            s_wasapi_event = NULL;
            return -1;
        }
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    } else {
        s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
    }

    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        return -1;
    }

    s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    }
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_wasapi_render) return;

    UINT32 padding = 0;
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available) {
        g_audio_dropped += num_samples - available;
        num_samples = available;
    }
    if (num_samples == 0) return;

    BYTE* buf = NULL;
    HRESULT hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        /* PS3_MUTE=1: silence at the device, nothing else changes -- the
         * title still gets a working cellAudio and the device still paces
         * it, unlike PS3_NO_AUDIO, which sends the title down its no-audio
         * path. For unattended runs. */
        static int mute = -1;
        if (mute < 0) mute = getenv("PS3_MUTE") ? 1 : 0;
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples,
                                               mute ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    }
}

/* Frames the device can take right now; -1 with no device open. */
static int audio_backend_room(void)
{
    if (!s_wasapi_client) return -1;
    UINT32 padding = 0;
    const HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) {
        /* Device gone (AUDCLNT_E_DEVICE_INVALIDATED on a default-device
         * change, sleep, ...): pace on the clock rather than spin. */
        static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
            fprintf(stderr, "[cellAudio] GetCurrentPadding failed 0x%08lX -- clock pacing%c", (unsigned long)hr, 10);
        return -1;
    }
    return (int)(s_wasapi_buf_frames - padding);
}

/* Block until the device has consumed some of its buffer. */
static void audio_backend_wait(void)
{
    if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
    else Sleep(1);
}


#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Guest PCM is BIG-ENDIAN float32; the port buffer is a raw vm_base view, so
 * every sample must be byte-swapped before use. Reading it as a host float
 * turned the whole mix into denormal noise -- silence after clipping (LBP
 * had a verified end-to-end pipeline with no audible output). */
static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static inline u32 rd_raw_u32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static inline void wr_raw_u32(float* p, u32 v)
{
    memcpy(p, &v, 4);
}

static u32 s_mix_holes_total = 0;

#if AUDIO_BACKEND_AAUDIO
/* ARMSX3/RPCS3 -0.0f completion tag positions (diagonal across channels +
 * front-right marks on 8ch ports to detect stereo-in-8ch titles). */
static inline void audio_port_tag_geometry(u32 nch, u32* first_pos, u32* delta)
{
    const u32 last = nch * CELL_AUDIO_BLOCK_SAMPLES - 1u;
    *delta     = last / (AUDIO_PORT_TAG_COUNT - 1u);
    *first_pos = last % (AUDIO_PORT_TAG_COUNT - 1u);
}

static inline u32 audio_port_mark_pos(u32 nch, u32 tag_nr)
{
    const u32 delta_sample = (CELL_AUDIO_BLOCK_SAMPLES - 1u) / (AUDIO_PORT_TAG_COUNT - 1u);
    return tag_nr * delta_sample * nch + 1u; /* channel 1 = front right */
}

static void audio_port_tag_block(AudioPortSlot* port, float* blk)
{
    const u32 nch = (u32)port->param.nChannel;
    if (!blk || !nch) return;

    u32 tag_first = 0, tag_delta = 0;
    audio_port_tag_geometry(nch, &tag_first, &tag_delta);

    for (u32 tag_pos = tag_first, tag_nr = 0; tag_nr < AUDIO_PORT_TAG_COUNT; tag_pos += tag_delta, tag_nr++) {
        wr_raw_u32(&blk[tag_pos], AUDIO_NEG_ZERO_BE_BITS);
        port->last_tag_bits[tag_nr] = AUDIO_NEG_ZERO_BE_BITS;
    }

    if (nch != 2u) {
        for (u32 tag_nr = 0; tag_nr < AUDIO_PORT_TAG_COUNT; tag_nr++) {
            wr_raw_u32(&blk[audio_port_mark_pos(nch, tag_nr)], AUDIO_NEG_ZERO_BE_BITS);
        }
    }

    port->prev_touched_tag_nr = 0xFFFFFFFFu;
}

static void audio_port_tag_all_blocks(AudioPortSlot* port)
{
    if (!port || !port->buffer || !port->param.nChannel || !port->param.nBlock) return;
    const u32 nch = (u32)port->param.nChannel;
    const u32 nblock = (u32)port->param.nBlock;
    const u32 stride = CELL_AUDIO_BLOCK_SAMPLES * nch;
    for (u32 b = 0; b < nblock; b++) {
        audio_port_tag_block(port, port->buffer + b * stride);
    }
    port->periods_without_tag = 0;
    port->front_only_port = 0;
    port->front_only_reported = 0;
}

/* ARMSX3 count_port_buffer_tags(): inspects current read block of every active
 * port to classify whether guest SPU/PPU finished writing it, is mid-write,
 * or has not touched it yet. Never reads an empty/half-written block. */
static void audio_check_port_tags(u32* out_active, u32* out_in_prog,
                                  u32* out_untouched, u32* out_incomplete)
{
    u32 active = 0, in_prog = 0, untouched = 0, incomplete = 0;

    mutex_lock(&s_audio_mutex);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer) continue;
        active++;

        const u32 nch = (u32)port->param.nChannel;
        const u32 nblock = (u32)port->param.nBlock;
        if (!nch || !nblock) continue;

        const u32 block_idx = (u32)(port->read_index % nblock);
        const float* blk = port->buffer + block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;

        u32 tag_first = 0, tag_delta = 0;
        audio_port_tag_geometry(nch, &tag_first, &tag_delta);

        u32 last_touched = port->prev_touched_tag_nr;
        int retouched = 0;
        int tag_moved = 0;

        for (u32 tag_pos = tag_first, tag_nr = 0; tag_nr < AUDIO_PORT_TAG_COUNT; tag_pos += tag_delta, tag_nr++) {
            const u32 bits = rd_raw_u32(&blk[tag_pos]);
            const u32 prev = port->last_tag_bits[tag_nr];
            /* Match IEEE-754 value change OR -0.0f -> +0.0f sign-bit clear. */
            if (bits != prev && !(bits == AUDIO_NEG_ZERO_BE_BITS && prev == 0x00000000u)) {
                port->last_tag_bits[tag_nr] = bits;
                if (port->prev_touched_tag_nr != 0xFFFFFFFFu && tag_nr <= port->prev_touched_tag_nr)
                    retouched = 1;
                last_touched = tag_nr;
                tag_moved = 1;
            }
        }

        if (tag_moved) {
            port->periods_without_tag = 0;
            port->front_only_port = 0;
        }

        if (last_touched == 0xFFFFFFFFu) {
            if (!port->front_only_port && nch != 2u && port->periods_without_tag > AUDIO_FRONT_ONLY_SETTLE) {
                for (u32 tag_nr = 0; tag_nr < AUDIO_PORT_TAG_COUNT; tag_nr++) {
                    const u32 mbits = rd_raw_u32(&blk[audio_port_mark_pos(nch, tag_nr)]);
                    if (mbits != AUDIO_NEG_ZERO_BE_BITS) {
                        port->front_only_port = 1;
                        break;
                    }
                }
                if (port->front_only_port && !port->front_only_reported) {
                    port->front_only_reported = 1;
                    fprintf(stderr, "[cellAudio] Port %d carries front-channel audio only (nch=%u); tracking front-right marks\n",
                            p, nch);
                }
            }

            if (!port->front_only_port) {
                untouched++;
            } else {
                /* Front-only 8ch port: verify at least one front-right mark moved
                 * so late writes still wait cleanly instead of producing holes. */
                int any_mark = 0;
                int last_mark = 0;
                for (u32 tag_nr = 0; tag_nr < AUDIO_PORT_TAG_COUNT; tag_nr++) {
                    const u32 mbits = rd_raw_u32(&blk[audio_port_mark_pos(nch, tag_nr)]);
                    if (mbits != AUDIO_NEG_ZERO_BE_BITS) {
                        any_mark = 1;
                        if (tag_nr == AUDIO_PORT_TAG_COUNT - 1u) last_mark = 1;
                    }
                }
                if (!any_mark) untouched++;
                else if (!last_mark) in_prog++;
                else incomplete++;
            }
        } else if (last_touched == AUDIO_PORT_TAG_COUNT - 1u) {
            if (retouched) in_prog++;
            port->prev_touched_tag_nr = last_touched;
        } else if (last_touched == port->prev_touched_tag_nr) {
            if (retouched) in_prog++;
            else incomplete++;
        } else {
            in_prog++;
            port->prev_touched_tag_nr = last_touched;
        }
    }
    mutex_unlock(&s_audio_mutex);

    *out_active     = active;
    *out_in_prog    = in_prog;
    *out_untouched  = untouched;
    *out_incomplete = incomplete;
}

/* Advance read_index and re-tag current blocks without mixing (used on untouched
 * timeout and steady-silence fast path, matching ARMSX3 advance()). */
static void audio_advance_ports_silent(void)
{
    mutex_lock(&s_audio_mutex);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer) continue;
        const u32 nch = (u32)port->param.nChannel;
        const u32 nblock = (u32)port->param.nBlock;
        if (!nch || !nblock) continue;
        const u32 block_idx = (u32)(port->read_index % nblock);
        float* src = port->buffer + block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));
        audio_port_tag_block(port, src);
        if (port->periods_without_tag < 0xFFu) port->periods_without_tag++;
        port->read_index++;
        if (port->read_idx_addr)
            vm_write64((u32)port->read_idx_addr, port->read_index % nblock);
    }
    mutex_unlock(&s_audio_mutex);
}
#endif /* AUDIO_BACKEND_AAUDIO */

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        /* Diagnostic: a port that HOLDS PCM but was never started is inaudible
         * by design -- flag it once so "no PortStart" vs "no data" is decidable
         * from the log (LBP's Bink movie audio port sat exactly there). */
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (ld_be_f32(&port->buffer[s]) != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f) level = 1.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;

        /* AUDIO_PEAK diag: which blocks of this port's WHOLE ring hold data,
         * vs which block the read cursor is on -- distinguishes "writer never
         * writes" from "writer and reader cycle out of phase". */
        { static int s_pk = -1; if (s_pk < 0) s_pk = getenv("AUDIO_PEAK") ? 1 : 0;
          if (s_pk) { static _Thread_local unsigned _c;
            if ((++_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (ld_be_f32(&bp[s2]) != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d ridx=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->read_index, block_idx, map);
            } } }

#ifdef _WIN32   /* reported by the Windows mix thread's AUDIO_RATE */
        /* AUDIO_RATE: a block with nothing in it right after one with sound is a hole, the guest's audio engine late
         * with it (what the mix then plays is a click or a gap, whatever the device does). */
        { static int hole_on = -1; static u32 prev_nz[CELL_AUDIO_PORT_MAX];
          if (hole_on < 0) hole_on = getenv("AUDIO_RATE") ? 1 : 0;
          if (hole_on) { int nz = 0;
            for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 2) if (src[s2] != 0.0f) { nz = 1; break; }
            if (!nz && prev_nz[p]) g_audio_holes[p]++;
            prev_nz[p] = nz; } }
#endif

        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                left  = ld_be_f32(&src[s * nch + 0]) * level;
                right = ld_be_f32(&src[s * nch + 1]) * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = ld_be_f32(&src[s]) * level;
            }

            /* If 7.1, mix center and surround channels into stereo with
             * headroom normalization so loud action scenes do not hard-clip. */
            if (nch == 8) {
                float center = ld_be_f32(&src[s * 8 + 2]) * level * 0.50f;
                float lfe    = ld_be_f32(&src[s * 8 + 3]) * level * 0.35f;
                float rl     = ld_be_f32(&src[s * 8 + 4]) * level * 0.38f;
                float rr     = ld_be_f32(&src[s * 8 + 5]) * level * 0.38f;
                float sl     = ld_be_f32(&src[s * 8 + 6]) * level * 0.28f;
                float sr     = ld_be_f32(&src[s * 8 + 7]) * level * 0.28f;
                left  = left  * 0.72f + center + lfe + rl + sl;
                right = right * 0.72f + center + lfe + rr + sr;
            }

            s_mix_buffer[s * 2 + 0] += left;
            s_mix_buffer[s * 2 + 1] += right;
        }

        /* Firmware clears a ring-buffer block after consuming it. Besides
         * preventing stale PCM from replaying when a producer stalls, the
         * cleared block is an observable consumption signal for producers. */
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));
#if AUDIO_BACKEND_AAUDIO
        audio_port_tag_block(port, src);
        if (port->periods_without_tag < 0xFFu) port->periods_without_tag++;
#endif

        /* Keep the monotonic counter for tags/timestamps, but publish the
         * ring slot to the guest. SurMixer multiplies this value by the block
         * size directly; an unbounded counter walks beyond the audio buffer. */
        port->read_index++;
        if (port->read_idx_addr)
            vm_write64((u32)port->read_idx_addr, port->read_index % nblock);
    }

    mutex_unlock(&s_audio_mutex);

    /* Smooth C1 soft-limiter (linear up to +/-0.82, asymptotic knee to +/-0.985)
     * plus NaN/Inf guard and 1-block mirrored Packet Loss Concealment (PLC). */
    {
        static float s_mix_prev_block[CELL_AUDIO_BLOCK_SAMPLES * 2u];
        static float s_mix_prev_l = 0.0f;
        static float s_mix_prev_r = 0.0f;
        static u32   s_mix_hole_streak = 0;
        static int   s_mix_was_active  = 0;
        float block_peak = 0.0f;

        for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2u; i++) {
            float x = s_mix_buffer[i];
            if (!(x == x) || x > 1e6f || x < -1e6f) {
                x = 0.0f;
            } else if (x > 0.82f) {
                const float d = x - 0.82f;
                x = 0.82f + 0.165f * (d / (0.165f + d));
            } else if (x < -0.82f) {
                const float d = -x - 0.82f;
                x = -(0.82f + 0.165f * (d / (0.165f + d)));
            }
            s_mix_buffer[i] = x;
            const float ax = x < 0.0f ? -x : x;
            if (ax > block_peak) block_peak = ax;
        }

        if (block_peak < 1e-6f) {
            if (s_mix_was_active) {
                s_mix_hole_streak++;
                if (s_mix_hole_streak == 1u) {
                    /* First missed 5.33 ms guest block: replay the previous block
                     * time-reversed (C0-continuous at the boundary) with a gentle
                     * 0.90 -> 0.45 fade instead of dropping into 4 ms of silence. */
                    s_mix_holes_total++;
                    const float inv_n = 1.0f / (float)CELL_AUDIO_BLOCK_SAMPLES;
                    for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
                        const u32 rev = (CELL_AUDIO_BLOCK_SAMPLES - 1u) - s;
                        const float g = 0.90f - 0.45f * ((float)s * inv_n);
                        s_mix_buffer[s * 2u + 0u] = s_mix_prev_block[rev * 2u + 0u] * g;
                        s_mix_buffer[s * 2u + 1u] = s_mix_prev_block[rev * 2u + 1u] * g;
                    }
                    s_mix_prev_l = s_mix_buffer[(CELL_AUDIO_BLOCK_SAMPLES - 1u) * 2u + 0u];
                    s_mix_prev_r = s_mix_buffer[(CELL_AUDIO_BLOCK_SAMPLES - 1u) * 2u + 1u];
                } else if (s_mix_hole_streak == 2u) {
                    /* Second consecutive zero block: treat as genuine silence/pause
                     * and smoothly decay the trailing sample to zero over 64 samples. */
                    float pl = s_mix_prev_l, pr = s_mix_prev_r;
                    for (u32 s = 0; s < 64u && s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
                        pl *= 0.92f;
                        pr *= 0.92f;
                        s_mix_buffer[s * 2u + 0u] = pl;
                        s_mix_buffer[s * 2u + 1u] = pr;
                    }
                    s_mix_prev_l = 0.0f;
                    s_mix_prev_r = 0.0f;
                    s_mix_was_active = 0;
                }
            }
        } else {
            if (s_mix_hole_streak > 0u) {
                /* 32-sample (0.67 ms) crossfade from PLC/decay tail into new block. */
                const float pl = s_mix_prev_l, pr = s_mix_prev_r;
                for (u32 s = 0; s < 32u && s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
                    const float t = (float)(s + 1u) * (1.0f / 32.0f);
                    s_mix_buffer[s * 2u + 0u] = pl + (s_mix_buffer[s * 2u + 0u] - pl) * t;
                    s_mix_buffer[s * 2u + 1u] = pr + (s_mix_buffer[s * 2u + 1u] - pr) * t;
                }
                s_mix_hole_streak = 0;
            }
            memcpy(s_mix_prev_block, s_mix_buffer, sizeof(s_mix_prev_block));
            s_mix_prev_l = s_mix_buffer[(CELL_AUDIO_BLOCK_SAMPLES - 1u) * 2u + 0u];
            s_mix_prev_r = s_mix_buffer[(CELL_AUDIO_BLOCK_SAMPLES - 1u) * 2u + 1u];
            s_mix_was_active = (block_peak > 1e-4f) ? 1 : 0;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

static void audio_notify_event_queues(void)
{
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        if (qid)
            sys_event_queue_push_by_id(qid, s_notify_queues[i].key,
                                       0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0);
    }
}

#if !AUDIO_BACKEND_AAUDIO
/* The loudest sample of the block just mixed (AUDIO_PEAK). */
static float audio_mix_peak(void)
{
    float pk = 0.0f;
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        const float a = s_mix_buffer[i] < 0 ? -s_mix_buffer[i] : s_mix_buffer[i];
        if (a > pk) pk = a;
    }
    return pk;
}
#endif

/* AUDIO_WAV=<file>: append the final mix as raw f32le stereo 48 kHz
 * (ffmpeg -f f32le -ar 48000 -ac 2 -i <file>). */
static void audio_dump_mix(void)
{
    static FILE* wf = (FILE*)-1;
    if (wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); wf = e ? fopen(e, "wb") : NULL; }
    if (wf) fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, wf);
}

#ifdef _WIN32
static unsigned long long g_audio_underruns;
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    ps3_thread_name("audio mix");
    printf("[cellAudio] Mixing thread started\n");

    /* The device deadline is every few ms and the guest threads keep every
     * core busy: run this one ahead of them (MMCSS "Pro Audio" when the
     * service is there, the highest normal priority anyway). avrt.dll is
     * loaded by hand so the runtime does not need avrt.lib to link. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    { HMODULE avrt = LoadLibraryW(L"avrt.dll");
      typedef HANDLE (WINAPI *set_mm_fn)(LPCWSTR, LPDWORD);
      set_mm_fn set_mm = avrt ? (set_mm_fn)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
      DWORD task = 0;
      if (set_mm && !set_mm(L"Pro Audio", &task))
          fprintf(stderr, "[cellAudio] MMCSS Pro Audio unavailable (%lu)%c", (unsigned long)GetLastError(), 10); }

    /* Pace to the device clock. The PS3 consumes one block every 5.33 ms;
     * the old Sleep(2)/Sleep(5) loop keyed on a queue depth the 1056-frame
     * WASAPI buffer can never reach, so it mixed ~225 blocks/s and the device
     * dropped the surplus: 20% of the audio thrown away, and a guest mixer
     * (GH3's FMOD) that could not keep ahead of the read index played gaps.
     * With no device, pace on the performance counter instead. */
    LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    unsigned long long blocks = 0;
    while (s_mix_thread_running) {
        const int room = audio_backend_room();
        if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) { audio_backend_wait(); continue; }
#if AUDIO_BACKEND_WASAPI
        /* Device queue under half a block: it is about to play silence (an
         * audible click/stutter). AUDIO_RATE reports how often. */
        if (room >= 0 && blocks > 64 &&
            (UINT32)room + CELL_AUDIO_BLOCK_SAMPLES / 2 >= s_wasapi_buf_frames) g_audio_underruns++;
#endif
        if (room < 0) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const long long due = q0.QuadPart +
                (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
            if (now.QuadPart < due) { Sleep(1); continue; }
            /* More than 50 ms behind (device just went away, a debugger
             * stop): drop the backlog instead of mixing it in a burst. */
            if (now.QuadPart - due > qf.QuadPart / 20)
                q0.QuadPart = now.QuadPart -
                    (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
        }
        blocks++;
        /* Mix and submit one block */
        audio_mix_one_block();
        /* AUDIO_PEAK=1: report the mixed block's peak amplitude periodically so
         * "is any port producing sound" is answerable (LBP Bink movie audio). */
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; const float pk = audio_mix_peak();
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        audio_dump_mix();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        /* AUDIO_RATE=1: blocks mixed vs real time, and frames dropped. */
        { static int on = -1; if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
          if (on) { static ULONGLONG t0; static unsigned n, z; static unsigned long long d0, u0;
            ULONGLONG now = GetTickCount64(); if (!t0) t0 = now; n++;
            { int nz = 0; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) if (s_mix_buffer[i] != 0.0f) { nz = 1; break; }
              if (!nz) z++; }
            if (now - t0 >= 5000) {
                fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, dropped %.0f frames/s, underruns %llu, device buf %u, holes %u+%u%c",
                        n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0, (g_audio_dropped - d0) * 1000.0 / (now - t0),
                        g_audio_underruns - u0,
                #if AUDIO_BACKEND_WASAPI
                        (unsigned)s_wasapi_buf_frames, g_audio_holes[0], g_audio_holes[1], 10);
#else
                        0u, g_audio_holes[0], g_audio_holes[1], 10);
#endif
                g_audio_holes[0] = g_audio_holes[1] = 0;
                n = z = 0; d0 = g_audio_dropped; u0 = g_audio_underruns; t0 = now; } } }

        /* Notify event queues */
        audio_notify_event_queues();
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return 0;
}
#else
/* ---------------------------------------------------------------------------
 * Lock-free 24 kHz Stereo IMA-ADPCM Audio Tap for Dual-Screen / Online Co-op
 * Zero overhead when disabled (single relaxed atomic check per mix block).
 * -------------------------------------------------------------------------*/
#define COOP_AUDIO_RING_FRAMES 8192u  /* ~341 ms at 24 kHz */
#define COOP_AUDIO_RING_MASK   (COOP_AUDIO_RING_FRAMES - 1u)
#define COOP_AUDIO_CHUNK_FRAMES 512u  /* 21.33 ms -> 8 + 512 = 520 bytes */

static volatile int  s_coop_audio_enabled = 0;
static s16           s_coop_audio_ring[COOP_AUDIO_RING_FRAMES * 2u];
static volatile u32  s_coop_audio_rpos = 0;
static volatile u32  s_coop_audio_wpos = 0;
static s32           s_coop_adpcm_pred[2] = { 0, 0 };
static s32           s_coop_adpcm_idx[2]  = { 0, 0 };

static const s16 k_ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767
};

static const s8 k_ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline s16 coop_f32_to_s16(float v)
{
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    s32 s = (s32)(v * 32767.0f);
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    return (s16)s;
}

static inline u8 coop_ima_encode_sample(s16 sample, s32* pred, s32* idx)
{
    s32 p = *pred;
    s32 i = *idx;
    s32 step = k_ima_step_table[i];
    s32 diff = (s32)sample - p;
    u8 code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    s32 delta = step >> 3;
    if (diff >= step)       { code |= 4; diff -= step;      delta += step; }
    if (diff >= (step >> 1)){ code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= (step >> 2)){ code |= 1;                    delta += step >> 2; }
    if (code & 8) p -= delta;
    else          p += delta;
    if (p >  32767) p =  32767;
    if (p < -32768) p = -32768;
    i += k_ima_index_table[code];
    if (i < 0)  i = 0;
    if (i > 88) i = 88;
    *pred = p;
    *idx  = i;
    return code;
}

static void cellAudio_coop_tap_push(const float* stereo_48k, u32 num_frames_48k)
{
    if (!__atomic_load_n(&s_coop_audio_enabled, __ATOMIC_RELAXED)) return;
    if (!stereo_48k || num_frames_48k < 2u) return;

    const u32 out_frames = num_frames_48k >> 1; /* 2:1 box-filtered 48kHz -> 24kHz */
    u32 w = __atomic_load_n(&s_coop_audio_wpos, __ATOMIC_RELAXED);
    const u32 r = __atomic_load_n(&s_coop_audio_rpos, __ATOMIC_ACQUIRE);
    const u32 free_frames = COOP_AUDIO_RING_FRAMES - (w - r);
    const u32 put = (out_frames < free_frames) ? out_frames : free_frames;

    for (u32 i = 0; i < put; i++) {
        const u32 si = i * 4u;
        const float l = 0.5f * (stereo_48k[si + 0u] + stereo_48k[si + 2u]);
        const float rch = 0.5f * (stereo_48k[si + 1u] + stereo_48k[si + 3u]);
        const u32 di = ((w + i) & COOP_AUDIO_RING_MASK) * 2u;
        s_coop_audio_ring[di + 0u] = coop_f32_to_s16(l);
        s_coop_audio_ring[di + 1u] = coop_f32_to_s16(rch);
    }
    __atomic_store_n(&s_coop_audio_wpos, w + put, __ATOMIC_RELEASE);
}

void cellAudio_coop_set_enabled(int enabled)
{
    if (enabled) {
        u32 w = __atomic_load_n(&s_coop_audio_wpos, __ATOMIC_ACQUIRE);
        __atomic_store_n(&s_coop_audio_rpos, w, __ATOMIC_RELEASE);
        s_coop_adpcm_pred[0] = s_coop_adpcm_pred[1] = 0;
        s_coop_adpcm_idx[0]  = s_coop_adpcm_idx[1]  = 0;
        __atomic_store_n(&s_coop_audio_enabled, 1, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&s_coop_audio_enabled, 0, __ATOMIC_RELEASE);
    }
}

u32 cellAudio_coop_read_adpcm(u8* out_buf, u32 max_bytes, u32* out_frames)
{
    if (out_frames) *out_frames = 0;
    if (!out_buf || max_bytes < (8u + COOP_AUDIO_CHUNK_FRAMES)) return 0;
    if (!__atomic_load_n(&s_coop_audio_enabled, __ATOMIC_ACQUIRE)) return 0;

    u32 r = __atomic_load_n(&s_coop_audio_rpos, __ATOMIC_RELAXED);
    const u32 w = __atomic_load_n(&s_coop_audio_wpos, __ATOMIC_ACQUIRE);
    u32 avail = w - r;
    if (avail < COOP_AUDIO_CHUNK_FRAMES) return 0;

    /* If consumer fell >120 ms behind (network stall), fast-forward to freshest audio */
    if (avail > COOP_AUDIO_CHUNK_FRAMES * 6u) {
        r = w - COOP_AUDIO_CHUNK_FRAMES * 2u;
    }

    s32 pL = s_coop_adpcm_pred[0], iL = s_coop_adpcm_idx[0];
    s32 pR = s_coop_adpcm_pred[1], iR = s_coop_adpcm_idx[1];

    /* 8-byte block header: [s16LE predL, u8 idxL, 0, s16LE predR, u8 idxR, 0] */
    out_buf[0] = (u8)(pL & 0xFF);
    out_buf[1] = (u8)((pL >> 8) & 0xFF);
    out_buf[2] = (u8)(iL & 0xFF);
    out_buf[3] = 0;
    out_buf[4] = (u8)(pR & 0xFF);
    out_buf[5] = (u8)((pR >> 8) & 0xFF);
    out_buf[6] = (u8)(iR & 0xFF);
    out_buf[7] = 0;

    for (u32 n = 0; n < COOP_AUDIO_CHUNK_FRAMES; n++) {
        const u32 si = ((r + n) & COOP_AUDIO_RING_MASK) * 2u;
        const u8 cL = coop_ima_encode_sample(s_coop_audio_ring[si + 0u], &pL, &iL);
        const u8 cR = coop_ima_encode_sample(s_coop_audio_ring[si + 1u], &pR, &iR);
        out_buf[8u + n] = (u8)((cL & 0x0Fu) | ((cR & 0x0Fu) << 4));
    }

    s_coop_adpcm_pred[0] = pL; s_coop_adpcm_idx[0] = iL;
    s_coop_adpcm_pred[1] = pR; s_coop_adpcm_idx[1] = iR;
    __atomic_store_n(&s_coop_audio_rpos, r + COOP_AUDIO_CHUNK_FRAMES, __ATOMIC_RELEASE);

    if (out_frames) *out_frames = COOP_AUDIO_CHUNK_FRAMES;
    return 8u + COOP_AUDIO_CHUNK_FRAMES;
}

#if AUDIO_BACKEND_AAUDIO
static inline u64 audio_clock_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL;
}

static inline void audio_sleep_until_us(u64 target_us)
{
    const struct timespec dl = {
        (time_t)(target_us / 1000000ULL),
        (long)((target_us % 1000000ULL) * 1000ULL)
    };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL) == EINTR) {}
}

static inline void audio_sleep_rel_us(u64 dur_us)
{
    audio_sleep_until_us(audio_clock_us() + dur_us);
}

static void audio_pin_fast_cores(void)
{
#if defined(__linux__)
    unsigned long freqs[16];
    unsigned long max_freq = 0;
    int n_cpus = 0;
    for (int i = 0; i < 16; i++) {
        freqs[i] = 0;
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        unsigned long khz = 0;
        if (fscanf(f, "%lu", &khz) == 1 && khz > 0) {
            freqs[i] = khz;
            if (khz > max_freq) max_freq = khz;
            n_cpus = i + 1;
        }
        fclose(f);
    }
    if (max_freq == 0 || n_cpus < 3) return;
    const unsigned long thresh = (max_freq * 3u) / 4u;
    cpu_set_t set;
    CPU_ZERO(&set);
    int fast_cnt = 0;
    for (int i = 0; i < n_cpus; i++) {
        if (freqs[i] >= thresh) {
            CPU_SET(i, &set);
            fast_cnt++;
        }
    }
    if (fast_cnt >= 2 && sched_setaffinity(0, sizeof(set), &set) == 0) {
        fprintf(stderr, "[cellAudio] mix thread pinned to %d fast core(s) (>=%lu MHz)\n",
                fast_cnt, thresh / 1000ul);
    }
#endif
}

static inline void audio_note_untouched(u32 count, u32 ports,
                                        u32* untouched_expected,
                                        u32* untouched_below_streak)
{
    if (*untouched_expected > ports) *untouched_expected = ports;
    const u32 capped = count < ports ? count : ports;
    if (capped >= *untouched_expected || ++(*untouched_below_streak) >= 8u) {
        *untouched_expected = capped;
        *untouched_below_streak = 0;
    }
}

static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    ps3_thread_name("audio mix");
#if defined(__linux__)
    if (setpriority(PRIO_PROCESS, 0, -16) != 0)
        (void)setpriority(PRIO_PROCESS, 0, -10);
#endif
    audio_pin_fast_cores();
    audio_ts_init();

    float ts_thresh = 0.75f;
    {
        const char* e = getenv("PS3_AUDIO_TS_THRESH");
        if (e && *e) {
            const int v = atoi(e);
            if (v <= 0) ts_thresh = 0.0f;
            else if (v <= 95) ts_thresh = (float)v * 0.01f;
        }
    }

    const u64 block_period_us = 5333ULL; /* 256 / 48000 s */
    const u64 min_period_us   = 2666ULL; /* 50% nominal (ARMSX3) */
    const u64 max_period_us   = 6399ULL; /* 120% nominal (ARMSX3) */
    const float avg_alpha     = 0.02f;
    static const float zero_block[CELL_AUDIO_BLOCK_SAMPLES * 2u] = { 0 };

    u64 last_period_end_us = audio_clock_us();
    u64 last_report_us     = last_period_end_us;
    float avg_playtime_us  = (float)s_aa_desired_ms * 1000.0f;
    u32 untouched_expected = 0;
    u32 untouched_below_streak = 0;
    u32 loop_count = 0;
    u32 waited_tags = 0;
    u32 timedout_tags = 0;
    u32 last_ur = __atomic_load_n(&s_aa_underruns, __ATOMIC_RELAXED);
    u32 last_holes = s_mix_holes_total;
    int backend_failed = 0;

    printf("[cellAudio] Mixing thread started (ARMSX3 dynamic period + -0.0f tags + WSOLA thresh=%.0f%%)\n",
           (double)ts_thresh * 100.0);

    while (s_mix_thread_running) {
        loop_count++;

        /* Periodic AAudio disconnect/error recovery (e.g. headphones/BT switch). */
        const u32 check_mask = backend_failed ? 255u : 63u;
        if ((loop_count & check_mask) == 0u) {
            const int op = audio_backend_is_operational();
            if (!op) {
                if (!backend_failed) {
                    fprintf(stderr, "[cellAudio] AAudio backend stopped/disconnected; attempting auto-recovery...\n");
                    backend_failed = 1;
                }
                audio_backend_shutdown();
                if (audio_backend_open_aaudio(0) == 0 && audio_backend_is_operational()) {
                    fprintf(stderr, "[cellAudio] AAudio backend recovered\n");
                    backend_failed = 0;
                }
            } else if (backend_failed) {
                fprintf(stderr, "[cellAudio] AAudio backend recovered\n");
                backend_failed = 0;
            }
        }

        const u64 now_us = audio_clock_us();
        const u64 time_since_last = now_us >= last_period_end_us ? (now_us - last_period_end_us) : 0ULL;
        const u64 desired_us = (u64)s_aa_desired_ms * 1000ULL;
        const int has_stream = (s_aa_stream != NULL);

        const u32 enqueued_frames = has_stream
            ? (audio_backend_queued_samples() + audio_ts_queued_out_frames())
            : 0u;
        const u64 enqueued_playtime_us = ((u64)enqueued_frames * 1000000ULL) / CELL_AUDIO_SAMPLE_RATE;

        u32 active_ports = 0, in_prog = 0, untouched = 0, incomplete = 0;
        audio_check_port_tags(&active_ports, &in_prog, &untouched, &incomplete);
        (void)incomplete;

        /* ARMSX3 fixed rolling average ratio (avg_playtime_us / desired_us)
         * widens the effective target buffer when sustained stalls drain the ring. */
        const float avg_ratio = (has_stream && desired_us) ? (avg_playtime_us / (float)desired_us) : 1.0f;
        float desired_adj = (float)desired_us + ((float)block_period_us * 0.5f);
        if (avg_ratio < 1.0f) {
            const float denom = avg_ratio > 0.35f ? avg_ratio : 0.35f;
            desired_adj /= denom;
            if (desired_adj > 160000.0f) desired_adj = 160000.0f;
        }

        const float rate = (has_stream && desired_adj > 1.0f)
            ? ((float)enqueued_playtime_us / desired_adj) : 1.0f;

        /* ARMSX3 adaptive WSOLA time-stretching (engaged only below threshold). */
        if (has_stream && ts_thresh > 0.0f && active_ports > 0u) {
            if (rate < ts_thresh) {
                const float norm = rate / ts_thresh;
                float req = (norm + s_wsola.tempo) * 0.5f;
                if (req < 0.50f) req = 0.50f;
                if (fabsf(req - s_wsola.tempo) > 0.025f)
                    audio_ts_set_tempo(req);
            } else if (s_wsola.tempo < 1.0f) {
                float next_t = s_wsola.tempo + 0.03f;
                if (next_t >= 0.995f) next_t = 1.0f;
                audio_ts_set_tempo(next_t);
            }
        } else if (s_wsola.tempo < 1.0f) {
            audio_ts_set_tempo(1.0f);
        }

        /* ARMSX3 dynamic block period controller ([50%, 120%] of 5.33 ms). */
        u64 dyn_period_us;
        if (!has_stream) {
            dyn_period_us = block_period_us;
        } else if (rate >= 1.0f) {
            const float mult = 1.0f / rate;
            dyn_period_us = max_period_us - (u64)((float)(max_period_us - block_period_us) * mult);
        } else {
            const float mult = rate * rate;
            dyn_period_us = min_period_us + (u64)((float)(block_period_us - min_period_us) * mult);
        }

        /* ARMSX3 10-second buffer telemetry (delta underruns/holes, not boot totals). */
        if (now_us >= last_report_us + 10000000ULL) {
            last_report_us = now_us;
            const u32 ur_now = __atomic_load_n(&s_aa_underruns, __ATOMIC_RELAXED);
            const u32 ur_delta = ur_now >= last_ur ? (ur_now - last_ur) : ur_now;
            last_ur = ur_now;
            const u32 holes_now = s_mix_holes_total;
            const u32 holes_delta = holes_now - last_holes;
            last_holes = holes_now;
            fprintf(stderr,
                    "[cellAudio] Audio buffer: queued=%.1fms target=%.1fms (%.0f%%) period=%.0f%% "
                    "ports=%u untouched=%u avg=%.2f tempo=%.2f underruns=%u holes=%u waited=%u timeout=%u dropped=%llu\n",
                    (double)enqueued_playtime_us / 1000.0,
                    (double)desired_adj / 1000.0,
                    (double)rate * 100.0,
                    (double)dyn_period_us * 100.0 / (double)block_period_us,
                    active_ports, untouched, (double)avg_ratio, (double)s_wsola.tempo,
                    ur_delta, holes_delta, waited_tags, timedout_tags,
                    (unsigned long long)g_audio_dropped);
            waited_tags = 0;
            timedout_tags = 0;
            {
                extern void android_watchdog_poll(void) __attribute__((weak));
                if (android_watchdog_poll) android_watchdog_poll();
            }
        }

        if (time_since_last + 150ULL < dyn_period_us) {
            audio_sleep_until_us(last_period_end_us + dyn_period_us - 100ULL);
            continue;
        }

        /* Fast path: 0 active ports -> feed silence at nominal cadence. */
        if (active_ports == 0u) {
            audio_ts_push_and_drain(zero_block);
            untouched_expected = 0;
            untouched_below_streak = 0;
            last_period_end_us = audio_clock_us();
            const u64 post_us = ((u64)(audio_backend_queued_samples() + audio_ts_queued_out_frames()) * 1000000ULL) / CELL_AUDIO_SAMPLE_RATE;
            avg_playtime_us = avg_alpha * (float)post_us + (1.0f - avg_alpha) * avg_playtime_us;
            audio_notify_event_queues();
            continue;
        }

        /* ARMSX3 -0.0f completion tag wait: never read a block before task_audio writes it. */
        if (untouched > untouched_expected) {
            const u64 fully_timeout = (enqueued_playtime_us >= 20000ULL)
                                    ? (block_period_us * 3ULL) : (block_period_us * 2ULL);
            const u64 partial_timeout = block_period_us * 4ULL;
            if ((untouched == active_ports && time_since_last > fully_timeout) ||
                (time_since_last > partial_timeout)) {
                /* Timed out waiting for guest: advance read_index WITHOUT pushing
                 * a silence gap into AAudio (ARMSX3 cellAudio.cpp:1199). */
                timedout_tags++;
                audio_note_untouched(untouched, active_ports, &untouched_expected, &untouched_below_streak);
                audio_advance_ports_silent();
                last_period_end_us = audio_clock_us();
                const u64 post_us = ((u64)(audio_backend_queued_samples() + audio_ts_queued_out_frames()) * 1000000ULL) / CELL_AUDIO_SAMPLE_RATE;
                avg_playtime_us = avg_alpha * (float)post_us + (1.0f - avg_alpha) * avg_playtime_us;
                audio_notify_event_queues();
                continue;
            }
            waited_tags++;
            audio_sleep_rel_us(1000ULL);
            continue;
        }

        /* Fast path: steady silence (e.g. pause menu / loading screen). */
        if (untouched == active_ports) {
            audio_ts_push_and_drain(zero_block);
            audio_note_untouched(untouched, active_ports, &untouched_expected, &untouched_below_streak);
            audio_advance_ports_silent();
            last_period_end_us = audio_clock_us();
            const u64 post_us = ((u64)(audio_backend_queued_samples() + audio_ts_queued_out_frames()) * 1000000ULL) / CELL_AUDIO_SAMPLE_RATE;
            avg_playtime_us = avg_alpha * (float)post_us + (1.0f - avg_alpha) * avg_playtime_us;
            audio_notify_event_queues();
            continue;
        }

        /* Wait briefly if a port is mid-write (prevents half-written block crackles). */
        if (in_prog > 0u && time_since_last <= block_period_us * 4ULL) {
            waited_tags++;
            audio_sleep_rel_us(500ULL);
            continue;
        }

        audio_note_untouched(untouched, active_ports, &untouched_expected, &untouched_below_streak);
        audio_mix_one_block();
        audio_dump_mix();
        audio_ts_push_and_drain(s_mix_buffer);
        audio_notify_event_queues();
        last_period_end_us = audio_clock_us();
        {
            const u64 post_us = ((u64)(audio_backend_queued_samples() + audio_ts_queued_out_frames()) * 1000000ULL) / CELL_AUDIO_SAMPLE_RATE;
            avg_playtime_us = avg_alpha * (float)post_us + (1.0f - avg_alpha) * avg_playtime_us;
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    ps3_thread_name("audio mix");
#if defined(__linux__)
    if (setpriority(PRIO_PROCESS, 0, -16) != 0)
        (void)setpriority(PRIO_PROCESS, 0, -10);
#endif
    printf("[cellAudio] Mixing thread started\n");

    /* One block per guest audio period on the monotonic clock, the device's
     * queue only guarding against the drift between its clock and ours
     * (audio_pace.h). A fixed sleep after each block left the rate to the
     * sleep's overshoot (197 blocks/s on Linux against the PS3's 187.5);
     * pacing on the queue alone turned the guest's events into pairs on a
     * device that pulls two blocks at once. */
    const uint64_t ns_per_second = 1000000000ull;
    struct timespec clock;
    clock_gettime(CLOCK_MONOTONIC, &clock);
    uint64_t now_ns = (uint64_t)clock.tv_sec * ns_per_second + (uint64_t)clock.tv_nsec;

    audio_pace_state pacer;
    audio_pace_init(&pacer, now_ns);

    unsigned long blocks = 0; float peak = 0.0f;
    while (s_mix_thread_running) {
        clock_gettime(CLOCK_MONOTONIC, &clock);
        now_ns = (uint64_t)clock.tv_sec * ns_per_second + (uint64_t)clock.tv_nsec;

        /* room() answers -1 while no device is open; the queue only guards the
         * drift of a device that is. */
        const int has_device = audio_backend_room() >= 0;
        const u32 queued_blocks = has_device
            ? audio_backend_queued_samples() / CELL_AUDIO_BLOCK_SAMPLES : 0;

        const audio_pace_decision dec = audio_pace_decide(&pacer, now_ns, queued_blocks, has_device != 0);

        if (dec.action == AUDIO_PACE_ACTION_SLEEP) {
#if defined(__linux__)
            const struct timespec deadline = { (time_t)(dec.deadline_ns / ns_per_second),
                                               (long)(dec.deadline_ns % ns_per_second) };
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {}
#else
            /* macOS has no clock_nanosleep and coalesces its timers, so this
             * sleeps the remaining time; the cadence is held by the deadline
             * arithmetic, not by the sleep's precision. */
            const struct timespec remain = { (time_t)(dec.sleep_ns / ns_per_second),
                                             (long)(dec.sleep_ns % ns_per_second) };
            nanosleep(&remain, NULL);
#endif
            continue;
        }
        if (dec.action == AUDIO_PACE_ACTION_SKIP) continue;   /* the queue is at HI */

        audio_mix_one_block();
        audio_dump_mix();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        cellAudio_coop_tap_push(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();
        /* AUDIO_PEAK=1: the loudest sample of every second of mixed output (187.5 blocks of 256). */
        { static int pk_on = -1; if (pk_on < 0) pk_on = getenv("AUDIO_PEAK") ? 1 : 0;
          if (pk_on) {
              const float pk = audio_mix_peak(); if (pk > peak) peak = pk;
              if (++blocks % 188 == 0) { fprintf(stderr, "[audio-peak] %lu s: peak %.4f\n", blocks / 188, peak); peak = 0.0f; }
          } }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#endif /* AUDIO_BACKEND_AAUDIO */
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
    s_mix_thread_live = 1;
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    /* Same guard. Init leaves s_audio_initialized set even when the mixing
     * thread failed to start ("continuing silently" -- a title without audio
     * still runs), so Quit reaches here with s_mix_thread never assigned.
     * pthread_t has no null value to test, and joining an unset one, or
     * joining the same one twice, is undefined rather than an error the way
     * WaitForSingleObject on a null HANDLE is. */
    if (s_mix_thread_live) {
        pthread_join(s_mix_thread, NULL);
        s_mix_thread_live = 0;
    }
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    /* PS3_NO_AUDIO=1: report that the audio library is unavailable. Middleware
     * that drives its mixer through SPU jobs will otherwise block the whole boot
     * waiting on work we cannot yet complete; failing here lets a title take its
     * silent path and keep going. Diagnostic first, workaround second. */
    if (getenv("PS3_NO_AUDIO")) {
        printf("[cellAudio] PS3_NO_AUDIO -- reporting audio unavailable\n");
        return (s32)CELL_AUDIO_ERROR_NOT_INIT;
    }
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
        /* Don't fail -- games should still run without audio */
    }


#ifdef _WIN32
    { LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
      s_audio_start_us = (u64)(c.QuadPart / f.QuadPart) * 1000000ULL +
          (u64)(c.QuadPart % f.QuadPart) * 1000000ULL / (u64)f.QuadPart; }
#else
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      s_audio_start_us = (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL; }
#endif
    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* param / portNum are GUEST addresses; translate, and read the BE u64 param
     * fields via vm_read64 (a raw param->nChannel faults / is host-endian). */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store the decoded (host-endian) param values, not the raw BE guest bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->read_index  = 0;
    port->write_index = 0;

    /* Allocate the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Bump from a free main-memory window; the
     * host-side `buffer` is just the vm_base view of the same bytes. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    /* Each port reuses its own two pages in the runtime-owned audio window. */
    _Static_assert(CELL_AUDIO_PORT_MAX * VM_AUDIO_PORT_SIZE <= VM_AUDIO_SIZE,
                   "Audio port storage exceeds its reserved VM window");
    u32 guest_buf = VM_AUDIO_BASE + (u32)found * VM_AUDIO_PORT_SIZE;
    u32 guest_ridx = guest_buf + 0x100000u;
    if (vm_commit(guest_buf, VM_AUDIO_PORT_SIZE) != CELL_OK) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
#if AUDIO_BACKEND_AAUDIO
    audio_port_tag_all_blocks(port);
#endif
    vm_write64(guest_ridx, 0);                        /* read index counter (u64, see below) */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (reserved per port in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
#if AUDIO_BACKEND_AAUDIO
    s_ports[portNum].periods_without_tag = 0;
    s_ports[portNum].front_only_port = 0;
    s_ports[portNum].front_only_reported = 0;
    s_ports[portNum].prev_touched_tag_nr = 0xFFFFFFFFu;
    for (u32 t = 0; t < AUDIO_PORT_TAG_COUNT; t++)
        s_ports[portNum].last_tag_bits[t] = AUDIO_NEG_ZERO_BE_BITS;
#endif
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    uint32_t tag_ea = (uint32_t)(uintptr_t)tag;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!tag_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    u64 nblk = port->param.nBlock ? port->param.nBlock : 1;
    if (blockNo >= nblk) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }
    u64 t = port->read_index + blockNo - (port->read_index % nblk);
    if (blockNo < port->read_index % nblk) t += nblk;
    mutex_unlock(&s_audio_mutex);

    vm_write64(tag_ea, t);
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    uint32_t stamp_ea = (uint32_t)(uintptr_t)stamp;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!stamp_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    u64 global_counter = s_ports[portNum].read_index;
    mutex_unlock(&s_audio_mutex);
    if (tag > global_counter)
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;

    u64 t = s_audio_start_us + tag * 256000000ULL / 48000ULL;
    vm_write64(stamp_ea, t);
    return CELL_OK;
}

/* cellAudioCreateNotifyEventQueue(sys_event_queue_t* id, u64* key)
 *
 * Creates the queue the game will block on for audio-period events, and
 * registers it for notification in one step -- the game never calls
 * SetNotifyEventQueue for this one.
 *
 * This was missing entirely. YDKJ imports it, got the unimplemented-NID path,
 * and then blocked on an id nothing had created: sys_event_queue_receive
 * answered CELL_ESRCH every time and the title logged 1.7 MILLION
 * "ERROR - LIBAUDIO DROPOUT" lines in a 60 s run.
 *
 * Both params are GUEST addresses and both are big-endian out-values; the id
 * is 32-bit and the key 64-bit, so they cannot be written with a plain store. */
s32 cellAudioCreateNotifyEventQueue(u32* id, u64* key)
{
    uint32_t id_ea  = (uint32_t)(uintptr_t)id;
    uint32_t key_ea = (uint32_t)(uintptr_t)key;

    printf("[cellAudio] CreateNotifyEventQueue(id_ea=0x%08X key_ea=0x%08X)\n",
           id_ea, key_ea);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (!id_ea || !key_ea)
        return CELL_AUDIO_ERROR_PARAM;

    /* A distinct key per queue: the notify path resolves queues BY KEY, and the
     * game creates its other queues with key 0, so reusing 0 here would make
     * find_queue_by_key ambiguous and post audio events to whichever matched. */
    static uint64_t s_next_key = 0x8000000000000001ull;
    uint64_t k = s_next_key++;

    uint32_t qid = sys_event_queue_create_direct(k, 8);
    if (!qid)
        return CELL_AUDIO_ERROR_EVENT_QUEUE;

    mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { mutex_unlock(&s_audio_mutex); return CELL_AUDIO_ERROR_PARAM; }
    s_notify_queues[slot].in_use = 1;
    s_notify_queues[slot].key    = k;
    mutex_unlock(&s_audio_mutex);

    vm_write32(id_ea, qid);
    vm_write64(key_ea, k);
    printf("[cellAudio] CreateNotifyEventQueue -> id=%u key=0x%llX\n",
           qid, (unsigned long long)k);
    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    /* `config` is a GUEST address; write the BE struct field-by-field at its
     * guest offsets (8-byte aligned u64s). */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    /* CellAudioPortConfig (cell/audio.h). sys_addr_t is uintptr_t, which on the
     * 32-bit PPU ABI is FOUR bytes -- not eight. Writing the two sys_addr_t
     * fields as u64 shifted every field past it and left the game reading the
     * high (zero) half of readIndexAddr as its pointer:
     *   sys_addr_t readIndexAddr;  u32 @  0
     *   uint32_t   status;         u32 @  4
     *   uint64_t   nChannel;       u64 @  8
     *   uint64_t   nBlock;         u64 @ 16
     *   uint32_t   portSize;       u32 @ 24
     *   sys_addr_t portAddr;       u32 @ 28      -> 32 bytes */
    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                             /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                               /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                   /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                             /* portAddr */

    { static int _n = 0; if (PS3_LOG_FIRST(_n, 24))
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_CLOSE);
    } else if (s_ports[portNum].running) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_RUN);
    } else {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_READY);
    }

    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
