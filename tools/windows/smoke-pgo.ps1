param([Parameter(Mandatory=$true)][string]$BinPath)
$ErrorActionPreference = 'Stop'
$bin = (Resolve-Path $BinPath).Path
$exe = Join-Path $bin 'eglantine.exe'
$stream = [System.IO.File]::OpenRead($exe)
try {
    $reader = [System.IO.BinaryReader]::new($stream)
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Missing MZ header' }
    $stream.Position = 0x3C
    $pe = $reader.ReadInt32()
    $stream.Position = $pe
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
        throw 'Expected a native Windows AMD64 PE executable'
    }
} finally { $stream.Dispose() }
if (-not (Test-Path (Join-Path $bin 'platforms/qwindows.dll'))) {
    throw 'Missing Qt Windows platform plugin'
}
# Remove MSYS2 from PATH so its DLLs cannot conceal an incomplete portable package.
$originalPath = $env:PATH
$originalPlatform = $env:QT_QPA_PLATFORM
$originalPlugins = $env:QT_PLUGIN_PATH
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$stage = Join-Path $tempRoot "eglantine-smoke-$([guid]::NewGuid())"
$smokeBin = Join-Path $stage 'bin'
$process = $null
try {
    New-Item -ItemType Directory -Path $smokeBin -Force | Out-Null
    Get-ChildItem -LiteralPath $bin -Force | Where-Object Name -ne 'user' | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $smokeBin -Recurse
    }
    New-Item -ItemType Directory -Path (Join-Path $smokeBin 'user\config') -Force | Out-Null
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = 'windows'
    $env:QT_PLUGIN_PATH = $smokeBin
    $process = Start-Process -FilePath (Join-Path $smokeBin 'eglantine.exe') `
        -WorkingDirectory $smokeBin -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 12
    $process.Refresh()
    if ($process.HasExited) { throw "GUI exited during startup: $($process.ExitCode)" }
    Write-Output 'AMD64 PE and packaged GUI loader smoke check passed. GPU gameplay is untested.'
} finally {
    if ($process) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        if (-not $process.WaitForExit(10000)) { throw 'Smoke-test process did not exit' }
        $process.Dispose()
    }
    $env:PATH = $originalPath
    $env:QT_QPA_PLATFORM = $originalPlatform
    $env:QT_PLUGIN_PATH = $originalPlugins
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    if ([IO.Path]::GetDirectoryName($resolvedStage) -ne $tempRoot -or
        -not [IO.Path]::GetFileName($resolvedStage).StartsWith('eglantine-smoke-')) {
        throw 'Unexpected smoke directory; cleanup refused'
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
