#!/usr/bin/env bash
# =============================================================================
# setup-debian13.sh - reproducible VC4CL build for Raspberry Pi 3B on
#                     Debian 13 (trixie) arm64
# =============================================================================
#
# What this script does, in order:
#   1. Installs the base toolchain and OpenCL development packages.
#   2. Provides LLVM 14 + clang 14, either from a local .deb directory or from
#      the Debian 12 (bookworm) archive with a narrow apt pin.
#   3. Provides the Raspberry Pi userland libraries (libraspberrypi0/-dev) from
#      the Raspberry Pi bookworm repository, again with a narrow apt pin.
#   4. Recreates the traditional /opt/vc/lib layout with symlinks into
#      /usr/lib/aarch64-linux-gnu, which is where Debian installs them.
#   5. Builds VC4C against LLVM 14 and then VC4CL against VC4C.
#   6. Installs and enables the vc4cl-v3d-power.service systemd unit.
#   7. Optionally registers libVC4CL.so as an OpenCL ICD vendor.
#
# Every step checks the current state first, so the script can be re-run after
# a partial failure without reinstalling or rebuilding everything.
#
# Usage:
#   ./setup-debian13.sh                 # normal build, ICD disabled
#   ./setup-debian13.sh --with-icd      # build with cl_khr_icd exports and
#                                       # register /etc/OpenCL/vendors/vc4cl.icd
#   ./setup-debian13.sh --skip-build    # only packages, symlinks and service
#   ./setup-debian13.sh --no-service    # do not install the power service
#   ./setup-debian13.sh --help
#
# Environment overrides:
#   ROOT=/home/raspi/vc4cl       Workspace containing VC4C, VC4CL, VC4CLStdLib
#   JOBS=2                       Parallel build jobs (1 GB Pi 3B: keep this low)
#   LLVM14_DEB_DIR=/path/debs    Install LLVM 14 from local .deb files instead of
#                                adding the Debian bookworm archive
#   VC4CL_BUILD_ICD=ON           Same effect as --with-icd
#
# Requirements: run as a normal user with sudo rights, or as root. The script
# only calls sudo for package management, copying into /etc and systemctl.
#
# -----------------------------------------------------------------------------
# Why LLVM 14 and not the LLVM shipped by Debian 13?
# -----------------------------------------------------------------------------
# VC4C generates and links LLVM IR with the C++ API of LLVM 14. LLVM 15+ made
# opaque pointers the only mode and changed/removed several APIs VC4C uses, so
# newer LLVM versions do not compile VC4C. Debian 13 (trixie) ships LLVM 19/20,
# therefore LLVM 14 must come from Debian 12 (bookworm) or from a manual .deb
# set. The bookworm LLVM 14 packages only need glibc/libstdc++ symbols that are
# also present in trixie, so they run fine on Debian 13.
#
# -----------------------------------------------------------------------------
# Why the Raspberry Pi userland packages from bookworm?
# -----------------------------------------------------------------------------
# VC4CL uses libbcm_host.so (model/userland detection), libvchiq_arm.so and
# libvcos.so (legacy GPU services). Debian 13 no longer ships these packages;
# Debian 12 bookworm arm64 still does. The library interfaces are stable, so the
# bookworm arm64 build works on trixie. The /opt/vc/lib symlinks are needed
# because VC4CL's CMake hints, plus the traditional Raspbian layout, look there
# while Debian installs the real files in /usr/lib/aarch64-linux-gnu.
#
# -----------------------------------------------------------------------------
# Why NO_VCSM is expected
# -----------------------------------------------------------------------------
# libvcsm was removed with kernel 5.9+. VC4CL's CMake detects the missing
# library and automatically compiles the hal/no_vcsm_sources.list variant
# (-DNO_VCSM). This port therefore uses the mailbox memory manager. Nothing in
# this script forces or fakes a VCSM library.
# =============================================================================

set -euo pipefail

ROOT="${ROOT:-/home/raspi/vc4cl}"
JOBS="${JOBS:-2}"
LLVM14_DEB_DIR="${LLVM14_DEB_DIR:-}"
VC4CL_BUILD_ICD="${VC4CL_BUILD_ICD:-OFF}"

SKIP_BUILD=0
INSTALL_SERVICE=1

LLVM_BOOKWORM_SOURCES=/etc/apt/sources.list.d/vc4cl-llvm14-bookworm.sources
LLVM_PIN=/etc/apt/preferences.d/vc4cl-llvm14.pref
RPI_SOURCES=/etc/apt/sources.list.d/vc4cl-raspberrypi-bookworm.sources
RPI_PIN=/etc/apt/preferences.d/vc4cl-raspberrypi.pref
RPI_KEYRING=/usr/share/keyrings/raspberrypi-archive-keyring.gpg
V3D_DEVICE=/sys/bus/platform/devices/3fc00000.v3d

APT_UPDATED=0

log()  { printf '\n==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --with-icd)   VC4CL_BUILD_ICD=ON ;;
        --skip-build) SKIP_BUILD=1 ;;
        --no-service) INSTALL_SERVICE=0 ;;
        --help|-h)    usage; exit 0 ;;
        *)            die "unknown argument: $1 (use --help)" ;;
    esac
    shift
done

if [ "$(id -u)" -eq 0 ]; then
    SUDO=()
else
    command -v sudo >/dev/null 2>&1 || die "sudo is required when not running as root"
    SUDO=(sudo)
fi

apt_update_once() {
    if [ "$APT_UPDATED" -eq 0 ]; then
        "${SUDO[@]}" apt-get update
        APT_UPDATED=1
    fi
}

# Would apt-get consider this package installed and configured?
have_pkg() {
    dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q '^install ok installed$'
}

ensure_pkgs() {
    local pkg
    local missing=()
    for pkg in "$@"; do
        if ! have_pkg "$pkg"; then
            missing+=("$pkg")
        fi
    done
    if [ "${#missing[@]}" -eq 0 ]; then
        info "already installed: $*"
        return 0
    fi
    apt_update_once
    "${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive \
        apt-get install -y --no-install-recommends "${missing[@]}"
}

# Write a root-owned configuration file from stdin, replacing it atomically as
# far as apt/systemd are concerned (they re-read on daemon-reload or next run).
write_root_file() {
    local path=$1
    "${SUDO[@]}" tee "$path" >/dev/null
}

check_platform() {
    local arch codename pretty="unknown"
    arch="$(uname -m)"
    if [ "$arch" != "aarch64" ]; then
        warn "expected aarch64, found $arch; this port targets the Pi 3B on arm64"
    fi
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        codename="${VERSION_CODENAME:-unknown}"
        pretty="${PRETTY_NAME:-unknown}"
    else
        codename=unknown
    fi
    info "OS: $pretty"
    if [ "$codename" != "trixie" ]; then
        warn "this script was written for Debian 13 (trixie); found '$codename'"
    fi
    info "workspace: $ROOT"
}

# -----------------------------------------------------------------------------
# 1. Base toolchain and OpenCL
# -----------------------------------------------------------------------------
install_base_packages() {
    log "Base toolchain and OpenCL development packages"
    # ocl-icd-opencl-dev provides libOpenCL.so and the ICD loader headers,
    # opencl-headers/ocl-icd-opencl-dev provide CL/cl.h. clinfo and strace are
    # diagnostic tools. pkg-config is needed by cmake/icd.cmake when
    # BUILD_ICD=ON. raspi-firmware satisfies libraspberrypi0's bootloader
    # dependency from Debian, so apt does not pull the Raspberry Pi bootloader
    # package and touch the boot partition.
    ensure_pkgs \
        ca-certificates curl gnupg \
        cmake make g++ git pkg-config \
        opencl-headers opencl-clhpp-headers \
        ocl-icd-opencl-dev ocl-icd-libopencl1 \
        clinfo strace \
        raspi-firmware
}

# -----------------------------------------------------------------------------
# 2. LLVM 14 + clang 14
# -----------------------------------------------------------------------------
install_llvm14_from_debs() {
    local -a debs=()
    local f
    shopt -s nullglob
    debs=("$LLVM14_DEB_DIR"/*.deb)
    shopt -u nullglob
    if [ "${#debs[@]}" -eq 0 ]; then
        die "LLVM14_DEB_DIR=$LLVM14_DEB_DIR contains no .deb files"
    fi
    log "Installing LLVM 14 from ${#debs[@]} local .deb file(s)"
    # dpkg can fail on ordering within one invocation; apt-get -f install then
    # resolves the dependencies. No package is removed or upgraded globally.
    if ! "${SUDO[@]}" dpkg -i "${debs[@]}"; then
        "${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive apt-get -f install -y
    fi
}

add_llvm14_bookworm_repo() {
    log "LLVM 14 not found; adding the Debian bookworm archive (pinned)"
    # trixie has LLVM 19/20 only; VC4C needs the LLVM 14 C++ API.
    write_root_file "$LLVM_BOOKWORM_SOURCES" <<'EOF'
# Added by vc4cl/scripts/setup-debian13.sh
# LLVM 14 is required by VC4C; Debian 13 only ships newer LLVM versions.
Types: deb
URIs: http://deb.debian.org/debian/
Suites: bookworm
Components: main
Architectures: arm64
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp
EOF
    # Keep bookworm at a low priority for everything except LLVM 14 and clang 14,
    # so a later "apt upgrade" cannot silently downgrade unrelated trixie
    # packages to bookworm.
    write_root_file "$LLVM_PIN" <<'EOF'
# Added by vc4cl/scripts/setup-debian13.sh
Package: llvm-14 llvm-14-* clang-14 clang-14-* libclang-14* libllvm14*
Pin: release n=bookworm
Pin-Priority: 500

Package: *
Pin: release n=bookworm
Pin-Priority: 100
EOF
    APT_UPDATED=0
    apt_update_once
}

install_llvm14() {
    if command -v llvm-config-14 >/dev/null 2>&1 && command -v clang-14 >/dev/null 2>&1; then
        info "LLVM 14 already present: $(llvm-config-14 --version 2>/dev/null || echo unknown)"
        return 0
    fi
    if [ -n "$LLVM14_DEB_DIR" ]; then
        install_llvm14_from_debs
    else
        add_llvm14_bookworm_repo
        "${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive \
            apt-get install -y --no-install-recommends -t bookworm \
            llvm-14 llvm-14-dev llvm-14-tools clang-14 libclang-14-dev
    fi
    command -v llvm-config-14 >/dev/null 2>&1 \
        || die "llvm-config-14 still missing after installation"
    command -v clang-14 >/dev/null 2>&1 \
        || die "clang-14 still missing after installation"
    info "LLVM 14 ready: $(llvm-config-14 --version)"
}

# -----------------------------------------------------------------------------
# 3. Raspberry Pi userland libraries
# -----------------------------------------------------------------------------
add_raspberrypi_repo() {
    if [ ! -s "$RPI_KEYRING" ]; then
        local tmp
        tmp="$(mktemp)"
        log "Fetching the Raspberry Pi archive signing key" >&2
        curl -fsSL https://archive.raspberrypi.org/debian/raspberrypi.gpg.key -o "$tmp"
        # The published file is ASCII-armored; apt wants a binary keyring.
        gpg --dearmor < "$tmp" | "${SUDO[@]}" tee "$RPI_KEYRING" >/dev/null
        rm -f "$tmp"
    fi

    write_root_file "$RPI_SOURCES" <<'EOF'
# Added by vc4cl/scripts/setup-debian13.sh
# Debian 13 dropped the Raspberry Pi userland packages that VC4CL loads.
Types: deb
URIs: http://archive.raspberrypi.org/debian/
Suites: bookworm
Components: main
Architectures: arm64
Signed-By: /usr/share/keyrings/raspberrypi-archive-keyring.gpg
EOF
    write_root_file "$RPI_PIN" <<'EOF'
# Added by vc4cl/scripts/setup-debian13.sh
Package: libraspberrypi0 libraspberrypi-dev libraspberrypi-bin libraspberrypi-doc
Pin: origin archive.raspberrypi.org
Pin-Priority: 500

Package: *
Pin: origin archive.raspberrypi.org
Pin-Priority: 100
EOF
    APT_UPDATED=0
}

install_rpi_userland() {
    log "Raspberry Pi userland libraries (bookworm arm64)"
    if have_pkg libraspberrypi0 && have_pkg libraspberrypi-dev; then
        info "libraspberrypi0/libraspberrypi-dev already installed"
        return 0
    fi
    add_raspberrypi_repo
    apt_update_once
    # -t bookworm picks the Raspberry Pi archive version, not a trixie package.
    # raspi-firmware was installed above, so the bootloader alternative does not
    # drag in raspberrypi-bootloader and cannot modify the boot partition.
    "${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive \
        apt-get install -y --no-install-recommends -t bookworm \
        libraspberrypi0 libraspberrypi-dev
}

link_opt_vc_libs() {
    log "Creating /opt/vc/lib compatibility symlinks"
    local libdir=/usr/lib/aarch64-linux-gnu
    local lib
    "${SUDO[@]}" install -d -m 0755 /opt/vc/lib
    # ln -sfn makes this idempotent and also repoints stale symlinks.
    for lib in libbcm_host.so libbcm_host.so.0 \
               libvchiq_arm.so libvchiq_arm.so.0 \
               libvcos.so libvcos.so.0; do
        if [ -e "$libdir/$lib" ]; then
            "${SUDO[@]}" ln -sfn "$libdir/$lib" "/opt/vc/lib/$lib"
        else
            warn "$libdir/$lib is missing; /opt/vc/lib/$lib not created"
        fi
    done
    ls -l /opt/vc/lib
}

# -----------------------------------------------------------------------------
# 4. Build VC4C then VC4CL
# -----------------------------------------------------------------------------
prepare_shims() {
    log "Preparing LLVM 14 command shims in $ROOT/bin"
    install -d "$ROOT/bin"
    ln -sfn /usr/bin/clang-14 "$ROOT/bin/clang"
    ln -sfn /usr/bin/llvm-config-14 "$ROOT/bin/llvm-config"
    info "clang -> $(readlink -f "$ROOT/bin/clang")"
    info "llvm-config -> $(readlink -f "$ROOT/bin/llvm-config")"
}

build_vc4c() {
    local src="$ROOT/VC4C"
    local build="$ROOT/VC4C/build"
    [ -f "$src/CMakeLists.txt" ] || die "VC4C source tree not found at $src"
    log "Configuring VC4C (LLVM 14)"
    install -d "$build"
    # Reconfiguring an existing build directory is supported and keeps object
    # files, so re-running this script does not start from scratch.
    PATH="$ROOT/bin:$PATH" cmake -S "$src" -B "$build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DLLVMLIB_FRONTEND=ON \
        -DCLANG_LIBRARY=OFF \
        -DVC4CL_STDLIB_DIR="$ROOT/VC4CLStdLib/include" \
        -DLLVM_DIR=/usr/lib/llvm-14/lib/cmake/llvm
    log "Building VC4C (-j$JOBS)"
    PATH="$ROOT/bin:$PATH" cmake --build "$build" --parallel "$JOBS"
    [ -e "$build/src/libVC4CC.so" ] || die "VC4C build did not produce libVC4CC.so"
}

build_vc4cl() {
    local src="$ROOT/VC4CL"
    local build="$ROOT/VC4CL/build"
    [ -f "$src/CMakeLists.txt" ] || die "VC4CL source tree not found at $src"
    log "Configuring VC4CL (BUILD_ICD=$VC4CL_BUILD_ICD)"
    install -d "$build"
    # INCLUDE_COMPILER=ON embeds the VC4C compiler; the explicit header/library
    # paths avoid installing VC4C into /usr/local.
    # NO_VCSM is selected automatically by src/CMakeLists.txt when libvcsm is
    # absent; nothing here has to force it.
    PATH="$ROOT/bin:$PATH" cmake -S "$src" -B "$build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DINCLUDE_COMPILER=ON \
        -DBUILD_ICD="$VC4CL_BUILD_ICD" \
        -DBUILD_TESTING=OFF \
        -DVC4C_HEADER_PATH="$ROOT/VC4C/include/VC4C.h" \
        -DVC4CC_LIBRARY="$ROOT/VC4C/build/src/libVC4CC.so"
    log "Building VC4CL (-j$JOBS)"
    PATH="$ROOT/bin:$PATH" cmake --build "$build" --parallel "$JOBS"
    [ -e "$build/src/libVC4CL.so" ] || die "VC4CL build did not produce libVC4CL.so"
}

# -----------------------------------------------------------------------------
# 5. V3D power service
# -----------------------------------------------------------------------------
install_power_service() {
    local unit_src="$ROOT/scripts/vc4cl-v3d-power.service"
    log "Installing vc4cl-v3d-power.service"
    if [ ! -f "$unit_src" ]; then
        warn "unit source not found at $unit_src; skipping service installation"
        return 0
    fi
    if [ ! -e "$V3D_DEVICE/power/control" ]; then
        warn "$V3D_DEVICE/power/control not present; this unit is for the Pi 3B V3D"
    fi
    # install is idempotent: it overwrites the unit with the pinned content.
    "${SUDO[@]}" install -m 0644 "$unit_src" \
        /etc/systemd/system/vc4cl-v3d-power.service
    "${SUDO[@]}" systemctl daemon-reload
    # enable --now is a no-op if it is already enabled and active.
    "${SUDO[@]}" systemctl enable --now vc4cl-v3d-power.service
    if command -v systemctl >/dev/null 2>&1; then
        info "enabled: $("${SUDO[@]}" systemctl is-enabled vc4cl-v3d-power.service 2>/dev/null || true)"
        info "active:  $("${SUDO[@]}" systemctl is-active vc4cl-v3d-power.service 2>/dev/null || true)"
    fi
    if [ -r "$V3D_DEVICE/power/control" ]; then
        info "power/control: $(cat "$V3D_DEVICE/power/control")"
        info "runtime_status: $(cat "$V3D_DEVICE/power/runtime_status")"
    fi
}

# -----------------------------------------------------------------------------
# 6. Optional OpenCL ICD registration
# -----------------------------------------------------------------------------
install_icd_file() {
    [ "$VC4CL_BUILD_ICD" = "ON" ] || return 0
    local lib="$ROOT/VC4CL/build/src/libVC4CL.so"
    log "Registering the VC4CL ICD"
    if [ ! -e "$lib" ]; then
        warn "$lib is missing; ICD file not written"
        return 0
    fi
    # Guard against --skip-build or a stale BUILD_ICD=OFF tree: registering such
    # a library makes every ICD-loader client (clinfo included) fail at dlopen
    # or at clIcdGetPlatformIDsKHR.
    if ! nm -D "$lib" 2>/dev/null | grep -q 'clIcdGetPlatformIDsKHR'; then
        warn "$lib has no clIcdGetPlatformIDsKHR export (BUILD_ICD=OFF); ICD file not written"
        return 0
    fi
    # The ICD loader accepts an absolute path to the vendor library. Only a
    # BUILD_ICD=ON build exports the cl_khr_icd entry points, so do not register
    # a BUILD_ICD=OFF library: clinfo would load it and fail.
    "${SUDO[@]}" install -d -m 0755 /etc/OpenCL/vendors
    printf '%s\n' "$lib" | "${SUDO[@]}" tee /etc/OpenCL/vendors/vc4cl.icd >/dev/null
    info "/etc/OpenCL/vendors/vc4cl.icd -> $lib"
}

# -----------------------------------------------------------------------------
# 7. Verification summary (read-only)
# -----------------------------------------------------------------------------
summarize() {
    log "Summary"
    info "cmake:      $(cmake --version | head -1)"
    info "compiler:   $(g++ --version | head -1)"
    info "clang-14:   $(clang-14 --version 2>/dev/null | head -1 || echo missing)"
    info "llvm-14:    $(llvm-config-14 --version 2>/dev/null || echo missing)"
    if [ -e "$ROOT/VC4C/build/src/libVC4CC.so" ]; then
        info "libVC4CC:   $ROOT/VC4C/build/src/libVC4CC.so"
    fi
    if [ -e "$ROOT/VC4CL/build/src/libVC4CL.so" ]; then
        info "libVC4CL:   $ROOT/VC4CL/build/src/libVC4CL.so"
    fi
    info "test binary: $ROOT/test_vc4cl (built separately, see README.debian13.md)"
    cat <<'EOF'

Next steps:
  - Run the CPU emulator as a normal user:
      VC4CL_EMULATOR=1 LD_LIBRARY_PATH=... ./test_vc4cl
  - Run the hardware vector add as root with VC4CL_NO_VCHI=1 and a timeout.
  - Read README.debian13.md for the exact commands and known limits.
EOF
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
check_platform

install_base_packages
install_llvm14
install_rpi_userland
link_opt_vc_libs

if [ "$SKIP_BUILD" -eq 0 ]; then
    prepare_shims
    build_vc4c
    build_vc4cl
else
    info "Skipping the build as requested"
fi

if [ "$INSTALL_SERVICE" -eq 1 ]; then
    install_power_service
else
    info "Skipping the power service as requested"
fi

install_icd_file
summarize
