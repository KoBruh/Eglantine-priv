#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Installs what the Linux release builds (.github/workflows/release-binaries.yml) need: the apt
# packages, clang and lld from apt.llvm.org, and CMake 3.31 or newer. Qt comes from the workflow's
# install-qt-action, or from /opt/Qt on the boat.dev snapshot.
#
# The boat.dev snapshot the x86_64 job starts from is made with this script
# (tools/ci/boat-snapshot.sh), so on it nothing is missing and the script ends in about a second.
# On a plain runner it installs everything.
#
# Usage: linux-deps.sh <llvm major>     for example: linux-deps.sh 23
set -euo pipefail

llvm=${1:?usage: linux-deps.sh <llvm major>}

packages=(
    autoconf cmake g++ gcc git glslang-tools libglu1-mesa-dev
    libpulse-dev libtool libudev-dev libxkbcommon-x11-0
    mesa-common-dev nasm ninja-build qt6-base-private-dev
    qt6-charts-dev qt6-tools-dev libavfilter-dev libavcodec-dev
    libswscale-dev pkg-config zlib1g-dev libva-dev libvdpau-dev
    spirv-tools spirv-headers libusb-1.0-0-dev
    libboost-dev libboost-fiber-dev libboost-context-dev
    libopus-dev libasound2-dev ccache
    libxcb-cursor0 libxcb-icccm4 libxcb-image0
    libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1
    patch perl make build-essential curl ca-certificates file xz-utils zstd unzip zip
    python3-venv gnupg
)

export DEBIAN_FRONTEND=noninteractive
updated=false
apt_update() {
    if [ "$updated" = false ]; then
        sudo apt-get update
        updated=true
    fi
}

missing=()
for package in "${packages[@]}"; do
    dpkg-query -W -f='${Status}' "$package" 2> /dev/null | grep -q 'install ok installed' ||
        missing+=("$package")
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "Installing ${#missing[@]} apt packages: ${missing[*]}"
    apt_update
    sudo apt-get install -y --no-install-recommends "${missing[@]}"
fi

# Ubuntu's own clang 18 cannot read a profile from a current LLVM, so clang comes from the LLVM apt
# repository. The versioned packages are named explicitly: that repository's unversioned clang/lld
# metapackages can lag the binaries they depend on and then resolve to nothing. lld-N only ships
# ld.lld under /usr/lib/llvm-N/bin, which is why the workflow passes -B there. llvm-N has llvm-ar-N,
# which CMake uses to archive the bitcode objects of a ThinLTO build (ENABLE_LTO).
if ! command -v "clang-$llvm" > /dev/null || [ ! -x "/usr/lib/llvm-$llvm/bin/ld.lld" ] ||
    ! command -v "llvm-ar-$llvm" > /dev/null; then
    codename=$(. /etc/os-release && echo "$VERSION_CODENAME")
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key |
        sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc > /dev/null
    echo "deb http://apt.llvm.org/$codename/ llvm-toolchain-$codename-$llvm main" |
        sudo tee /etc/apt/sources.list.d/llvm.list > /dev/null
    updated=false
    apt_update
    sudo apt-get install -y --no-install-recommends "clang-$llvm" "lld-$llvm" "llvm-$llvm"
fi

# GitHub's Ubuntu images have a current CMake in /usr/local/bin; plain Ubuntu 24.04 only has 3.28
# from apt, and the build needs 3.31 or newer.
if ! cmake --version | head -1 | grep -qE 'version (3\.(3[1-9]|[4-9][0-9])|[4-9])\.'; then
    sudo python3 -m venv /opt/cmake-venv
    sudo /opt/cmake-venv/bin/pip install -q "cmake==3.31.6"
    for tool in cmake ctest cpack; do
        sudo ln -sf "/opt/cmake-venv/bin/$tool" "/usr/local/bin/$tool"
    done
    hash -r
fi

cmake --version | head -1
"clang-$llvm" --version | head -1
ccache --version | head -1
