#!/bin/sh
# build esp_overlay.dylib (roothide layout deb)
set -e
cd "$(dirname "$0")"

CLANG=/opt/theos/toolchain/linux/iphone/bin/clang
SDK=/opt/theos/sdks/iPhoneOS26.0.sdk
OUT=esp_overlay.dylib

[ -x "$CLANG" ] || { echo "theos clang missing: $CLANG"; exit 1; }

echo "[+] compile"
"$CLANG" --target=arm64-apple-ios14.0 -x objective-c -shared \
  -isysroot "$SDK" \
  -framework UIKit -framework QuartzCore -framework Foundation -lobjc \
  -fblocks -fobjc-arc -O2 -Wno-deprecated-declarations \
  -o "$OUT" esp_overlay.m

echo "[+] sign"
ldid -S "$OUT"

echo "[+] package deb"
rm -rf pkg && mkdir -p pkg/var/jb/usr/lib/TweakInject pkg/DEBIAN
cp "$OUT" esp_overlay.plist pkg/var/jb/usr/lib/TweakInject/
cat > pkg/DEBIAN/control <<'EOF'
Package: com.esp.overlay
Name: esp-overlay
Version: 0.1.0
Architecture: iphoneos-arm64
Description: HEN-K ESP renderer (esp-android port)
Maintainer: henk
Section: Tweaks
EOF
chmod 755 pkg/DEBIAN
dpkg-deb -Zgzip -b pkg com.esp.overlay_0.1.0_iphoneos-arm64.deb
echo "[+] done: com.esp.overlay_0.1.0_iphoneos-arm64.deb"
ls -la "$OUT" com.esp.overlay_0.1.0_iphoneos-arm64.deb
