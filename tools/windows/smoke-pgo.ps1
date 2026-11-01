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
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_PLUGIN_PATH = $bin
$process = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
try {
    Start-Sleep -Seconds 12
    $process.Refresh()
    if ($process.HasExited) { throw "GUI exited during startup: $($process.ExitCode)" }
    Write-Output 'AMD64 PE and packaged GUI loader smoke check passed. GPU gameplay is untested.'
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
