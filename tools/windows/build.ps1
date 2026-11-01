# SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Configure and build Eglantine on Windows with MSVC + Ninja (no admin rights needed).

.DESCRIPTION
    Loads the Visual C++ environment, makes sure glslangValidator is reachable, configures
    CMake on first run, then builds the GUI (eglantine.exe) and CLI (eden-cli.exe).
    Qt and SDL3 are downloaded by CPM into .cache/cpm on the first configure.

    glslangValidator comes from the Vulkan SDK (found through VULKAN_SDK). The SDK installer
    needs administrator rights; without admin, download glslang-<version>-windows-x86_64-release.zip from
    https://github.com/KhronosGroup/glslang/releases, extract it, copy bin\glslang.exe to
    bin\glslangValidator.exe, and pass that bin folder as -GlslangDir.

.EXAMPLE
    .\tools\windows\build.ps1 -GlslangDir D:\tools\glslang\bin
#>
param(
    [string]$BuildDir = "build",
    [ValidateSet("Release", "RelWithDebInfo", "Debug")]
    [string]$BuildType = "RelWithDebInfo",
    [string]$GlslangDir = "",
    [string[]]$Targets = @("yuzu", "yuzu-cmd"),
    [switch]$Reconfigure
)

$ErrorActionPreference = "Stop"
Set-Location (Resolve-Path "$PSScriptRoot\..\..")

. "$PSScriptRoot\load-msvc-env.ps1"

if ($GlslangDir) {
    $env:PATH = "$GlslangDir;$env:PATH"
} else {
    # The Vulkan SDK installer sets VULKAN_SDK machine-wide, but a shell or editor that was
    # already running when it was installed does not have it yet, so read it from the system.
    if (-not $env:VULKAN_SDK) {
        foreach ($scope in 'User', 'Machine') {
            $sdk = [Environment]::GetEnvironmentVariable('VULKAN_SDK', $scope)
            if ($sdk) { $env:VULKAN_SDK = $sdk; break }
        }
    }
    if ($env:VULKAN_SDK) { $env:PATH = "$env:VULKAN_SDK\Bin;$env:PATH" }
}
$glslang = Get-Command glslangValidator -ErrorAction SilentlyContinue
if (-not $glslang) {
    throw "glslangValidator not found. Install the Vulkan SDK or pass -GlslangDir (see 'Get-Help $PSCommandPath')."
}
Write-Host "glslangValidator: $($glslang.Source)"

if ($Reconfigure -or -not (Test-Path "$BuildDir\CMakeCache.txt")) {
    # ENABLE_WERROR is off because newer MSVC releases warn on code CI's compiler accepts.
    # -UGLSLANGVALIDATOR drops the cached tool path, so a changed glslang location is picked up.
    cmake -S . -B $BuildDir -G Ninja `
        -UGLSLANGVALIDATOR `
        "-DCMAKE_BUILD_TYPE=$BuildType" `
        -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF `
        -DYUZU_CMD=ON -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF `
        -DYUZU_USE_BUNDLED_QT=ON -DYUZU_USE_BUNDLED_SDL3=ON `
        -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF `
        -DENABLE_WERROR=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
}

cmake --build $BuildDir --target @Targets --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

Write-Host "Built: $BuildDir\bin\eglantine.exe and $BuildDir\bin\eden-cli.exe"
