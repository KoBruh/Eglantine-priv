param(
    [string]$UserPath,
    [string]$MemoryCsv,
    [string]$OutputPath = "eglantine-debug-$(Get-Date -Format 'yyyyMMdd-HHmmss').zip"
)
$ErrorActionPreference = 'Stop'
if (-not $UserPath) {
    $portable = Join-Path $PSScriptRoot 'user'
    $UserPath = if (Test-Path -LiteralPath $portable -PathType Container) {
        $portable
    } else { Join-Path $env:APPDATA 'eden' }
}
$logPath = Join-Path $UserPath 'log'
if (-not (Test-Path -LiteralPath $logPath -PathType Container)) {
    throw "No logs found at $logPath. Pass -UserPath with the emulator's user directory."
}
if (Test-Path -LiteralPath $OutputPath) { throw "Output already exists: $OutputPath" }
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$stage = Join-Path $tempRoot "eglantine-debug-$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    Get-ChildItem -LiteralPath $logPath -File -Filter '*.txt' | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $stage
    }
    foreach ($name in @('source-commit.txt', 'source-state.txt', 'source-changes.patch',
                        'pgo-profile.sha256', 'pgo-profile-source.txt', 'windows-dependencies.txt')) {
        $source = Join-Path $PSScriptRoot $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination $stage
        }
    }
    if ($MemoryCsv) { Copy-Item -LiteralPath $MemoryCsv -Destination (Join-Path $stage 'memory.csv') }
    $os = Get-CimInstance Win32_OperatingSystem
    $cpu = Get-CimInstance Win32_Processor
    $gpu = Get-CimInstance Win32_VideoController
    [pscustomobject]@{
        CapturedUtc = [DateTime]::UtcNow.ToString('o')
        Windows = $os.Caption
        WindowsBuild = $os.BuildNumber
        PhysicalMemoryKiB = $os.TotalVisibleMemorySize
        CPU = @($cpu | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
        GPU = @($gpu | Select-Object Name, DriverVersion, DriverDate)
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage 'system.json')
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $OutputPath -CompressionLevel Fastest
    Write-Output "Debug bundle: $([IO.Path]::GetFullPath($OutputPath))"
} finally {
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    if ([IO.Path]::GetDirectoryName($resolvedStage) -ne $tempRoot -or
        -not [IO.Path]::GetFileName($resolvedStage).StartsWith('eglantine-debug-')) {
        throw 'Unexpected temporary directory; cleanup refused'
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
