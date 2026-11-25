# LEGO® Marvel Super Heroes (`BLUS-31161`) — Native Android ARM64 Port (OBB Edition)

Native Android ARM64 static-recompilation port of **LEGO® Marvel Super Heroes (USA / `BLUS31161`)** built on [`ps3recomp`](ps3recomp/) with **Vulkan 1.1/1.3**, **GPU Lossless Scaling**, **60 FPS unlock**, **Custom Turnip/Adreno Driver Loader**, and **Zero-Copy Android ZIP64 OBB Mounting**.

> **Credit:** **by ArifGans** — Telegram: [t.me/Apa_Itu_Telegram](https://t.me/Apa_Itu_Telegram)

---

## Features

- **Zero-Copy Android OBB Mounting (ZIP64 `STORED`):** Mounts `main.2.com.ps3recomp.legomarvel.obb` directly in [`ppu_fs.cpp`](ps3recomp/runtime/ppu/ppu_fs.cpp) using thread-safe `pread()` streaming — **zero extra storage duplication** for `GAME*.DAT` (~7.55 GB). Only tiny boot/metadata files (`EBOOT.ELF`, `PARAM.SFO`, `LIC.DAT`, `TROPHY.TRP`) are staged once into app-internal cache.
- **RSX Vulkan 1.1/1.3 + GPU Lossless Scaling:**
  - Fullscreen GPU upscaling modes (`PS3RECOMP_VK_SCALING`): **FSR1 + RCAS**, **CAS Sharpen**, **Integer / Nearest**, and **Bilinear**.
  - Resolution presets (`480p`, `540p`, `720p`, `1080p`), persistent SPIR-V shader cache + async `VkPipelineCache`, double-buffered async draw batching (`VK_DRAW_BATCH=1`), and Adreno TBDR render-pass coalescing.
- **60 FPS / Uncapped Unlock:** Patches `NuRenderSetVBlankInterval` (`0x015B369C`) with nanosecond 60 Hz monotonic VBlank pacing (`60 FPS`, `30 FPS`, `Uncapped`).
- **Custom GPU Driver Manager (`libadrenotools`):** Auto-imports AdrenoTools / Mesa Turnip `.zip` drivers from `/storage/emulated/0/Download/` + Adreno Turbo Mode.
- **Native Audio (`AAudio`) & Animated DualShock 3 Controls:** 48 kHz stereo float32 AAudio mixer, USB/Bluetooth gamepad support, and animated multi-touch DualShock 3 virtual gamepad with square outer-gate analog projection + live FPS HUD.
- **OBB-Only Android Launcher:** Isolated `:game` process (`GameActivity`), `settings.ini`, and built-in `latest.log` / `crash_last.log` viewer.

---

## Installation (APK + OBB)

> **Note:** Game assets / OBB are **not** included in this repository or GitHub Releases. You must package your own legal PS3 copy of `BLUS-31161` using [`lego_marvel_android/scripts/pack_obb.py`](lego_marvel_android/scripts/pack_obb.py).

1. Download **`lego-marvel-ps3-arm64.apk`** from the [GitHub Releases](../../releases) page and install it on your ARM64 Android device.
2. Package your legal `PS3_GAME/` dump into an uncompressed ZIP64 `.obb`:
   ```bash
   python3 lego_marvel_android/scripts/pack_obb.py /path/to/PS3_GAME main.2.com.ps3recomp.legomarvel.obb
   ```
3. Place the OBB file at:
   ```text
   /storage/emulated/0/Android/obb/com.ps3recomp.legomarvel/main.2.com.ps3recomp.legomarvel.obb
   ```
4. Open the app — the Launcher will display **`Mode Data : OBB SIAP`** and is ready to play.

---

## Repository Structure

- [`lego_marvel_android/`](lego_marvel_android/) — Android port, Java Launcher/GameActivity, NativeActivity C++ host (`android_main.cpp`), HLE stubs, `libadrenotools`, OBB packager (`scripts/pack_obb.py`), and APK build scripts.
- [`ps3recomp/`](ps3recomp/) — Android-ported `ps3recomp` static recompiler, RSX Vulkan + Lossless Scaling backend, zero-copy ZIP64 OBB `cellFs` VFS (`runtime/ppu/ppu_fs.cpp`), AAudio backend, SPU/SPURS runtime, and AArch64 fiber context switcher.

See [`lego_marvel_android/README.md`](lego_marvel_android/README.md) for full technical architecture and build instructions.

---

## Credits

- **Port, Optimizations & Android OBB Build:** **by ArifGans** — Telegram: [t.me/Apa_Itu_Telegram](https://t.me/Apa_Itu_Telegram)
- **Base Static Recompiler:** `ps3recomp`
- **Custom Vulkan Driver Loader:** `libadrenotools` / `liblinkernsbypass`
