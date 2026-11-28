# LEGO® Marvel Super Heroes (`BLUS-31161`) — Native Android ARM64 Port (OBB Edition)

Native Android ARM64 static-recompilation port of **LEGO® Marvel Super Heroes (USA / `BLUS31161`)** built on [`ps3recomp`](ps3recomp/) with **Vulkan 1.1/1.3**, **GPU Lossless Scaling**, **GPU Optical-Flow 2× Frame Generation**, **Anti-Crackle AAudio**, **Dual-Screen 2-Phone Online & LAN Co-op**, **Full 2-Player USB/Bluetooth Gamepad Support**, **Low-End Mode (360p)**, and **Zero-Copy Android ZIP64 OBB Mounting**.

> **Credit:** **by ArifGans** — Telegram: [t.me/Apa_Itu_Telegram](https://t.me/Apa_Itu_Telegram)

---

## Features (`v0.2.7-BLUS31161`)

- **ARMSX3-Inspired AAudio Engine & Anti-Crackle Audio (Zero Underruns / Zero Holes):**
  - **`AAudio` Tuning ala ARMSX3 `OboeBackend`:** Mode `LOW_LATENCY` default (opsi `PS3_AUDIO_RECORDING_COMPAT=1` untuk screen-recording), `SHARING_MODE_SHARED`, `USAGE_GAME`, sizing buffer whole-burst (`PS3_AUDIO_BUF_MS=64`), verifikasi sample rate, dan auto-recovery saat error/ganti headset Bluetooth.
  - **$-0.0f$ Port Completion Tagging + Dynamic Period Controller:** 6 diagonal tag + mark front-right 8ch + hysteresis 8 periode mencegah mixer membaca blok PCM yang belum/setengah ditulis SPU `task_audio`, dipadukan dengan pengendali periode dinamis `[50%, 120%]` dari `5.33 ms` + target adaptif `avg_playtime / desired_us`.
  - **Zero-Allocation Stereo WSOLA Time-Stretcher:** Passthrough 0 ms / 0% CPU saat normal (`1.00×`), dan otomatis melambatkan tempo secara halus (`0.50×–1.00×`, tanpa merubah pitch, transisi masuk/keluar C0-continuous bebas klik) saat buffer turun di bawah `75%` ketika FPS drop berat di map hujan.
  - **4 Plugin DSP MultiStream Lifted Native (`--reloc`) + SPU Fast Path:** 4 plugin DSP audio runtime-DMA (`dsp_615f0131`, `dsp_16bab42d`, `dsp_01f7d591`, `dsp_ffdf6c0a`) dijalankan native NEON tanpa fallback interpreter, dipercepat dengan O(1) `spu_lifted_lookup` `musttail`, O(1) `t_reloc_memo` + single-pass `16 B` scan agar dcache tetap hangat, serta big.LITTLE fast-core pinning (`≥75%` peak freq) + prioritas tinggi (`nice -16` mixer / `-12` `task_audio`).
- **Perbaikan Anti-Stuck / Anti-Freeze Android & Vulkan:**
  - **GCM VBlank/Flip Pump di Syscall Wait Android:** Slicing `4 ms` + `ppu_gcm_pump()` pada `sys_semaphore_wait`, `sys_event_flag_wait`, dan `sys_event_queue_receive` mencegah deadlock callback VBlank/Flip saat semua thread PPU sedang menunggu.
  - **Bounded Vulkan Acquire & Fence Recovery + `[WATCHDOG]`:** Timeout `500 ms` pada `vkAcquireNextImageKHR` dengan auto-recreate swapchain, `2 s` logged `vkWaitForFences`, dan zero-overhead `[WATCHDOG]` dumper (RSX/GCM/SPURS/syscall).
- **Optimasi Map Hujan & Anti-Stutter Cutscene (Adreno / Vulkan — Terinspirasi ARMSX3):**
  - **Static-Only GPU-Resident Persistent VBO/IBO Cache (`s_gpuvc`):** Hanya mempromosikan mesh statis (`≥2` frame reuse, hingga `64 MB VB + 24 MB IB` pada RAM ≥6 GB) agar partikel hujan/dinamis tidak memicu overflow arena atau sync flush GPU (menurunkan `sync` dari `36` → `0–7/f`).
  - **Smart Deferred `LOAD_OP_CLEAR` Render Pass Folding (Open-Pass Guard) & Per-Pass Feedback Sharing:** Mencegah clear offscreen memecah TBDR render pass aktif, serta berbagi snapshot color/depth dalam satu pass (`PS3RECOMP_ENG_FEEDBACK_PER_PASS=1`).
  - **Multi-Threaded Async Shader & Pipeline Compiler Pool (`PS3RECOMP_VK_ASYNC_PIPE=1`):** Meng-offload kompilasi HLSL→SPIR-V (`glslang`) + `vkCreateGraphicsPipelines` (`TRIANGLE_LIST` pre-build) ke 2–3 worker thread (`nice +4`), disk I/O `spirv_cache/` paralel, dan `vkGetPipelineCacheData` (>9 MB) sepenuhnya async di background — **menghilangkan framedrop saat ganti cutscene**.
  - **Triple Buffering (`VK_EB_SLOTS=3`) + Ring Dinamis RAM-Aware + Early Submit + Coalescing:** UBO ring hingga `48 MB`/slot (`32 MB VB`, `10 MB IB`), non-blocking `vkGetFenceStatus` Early Submit, native Adreno FP16 `RelaxedPrecision` (`PS3RECOMP_FP_HALF=1`), deferred framebuffer destruction, dan Qualcomm **SGSR** + **FSR1+RCAS** + **CAS** upscaler.
- **Stabilitas List Musik (`0x01CC04E8`):** Mutex rekursif global (`lego_music_list_lock`/`unlock`) melindungi seluruh 7 titik akses list musik global dari race condition multi-thread dan infinite loop pointer NULL.
- **Co-op 2 HP Online (Server Relay Multi-Room) & LAN Direct (Dual-Screen):**
  - **Mode Online Relay (`wss://` / `tcp://`) + LAN fallback** dengan isolasi multi-room berdasarkan **Port / Nomor Room (`1–65535`) + Password Room (SHA-256)** ([`relay_server/`](lego_marvel_android/relay_server/) siap deploy ke Render.com Singapore / Cloudflare Workers / Koyeb / VPS).
  - **HP 1 (Host, butuh OBB):** Auto-deteksi split-screen RSX + crop P1 layar penuh (mendukung FSR/SGSR/CAS & 2× Frame Gen) + capture GPU P2 async `640×360` → Hardware `MediaCodec` H.264 (`800 Kbps` / `1.5 Mbps` / `3.5 Mbps`, fallback JPEG) + audio stereo **24 kHz IMA-ADPCM** + injeksi input Player 2 langsung ke `cellPad` Port 1.
  - **HP 2 (Client, TANPA OBB!):** `CoopClientActivity` mandiri tanpa perlu file OBB 7.5 GB, mendukung hardware decoder H.264 `SurfaceView`, audio stereo `AudioTrack` (1-tap mute di HUD), tombol sentuh DualShock 3, serta gamepad fisik USB/Bluetooth.
- **Zero-Copy Android OBB Mounting (ZIP64 `STORED`):** Mounts `main.2.com.ps3recomp.legomarvel.obb` directly in [`ppu_fs.cpp`](ps3recomp/runtime/ppu/ppu_fs.cpp) using thread-safe `pread()` streaming — **zero extra storage duplication** for `GAME*.DAT` (~7.55 GB). Only tiny boot/metadata files (`EBOOT.ELF`, `PARAM.SFO`, `LIC.DAT`, `TROPHY.TRP`) are staged once into app-internal cache.
- **GPU Optical-Flow 2× Frame Generation (Vulkan Native):** Mode **`Optical-Flow 2×`** (bidirectional coarse-to-fine + static-HUD protection & scene-cut guard) dan **`Fast Blend 2× (Low-End)`**, auto-bypass mendekati 60 real FPS.
- **Full Physical Gamepad Support (USB / Bluetooth — P1 & P2):** PS4/DualSense, Xbox, Switch Pro/Nintendo (A↔B & X↔Y swap), Gamesir, generic HID, DualShock 3 square outer-gate projection (100% kecepatan lari diagonal), & auto-hide virtual controls.
- **60 FPS / Uncapped Unlock & Custom GPU Driver Manager (`libadrenotools`):** Patches `NuRenderSetVBlankInterval` (`0x015B369C`), auto-imports AdrenoTools / Mesa Turnip `.zip` drivers + Adreno Turbo Mode.

---

## Download & Installation (APK + OBB)

1. **Download APK:** Unduh **`lego-marvel-ps3-arm64.apk`** dari halaman [GitHub Releases](../../releases) lalu install di perangkat Android ARM64.
2. **Download OBB (Wajib untuk bermain / HP 1 Host; HP 2 Client Co-op TIDAK perlu OBB):** Unduh file OBB (`main.2.com.ps3recomp.legomarvel.obb`) di Telegram:
   - **Link OBB:** [https://t.me/inigsi/27](https://t.me/inigsi/27)
   - *(Atau Anda juga dapat membuat OBB sendiri dari dump legal `BLUS-31161` menggunakan [`lego_marvel_android/scripts/pack_obb.py`](lego_marvel_android/scripts/pack_obb.py)).*
3. **Penempatan File OBB:**
   - **Lokasi Utama (Disarankan):**
     ```text
     /storage/emulated/0/Android/obb/com.ps3recomp.legomarvel/main.2.com.ps3recomp.legomarvel.obb
     ```
   - **Lokasi Alternatif (Jika tidak bisa menyimpan ke folder `Android/obb`):**
     Jika file manager Anda tidak bisa menyimpan atau memindahkan file OBB ke folder `Android/obb/com.ps3recomp.legomarvel` (misalnya karena pembatasan akses Android 11–15), **Anda bisa langsung menyimpannya di folder `Download`**:
     ```text
     /storage/emulated/0/Download/main.2.com.ps3recomp.legomarvel.obb
     ```
     *(Pastikan izin **All Files Access** diaktifkan saat diminta oleh Launcher.)*
4. Buka aplikasi — Launcher akan menampilkan **`Mode Data : OBB SIAP`** dan game siap dimainkan.

---

## Repository Structure

- [`lego_marvel_android/`](lego_marvel_android/) — Android port, Java Launcher/GameActivity/CoopClientActivity, Online Relay & LAN Co-op streamer, NativeActivity C++ host (`android_main.cpp`), HLE stubs & music-list mutex (`stubs.cpp`), `libadrenotools`, `relay_server/`, OBB packager (`scripts/pack_obb.py`), and APK build scripts.
- [`ps3recomp/`](ps3recomp/) — Android-ported `ps3recomp` static recompiler, RSX Vulkan + Lossless Scaling + Optical-Flow Frame Gen + Split-Screen P2 GPU capture backend, zero-copy ZIP64 OBB `cellFs` VFS (`runtime/ppu/ppu_fs.cpp`), anti-crackle AAudio + Co-op ADPCM audio tap, SPU/SPURS runtime, and AArch64 fiber context switcher.

See [`lego_marvel_android/README.md`](lego_marvel_android/README.md) for full technical architecture and build instructions.

---

## Credits

- **Port, Optimizations & Android OBB Build:** **by ArifGans** — Telegram: [t.me/Apa_Itu_Telegram](https://t.me/Apa_Itu_Telegram)
- **OBB Mirror:** [https://t.me/inigsi/27](https://t.me/inigsi/27)
- **Co-op Online Relay Server Repo:** [github.com/IkehKimci/lego-marvel-coop-relay](https://github.com/IkehKimci/lego-marvel-coop-relay)
- **Base Static Recompiler:** `ps3recomp`
- **Custom Vulkan Driver Loader:** `libadrenotools` / `liblinkernsbypass`
