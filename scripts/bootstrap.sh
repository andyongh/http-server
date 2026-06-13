#!/usr/bin/env bash
# =============================================================================
# scripts/bootstrap.sh  –  build all deps (Linux + macOS / Apple Silicon)
#
# Usage:
#   ./scripts/bootstrap.sh            # normal build
#   ./scripts/bootstrap.sh --force    # rebuild all (ignore stamps)
#
# Dependencies:
#   Linux:  gcc/clang, cmake, make, autoconf, libtool
#   macOS:  brew install cmake autoconf automake libtool
#           (Node.js is NOT required)
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/deps"
B="${BUILD_DEPS_DIR:-$ROOT/build/deps}"
FORCE="${1:-}"


# ── colour helpers ─────────────────────────────────────────────────────────
RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; CYN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${GRN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YLW}[WARN]${NC}  $*"; }
step()  { echo -e "${CYN}[....] $*${NC}"; }
die()   { echo -e "${RED}[ERR]${NC}   $*" >&2; exit 1; }

stamp() {
    local s="$B/.$1_done"
    [[ "$FORCE" == "--force" ]] && rm -f "$s"
    echo "$s"
}

# ── detect OS and CPU ──────────────────────────────────────────────────────
OS="$(uname -s)"
ARCH="$(uname -m)"
NPROC=4
case "$OS" in
    Linux)  NPROC="$(nproc 2>/dev/null || echo 4)" ;;
    Darwin) NPROC="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)" ;;
esac

info "OS=$OS  ARCH=$ARCH  NPROC=$NPROC"

# ── macOS: ensure homebrew tools are on PATH ───────────────────────────────
if [[ "$OS" == "Darwin" ]]; then
    # Homebrew on Apple Silicon installs to /opt/homebrew; Intel to /usr/local
    for brew_prefix in /opt/homebrew /usr/local; do
        [[ -d "$brew_prefix/bin" ]] && export PATH="$brew_prefix/bin:$PATH"
        [[ -d "$brew_prefix/opt/libtool/bin" ]] && \
            export PATH="$brew_prefix/opt/libtool/bin:$PATH"
    done

    # macOS deployment target for LuaJIT + jemalloc
    # export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-10.14}"
    # info "MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
    export MACOSX_DEPLOYMENT_TARGET=$(sw_vers -productVersion)
    info "MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"

    # Verify required tools
    for tool in cmake autoconf automake libtool; do
        command -v "$tool" &>/dev/null || \
            die "Missing: $tool  →  brew install cmake autoconf automake libtool"
    done
fi

# ── portable in-place sed ─────────────────────────────────────────────────
# BSD sed (macOS) requires -i ''  ;  GNU sed accepts -i ''  or  -i
sed_inplace() {
    local file="$1"; shift
    if sed --version 2>/dev/null | grep -q 'GNU'; then
        sed -i "$@" "$file"
    else
        sed -i '' "$@" "$file"   # BSD / macOS
    fi
}

# ── init submodules ───────────────────────────────────────────────────────
cd "$ROOT"
step "Updating git submodules …"
git submodule update --init --recursive || warn "Submodule update failed, continuing anyway..."
info "Submodules OK"
mkdir -p "$B"

# ═══════════════════════════════════════════════════════════════════════════
# jemalloc
# ═══════════════════════════════════════════════════════════════════════════
S=$(stamp jemalloc)
if [[ ! -f "$S" ]]; then
    step "Building jemalloc …"
    cd "$DEPS/jemalloc"

    [[ -f configure ]] || ./autogen.sh

    JE_CONF_EXTRA=()
    if [[ "$OS" == "Darwin" ]]; then
        # On macOS, initial-exec TLS is not supported for static libs
        JE_CONF_EXTRA+=(--disable-initial-exec-tls)
        # Prefer brew's libtool over Apple's
        export LIBTOOL="$(command -v glibtool  || command -v libtool)"
        export LIBTOOLIZE="$(command -v glibtoolize || command -v libtoolize)"
    fi

    ./configure \
        --prefix="$B/jemalloc" \
        --with-jemalloc-prefix=je_ \
        --disable-shared \
        --enable-static \
        "${JE_CONF_EXTRA[@]}" \
        --quiet

    make -j"$NPROC" install
    touch "$S"
    info "jemalloc  ✓"
else
    info "jemalloc: cached"
fi

# ═══════════════════════════════════════════════════════════════════════════
# llhttp
#
# Problem: the git repo's CMakeLists.txt contains:
#   project(llhttp VERSION _RELEASE_)
# where _RELEASE_ is a npm template placeholder, not a real version.
# cmake rejects it with "VERSION ... format invalid".
#
# Fix: read the real version from package.json and patch CMakeLists.txt.
# This avoids any npm dependency.
# ═══════════════════════════════════════════════════════════════════════════
S=$(stamp llhttp)
if [[ ! -f "$S" ]]; then
    step "Building llhttp …"
    cd "$DEPS/llhttp"

    # ── 1. Extract version from package.json (portable, no node/jq needed) ──
    LLHTTP_VER=""
    if [[ -f package.json ]]; then
        # Pattern: "version": "9.2.1"
        LLHTTP_VER="$(grep '"version"' package.json | head -1 \
            | sed 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')"
    fi
    # Fallback: use git tag
    if [[ -z "$LLHTTP_VER" ]]; then
        LLHTTP_VER="$(git describe --tags --abbrev=0 2>/dev/null \
            | sed 's/^[vV]release\///' | sed 's/^[vV]//' || echo "9.2.1")"
    fi
    # Last-resort hardcoded safe version
    [[ "$LLHTTP_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || LLHTTP_VER="9.2.1"
    info "llhttp version: $LLHTTP_VER"

    # ── 2. Patch _RELEASE_ in CMakeLists.txt ──────────────────────────────
    # if grep -q '_RELEASE_' CMakeLists.txt; then
    #     info "Patching CMakeLists.txt: VERSION _RELEASE_ → VERSION $LLHTTP_VER"
    #     cp CMakeLists.txt CMakeLists.txt.orig
    #     sed_inplace CMakeLists.txt "s/VERSION _RELEASE_/VERSION $LLHTTP_VER/"
    # fi

    # ── 3. cmake build ─────────────────────────────────────────────────────
    LLHTTP_EXTRA_FLAGS=()
    if [[ "$OS" == "Darwin" ]]; then
        # Avoid shared lib versioning issues on macOS
        LLHTTP_EXTRA_FLAGS+=("-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-undefined,dynamic_lookup")
    fi

    # cmake -S . -B build \
    #     -DCMAKE_INSTALL_PREFIX="$B/llhttp" \
    #     -DCMAKE_BUILD_TYPE=Release \
    #     -DBUILD_SHARED_LIBS=OFF \
    #     -DLLHTTP_BUILD_SHARED_LIBS=OFF \
    #     -DLLHTTP_BUILD_STATIC_LIBS=ON \
    #     -DCMAKE_C_FLAGS="-fPIC" \
    #     "${LLHTTP_EXTRA_FLAGS[@]}" \
    #     --log-level=WARNING

    # cmake --build build --target install -j"$NPROC"

    if [[ -f "$DEPS/llhttp/build/libllhttp.a" && -f "$DEPS/llhttp/build/llhttp.h" ]]; then
        info "llhttp: using pre-built binaries"
        mkdir -p "$B/llhttp/lib" "$B/llhttp/include"
        cp "$DEPS/llhttp/build/llhttp.h" "$B/llhttp/include/"
        cp "$DEPS/llhttp/build/libllhttp.a" "$B/llhttp/lib/"
        [[ -f "$DEPS/llhttp/build/libllhttp.so" ]] && cp "$DEPS/llhttp/build/libllhttp.so" "$B/llhttp/lib/" || true
    else
        git checkout v9.4.1
        npm ci
        make
        make install PREFIX="$B/llhttp"
    fi

    touch "$S"
    info "llhttp $LLHTTP_VER  ✓"
else
    info "llhttp: cached"
fi

# ═══════════════════════════════════════════════════════════════════════════
# LuaJIT
# ═══════════════════════════════════════════════════════════════════════════
S=$(stamp luajit)
if [[ ! -f "$S" ]]; then
    step "Building LuaJIT …"
    cd "$DEPS/luajit"

    LUAJIT_EXTRA=()
    if [[ "$OS" == "Darwin" ]]; then
        if [[ "$ARCH" == "arm64" ]]; then
            # Apple Silicon: LuaJIT requires these linker flags for the JIT
            # to map executable memory above the 32-bit boundary
            LUAJIT_EXTRA+=(
                "LDFLAGS=-pagezero_size 10000 -image_base 100000000"
            )
        fi
        LUAJIT_EXTRA+=(
            "MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
        )
    fi

    # make -j"$NPROC" \
    #     PREFIX="$B/luajit" \
    #     XCFLAGS="-DLUAJIT_ENABLE_LUA52COMPAT" \
    #     "${LUAJIT_EXTRA[@]}" \
    #     amalg

    # make install \
    #     PREFIX="$B/luajit" \
    #     "${LUAJIT_EXTRA[@]}"

    make -j"$NPROC" PREFIX="$B/luajit"
    make install PREFIX="$B/luajit"

    # Create a symlink without the version suffix for simpler linking
    local_lib="$B/luajit/lib"
    for name in libluajit-5.1.a libluajit.a; do
        [[ -f "$local_lib/$name" ]] && \
            ln -sf "$local_lib/$name" "$local_lib/libluajit.a" 2>/dev/null || true
    done

    touch "$S"
    info "LuaJIT  ✓"
else
    info "LuaJIT: cached"
fi

# ═══════════════════════════════════════════════════════════════════════════
# fsae – single-header, no build step
# ═══════════════════════════════════════════════════════════════════════════
if [[ -d "$DEPS/fsae" ]]; then
    info "fsae: single-header, no build step required  ✓"
else
    warn "deps/fsae not found – did git submodule update run?"
fi

# ═══════════════════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════════════════
echo ""
info "All deps ready under $B"
echo ""
echo "  ┌──────────────────────────────────────────────────────────────┐"
echo "  │  Next steps                                                  │"
echo "  ├──────────────────────────────────────────────────────────────┤"
echo "  │  make                   # Release build                      │"
echo "  │  make debug             # Debug build (-g3 -O0)              │"
echo "  │  make asan              # AddressSanitizer + UBSan           │"
echo "  │  make test              # Build + run all 4 test suites      │"
echo "  │  make test-asan         # Tests under ASAN                   │"
echo "  │                                                              │"
echo "  │  cmake workflow:                                             │"
echo "  │    cmake -S . -B build -DSANITIZE=asan                      │"
echo "  │    cmake --build build -j\$(nproc)                           │"
echo "  │    ctest --test-dir build -V                                 │"
echo "  └──────────────────────────────────────────────────────────────┘"