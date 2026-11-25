#!/usr/bin/env bash
# Package libps3recomp_lego_marvel.so + AdrenoTools hooks + Launcher DEX + Vulkan shader libs
# into a zipaligned, v2/v3-signed native Android ARM64 APK.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-$PROJ_DIR/build}"
OUT_APK="${2:-$PROJ_DIR/dist/lego-marvel-ps3-arm64.apk}"
PREFIX_DIR="${PREFIX:-$(cd -- "$(dirname -- "$(command -v clang)")/.." && pwd)}"

JAVA_BIN="$(command -v java 2>/dev/null || echo "$PREFIX_DIR/lib/jvm/java-21-openjdk/bin/java")"
JAVAC_BIN="$(command -v javac 2>/dev/null || echo "$PREFIX_DIR/lib/jvm/java-21-openjdk/bin/javac")"
KEYTOOL_BIN="$(command -v keytool 2>/dev/null || echo "$PREFIX_DIR/lib/jvm/java-21-openjdk/bin/keytool")"
APKSIGNER_JAR="$PREFIX_DIR/share/java/apksigner.jar"
D8_JAR="$PREFIX_DIR/share/java/d8.jar"
ANDROID_JAR="$PROJ_DIR/../.tools/android-34.jar"

LIB_SO="$BUILD_DIR/libps3recomp_lego_marvel.so"
if [ ! -f "$LIB_SO" ]; then
    echo "ERROR: $LIB_SO not found. Build CMake project first." >&2
    exit 1
fi

STAGE="$BUILD_DIR/apk_stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/res_flat" "$STAGE/classes" "$STAGE/root/lib/arm64-v8a" "$STAGE/root/assets/PS3_GAME" "$(dirname -- "$OUT_APK")"

# 1. Compile Android resources (ic_launcher.png from BLUS-31161 ICON0.PNG)
aapt2 compile --dir "$PROJ_DIR/android/res" -o "$STAGE/res_flat/res.zip"

# 2. Link resources + AndroidManifest.xml against Android system framework-res.apk
aapt2 link \
    -o "$STAGE/base.apk" \
    -I /system/framework/framework-res.apk \
    --manifest "$PROJ_DIR/android/AndroidManifest.xml" \
    --min-sdk-version 26 \
    --target-sdk-version 34 \
    "$STAGE/res_flat/res.zip"

# 3. Compile Java launcher & game activities -> classes.dex
JAVA_SRCS=()
while IFS= read -r -d '' f; do
    JAVA_SRCS+=("$f")
done < <(find "$PROJ_DIR/android/java" -name '*.java' -print0 2>/dev/null || true)

if [ "${#JAVA_SRCS[@]}" -gt 0 ]; then
    if [ ! -f "$ANDROID_JAR" ]; then
        echo "ERROR: Android platform jar not found at $ANDROID_JAR" >&2
        exit 1
    fi
    echo "[package_apk] Compiling ${#JAVA_SRCS[@]} Java source(s) -> classes.dex"
    "$JAVAC_BIN" --release 11 -cp "$ANDROID_JAR" -d "$STAGE/classes" "${JAVA_SRCS[@]}"
    CLASS_FILES=()
    while IFS= read -r -d '' c; do
        CLASS_FILES+=("$c")
    done < <(find "$STAGE/classes" -name '*.class' -print0)
    "$JAVA_BIN" -cp "$D8_JAR" com.android.tools.r8.D8 \
        --min-api 26 \
        --lib "$ANDROID_JAR" \
        --output "$STAGE/root" \
        "${CLASS_FILES[@]}"
fi

# 4. Stage native ARM64 libraries (game .so + shader runtime + AdrenoTools hooks)
cp "$LIB_SO" "$STAGE/root/lib/arm64-v8a/libps3recomp_lego_marvel.so"
for dep in libc++_shared.so libglslang.so libSPIRV.so libglslang-default-resource-limits.so; do
    if [ -f "$PREFIX_DIR/lib/$dep" ]; then
        cp "$PREFIX_DIR/lib/$dep" "$STAGE/root/lib/arm64-v8a/$dep"
    else
        echo "ERROR: Required shared library $PREFIX_DIR/lib/$dep not found" >&2
        exit 1
    fi
done

HOOK_DIR="$BUILD_DIR/adrenotools_build/src/hook"
for hook in libhook_impl.so libmain_hook.so libfile_redirect_hook.so libgsl_alloc_hook.so; do
    if [ -f "$HOOK_DIR/$hook" ]; then
        cp "$HOOK_DIR/$hook" "$STAGE/root/lib/arm64-v8a/$hook"
    fi
done

# 5. Stage PS3_GAME metadata (+ smoke.elf when built in smoke/bring-up mode)
if [ -f "$PROJ_DIR/assets/PS3_GAME/PARAM.SFO" ]; then
    cp "$PROJ_DIR/assets/PS3_GAME/PARAM.SFO" "$STAGE/root/assets/PS3_GAME/PARAM.SFO"
fi
if [ -f "$BUILD_DIR/smoke_gen/smoke.elf" ]; then
    cp "$BUILD_DIR/smoke_gen/smoke.elf" "$STAGE/root/assets/smoke.elf"
fi

# 6. Add classes.dex, lib/, and assets/ into base.apk (store .so uncompressed with -0)
cp "$STAGE/base.apk" "$STAGE/unaligned.apk"
(
    cd "$STAGE/root"
    if [ -f classes.dex ]; then
        zip -9 "$STAGE/unaligned.apk" classes.dex
    fi
    zip -r -0 "$STAGE/unaligned.apk" lib/
    zip -r -9 "$STAGE/unaligned.apk" assets/
)

# 7. Page-align APK for Android ART / dynamic linker
zipalign -f -p 4 "$STAGE/unaligned.apk" "$STAGE/aligned.apk"

# 8. Generate debug keystore if needed, then sign with apksigner (v2 + v3)
KEYSTORE="$PROJ_DIR/android/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    "$KEYTOOL_BIN" -genkeypair -v \
        -keystore "$KEYSTORE" \
        -storepass android \
        -keypass android \
        -alias ps3recomp \
        -keyalg RSA \
        -keysize 2048 \
        -validity 10000 \
        -dname "CN=ps3recomp,OU=Android,O=BLUS31161,L=Local,ST=Local,C=US" >/dev/null
fi

"$JAVA_BIN" -jar "$APKSIGNER_JAR" sign \
    --ks "$KEYSTORE" \
    --ks-key-alias ps3recomp \
    --ks-pass pass:android \
    --key-pass pass:android \
    --out "$OUT_APK" \
    "$STAGE/aligned.apk"

"$JAVA_BIN" -jar "$APKSIGNER_JAR" verify --verbose "$OUT_APK"
ls -lh "$OUT_APK"
echo "[package_apk] Built and verified: $OUT_APK"
