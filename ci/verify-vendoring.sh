#!/bin/bash
# verify-vendoring.sh — Verify that the vendored HAProxy binary is correctly
# linked and will work on any manylinux2014-compatible system.
#
# Run this OUTSIDE the build container (on a different distro) to prove
# the vendoring actually works.
#
# Usage:
#   ./ci/verify-vendoring.sh ray_haproxy/bin/haproxy ray_haproxy/bin/lib

set -euo pipefail

BINARY="${1:?Usage: $0 <haproxy-binary> <lib-dir>}"
LIB_DIR="${2:?Usage: $0 <haproxy-binary> <lib-dir>}"
ERRORS=0

pass() { echo "  PASS: $1"; }
fail() { echo "  FAIL: $1"; ERRORS=$((ERRORS + 1)); }

echo "=== Vendoring verification ==="
echo "  Binary: $BINARY"
echo "  Lib dir: $LIB_DIR"
echo

# ---------------------------------------------------------------------------
# 1. RPATH on binary must be $ORIGIN/lib
# ---------------------------------------------------------------------------
echo "--- RPATH/RUNPATH checks ---"
# patchelf --set-rpath sets RUNPATH on modern patchelf. Both RPATH and RUNPATH
# serve the same purpose; check for either.
get_rpath() {
    local elf="$1"
    patchelf --print-rpath "$elf" 2>/dev/null \
        || readelf -d "$elf" 2>/dev/null | grep -E 'RPATH|RUNPATH' | sed 's/.*\[\(.*\)\]/\1/' \
        || echo ""
}

BINARY_RPATH="$(get_rpath "$BINARY")"
if [[ "$BINARY_RPATH" == *'$ORIGIN/lib'* ]]; then
    pass "Binary RPATH/RUNPATH = $BINARY_RPATH"
else
    fail "Binary RPATH/RUNPATH = '$BINARY_RPATH' (expected \$ORIGIN/lib)"
fi

# RPATH on each vendored .so must contain $ORIGIN
for so in "$LIB_DIR"/*.so*; do
    [ -f "$so" ] || continue
    [ "$(basename "$so")" = ".gitkeep" ] && continue
    SO_RPATH="$(get_rpath "$so")"
    if [[ "$SO_RPATH" == *'$ORIGIN'* ]]; then
        pass "$(basename "$so") RPATH/RUNPATH = $SO_RPATH"
    else
        fail "$(basename "$so") RPATH/RUNPATH = '$SO_RPATH' (expected \$ORIGIN)"
    fi
done

# ---------------------------------------------------------------------------
# 2. All deps resolve WITHOUT LD_LIBRARY_PATH
# ---------------------------------------------------------------------------
echo
echo "--- Dependency resolution (no LD_LIBRARY_PATH) ---"
LDD_OUTPUT="$(ldd "$BINARY" 2>&1)"

# Check for "not found"
NOT_FOUND="$(echo "$LDD_OUTPUT" | grep "not found" || true)"
if [ -z "$NOT_FOUND" ]; then
    pass "All dependencies resolve"
else
    fail "Unresolved dependencies:"
    echo "$NOT_FOUND" | sed 's/^/         /'
fi

# ---------------------------------------------------------------------------
# 3. Vendored libs resolve from $ORIGIN/lib, not system paths
# ---------------------------------------------------------------------------
echo
echo "--- Vendored lib resolution paths ---"
for so in "$LIB_DIR"/*.so*; do
    [ -f "$so" ] || continue
    SONAME="$(basename "$so")"
    [ "$SONAME" = ".gitkeep" ] && continue

    # Check where ldd resolves this soname
    RESOLVED="$(echo "$LDD_OUTPUT" | grep "$SONAME" | head -1 || true)"
    if [ -z "$RESOLVED" ]; then
        # Lib is vendored but not directly referenced by binary (transitive dep)
        pass "$SONAME vendored (transitive dep, not in direct ldd output)"
    elif echo "$RESOLVED" | grep -q "$LIB_DIR"; then
        pass "$SONAME resolves to vendored copy"
    else
        # Resolves from system — still works but means vendoring is redundant
        echo "  WARN: $SONAME resolves from system: $RESOLVED"
    fi
done

# ---------------------------------------------------------------------------
# 4. Binary executes successfully
# ---------------------------------------------------------------------------
echo
echo "--- Execution test ---"
VERSION_OUTPUT="$("$BINARY" -v 2>&1 || true)"
if echo "$VERSION_OUTPUT" | grep -q "HAProxy version"; then
    pass "haproxy -v works: $(echo "$VERSION_OUTPUT" | head -1)"
else
    fail "haproxy -v failed: $VERSION_OUTPUT"
fi

# ---------------------------------------------------------------------------
# 5. No manylinux2014 allowlist libs accidentally vendored
# ---------------------------------------------------------------------------
echo
echo "--- Allowlist check (should NOT vendor these) ---"
ALLOWLIST_LIBS="libc.so.6 libm.so libdl.so librt.so libpthread.so libz.so libgcc_s.so"
for lib in $ALLOWLIST_LIBS; do
    if ls "$LIB_DIR"/$lib* 2>/dev/null | grep -q .; then
        fail "Vendored manylinux2014 allowlist lib: $lib (should use system copy)"
    fi
done
pass "No allowlist libs accidentally vendored"

# ---------------------------------------------------------------------------
# 6. ELF sanity
# ---------------------------------------------------------------------------
echo
echo "--- ELF sanity ---"
FILE_TYPE="$(file "$BINARY")"
if echo "$FILE_TYPE" | grep -q "ELF 64-bit.*x86-64"; then
    pass "Binary is ELF 64-bit x86-64"
elif echo "$FILE_TYPE" | grep -q "ELF 64-bit.*aarch64"; then
    pass "Binary is ELF 64-bit aarch64"
else
    fail "Unexpected binary type: $FILE_TYPE"
fi

if echo "$FILE_TYPE" | grep -q "dynamically linked"; then
    pass "Binary is dynamically linked"
else
    fail "Binary is NOT dynamically linked (expected dynamic)"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
if [ "$ERRORS" -eq 0 ]; then
    echo "=== ALL CHECKS PASSED ==="
else
    echo "=== $ERRORS CHECK(S) FAILED ==="
    exit 1
fi
