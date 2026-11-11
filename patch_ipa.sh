#!/usr/bin/env bash
# patch_ipa.sh - inject the memory-limit dylib into an RPCS3 iOS IPA and add entitlements.
#
# Usage: patch_ipa.sh <input.ipa> <output.ipa> [limit_mb]
# Needs macOS (clang, xcrun, lipo, plutil) plus: ldid  (brew install ldid)
# The result is meant for TrollStore only.

set -euo pipefail

IN="${1:?input ipa}"
OUT="${2:?output ipa}"
LIMIT_MB="${3:-3584}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> Unpacking $IN"
unzip -q "$IN" -d "$WORK/ipa"
# Drop macOS metadata (AppleDouble "._*" files, __MACOSX) that some IPAs carry. A stray
# "Payload/._Name.app" file would otherwise match the *.app search below.
find "$WORK/ipa" -name '._*' -delete
rm -rf "$WORK/ipa/__MACOSX"
APP="$(find "$WORK/ipa/Payload" -maxdepth 1 -type d -name '*.app' | head -n1)"
[ -d "$APP" ] || { echo "no .app inside IPA. Top level of Payload:" >&2; ls -la "$WORK/ipa/Payload" >&2; exit 1; }
# The old seal would be invalid after we modify the binary; TrollStore re-signs anyway.
rm -rf "$APP/_CodeSignature"
EXEC_NAME="$(plutil -extract CFBundleExecutable raw "$APP/Info.plist")"
MAIN="$APP/$EXEC_NAME"
ARCHS="$(lipo -archs "$MAIN")"
echo "==> App: $(basename "$APP"), executable: $EXEC_NAME, archs: $ARCHS"

# Read the ORIGINAL entitlements now, while the binary is still signed. Injecting the dylib
# below strips the code signature, and reading afterwards returns nothing - which would
# silently drop get-task-allow (needed to attach a debugger for JIT) and the rest.
echo "==> Reading original entitlements"
ldid -e "$MAIN" > "$WORK/orig-ents.plist" 2>/dev/null || true
if [ -s "$WORK/orig-ents.plist" ]; then
  echo "    found: $(grep -o '<key>[^<]*</key>' "$WORK/orig-ents.plist" | sed 's/<[^>]*>//g' | tr '\n' ' ')"
else
  echo "    WARNING: no entitlements found in the original binary; using JIT-safe fallback" >&2
fi

echo "==> Building libmemlimit.dylib (limit ${LIMIT_MB} MB)"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
ARCH_FLAGS=()
for a in $ARCHS; do
  case "$a" in arm64|arm64e) ARCH_FLAGS+=(-arch "$a");; esac
done
[ ${#ARCH_FLAGS[@]} -gt 0 ] || ARCH_FLAGS=(-arch arm64)
# RAM_PROBE=1 builds the experiment in ramhelper/: a RamHelper.appex extension plus a probe in the
# dylib that measures whether memory lent by the helper escapes this app's memory limit.
RAM_PROBE="${RAM_PROBE:-0}"
if [ "$RAM_PROBE" = "1" ]; then
  echo "==> RAM_PROBE=1: building helper extension and probe"
  clang -c "${ARCH_FLAGS[@]}" -isysroot "$SDK" -miphoneos-version-min=16.0 -O2 \
    -DDEFAULT_LIMIT_MB="$LIMIT_MB" -DRAM_PROBE=1 "$HERE/memlimit.c" -o "$WORK/memlimit.o"
  clang++ -c "${ARCH_FLAGS[@]}" -isysroot "$SDK" -miphoneos-version-min=16.0 -O2 -fobjc-arc \
    -I"$HERE/ramhelper" "$HERE/ramhelper/ramprobe.mm" -o "$WORK/ramprobe.o"
  clang++ "${ARCH_FLAGS[@]}" -isysroot "$SDK" -miphoneos-version-min=16.0 \
    -dynamiclib -install_name "@rpath/libmemlimit.dylib" \
    "$WORK/memlimit.o" "$WORK/ramprobe.o" -framework Foundation -o "$WORK/libmemlimit.dylib"
else
  clang "${ARCH_FLAGS[@]}" -isysroot "$SDK" -miphoneos-version-min=16.0 -O2 \
    -dynamiclib -install_name "@rpath/libmemlimit.dylib" \
    -DDEFAULT_LIMIT_MB="$LIMIT_MB" "$HERE/memlimit.c" -o "$WORK/libmemlimit.dylib"
fi

mkdir -p "$APP/Frameworks"
cp "$WORK/libmemlimit.dylib" "$APP/Frameworks/libmemlimit.dylib"

if [ "$RAM_PROBE" = "1" ]; then
  APP_BUNDLE_ID="$(plutil -extract CFBundleIdentifier raw "$APP/Info.plist")"
  EXT="$APP/PlugIns/RamHelper.appex"
  rm -rf "$EXT"
  mkdir -p "$EXT"
  clang++ "${ARCH_FLAGS[@]}" -isysroot "$SDK" -miphoneos-version-min=16.3 -O2 -fobjc-arc -fapplication-extension \
    -I"$HERE/ramhelper" "$HERE/ramhelper/extension.mm" -framework Foundation -o "$EXT/RamHelper"
  sed "s|@APP_BUNDLE_ID@|$APP_BUNDLE_ID|" "$HERE/ramhelper/Info.plist" > "$EXT/Info.plist"
  plutil -lint "$EXT/Info.plist"
  ldid -S"$HERE/ramhelper/helper-entitlements.plist" "$EXT/RamHelper"
  echo "    embedded $EXT (bundle id $APP_BUNDLE_ID.RamHelper)"
fi

echo "==> Adding load command to $EXEC_NAME"
if ! command -v insert_dylib >/dev/null; then
  git clone --depth 1 https://github.com/tyilo/insert_dylib "$WORK/insert_dylib"
  clang -o "$WORK/insert_dylib_bin" "$WORK/insert_dylib/insert_dylib/main.c"
  INSERT="$WORK/insert_dylib_bin"
else
  INSERT="$(command -v insert_dylib)"
fi
if otool -L "$MAIN" | grep -q libmemlimit; then
  echo "    already injected, skipping"
else
  "$INSERT" --strip-codesig --inplace --all-yes \
    "@rpath/libmemlimit.dylib" "$MAIN"
fi

echo "==> Merging entitlements"
python3 - "$WORK/orig-ents.plist" "$HERE/entitlements-extra.plist" "$WORK/ents.plist" <<'PY'
import plistlib, sys, os
orig_p, extra_p, out_p = sys.argv[1:4]
orig = {}
if os.path.exists(orig_p) and os.path.getsize(orig_p) > 0:
    try:
        with open(orig_p, "rb") as f:
            orig = plistlib.load(f)
    except Exception:
        orig = {}
if not orig:
    # Fallback: the two entitlements the original IPA is known to carry for JIT/performance.
    orig = {"get-task-allow": True, "com.apple.developer.sustained-execution": True}
with open(extra_p, "rb") as f:
    extra = plistlib.load(f)
orig.update(extra)
# Never ship a build that can't have a debugger attached; JIT enablers depend on it.
if not orig.get("get-task-allow"):
    print("    ERROR: get-task-allow missing; JIT would not work", file=sys.stderr)
    sys.exit(1)
with open(out_p, "wb") as f:
    plistlib.dump(orig, f)
print("    entitlements:", ", ".join(sorted(orig)))
PY

echo "==> Re-signing (ldid, pseudo-signature for TrollStore)"
ldid -S"$WORK/ents.plist" "$MAIN"
ldid -S "$APP/Frameworks/libmemlimit.dylib"

echo "==> Packing $OUT"
OUT_ABS="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
rm -f "$OUT_ABS"
( cd "$WORK/ipa" && zip -qry "$OUT_ABS" Payload )
echo "Done: $OUT"
