# Host-only regression tests. Never opens a real device port or resets a chip.
[CmdletBinding()]
param([string]$MonitorPath)

$ErrorActionPreference = 'Stop'
$monitor = Join-Path $PSScriptRoot 'monitor_serial.ps1'
if ($MonitorPath) { $monitor = $MonitorPath }
$psExe = (Get-Process -Id $PID).Path
$testDir = Join-Path ([System.IO.Path]::GetTempPath()) ('wqn-monitor-test-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $testDir | Out-Null
$passed = 0

function Check([bool]$condition, [string]$name) {
    if (-not $condition) { throw "FAIL: $name" }
    Write-Host "PASS: $name"
    $script:passed++
}

try {
    $parseErrors = $null
    $tokens = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($monitor, [ref]$tokens, [ref]$parseErrors)
    Check ($parseErrors.Count -eq 0) 'PowerShell syntax'

    $missing = Join-Path $testDir 'missing-port.log'
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $output = & $psExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $monitor `
        -Port 'WQN_MONITOR_TEST_MISSING' -DurationSeconds 2 -LogPath $missing 2>&1
    $exitCode = $LASTEXITCODE
    $timer.Stop()
    Check ($exitCode -eq 0 -and $timer.Elapsed.TotalSeconds -lt 10) 'missing port stops within bounded capture'
    $raw = [System.IO.File]::ReadAllBytes($missing)
    $content = [System.IO.File]::ReadAllText($missing)
    Check ($raw.Length -gt 3 -and -not ($raw[0] -eq 239 -and $raw[1] -eq 187 -and $raw[2] -eq 191)) 'UTF-8 without BOM'
    Check ($content -match 'unavailable:' -and $content -match 'capture duration limit reached: 2 s' `
        -and $content -match 'monitor stopped') 'retry cause and clean stop are recorded'
    Check ($content -notmatch '(?m)^[IWEDV] \(' -and $content -notmatch 'reset pulse sent|opened COM') 'listener status cannot invent a device boot or reset'

    $before = [Convert]::ToBase64String($raw)
    $output = & $psExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $monitor `
        -Port 'WQN_MONITOR_TEST_MISSING' -DurationSeconds 1 -LogPath $missing 2>&1
    $exitCode = $LASTEXITCODE
    $after = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($missing))
    Check ($exitCode -ne 0 -and $before -ceq $after) 'existing evidence is not overwritten'

    $invalid = Join-Path $testDir 'invalid-duration.log'
    # Native stderr from expected parameter-validation failures must be data,
    # not a terminating PowerShell pipeline error on Windows PowerShell 5.1.
    $ErrorActionPreference = 'Continue'
    $output = & $psExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $monitor `
        -Port 'WQN_MONITOR_TEST_MISSING' -DurationSeconds -1 -LogPath $invalid 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    Check ($exitCode -ne 0 -and -not (Test-Path $invalid)) 'invalid duration fails before opening a log or port'

    Write-Host "$passed PASS / 0 FAIL; test artifacts: $testDir"
}
catch {
    Write-Host $_ -ForegroundColor Red
    Write-Host "test artifacts: $testDir"
    exit 1
}
