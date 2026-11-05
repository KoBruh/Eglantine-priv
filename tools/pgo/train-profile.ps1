# SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
    Trains eglantine.profdata, the profile the release PGO builds use (tools/pgo/README.md).

.DESCRIPTION
    1. Builds an instrumented (-fprofile-generate) eden-cli and eglantine with MSYS2's UCRT64
       clang, configured like the "Configure MinGW PGO" step of release-binaries.yml, and bundles
       its DLLs so it runs outside MSYS2.
    2. Plays Pokemon Pokopia through the training scenarios with tools/nx2/run_scenario.py, docked
       and handheld. Each emulator process writes its own .profraw when it quits.
    3. Merges them with MSYS2's llvm-profdata into eglantine.profdata at the repository root.

    The runs render off-screen, never take focus and are muted, so the PC stays usable. They use
    the normal emulator user folder (%APPDATA%\eglantine): keys, firmware and a Pokopia save that is
    past the intro must be there.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\pgo\train-profile.ps1 -Game E:\Pokopia\exefs\main -Jobs 8
#>
[CmdletBinding()]
param(
    # Pokemon Pokopia's exefs\main.
    [Parameter(Mandatory = $true)] [string] $Game,
    [string] $Msys2 = 'C:\msys64',
    # Parallel compile jobs. Each clang process can take 2 GB or more.
    [int] $Jobs = [Environment]::ProcessorCount,
    [string] $BuildDir = 'build\pgo-generate',
    # Run reports and the raw profiles.
    [string] $WorkDir = 'build\pgo-training',
    # The sdl2-config.ini the runs read their key mappings and settings from. Default: the one in
    # the emulator's user folder. With a portable user folder next to the instrumented eden-cli
    # (<BuildDir>\bin\user) and its config here, the training leaves %APPDATA%\eglantine alone.
    [string] $Config,
    [string] $Output = 'eglantine.profdata',
    [string[]] $Scenarios = @(
        'tools\nx2\scenarios\pokopia_explore.txt',
        'tools\nx2\scenarios\pokopia_pokedex.txt',
        'tools\nx2\scenarios\pokopia_sky.txt',
        'tools\nx2\scenarios\pokopia_walk.txt'),
    [ValidateSet('docked', 'handheld')] [string[]] $Modes = @('docked', 'handheld'),
    # Skip installing the MSYS2 packages (they are installed with --needed, so this only saves a
    # few seconds).
    [switch] $SkipPackages,
    # Reuse the instrumented build in BuildDir as it is.
    [switch] $SkipBuild,
    # Only merge the .profraw files already in WorkDir\raw.
    [switch] $SkipTraining
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repo
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$WorkDir = (Resolve-Path $WorkDir).Path
$bash = Join-Path $Msys2 'usr\bin\bash.exe'
$profdata = Join-Path $Msys2 'ucrt64\bin\llvm-profdata.exe'
if (-not (Test-Path $bash)) {
    throw "No MSYS2 at $Msys2 (install it with: winget install MSYS2.MSYS2), or pass -Msys2."
}

# Runs a bash script in an MSYS2 UCRT64 login shell, in the repository. The script goes through a
# file: Windows PowerShell mangles quotes in arguments to native programs.
function Invoke-Msys2([string] $Name, [string] $Script) {
    $file = Join-Path $WorkDir "$Name.sh"
    [IO.File]::WriteAllText($file, "set -euo pipefail`n" + $Script.Replace("`r`n", "`n"))
    $env:MSYSTEM = 'UCRT64'
    $env:CHERE_INVOKING = '1'
    & $bash -l ($file -replace '\\', '/')
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 step '$Name' failed (exit code $LASTEXITCODE)" }
}

# The release workflow's MSYS2 packages, plus compiler-rt (the profile runtime that
# -fprofile-generate links) and llvm (llvm-profdata).
if (-not $SkipPackages -and -not $SkipBuild) {
    $mingw = @('toolchain', 'clang', 'lld', 'cmake', 'python-pip', 'qt6-base', 'qt6-charts',
        'qt6-tools', 'qt6-translations', 'qt6-svg', 'openssl', 'boost', 'fmt', 'lz4', 'nlohmann-json',
        'zlib', 'zstd', 'libiconv', 'vulkan-memory-allocator', 'vulkan-devel', 'glslang', 'enet',
        'opus', 'libusb', 'unordered_dense', 'jbigkit', 'xz', 'libdeflate', 'bzip2', 'lerc',
        'graphite2', 'libwebp', 'libtiff', 'brotli', 'ccache', 'compiler-rt', 'llvm') |
        ForEach-Object { "mingw-w64-ucrt-x86_64-$_" }
    Invoke-Msys2 'packages' ("pacman -S --needed --noconfirm git make autoconf libtool " +
        "automake-wrapper patch jq pactoys ninja " + ($mingw -join ' '))
}

# Every CI compiler has to read the profile, and a clang refuses one written by a newer LLVM major.
# MSYS2's llvm-profdata writes it, so it must not be newer than the Linux jobs' clang.
if (-not (Test-Path $profdata)) {
    throw "No llvm-profdata at $profdata (MSYS2 package mingw-w64-ucrt-x86_64-llvm)"
}
$llvm = [int]((& $profdata --version | Out-String) -replace '(?s).*LLVM version (\d+).*', '$1')
$workflow = Get-Content '.github\workflows\release-binaries.yml' -Raw
if ($workflow -match 'PGO_LLVM_VERSION:\s*"(\d+)"' -and $llvm -gt [int]$Matches[1]) {
    throw ("MSYS2's LLVM $llvm is newer than PGO_LLVM_VERSION $($Matches[1]), the Linux jobs' " +
        "clang, which could not read the profile. Raise PGO_LLVM_VERSION to $llvm (and rebuild the " +
        "boat snapshot with tools/ci/boat-snapshot.sh) first.")
}

$buildUnix = $BuildDir -replace '\\', '/'
if (-not $SkipBuild) {
    # Keep these in step with "Configure MinGW PGO" in .github/workflows/release-binaries.yml:
    # the same flags, with -fprofile-generate in place of -fprofile-use. The -mllvm option names
    # static functions in the profile by file name only, without the checkout's path, so the
    # profile matches builds made in any directory (see README.md).
    Invoke-Msys2 'build' @"
flags='-fprofile-generate -mllvm -static-func-full-module-prefix=false'
cmake -S . -B '$buildUnix' -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DYUZU_TESTS=OFF \
  -DYUZU_CMD=ON \
  -DYUZU_ROOM=OFF \
  -DYUZU_ROOM_STANDALONE=OFF \
  -DYUZU_USE_BUNDLED_QT=OFF \
  -DYUZU_USE_BUNDLED_SDL3=ON \
  -DYUZU_USE_BUNDLED_FFMPEG=ON \
  -DVulkanHeaders_FORCE_BUNDLED=ON \
  -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
  -DENABLE_UPDATE_CHECKER=OFF \
  -DUSE_DISCORD_PRESENCE=OFF \
  -DUSE_CCACHE=ON \
  -DYUZU_BUILD_PRESET=v3 \
  -DENABLE_LTO=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
  "-DCMAKE_C_FLAGS=`$flags" \
  "-DCMAKE_CXX_FLAGS=`$flags"
cmake --build '$buildUnix' --target yuzu yuzu-cmd --parallel $Jobs
clang --version | head -1
"@

    # Same bundling as "Package MinGW binary" in the workflow: Qt through windeployqt6, then every
    # non-system DLL the programs and plugins load.
    Invoke-Msys2 'package' @"
export PATH="/ucrt64/bin:`$PATH"
cd '$buildUnix/bin'
test -f eglantine.exe
test -f eden-cli.exe
windeployqt6 --no-translations --no-compiler-runtime --no-opengl-sw \
  --no-system-dxc-compiler --no-system-d3d-compiler eglantine.exe > /dev/null
deps() {
  objdump -p "`$1" | awk '/DLL Name:/ {print `$3}' | while read -r dll; do
    [ -z "`$dll" ] && continue
    source=`$(command -v "`$dll" 2>/dev/null || true)
    [ -z "`$source" ] && continue
    case "`$source" in
      *System32* | *SysWOW64*) continue ;;
    esac
    if [ ! -f "`$dll" ]; then
      cp "`$source" "`$dll"
      deps "`$source"
    fi
  done
}
for exe in eglantine.exe eden-cli.exe; do
  deps "`$exe"
done
find . -name '*.dll' | while read -r dll; do
  deps "`$dll"
done
"@
}

$exe = Join-Path (Resolve-Path $BuildDir).Path 'bin\eden-cli.exe'
$raw = Join-Path $WorkDir 'raw'
$runs = Join-Path $WorkDir 'runs'
if (-not $SkipTraining) {
    if (-not (Test-Path $exe)) { throw "No instrumented eden-cli at $exe" }
    if (-not (Test-Path $Game)) { throw "No game at $Game" }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) { $python = $python.Source; $pythonArgs = @() }
    else { $python = (Get-Command py).Source; $pythonArgs = @('-3') }
    Remove-Item -Recurse -Force $raw, $runs -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $raw, $runs | Out-Null
    $summary = @()
    foreach ($scenario in $Scenarios) {
        foreach ($mode in $Modes) {
            $name = '{0}-{1}' -f [IO.Path]::GetFileNameWithoutExtension($scenario), $mode
            $docked = if ($mode -eq 'docked') { 1 } else { 0 }
            # %p: one file per process; %m: per instrumented module.
            $env:LLVM_PROFILE_FILE = Join-Path $raw "$name-%p-%m.profraw"
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $runArgs = @('tools\nx2\run_scenario.py', $scenario, '--exe', $exe, '--game', $Game,
                '--set', "use_docked_mode=$docked", '--clean-exit', '--out', (Join-Path $runs $name))
            if ($Config) { $runArgs += @('--config', $Config) }
            & $python @($pythonArgs + $runArgs)
            $code = $LASTEXITCODE
            Remove-Item Env:LLVM_PROFILE_FILE
            $written = @(Get-ChildItem $raw -Filter "$name-*.profraw")
            $bytes = ($written | Measure-Object Length -Sum).Sum
            $summary += [pscustomobject]@{
                Run = $name; Seconds = [int]$clock.Elapsed.TotalSeconds; ExitCode = $code
                Profiles = $written.Count; MB = [math]::Round($bytes / 1MB, 1)
            }
            if ($code -ne 0 -or $written.Count -eq 0) {
                Write-Warning "$name did not finish cleanly (exit code $code, $($written.Count) profiles); see $runs\$name\report.md"
            }
        }
    }
    $summary | Format-Table -AutoSize | Out-String | Write-Host
}

$files = @(Get-ChildItem $raw -Filter '*.profraw' | Where-Object Length -gt 0)
if ($files.Count -eq 0) { throw "No .profraw files in $raw" }
# --sparse leaves out the functions training never ran. The compiler then optimizes them as it
# would without a profile; with zero counts it would treat them as cold and build them for size,
# which slows down whatever other games use them.
& $profdata merge --sparse -o $Output ($files | ForEach-Object FullName)
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge failed ($LASTEXITCODE)" }
& $profdata show $Output
& $profdata --version | Select-Object -Skip 1 -First 1
'{0}: {1:N1} MB from {2} raw profiles' -f $Output, ((Get-Item $Output).Length / 1MB), $files.Count
