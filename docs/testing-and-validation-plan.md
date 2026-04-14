# ray-haproxy: Testing and Validation Plan

## Overview

`ray-haproxy` is a standalone PyPI package that distributes a pre-built HAProxy
binary with vendored shared libraries (`.so` files). The binary is dynamically
linked but ships its own copies of non-system libraries (OpenSSL, PCRE, Lua,
libxcrypt), with RPATH patched to `$ORIGIN/lib` so it runs on any
manylinux2014-compatible Linux system without requiring a system HAProxy
install.

This document defines the milestones, testing strategy, and validation criteria
for rolling out `ray-haproxy` across Ray's supported platforms.

---

## Milestones

### Milestone 1: Publish `ray-haproxy` PyPI package for x86_64

Vendor an HAProxy binary that has been dynamically linked against shared
libraries, with those shared libraries vendored alongside the binary in the
wheel.

**Testing (GitHub Actions, `ray-project/ray-haproxy`):**
- Smoke test the binary against a variety of Linux distributions
- Test wheel installation via `pip install` across Python versions

**Exit criteria:**
- `pip install ray-haproxy` works on a clean Linux machine
- `from ray_haproxy import get_haproxy_binary` returns a working binary
- Package is published to PyPI

### Milestone 2: Install `ray-haproxy` in Ray CI only

Add `ray-haproxy` to the Serve CI test environment so that Ray's HAProxy
integration tests run against the bundled binary instead of (or in addition to)
a system-installed HAProxy.

**Testing (Buildkite, `ray-project/ray`):**
- Unit tests for `get_haproxy_binary()` pass
- Serve integration tests pass with the bundled binary
- No regressions in existing Serve tests that don't use HAProxy

**Exit criteria:**
- Serve CI is green with `ray-haproxy` installed
- Existing test behavior is unchanged for users who don't have `ray-haproxy`

### Milestone 3: Add `ray-haproxy` as a dependency in `ray[serve]`

Make `ray-haproxy` install automatically for all `ray[serve]` users on Linux.
Update `serve.build.Dockerfile` to include it.

**Testing (Buildkite, `ray-project/ray`):**
- Serve Docker images build and boot successfully
- `get_haproxy_binary()` returns the bundled path in Docker images
- End-to-end traffic routing through the bundled HAProxy

**Exit criteria:**
- `pip install "ray[serve]"` on Linux installs `ray-haproxy` automatically
- Serve Docker images ship with the bundled HAProxy
- CVE update pipeline has been exercised at least once

---

## Platform Support

### Ray's platform matrix

| Platform | Arch | Ray wheels | Ray Docker images |
|---|---|---|---|
| Linux (manylinux2014, glibc 2.17+) | x86_64 | Yes | Yes |
| Linux (manylinux2014, glibc 2.17+) | aarch64 | Yes | Yes |
| macOS | arm64 (Apple Silicon) | Yes | No |
| Windows | x86_64 | Yes | No |

### ray-haproxy coverage

| Platform | Arch | Status | Notes |
|---|---|---|---|
| Linux | x86_64 | **Milestone 1** | Shipping now |
| Linux | aarch64 | **Future** | Blocked on upstream arm packages (~2 weeks) |
| macOS | arm64 | **Deferred** | HAProxy team offered help; not yet started |
| Windows | x86_64 | **Not planned** | HAProxy does not support Windows natively |

Users on unsupported platforms are unaffected: the `sys_platform == 'linux'`
environment marker in the extras dependency means `pip install` skips
`ray-haproxy` on macOS and Windows. Ray Serve falls back to system-installed
HAProxy or the `RAY_SERVE_HAPROXY_BINARY` env var.

---

## Test Layers

### Layer 1: Build-time verification

- **Where**: GitHub Actions (`ray-project/ray-haproxy`, `release.yml` &rarr; `build` job)
- **Runner**: `ubuntu-22.04` host, executing `build-haproxy-dist.sh` inside a `quay.io/pypa/manylinux2014_x86_64:2026.01.02-1` Docker container
- **When**: Every release (tag push `v*`) and manual `workflow_dispatch`
- **Milestone**: 1

| Check | What it validates | Implementation |
|---|---|---|
| Source checksum verification | No supply-chain tampering of OpenSSL, Lua, HAProxy tarballs | `verify_checksum()` in build script |
| Compilation succeeds | HAProxy links correctly against vendored OpenSSL + Lua | `make` exit code |
| `ldd` resolution | All shared libraries resolve in the staging directory | `LD_LIBRARY_PATH=lib ldd haproxy` |
| RPATH patching | Binary has `$ORIGIN/lib`, vendored libs have `$ORIGIN` | `patchelf --print-rpath` |
| Binary execution | HAProxy runs and prints version inside the build container | `haproxy -v` |
| Binary hardening | RELRO, NX, stack canary status logged | `readelf` checks |

### Layer 2: Cross-distro vendoring verification

- **Where**: GitHub Actions (`ray-project/ray-haproxy`, `release.yml` &rarr; `verify` job)
- **Runner**: `ubuntu-22.04` host, launching 6 Docker containers in parallel
- **When**: Every release, after `build` job completes
- **Milestone**: 1

Runs `ci/verify-vendoring.sh` inside Docker containers for distros with
different glibc versions. Proves the binary works outside the build container.

| Distro | glibc | Why this distro |
|---|---|---|
| ubuntu:20.04 | 2.31 | Common in existing Ray deployments |
| ubuntu:22.04 | 2.35 | Current Ray CI runner OS |
| ubuntu:24.04 | 2.39 | Latest Ubuntu LTS |
| debian:bookworm-slim | 2.36 | Common Docker base image |
| rockylinux:9 | 2.34 | RHEL-family (enterprise users) |
| amazonlinux:2023 | 2.34 | AWS ECS/EKS deployments |

Each distro runs the full `verify-vendoring.sh` suite:

| Check | What it validates |
|---|---|
| RPATH/RUNPATH present | `$ORIGIN/lib` on binary, `$ORIGIN` on each .so |
| All deps resolve (no `LD_LIBRARY_PATH`) | Dynamic linker finds vendored libs via RUNPATH alone |
| Vendored libs resolve from `lib/` dir | Not accidentally picking up system copies |
| `haproxy -v` executes | Binary actually runs on this distro |
| No allowlist libs vendored | We didn't accidentally bundle libc, libz, etc. |
| ELF format correct | 64-bit, dynamically linked, correct arch |

Additionally, each container installs the `.whl` via pip and verifies the
Python import path end-to-end:

```python
from ray_haproxy import get_haproxy_binary
binary = get_haproxy_binary()    # resolves to site-packages/.../bin/haproxy
subprocess.run([binary, '-v'])   # binary executes from installed location
```

This catches wheel packaging errors (missing files, lost executable bits,
broken path resolution in `__init__.py`).

**Fail criteria**: Any single distro failure blocks the PyPI publish.

### Layer 3: Wheel installation smoke test

- **Where**: GitHub Actions (`ray-project/ray-haproxy`, `release.yml` &rarr; `smoke-test` job)
- **Runner**: `ubuntu-22.04` with `actions/setup-python` for each Python version
- **When**: Every release, after `build` job completes (runs in parallel with Layer 2)
- **Milestone**: 1

Installs the built wheel via `pip install` across Python versions.

| Python | Runner |
|---|---|
| 3.9 | ubuntu-22.04 |
| 3.10 | ubuntu-22.04 |
| 3.11 | ubuntu-22.04 |
| 3.12 | ubuntu-22.04 |

Each Python version runs:
```python
from ray_haproxy import get_haproxy_binary
import subprocess
binary = get_haproxy_binary()
result = subprocess.run([binary, '-v'], capture_output=True, text=True)
assert result.returncode == 0
assert 'HAProxy version' in result.stdout
```

### Layer 4: Ray Serve unit tests

- **Where**: Buildkite (`ray-project/ray`, Serve test suite)
- **Runner**: Ray's Wanda-managed containers (manylinux2014-based)
- **When**: Every Ray PR that touches Serve code
- **Milestone**: 2

| Test | What it validates |
|---|---|
| `test_haproxy_binary.py` (unit) | `get_haproxy_binary()` resolution: env var, `ray_haproxy` package, PATH fallback, error messages |
| Serve startup with `ray-haproxy` installed | `get_haproxy_binary()` returns bundled path, HAProxy process starts |
| Serve startup without `ray-haproxy` installed | Falls back to system PATH or raises clear error |
| HAProxy config generation + reload | Generated config is accepted by the bundled binary |

### Layer 5: Ray Serve integration tests

- **Where**: Buildkite (`ray-project/ray`, Serve test suite)
- **Runner**: Ray's Wanda-managed containers (manylinux2014-based)
- **When**: Every Ray PR that touches Serve code
- **Milestone**: 2, 3

| Test | What it validates |
|---|---|
| Traffic routing through HAProxy | End-to-end request flows through the bundled HAProxy |
| Serve Docker image boots | HAProxy starts from the bundled binary inside the Serve image |
| `get_haproxy_binary()` returns bundled path in Docker | Not falling back to a system HAProxy |
| No conflicting system HAProxy | Vendored .so files take precedence over any system copies |

---

## Architecture-Specific Concerns

### x86_64 (Milestone 1 — active)

- **Build environment**: `quay.io/pypa/manylinux2014_x86_64:2026.01.02-1`
- **glibc minimum**: 2.17 (manylinux2014 guarantee)
- **Vendored libraries**: libssl.so.3, libcrypto.so.3, libpcre.so.1, libpcreposix.so.0, libcrypt.so.2
- **Known quirk**: `libcrypt.so.2` (from libxcrypt) must be vendored — it is NOT on the manylinux2014 PEP 599 allowlist despite being present in the build container. The allowlist only guarantees `libcrypt.so.1` (from glibc).

### aarch64 (Future)

- **Build environment**: `quay.io/pypa/manylinux2014_aarch64:2026.01.02-1`
- **Blocker**: Upstream HAProxy arm packages not yet available (~2 weeks)
- **Expected differences from x86_64**:
  - Same vendored libraries (OpenSSL, PCRE, Lua, libcrypt)
  - `ld-linux-aarch64.so.1` instead of `ld-linux-x86-64.so.2`
  - Wheel tag: `manylinux_2_17_aarch64`
- **Validation**: Same Layer 1–3 tests, targeting aarch64 containers
- **Additional note**: Cross-compilation is NOT used. The build script runs natively inside the aarch64 manylinux2014 container (requires an arm64 CI runner or QEMU).

### macOS arm64 (Deferred)

- **Build approach**: TBD. Likely Homebrew-based or from-source with `install_name_tool` / `@loader_path` (macOS equivalent of `$ORIGIN`)
- **Key differences from Linux**:
  - `otool -L` instead of `ldd`
  - `install_name_tool -change` instead of `patchelf --set-rpath`
  - `@loader_path/lib` instead of `$ORIGIN/lib`
  - `.dylib` instead of `.so`
  - No manylinux — uses `macosx_11_0_arm64` wheel tag
  - Code signing may be required for Gatekeeper
- **Validation**: Separate verify script needed (`verify-vendoring-macos.sh`)

### Windows (Not planned)

HAProxy does not support Windows. Ray Serve on Windows should either:
- Not attempt to use HAProxy (graceful fallback)
- Raise a clear error directing users to WSL

The `sys_platform == 'linux'` environment marker ensures `ray-haproxy` is never
installed on Windows.

---

## Regression Testing

### What could break after initial release

| Risk | How we catch it | When |
|---|---|---|
| New HAProxy patch changes linked libraries | `collect_deps()` in build script re-scans `ldd` every build | Every release |
| OpenSSL CVE | `check-haproxy-version.yml` doesn't cover OpenSSL — **gap** | Need separate OpenSSL version check |
| manylinux2014 image update changes system libs | Pinned image tag `2026.01.02-1` prevents drift | Manual update only |
| New Linux distro drops a lib we assumed was system-provided | Cross-distro verification matrix catches this | Every release |
| pip behavior change breaks wheel install | Smoke tests across Python versions | Every release |
| Ray Serve changes `get_haproxy_binary()` call site | Unit tests in Ray repo | Ray CI |

### Gap: OpenSSL version monitoring

The `check-haproxy-version.yml` workflow monitors HAProxy releases but does NOT
monitor OpenSSL. OpenSSL 3.0 LTS EOLs **2026-09-07**. Before that date:

1. Add a similar daily workflow checking OpenSSL 3.0.x tags
2. Plan migration to OpenSSL 3.2+ (requires solving the Perl 5.16 limitation
   in manylinux2014, or migrating to manylinux_2_28)

---

## Manual Validation Checklist (Milestone 1, first release)

Before tagging `v2.8.12`:

- [ ] `build-haproxy-dist.sh` succeeds in manylinux2014 container
- [ ] All three source checksums verify (OpenSSL, Lua, HAProxy)
- [ ] `verify-vendoring.sh` passes on the host machine
- [ ] Wheel builds with correct tag (`py3-none-manylinux_2_17_x86_64`)
- [ ] Wheel contents include: `__init__.py`, `bin/haproxy`, `bin/lib/*.so*`, `LICENSE`, `THIRD_PARTY_LICENSES`
- [ ] `haproxy -v` works from the installed wheel without `LD_LIBRARY_PATH`
- [ ] `get_haproxy_binary()` returns the bundled path (not system PATH)
- [ ] `RAY_SERVE_HAPROXY_BINARY` override works
- [ ] Missing package raises `FileNotFoundError` with actionable message
- [ ] GPL license text is present in the wheel
- [ ] PyPI trusted publisher is configured
- [ ] GitHub environment `pypi` exists with branch protection
