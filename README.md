# LEGO® Marvel Super Heroes (`BLUS-31161`) — Native Android ARM64 Port (OBB Edition)

Native Android ARM64 static-recompilation port of **LEGO® Marvel Super Heroes (USA / `BLUS31161`)** built on [`ps3recomp`](ps3recomp/) with **Vulkan 1.1/1.3**, **GPU Lossless Scaling**, **GPU Optical-Flow 2× Frame Generation**, **Anti-Crackle AAudio**, **Dual-Screen 2-Phone Online & LAN Co-op**, **Full 2-Player USB/Bluetooth Gamepad Support**, **Low-End Mode (360p)**, and **Zero-Copy Android ZIP64 OBB Mounting**.

> **Credit:** **by ArifGans** — Telegram: [t.me/Apa_Itu_Telegram](https://t.me/Apa_Itu_Telegram)

---

## Features (`v0.2.3-BLUS31161`)

- **Optimasi Besar Adreno / Vulkan (Terinspirasi dari ARMSX3):**
  - **GPU-Resident Persistent VBO/IBO Cache (`s_gpuvc`):** Menyimpan mesh statis hingga `64 MB VB + 24 MB IB` (RAM ≥6 GB) langsung di memori GPU/UMA berdasarkan `vc_id` — menghilangkan 40–60 MB/frame `memcpy` vertex/index setiap frame.
  - **Triple Buffering (`VK_EB_SLOTS=3`) + Ring Dinamis RAM-Aware + Early Submit:** UBO ring hingga `48 MB`/slot (nol ring overflow) dan **Non-Blocking Early Submit** via `vkGetFenceStatus` di batas render pass agar GPU merender pass pertama secara paralel saat CPU merekam pass berikutnya.
  - **Deferred `LOAD_OP_CLEAR` Render Pass Folding & Tight Pass Barriers:** 64 varian render pass kompatibel untuk melipat clear langsung ke `LOAD_OP_CLEAR` (tanpa Sysmem clear+reload pada TBDR Adreno) + barrier graphics+transfer ketat menggantikan `ALL_COMMANDS`.
  - **Native Adreno FP16 Fragment Shaders (`PS3RECOMP_FP_HALF=1`):** Register `h[48]` otomatis memakai `min16float4` (`RelaxedPrecision` Vulkan 1.0) untuk shader non-packing.
  - **O(1) FP & Pipeline Cache + Eliminasi Sync Stall:** Caching FP (`0x08E4`), pre-caching mask tekstur (`shadow`/`gamma`/`expand`), 1-entry MRU pipeline cache, 4096-entry compact division-free `eng_vcache`, deferred framebuffer destruction (64-entry snapshot pool), merged single-submit readback (`readback_self_flushes`), dan perbaikan sub-rect `NV3089` GPU blit.
  - **Qualcomm SGSR Upscaler:** Mode **SGSR (Snapdragon Super Resolution)** 1-pass di Launcher berdampingan dengan FSR1+RCAS, CAS, Integer, dan Bilinear.
- **Stabilitas Audio & Perbaikan Stuck/Freeze (`0x01CC04E8`):** Mutex rekursif global (`lego_music_list_lock`/`unlock`) yang melindungi seluruh 7 titik akses list musik global (`0x01CC04E8`) dari race condition multi-thread dan infinite loop pointer NULL (`func_006A8C38`, `func_006A905C`, `func_006A9230`, `func_0066F1D0`, `func_0068B660`, `loc_006BC1D4`), plus guard NULL pointer pada helper `NuSoundStream` dan `NuList`.
- **Co-op 2 HP Online (Server Relay Multi-Room) & LAN Direct (Dual-Screen):**
  - **Mode Online Relay (`wss://` / `tcp://`) + LAN fallback** dengan isolasi multi-room berdasarkan **Port / Nomor Room (`1–65535`) + Password Room (SHA-256)** sehingga banyak pasang pemain bisa bermain bersama di satu server gratis ([`relay_server/`](lego_marvel_android/relay_server/) siap deploy ke Render.com Singapore / Cloudflare Workers / Koyeb / VPS).
  - **HP 1 (Host, butuh OBB):** Auto-deteksi split-screen RSX + crop P1 layar penuh (tetap mendukung FSR/SGSR/CAS & 2× Frame Gen) + capture GPU P2 async `640×360` → Hardware `MediaCodec` H.264 (`800 Kbps` / `1.5 Mbps` / `3.5 Mbps`, fallback JPEG) + audio stereo **24 kHz IMA-ADPCM** + injeksi input Player 2 langsung ke `cellPad` Port 1.
  - **HP 2 (Client, TANPA OBB!):** `CoopClientActivity` mandiri tanpa perlu file OBB 7.5 GB, mendukung hardware decoder H.264 `SurfaceView`, audio stereo `AudioTrack` (1-tap mute di HUD), tombol sentuh DualShock 3, serta gamepad fisik USB/Bluetooth.
- **Zero-Copy Android OBB Mounting (ZIP64 `STORED`):** Mounts `main.2.com.ps3recomp.legomarvel.obb` directly in [`ppu_fs.cpp`](ps3recomp/runtime/ppu/ppu_fs.cpp) using thread-safe `pread()` streaming — **zero extra storage duplication** for `GAME*.DAT` (~7.55 GB). Only tiny boot/metadata files (`EBOOT.ELF`, `PARAM.SFO`, `LIC.DAT`, `TROPHY.TRP`) are staged once into app-internal cache.
- **RSX Vulkan 1.1/1.3 + GPU Upscaling + Low-End Optimizations:**
  - Fullscreen GPU upscaling modes (`PS3RECOMP_VK_SCALING`): **FSR1 + RCAS**, **Qualcomm SGSR**, **CAS Sharpen**, **Integer / Nearest**, and **Bilinear**.
  - Resolution presets: **`360p` (Low-End Ultra Boost)**, **`480p`**, **`540p`**, **`720p`**, and **`1080p`**, plus **`Mode Performa Low-End (HP Kentang)`** (`PS3RECOMP_ENG_LOW_END=1`) and RAM-aware batch rings.
  - **4096-entry 2-way Converted Vertex/Index Cache + GPU-Resident VBO/IBO Cache**, async GPU-to-GPU **`surface_blit`** (`NV0039`/`NV3089`), async `depth_as_color` + same-pass `DEPTH24_D8` snapshot, active `vp_c[lo..hi)` UBO range copy + `memcmp` dedup, dirty-tracked render state & MRU descriptor/FB/sampler caches, persistent SPIR-V cache + async `VkPipelineCache`.
- **GPU Optical-Flow 2× Frame Generation (DLSS/FSR3 Style — Vulkan Native):**
  - **`Optical-Flow 2×`**: Symmetric bidirectional coarse-to-fine optical flow at $t = 0.5$ with static-HUD protection and disocclusion / scene-cut anti-ghosting guard.
  - **`Fast Blend 2× (Low-End)`**: Lightweight 2-tap motion blend for entry-level GPUs.
  - Auto-bypasses when real FPS is already ~60 FPS; preserves FSR1/CAS/Integer scaling on every real frame.
- **Anti-Crackle Native Audio (`AAudio`):** 16384-frame (~341 ms) lock-free SPSC ring, 42.7 ms prefill cushion, Android-tuned SPU pacing watermarks, smooth exponential-decay underrun/hole de-click ramps, normalized 7.1→stereo downmix, and $C^1$ soft-limiter (eliminates crackling/clipping in busy scenes).
- **Full Physical Gamepad Support (USB / Bluetooth — P1 & P2):**
  - Supports **PS4 / DualSense, Xbox, Switch Pro / Nintendo (A↔B & X↔Y swap), Gamesir, and generic HID controllers** with simultaneous 2-player local & 2-phone co-op.
  - Analog + digital triggers (`LTRIGGER`/`BRAKE`, `RTRIGGER`/`GAS`), right-stick `Z/RZ` + `RX/RY` fallback, smooth radial deadzone (`0.08`), **DualShock 3 square outer-gate projection** (100% diagonal run speed), gamepad `BACK`-button guard, live controller detector in Launcher, and **auto-hide virtual touch controls** when physical gamepad is active (tap screen anytime to show again).
- **60 FPS / Uncapped Unlock & Custom GPU Driver Manager (`libadrenotools`):** Patches `NuRenderSetVBlankInterval` (`0x015B369C`), auto-imports AdrenoTools / Mesa Turnip `.zip` drivers from `/storage/emulated/0/Download/` + Adreno Turbo Mode.

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
