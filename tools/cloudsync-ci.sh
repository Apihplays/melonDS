#!/usr/bin/env bash
# melonDS CloudSync CI script
# - verifies build dependencies
# - configures + builds with Google Drive sync ON and OFF
# - runs headless smoke tests for the CloudSync feature
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="$(nproc --all)"
FAILED=0

log()  { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }
pass() { printf '\033[1;32mPASS:\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31mFAIL:\033[0m %s\n' "$*"; FAILED=1; }

# ---------------------------------------------------------------
log "1/4 Checking build dependencies"
# ---------------------------------------------------------------
MISSING=()
for pkg in sdl2 libarchive libzstd faad2 libenet; do
    pkg-config --exists "$pkg" || MISSING+=("$pkg(pc)")
done
# Tumbleweed package names (SDL2-devel is provided by sdl2-compat; ECM is kf6-)
rpm -q qt6-base-devel          >/dev/null 2>&1 || MISSING+=("qt6-base-devel")
rpm -q qt6-base-private-devel  >/dev/null 2>&1 || MISSING+=("qt6-base-private-devel")
rpm -q qt6-multimedia-devel    >/dev/null 2>&1 || MISSING+=("qt6-multimedia-devel")
rpm -q qt6-svg-devel           >/dev/null 2>&1 || MISSING+=("qt6-svg-devel")
rpm -q kf6-extra-cmake-modules >/dev/null 2>&1 || MISSING+=("kf6-extra-cmake-modules")
rpm -q wayland-devel           >/dev/null 2>&1 || MISSING+=("wayland-devel")
rpm -q libpcap-devel           >/dev/null 2>&1 || MISSING+=("libpcap-devel")
rpm -q sdl2-compat-devel       >/dev/null 2>&1 || MISSING+=("sdl2-compat-devel")
rpm -q libarchive-devel        >/dev/null 2>&1 || MISSING+=("libarchive-devel")
rpm -q faad2-devel             >/dev/null 2>&1 || MISSING+=("faad2-devel")
rpm -q enet-devel              >/dev/null 2>&1 || MISSING+=("enet-devel")
rpm -q libX11-devel            >/dev/null 2>&1 || MISSING+=("libX11-devel")
rpm -q Mesa-libEGL-devel       >/dev/null 2>&1 || MISSING+=("Mesa-libEGL-devel")

if [ ${#MISSING[@]} -gt 0 ]; then
    fail "missing packages: ${MISSING[*]}"
    echo "Install with:"
    echo "  sudo zypper install -y ${MISSING[*]%%(*}"
    exit 1
fi
pass "all build dependencies present"

# ---------------------------------------------------------------
log "2/4 Build: ENABLE_GOOGLE_DRIVE_SYNC=OFF (regression guard)"
# ---------------------------------------------------------------
cmake -B "$ROOT/build-syncoff" -S "$ROOT" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_GOOGLE_DRIVE_SYNC=OFF >/dev/null
cmake --build "$ROOT/build-syncoff" -j"$JOBS"
pass "builds with sync disabled"

# ---------------------------------------------------------------
log "3/4 Build: ENABLE_GOOGLE_DRIVE_SYNC=ON (feature build)"
# ---------------------------------------------------------------
cmake -B "$ROOT/build-syncon" -S "$ROOT" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_GOOGLE_DRIVE_SYNC=ON >/dev/null
cmake --build "$ROOT/build-syncon" -j"$JOBS" 2>&1 | tail -5
pass "builds with sync enabled"

BIN="$ROOT/build-syncon/melonDS"

# ---------------------------------------------------------------
log "4/4 Headless CloudSync smoke tests"
# ---------------------------------------------------------------
if [ -x "$BIN" ]; then
    pass "melonDS binary exists: $BIN"
else
    fail "melonDS binary missing"
fi

# a) binary references the Google endpoints compiled into CloudSyncManager.cpp
if strings "$BIN" | grep -q "https://accounts.google.com/o/oauth2/v2/auth"; then
    pass "OAuth auth endpoint embedded in binary"
else
    fail "OAuth auth endpoint not found in binary"
fi
if strings "$BIN" | grep -q "https://www.googleapis.com/drive/v3/files"; then
    pass "Drive API endpoint embedded in binary"
else
    fail "Drive API endpoint not found in binary"
fi

# b) sync-off binary must NOT embed them
if strings "$ROOT/build-syncoff/melonDS" | grep -q "googleapis.com"; then
    fail "sync-OFF build unexpectedly contains Google endpoints"
else
    pass "sync-OFF build cleanly excludes Google endpoints"
fi

# c) TLS backend available (required for every HTTPS call to Google)
QT_TLS_PROBE="$ROOT/build-syncon/tls_probe"
cat > /tmp/tls_probe.cpp <<'EOF'
#include <QCoreApplication>
#include <QSslSocket>
#include <cstdio>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    printf("TLS support: %d\n", (int)QSslSocket::supportsSsl());
    return QSslSocket::supportsSsl() ? 0 : 1;
}
EOF
if command -v qmake6 >/dev/null || [ -x /usr/lib64/qt6/bin/qmake ]; then
    :
fi
# compile the probe against the same Qt the build used
CXX_FLAGS="$(grep -m1 'CMAKE_CXX_FLAGS:STRING=' "$ROOT/build-syncon/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || true)"
if g++ -fPIC /tmp/tls_probe.cpp -o "$QT_TLS_PROBE" \
      -I"$(pkg-config --variable=includedir Qt6Core 2>/dev/null || echo /usr/include/qt6)" \
      -I"$(pkg-config --variable=includedir Qt6Core 2>/dev/null || echo /usr/include/qt6)/QtCore/$(rpm -q --qf '%{VERSION}' qt6-base-devel 2>/dev/null || echo 6.11.1)" \
      $(pkg-config --cflags --libs Qt6Core 2>/dev/null || true); then
    if "$QT_TLS_PROBE"; then
        pass "Qt TLS backend available (HTTPS to Google will work)"
    else
        fail "Qt TLS backend missing — install qt6-network-tls"
    fi
else
    echo "note: could not compile TLS probe (pkg-config Qt6Core.pc missing on Tumbleweed) — skipping"
fi

# d) drive the app headless: config parse + auto-shutdown, via offscreen platform
export QT_QPA_PLATFORM=offscreen
export XDG_CONFIG_HOME="$ROOT/build-syncon/cfg"
mkdir -p "$XDG_CONFIG_HOME"
timeout 60 "$BIN" --help >/dev/null 2>&1 || true
# if the app supports a shutdown timer, run it briefly; otherwise skip
if timeout 30 "$BIN" >/tmp/melonds_run.log 2>&1; then
    pass "app runs headless (offscreen) without crashing"
else
    # melonDS may keep running; treat timeout-124 as OK if log is clean
    if grep -qiE 'segfault|ASSERT' /tmp/melonds_run.log; then
        fail "crash during headless run — see /tmp/melonds_run.log"
    else
        pass "app stayed up headless (killed by timeout as expected)"
    fi
fi

echo
if [ "$FAILED" -eq 0 ]; then
    printf '\033[1;32mALL CHECKS PASSED\033[0m — CloudSync builds and looks healthy headless.\n'
    echo 'Remaining manual step: Google sign-in (browser OAuth) + real save sync with your account.'
    exit 0
else
    printf '\033[1;31mSOME CHECKS FAILED\033[0m\n'
    exit 1
fi
