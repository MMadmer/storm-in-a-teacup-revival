param(
    [string]$Label = "run",
    [int]$Track = 0,
    [int]$Glider = 0,
    [double]$Warmup = 6,
    [double]$Duration = 20,
    [int]$Profile = 0,
    [int]$Drive = 0,
    [int]$Screenshot = 0,
    [string]$Optimizer = "",
    [int]$DisplayMode = -1,
    [string]$Baseline = ""
)

# Runs one automated Storm.exe benchmark through the StormBench dev mod and prints its frame summary.
# Mods.ini, ExpandedSettings.ini and, with -Baseline, the swapped binaries are restored afterwards.
$game = Split-Path -Parent $PSScriptRoot
$mods = Join-Path $game "Mods"
$report = Join-Path $mods "StormBench\Benchmark-$Label.txt"
$gpuLog = Join-Path $env:TEMP "storm-bench-gpu-$Label.csv"
Remove-Item $report, $gpuLog -ErrorAction SilentlyContinue

# Original contents by path; $null marks a file that did not exist before the run.
$restore = [ordered]@{}
function Save-File([string]$path) {
    if (-not $restore.Contains($path)) {
        $restore[$path] = (Test-Path $path) ? [IO.File]::ReadAllBytes($path) : $null
    }
}

# Rewrites a "Key=value" line the way GetPrivateProfileInt reads it, tolerating spaces around "=". A missing
# key is appended; every ini edited here has a single section.
function Set-IniValue([string]$text, [string]$key, [string]$value) {
    $pattern = "(?m)^([ \t]*$key[ \t]*=[ \t]*)[^\r\n]*"
    if ($text -match $pattern) {
        return $text -replace $pattern, "`${1}$value"
    }
    $separator = ($text -and -not $text.EndsWith("`n")) ? "`r`n" : ""
    "$text$separator$key=$value`r`n"
}

# Scanners briefly lock files the previous run just touched, so file writes are retried.
function Invoke-WithRetry([scriptblock]$action) {
    for ($attempt = 1; ; ++$attempt) {
        try {
            & $action
            return
        } catch [IO.IOException] {
            if ($attempt -ge 10) { throw }
            Start-Sleep -Milliseconds 500
        }
    }
}

function Write-Text([string]$path, [string]$text) {
    Invoke-WithRetry { [IO.File]::WriteAllText($path, $text) }
}

$modsIni = Join-Path $mods "Mods.ini"
$displayIni = Join-Path $mods "ExpandedSettings\ExpandedSettings.ini"
$baselineFiles = "d3d9.dll", "StormModLoader.dll", "Mods\ExpandedSettings\ExpandedSettings.dll",
    "Mods\PerformanceOptimizer\PerformanceOptimizer.dll", "Mods\PerformanceOptimizer\dxvk-d3d9.backend"
$gpuSampler = $null
$process = $null

try {
    Save-File $modsIni
    Write-Text $modsIni (Set-IniValue (Get-Content $modsIni -Raw) "StormBench" "1")

    # Frame pacing would cap the measurement, so VSync and the FPS limit are off for every run.
    if (Test-Path $displayIni) {
        Save-File $displayIni
        $text = Set-IniValue (Set-IniValue (Get-Content $displayIni -Raw) "VSync" "0") "FpsLimit" "0"
        if ($DisplayMode -ge 0) {
            $text = Set-IniValue $text "DisplayMode" "$DisplayMode"
        }
        Write-Text $displayIni $text
    }

    if ($Baseline) {
        foreach ($file in $baselineFiles) {
            $target = Join-Path $game $file
            Save-File $target
            Invoke-WithRetry { [IO.File]::Copy((Join-Path $Baseline $file), $target, $true) }
        }
    }

    # GPU utilization is polled every ~250 ms; the tail of the log covers the measured window.
    if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
        $gpuSampler = Start-Job -ArgumentList $gpuLog -ScriptBlock {
            param($path)
            while ($true) {
                nvidia-smi --query-gpu=utilization.gpu,clocks.gr,power.draw --format=csv,noheader,nounits | Add-Content $path
                Start-Sleep -Milliseconds 200
            }
        }
    }

    $env:STORM_BENCH = "label=$Label;track=$Track;glider=$Glider;warmup=$Warmup;duration=$Duration;profile=$Profile;drive=$Drive;screenshot=$Screenshot"
    $env:STORM_OPTIMIZER = $Optimizer
    $process = Start-Process (Join-Path $game "Storm.exe") -WorkingDirectory $game -PassThru
    Remove-Item Env:STORM_BENCH, Env:STORM_OPTIMIZER -ErrorAction SilentlyContinue

    # StormBench gives up after 240 s and still writes a report, so the script waits a little longer.
    if (-not $process.WaitForExit(270000)) {
        Write-Output "TIMEOUT: Storm.exe killed"
    }
}
finally {
    # A running game keeps the swapped DLLs loaded, so it is stopped before anything is restored.
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit(10000) | Out-Null
    }
    if ($gpuSampler) {
        Stop-Job $gpuSampler
        Remove-Job $gpuSampler
    }
    foreach ($entry in $restore.GetEnumerator()) {
        try {
            if ($null -eq $entry.Value) {
                Remove-Item $entry.Key -ErrorAction Stop
            } else {
                Invoke-WithRetry { [IO.File]::WriteAllBytes($entry.Key, $entry.Value) }
            }
        } catch {
            Write-Warning "Could not restore $($entry.Key): $_"
        }
    }
}

if (Test-Path $report) {
    Get-Content $report -TotalCount 2
} else {
    Write-Output "NO REPORT"
    Get-Content (Join-Path $mods "StormModLoader.log") -Tail 15
}

if (Test-Path $gpuLog) {
    $rows = Get-Content $gpuLog | Where-Object { $_ -match '^\s*\d' } | ForEach-Object { , ($_ -split ',\s*' | ForEach-Object { [double]$_ }) }
    $window = [int]($Duration * 3)
    $tail = $rows | Select-Object -Last ($window + 2) | Select-Object -First $window
    if ($tail) {
        $util = ($tail | ForEach-Object { $_[0] } | Measure-Object -Average).Average
        $clock = ($tail | ForEach-Object { $_[1] } | Measure-Object -Average).Average
        $power = ($tail | ForEach-Object { $_[2] } | Measure-Object -Average).Average
        Write-Output ("gpu_util={0:N1}% gpu_clock={1:N0}MHz gpu_power={2:N1}W" -f $util, $clock, $power)
    }
}
