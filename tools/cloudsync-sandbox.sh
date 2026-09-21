#!/usr/bin/env bash
# Build melonDS + Google Drive CloudSync feature inside the flatpak KDE SDK 6.11 sandbox.
# No sudo, no host changes. Output binary: build-sandboxed/melonDS
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDDIR="$ROOT/build-sandboxed"
SDKVER=6.11
SDK="org.kde.Sdk"
RUNTIME="org.kde.Platform"
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

echo "== 1. Sandbox + SDK sanity =="
flatpak info --show-metadata "$RUNTIME//$SDKVER" >/dev/null 2>&1 || fail "runtime $RUNTIME//$SDKVER missing"
flatpak info --show-metadata "$SDK//$SDKVER" >/dev/null 2>&1 || fail "sdk $SDK//$SDKVER missing"
pass "KDE SDK $SDKVER available (user install)"

flatpak run --command=bash "$SDK//$SDKVER" -c '
set -e
echo "-- toolchain in sandbox --"
cmake --version | head -1
ninja --version
gcc --version | head -1
pkg-config --modversion Qt6Core Qt6Widgets Qt6Network Qt6Multimedia Qt6Svg
test -d /usr/lib64/qt6/privateincludedir 2>/dev/null || true
ls /usr/include/qt6/QtCore/*/QtCore/private 2>/dev/null | head -1 || ls /usr/include/qt6/QtCore/private 2>/dev/null | head -1
pkg-config --exists ecm && echo "ecm: $(pkg-config --modversion ecm)"
'

echo "== 2. Building missing libs in-sandbox (SDL2, enet, faad2, libpcap) =="
DEPSRC="$ROOT/.sandbox-deps"
mkdir -p "$DEPSRC/src" "$DEPSRC/prefix/lib" "$DEPSRC/prefix/include" "$DEPSRC/prefix/lib/pkgconfig"
cd "$DEPSRC/src"

fetch() { # fetch <url> <dir>
    local url="$1" dir="$2"
    if [ ! -d "$dir" ]; then
        echo "downloading $url"
        curl -fL --retry 3 -o dl.tar.gz "$url"
        tar xf dl.tar.gz
        mv "$(tar tf dl.tar.gz | head -1 | cut -d/ -f1)" "$dir"
        rm dl.tar.gz
    fi
}

# libpcap (cmake, no deps)
fetch https://www.tcpdump.org/release/libpcap-1.10.5.tar.gz libpcap
# SDL2 (cmake)
fetch https://github.com/libsdl-org/SDL/releases/download/release-2.32.8/SDL2-2.32.8.tar.gz sdl2
# enet (autotools)
fetch https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz enet
# faad2 (cmake)
fetch https://github.com/knik0/faad2/archive/refs/tags/2.11.1.tar.gz faad2

flatpak run --command=bash --filesystem="$DEPSRC" "$SDK//$SDKVER" -c "
set -e
P=$DEPSRC/prefix
cd $DEPSRC/src/libpcap
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=\$P -DDBUS=OFF -DINET6=ON >/dev/null
cmake --build build -j\$(nproc) >/dev/null
cmake --install build >/dev/null

cd $DEPSRC/src/sdl2
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=\$P \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
  -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF \
  -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_OPENGL=ON -DSDL_OPENGLES=OFF \
  -DSDL_DBUS=OFF -DSDL_UDEV=ON -DSDL_JOYSTICK=ON -DSDL_HIDAPI=OFF \
  -DSDL_AUDIO=OFF -DSDL_CAMERA=OFF -DSDL_POWER=ON -DSDL_TIMERS=ON \
  -DSDL_LOCALE=OFF -DSDL_FILESYSTEM=OFF -DSDL_SENSOR=OFF \
  -DSDL_VULKAN=OFF -DSDL_DIRECTFB=OFF -DSDL_KMSDRM=OFF \
  -DSDL_RPI=OFF -DSDL_X11_XCURSOR=OFF -DSDL_X11_XDBE=OFF \
  -DSDL_X11_XINPUT=OFF -DSDL_X11_XRANDR=OFF -DSDL_X11_XSCRNSAVER=OFF \
  -DSDL_X11_XSHAPE=OFF -DSDL_X11_XVM=OFF -DSDL_X11_XFIXES=OFF \
  -DSDL_X11_XTEST=OFF >/dev/null
cmake --build build -j\$(nproc) >/dev/null
cmake --install build >/dev/null

cd $DEPSRC/src/enet
autoreconf -if >/dev/null 2>&1
./configure --prefix=\$P --disable-shared --enable-static >/dev/null
make -j\$(nproc) >/dev/null
make install >/dev/null

cd $DEPSRC/src/faad2
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=\$P >/dev/null
cmake --build build -j\$(nproc) >/dev/null
cmake --install build >/dev/null

echo '-- pkg-config files produced --'
ls \$P/lib/pkgconfig/
"
pass "SDL2/enet/faad2/libpcap built into $DEPSRC/prefix"

echo "== 3. Configure + build melonDS (CloudSync ON) =="
ENVARG="--env=PKG_CONFIG_PATH=$DEPSRC/prefix/lib/pkgconfig:/usr/lib64/pkgconfig:/usr/share/pkgconfig \
        --env=PATH=$DEPSRC/prefix/bin:/usr/bin:/bin \
        --env=LD_LIBRARY_PATH=$DEPSRC/prefix/lib"

flatpak run --command=bash --filesystem="$ROOT" $ENVARG "$SDK//$SDKVER" -c "
set -e
cmake -B $BUILDDIR -S $ROOT -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_GOOGLE_DRIVE_SYNC=ON >/dev/null
cmake --build $BUILDDIR -j\$(nproc) 2>&1 | tail -3
"
pass "melonDS built (CloudSync ON)"

echo "== 4. Configure + build melonDS (CloudSync OFF, regression guard) =="
flatpak run --command=bash --filesystem="$ROOT" $ENVARG "$SDK//$SDKVER" -c "
set -e
cmake -B $ROOT/build-syncoff -S $ROOT -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_GOOGLE_DRIVE_SYNC=OFF >/dev/null
cmake --build $ROOT/build-syncoff -j\$(nproc) >/dev/null
"
pass "melonDS built (CloudSync OFF)"

echo "== 5. Headless CloudSync smoke tests =="
BIN="$BUILDDIR/melonDS"
test -x "$BIN" || fail "binary missing"
strings "$BIN" | grep -q "https://accounts.google.com/o/oauth2/v2/auth" && pass "OAuth endpoint embedded" || fail "OAuth endpoint missing"
strings "$BIN" | grep -q "https://www.googleapis.com/drive/v3/files" && pass "Drive API endpoint embedded" || fail "Drive API endpoint missing"
if strings "$ROOT/build-syncoff/melonDS" | grep -q googleapis.com; then
    fail "sync-OFF build contains Google endpoints"
else
    pass "sync-OFF build cleanly excludes Google endpoints"
fi

# TLS backend check via tiny probe app run in sandbox
cat > "$BUILDDIR/tls_probe.cpp" <<'EOF'
#include <QCoreApplication>
#include <QSslSocket>
#include <cstdio>
int main(int argc, char** argv){
    QCoreApplication app(argc, argv);
    printf("TLS=%d\n", (int)QSslSocket::supportsSsl());
    return QSslSocket::supportsSsl() ? 0 : 1;
}
EOF
flatpak run --command=bash --filesystem="$ROOT" $ENVARG "$SDK//$SDKVER" -c "
set -e
g++ -fPIC -std=c++17 $BUILDDIR/tls_probe.cpp -o $BUILDDIR/tls_probe \
  \$(pkg-config --cflags --libs Qt6Core) \$(pkg-config --cflags --libs Qt6Network)
QT_QPA_PLATFORM=offscreen $BUILDDIR/tls_probe
" && pass "Qt TLS backend usable (HTTPS to Google possible)" || fail "no TLS backend in sandbox"

# app boots headless without crashing
timeout 25 flatpak run --command=bash --filesystem="$ROOT" $ENVARG "$SDK//$SDKVER" -c \
  "QT_QPA_PLATFORM=offscreen timeout 20 $BUILDDIR/melonDS" >/tmp/melonds_headless.log 2>&1
RC=$?
if [ $RC -eq 0 ] || [ $RC -eq 124 ]; then
    if grep -qiE 'segfault|cannot|error while loading' /tmp/melonds_headless.log; then
        fail "runtime load error — see /tmp/melonds_headless.log"
    else
        pass "app runs headless (offscreen), no crash"
    fi
else
    fail "app exited rc=$RC — see /tmp/melonds_headless.log"
fi

echo
echo "ALL CHECKS DONE. Binary: $BIN"
echo "GUI test on host: flatpak run --command=$BIN --filesystem=$ROOT org.kde.Sdk//$SDKVER"
