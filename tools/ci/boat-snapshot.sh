#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Makes (or replaces) the boat.dev snapshot the Linux x86_64 release job starts from
# (runs-on: boat-large/from=eglantine-ci in .github/workflows/release-binaries.yml): Ubuntu with
# tools/ci/linux-deps.sh already run and Qt installed in /opt/Qt, so the job skips every install.
#
# Run it again after changing linux-deps.sh, the Qt version or PGO_LLVM_VERSION in the workflow;
# saving under the same name replaces the snapshot, and the next job uses the new one. Needs the
# boat CLI (https://docs.boat.dev), signed in to the account the CI installation belongs to, and jq.
#
# Usage: tools/ci/boat-snapshot.sh [name]     (default name: eglantine-ci)
set -euo pipefail

name=${1:-eglantine-ci}
here=$(cd "$(dirname "$0")" && pwd)
workflow="$here/../../.github/workflows/release-binaries.yml"
llvm=$(sed -n 's/^  PGO_LLVM_VERSION: "\([0-9]*\)".*/\1/p' "$workflow")
qt=$(sed -n 's/^  QT_VERSION: "\([0-9.]*\)".*/\1/p' "$workflow")
[ -n "$llvm" ] && [ -n "$qt" ] || { echo "PGO_LLVM_VERSION or QT_VERSION not found in $workflow" >&2; exit 1; }

deps="$here/linux-deps.sh"
# boat is a native program on Windows: Git Bash and MSYS2 must not turn the sandbox's paths
# (/opt/...) into Windows ones, and the local file needs its Windows path.
export MSYS_NO_PATHCONV=1
command -v cygpath > /dev/null && deps=$(cygpath -m "$deps")

echo "Making boat snapshot '$name': clang $llvm, Qt $qt"
id=$(boat new --type large --ttl 7200 --name "$name-setup" \
    --file "linux-deps.sh=$deps" --json | jq -r 'select(.event == "ready") | .id')
[ -n "$id" ] || { echo "boat new did not report a ready sandbox" >&2; exit 1; }
trap 'boat delete "$id" --yes > /dev/null' EXIT

boat exec "$id" --timeout 2400 -- bash -lc "set -euo pipefail
bash ~/linux-deps.sh $llvm
sudo python3 -m venv /opt/aqt
sudo /opt/aqt/bin/pip install -q aqtinstall
sudo /opt/aqt/bin/aqt install-qt linux desktop $qt linux_gcc_64 -m qtcharts -O /opt/Qt
test -x /opt/Qt/$qt/gcc_64/bin/qmake
sudo apt-get clean
echo \"$name: clang $llvm, Qt $qt, made \$(date -u +%F)\" | sudo tee /opt/eglantine-ci-ready"

# Only a finished setup writes the marker; never save a half-installed sandbox.
boat exec "$id" -- cat /opt/eglantine-ci-ready | grep -q "$name: clang $llvm, Qt $qt" ||
    { echo "the setup did not finish; nothing saved" >&2; exit 1; }
boat snapshot "$id" "$name"
echo "Saved boat snapshot '$name'."
