param(
    [string]$ProcessName = 'eglantine',
    [int]$Minutes = 65,
    [int]$IntervalSeconds = 5,
    [string]$OutputPath = 'mkw-memory.csv',
    [int]$TargetProcessId = 0,
    [int]$Samples = 0
)
$ErrorActionPreference = 'Stop'
if ($Minutes -lt 1 -or $IntervalSeconds -lt 1) { throw 'Durations must be positive' }
$target = if ($TargetProcessId) { Get-Process -Id $TargetProcessId } else {
    Get-Process -Name $ProcessName | Select-Object -First 1
}
if (-not $target) { throw "Start $ProcessName before running this recorder" }
$targetId = $target.Id
$until = (Get-Date).AddMinutes($Minutes)
$first = $true
$sampleCount = 0
$clock = [Diagnostics.Stopwatch]::StartNew()
$lastSample = $null
$lastCpu = $null
$lastThreads = @{}
$logicalProcessors = [Environment]::ProcessorCount
while ((Get-Date) -lt $until) {
    $target = Get-Process -Id $targetId -ErrorAction SilentlyContinue
    if (-not $target) { Write-Output 'Emulator exited; recording stopped'; break }
    $sampleTime = $clock.Elapsed.TotalSeconds
    $cpuTime = $target.TotalProcessorTime.TotalSeconds
    $cpuPercent = $null
    $busiestThreadPercent = $null
    $busiestThreadId = $null
    $threadTimes = @{}
    $elapsed = if ($null -ne $lastSample) { $sampleTime - $lastSample } else { 0 }
    if ($elapsed -gt 0) {
        $cpuPercent = [Math]::Max(0.0, 100 * ($cpuTime - $lastCpu) / $elapsed)
    }
    foreach ($thread in $target.Threads) {
        try {
            $threadTime = $thread.TotalProcessorTime.TotalSeconds
            $threadTimes[$thread.Id] = $threadTime
            if ($elapsed -gt 0 -and $lastThreads.ContainsKey($thread.Id)) {
                $percent = [Math]::Min(100.0, [Math]::Max(0.0,
                    100 * ($threadTime - $lastThreads[$thread.Id]) / $elapsed))
                if ($null -eq $busiestThreadPercent -or $percent -gt $busiestThreadPercent) {
                    $busiestThreadPercent = $percent
                    $busiestThreadId = $thread.Id
                }
            }
        } catch { } # Threads can exit between enumeration and sampling.
    }
    $lastSample = $sampleTime
    $lastCpu = $cpuTime
    $lastThreads = $threadTimes
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
    $gpuEnginePeak = $null
    $gpuEngineName = $null
    try {
        $engines = @(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine |
            Where-Object { $_.Name -match "^pid_${targetId}_" } |
            Sort-Object UtilizationPercentage -Descending)
        if ($engines.Count -gt 0) {
            $gpuEnginePeak = $engines[0].UtilizationPercentage
            $gpuEngineName = $engines[0].Name
        }
    } catch { }
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
        CpuCoreEquivalentPercent = $cpuPercent
        CpuNormalizedPercent = if ($null -ne $cpuPercent) { $cpuPercent / $logicalProcessors } else { $null }
        BusiestThreadPercent = $busiestThreadPercent
        BusiestThreadId = $busiestThreadId
        LogicalProcessors = $logicalProcessors
        GpuEnginePeakPercent = $gpuEnginePeak
        GpuEnginePeakName = $gpuEngineName
    }
    if ($first) {
        $row | Export-Csv $OutputPath -NoTypeInformation
        $first = $false
    } else { $row | Export-Csv $OutputPath -NoTypeInformation -Append }
    ++$sampleCount
    if ($Samples -gt 0 -and $sampleCount -ge $Samples) { break }
    Start-Sleep -Seconds $IntervalSeconds
}
Write-Output "Memory samples: $OutputPath"
