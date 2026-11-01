param(
    [string]$ProcessName = 'eglantine',
    [int]$Minutes = 65,
    [int]$IntervalSeconds = 5,
    [string]$OutputPath = 'mkw-memory.csv'
)
$ErrorActionPreference = 'Stop'
if ($Minutes -lt 1 -or $IntervalSeconds -lt 1) { throw 'Durations must be positive' }
$target = Get-Process -Name $ProcessName | Select-Object -First 1
if (-not $target) { throw "Start $ProcessName before running this recorder" }
$targetId = $target.Id
$until = (Get-Date).AddMinutes($Minutes)
$first = $true
while ((Get-Date) -lt $until) {
    $target = Get-Process -Id $targetId -ErrorAction SilentlyContinue
    if (-not $target) { Write-Output 'Emulator exited; recording stopped'; break }
    $dedicated = $null
    $shared = $null
    try {
        $gpu = @(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory |
            Where-Object { $_.Name -match "^pid_${targetId}_" })
        if ($gpu.Count -gt 0) {
            $dedicated = ($gpu | Measure-Object DedicatedUsage -Sum).Sum
            $shared = ($gpu | Measure-Object SharedUsage -Sum).Sum
        }
    } catch { } # Blank values indicate unavailable counters, not zero usage.
    $committed = $null
    $limit = $null
    try {
        $memory = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
        $committed = $memory.CommittedBytes
        $limit = $memory.CommitLimit
    } catch { }
    $row = [pscustomobject]@{
        TimeUtc = (Get-Date).ToUniversalTime().ToString('o')
        ProcessId = $targetId
        PrivateBytes = $target.PrivateMemorySize64
        WorkingSetBytes = $target.WorkingSet64
        DedicatedGpuBytes = $dedicated
        SharedGpuBytes = $shared
        SystemCommittedBytes = $committed
        SystemCommitLimitBytes = $limit
        Handles = $target.HandleCount
        Threads = $target.Threads.Count
    }
    if ($first) {
        $row | Export-Csv $OutputPath -NoTypeInformation
        $first = $false
    } else { $row | Export-Csv $OutputPath -NoTypeInformation -Append }
    Start-Sleep -Seconds $IntervalSeconds
}
Write-Output "Memory samples: $OutputPath"
