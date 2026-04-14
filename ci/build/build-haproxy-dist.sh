#!/bin/bash
# build-haproxy-dist.sh — Build an HAProxy distribution tarball with vendored
# shared libraries for bundling inside the ray-haproxy PyPI wheel.
#
# This script does what auditwheel/delocate do for Python wheels, but for a
# standalone ELF binary: it collects non-system shared library deps, copies
# them alongside the binary, and patches RPATH so the binary finds them
# at $ORIGIN/lib at runtime.
#
# Output: haproxy-linux-<arch>.tar.gz containing:
#   haproxy      — the binary (RPATH patched to $ORIGIN/lib)
#   lib/         — vendored shared libraries (RPATH patched to $ORIGIN)
#
# Usage (inside a manylinux2014 container — pin the same tag Ray uses):
#   docker run --rm -v $PWD:/work -e OUTPUT_DIR=/work/dist \
#     quay.io/pypa/manylinux2014_x86_64:2026.01.02-1 \
#     /work/ci/build/build-haproxy-dist.sh
#
# Environment variables:
#   HAPROXY_VERSION  — version to build (default: 3.2.3)
#   OPENSSL_VERSION  — OpenSSL version to build from source (default: 3.0.15)
#                      manylinux2014 ships EOL OpenSSL 1.1.1; we build 3.0.x.
#                      NOTE: OpenSSL 3.0 LTS EOLs 2026-09-07. Upgrading to 3.2+
#                      requires a newer Perl than CentOS 7 provides — either
#                      vendor Perl or migrate to manylinux_2_28 (AlmaLinux 8).
#   LUA_VERSION      — Lua version to build from source (default: 5.4.7)
#   OUTPUT_DIR       — where to write the tarball (default: /tmp/haproxy-dist)

set -euo pipefail

HAPROXY_VERSION="${HAPROXY_VERSION:-2.8.0}"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.0.15}"
LUA_VERSION="${LUA_VERSION:-5.4.7}"
OUTPUT_DIR="${OUTPUT_DIR:-/tmp/haproxy-dist}"

# ---------------------------------------------------------------------------
# Pinned source tarball checksums — update these when bumping versions.
# Prevents supply-chain attacks via compromised mirrors or CDN poisoning.
# ---------------------------------------------------------------------------
OPENSSL_SHA256="23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533"
LUA_SHA256="9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30"
HAPROXY_SHA256="f38461bce4d9a12c8ef0999fd21e33821b9146ef5fe73de37fae985a63d5f311"

verify_checksum() {
    local file="$1" expected="$2" label="$3"
    local actual
    actual="$(sha256sum "$file" | awk '{print $1}')"
    if [ "$actual" != "$expected" ]; then
        echo "ERROR: $label checksum mismatch!"
        echo "  expected: $expected"
        echo "  actual:   $actual"
        exit 1
    fi
    echo "  Checksum OK: $label"
}

ARCH="$(uname -m)"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"

# Normalise arch names to match Ray's convention.
case "$ARCH" in
    x86_64)  ARCH_LABEL="x86_64" ;;
    aarch64) ARCH_LABEL="arm64" ;;
    arm64)   ARCH_LABEL="arm64" ;;  # macOS (future)
    *)       echo "Unsupported architecture: $ARCH"; exit 1 ;;
esac

case "$OS" in
    linux)  OS_LABEL="linux" ;;
    # TODO: add macOS support once osx builds are validated.
    *)      echo "Unsupported OS: $OS (only Linux is supported for now)"; exit 1 ;;
esac

TARBALL_NAME="haproxy-${OS_LABEL}-${ARCH_LABEL}.tar.gz"
STAGE_DIR="$(mktemp -d)"
BUILD_DIR="$(mktemp -d)"
DEPS_DIR="$(mktemp -d)"  # installed deps (OpenSSL, etc.)

cleanup() { rm -rf "$STAGE_DIR" "$BUILD_DIR" "$DEPS_DIR"; }
trap cleanup EXIT

echo "==> Building HAProxy ${HAPROXY_VERSION} for ${OS_LABEL}-${ARCH_LABEL}"

# ---------------------------------------------------------------------------
# 0. Install build dependencies
#
# manylinux2014 is CentOS 7 — minimal install. We need:
#   perl-IPC-Cmd  — required by OpenSSL 3.x's Configure script
#   pcre-devel    — for HAProxy USE_PCRE=1
#   zlib-devel    — for HAProxy USE_ZLIB=1
#   lua-devel     — for HAProxy USE_LUA=1 (Lua 5.1 on CentOS 7)
# ---------------------------------------------------------------------------
echo "==> Installing build dependencies"
yum install -y perl-IPC-Cmd pcre-devel zlib-devel readline-devel 2>/dev/null

# ---------------------------------------------------------------------------
# 1. Build OpenSSL from source
#
# manylinux2014 ships OpenSSL 1.1.1 which is EOL (2023-09-11).
# We build OpenSSL 3.x from source and link HAProxy against it.
# This is the same approach used by pyca/cryptography's manylinux builds.
# ---------------------------------------------------------------------------
OPENSSL_MAJOR="${OPENSSL_VERSION%%.*}"
OPENSSL_SRC="$BUILD_DIR/openssl-${OPENSSL_VERSION}"

echo "==> Building OpenSSL ${OPENSSL_VERSION}"
curl -fsSL -o "$BUILD_DIR/openssl.tar.gz" \
    "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
verify_checksum "$BUILD_DIR/openssl.tar.gz" "$OPENSSL_SHA256" "OpenSSL"
tar -xzf "$BUILD_DIR/openssl.tar.gz" -C "$BUILD_DIR"

pushd "$OPENSSL_SRC" > /dev/null
./config --prefix="$DEPS_DIR" --openssldir="$DEPS_DIR/ssl" \
    shared \
    -fPIC -O2
make -j"$(nproc)" > /dev/null
make install_sw > /dev/null
popd > /dev/null

echo "==> OpenSSL $(${DEPS_DIR}/bin/openssl version)"

# ---------------------------------------------------------------------------
# 1b. Build Lua 5.4 from source
#
# HAProxy 3.2 requires Lua 5.3+, but manylinux2014 (CentOS 7) ships Lua 5.1.
# Lua is tiny (~30s compile).
# ---------------------------------------------------------------------------
LUA_SRC="$BUILD_DIR/lua-${LUA_VERSION}"

echo "==> Building Lua ${LUA_VERSION}"
curl -fsSL -o "$BUILD_DIR/lua.tar.gz" \
    "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz"
verify_checksum "$BUILD_DIR/lua.tar.gz" "$LUA_SHA256" "Lua"
tar -xzf "$BUILD_DIR/lua.tar.gz" -C "$BUILD_DIR"

make -C "$LUA_SRC" linux MYCFLAGS="-fPIC" -j"$(nproc)" > /dev/null
make -C "$LUA_SRC" install INSTALL_TOP="$DEPS_DIR" > /dev/null

echo "==> Lua $($DEPS_DIR/bin/lua -v 2>&1 || true)"

# Detect OpenSSL lib directory (lib/ on some platforms, lib64/ on others).
if [ -d "$DEPS_DIR/lib64" ]; then
    OPENSSL_LIB_DIR="$DEPS_DIR/lib64"
elif [ -d "$DEPS_DIR/lib" ]; then
    OPENSSL_LIB_DIR="$DEPS_DIR/lib"
else
    echo "ERROR: cannot find OpenSSL lib directory under $DEPS_DIR"; exit 1
fi

# ---------------------------------------------------------------------------
# 2. Download and compile HAProxy
# ---------------------------------------------------------------------------
HAPROXY_MAJOR_MINOR="${HAPROXY_VERSION%.*}"  # e.g. 3.2 from 3.2.3

echo "==> Downloading HAProxy ${HAPROXY_VERSION}"
curl -fsSL -o "$BUILD_DIR/haproxy.tar.gz" \
    "https://www.haproxy.org/download/${HAPROXY_MAJOR_MINOR}/src/haproxy-${HAPROXY_VERSION}.tar.gz"
verify_checksum "$BUILD_DIR/haproxy.tar.gz" "$HAPROXY_SHA256" "HAProxy"
tar -xzf "$BUILD_DIR/haproxy.tar.gz" -C "$BUILD_DIR" --strip-components=1

echo "==> Compiling HAProxy"
make -C "$BUILD_DIR" \
    TARGET=linux-glibc \
    USE_OPENSSL=1 \
    SSL_INC="$DEPS_DIR/include" \
    SSL_LIB="$OPENSSL_LIB_DIR" \
    USE_ZLIB=1 \
    USE_PCRE=1 \
    USE_LUA=1 \
    LUA_INC="$DEPS_DIR/include" \
    LUA_LIB="$DEPS_DIR/lib" \
    USE_PROMEX=1 \
    -j"$(nproc)"

# ---------------------------------------------------------------------------
# 3. Stage binary
# ---------------------------------------------------------------------------
mkdir -p "$STAGE_DIR/lib"
cp "$BUILD_DIR/haproxy" "$STAGE_DIR/haproxy"
chmod 755 "$STAGE_DIR/haproxy"

# ---------------------------------------------------------------------------
# 4. Vendor shared libraries
#
# Walk ldd output, skip manylinux2014-guaranteed system libs (PEP 599),
# copy the rest to lib/, recursively resolve transitive deps.
# This is functionally equivalent to what auditwheel does for Python wheels,
# but applied to a standalone binary. auditwheel only operates on .whl files
# (Python extension modules), so we replicate its logic directly.
# ---------------------------------------------------------------------------

# manylinux2014 system library allowlist (PEP 599).
# These are guaranteed present on any manylinux2014-compatible system.
# NOTE: libcrypt.so.1 (glibc) is on the allowlist, but libcrypt.so.2
# (libxcrypt, installed in manylinux2014) is NOT. We use exact soname
# matching where the version suffix matters.
MANYLINUX_ALLOWLIST="linux-vdso|ld-linux|libc\\.so\\.6|libm\\.so|libdl\\.so|librt\\.so|libpthread\\.so|libz\\.so|libgcc_s|libstdc\\+\\+|libutil\\.so|libresolv\\.so|libnsl\\.so|libcrypt\\.so\\.1|libX11\\.so|libXext\\.so|libXrender\\.so|libICE\\.so|libSM\\.so|libGL\\.so|libgobject\\.so|libgthread\\.so|libglib\\.so"

# Recursively collect all non-system .so deps (handles transitive deps).
collect_deps() {
    local binary="$1"
    ldd "$binary" 2>/dev/null | while read -r line; do
        local lib_name lib_path
        lib_name=$(echo "$line" | awk '{print $1}')
        lib_path=$(echo "$line" | awk '{print $3}')

        # Skip virtual/not-found/allowlisted entries.
        [ -z "$lib_path" ] && continue
        [ "$lib_path" = "not" ] && continue
        echo "$lib_name" | grep -qE "$MANYLINUX_ALLOWLIST" && continue

        # Skip if already vendored.
        [ -f "$STAGE_DIR/lib/$lib_name" ] && continue

        echo "  Vendoring: $lib_name ($lib_path)"
        cp "$lib_path" "$STAGE_DIR/lib/$lib_name"
        chmod 755 "$STAGE_DIR/lib/$lib_name"

        # Recurse into this lib's own deps.
        collect_deps "$lib_path"
    done
}

collect_deps "$STAGE_DIR/haproxy"

# Also vendor the OpenSSL .so files we built from source — ldd sees them by
# their build path, but make sure their sonames are in lib/.
find "$OPENSSL_LIB_DIR" -maxdepth 1 -name 'libssl.so.*' -o -name 'libcrypto.so.*' | \
while read -r sopath; do
    soname="$(basename "$sopath")"
    [ -f "$STAGE_DIR/lib/$soname" ] && continue
    echo "  Vendoring (openssl): $soname"
    cp "$sopath" "$STAGE_DIR/lib/$soname"
    chmod 755 "$STAGE_DIR/lib/$soname"
done

# Strip debug symbols (reduces size ~30%).
find "$STAGE_DIR/lib" -name '*.so*' -exec strip --strip-debug {} \; 2>/dev/null || true
strip --strip-debug "$STAGE_DIR/haproxy" 2>/dev/null || true

# Patch RPATH so the binary finds vendored libs at $ORIGIN/lib.
# patchelf is pre-installed in manylinux2014 images.
patchelf --set-rpath '$ORIGIN/lib' "$STAGE_DIR/haproxy"

# Also patch vendored libs to find each other via $ORIGIN.
find "$STAGE_DIR/lib" -name '*.so*' -exec patchelf --set-rpath '$ORIGIN' {} \;

echo "  RPATH: $(patchelf --print-rpath "$STAGE_DIR/haproxy")"
echo "  Verifying all deps resolve:"
LD_LIBRARY_PATH="$STAGE_DIR/lib" ldd "$STAGE_DIR/haproxy"

# ---------------------------------------------------------------------------
# 5. Sanity checks
# ---------------------------------------------------------------------------
echo "  Binary hardening:"
# Verify RELRO (full or partial)
if readelf -l "$STAGE_DIR/haproxy" 2>/dev/null | grep -q GNU_RELRO; then
    echo "    RELRO:        yes"
else
    echo "    RELRO:        NO — consider adding -Wl,-z,relro,-z,now"
fi
# Verify NX (non-executable stack)
if readelf -l "$STAGE_DIR/haproxy" 2>/dev/null | grep -q 'GNU_STACK.*RW '; then
    echo "    NX (stack):   yes"
else
    echo "    NX (stack):   check manually"
fi
# Verify stack canary
if readelf -s "$STAGE_DIR/haproxy" 2>/dev/null | grep -q __stack_chk_fail; then
    echo "    Stack canary: yes"
else
    echo "    Stack canary: NO — consider -fstack-protector-strong"
fi

LD_LIBRARY_PATH="$STAGE_DIR/lib" "$STAGE_DIR/haproxy" -v

# ---------------------------------------------------------------------------
# 6. Package
# ---------------------------------------------------------------------------
mkdir -p "$OUTPUT_DIR"
tar -czf "$OUTPUT_DIR/$TARBALL_NAME" -C "$STAGE_DIR" .

echo ""
echo "==> Built: $OUTPUT_DIR/$TARBALL_NAME"
echo "    Contents:"
tar -tzf "$OUTPUT_DIR/$TARBALL_NAME"
echo "    Size: $(du -h "$OUTPUT_DIR/$TARBALL_NAME" | cut -f1)"
echo "    sha256: $(sha256sum "$OUTPUT_DIR/$TARBALL_NAME" | cut -d' ' -f1)"
