#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Prunes lld's ThinLTO cache after a release build (.github/workflows/release-binaries.yml).
#
# Every ThinLTO link reads one cache entry per module, and lld updates the access time of an entry
# it reuses, or writes a new entry for a module whose code or imports changed. An entry the build
# neither read nor wrote belongs to code that has changed since, so it is removed: the cache the
# workflow saves then holds what the last build used, instead of growing with every commit.
#
# Usage: prune-lto-cache.sh <cache dir> <file created before the build started>
set -euo pipefail

dir=${1:?usage: prune-lto-cache.sh <cache dir> <stamp file>}
stamp=${2:?usage: prune-lto-cache.sh <cache dir> <stamp file>}
[ -d "$dir" ] || { echo "ThinLTO cache: $dir does not exist"; exit 0; }

reused=$(find "$dir" -type f -anewer "$stamp" ! -newer "$stamp" | wc -l)
written=$(find "$dir" -type f -newer "$stamp" | wc -l)
removed=$(find "$dir" -type f ! -anewer "$stamp" -print -delete | wc -l)
echo "ThinLTO cache: $reused entries reused, $written written, $removed unused ones removed;" \
    "$(du -sh "$dir" | cut -f1) kept"
