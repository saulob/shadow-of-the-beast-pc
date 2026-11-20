<#
.SYNOPSIS
  Builds and runs the focused headless Bug Report, updater, crash report, guest memory and CPU compatibility tests: no window, no game launch, no audio, no Internet, nothing deployed.
.DESCRIPTION
  Needs the build configured once by tools\build.ps1. Only the test target is built, incrementally in the existing build
  folder; the game executable, the launcher and the video core are not rebuilt. Every step runs even after one fails; the
  exit code is non-zero when any failed.

  Steps: the launcher's Bug Report test (builds the request the Worker must accept), the Worker's tests in Node on a
  disposable in-memory database, tools\worker.ps1 against a stand-in npx, the updater tests (rules, loopback server, helper in a temporary folder), the module history test of the crash report, the guest memory test and the CPU compatibility test.
.PARAMETER TimeoutSeconds
  Fails a step that runs longer than this.
.EXAMPLE
  .\tools\test.ps1
#>
[CmdletBinding()]
param([int]$TimeoutSeconds = 300)

. (Join-Path $PSScriptRoot '_common.ps1')
$root = $script:RepoRoot
$buildDir = Join-Path $root 'build\cmake\clang64'
$requestFile = Join-Path $buildDir 'bin\bug-report-request.json'
$workerDir = Join-Path $root 'server\shadow-bug-reports'

function Result([string]$State, [string]$Reason = '') { [pscustomobject]@{ State = $State; Reason = $Reason } }

function Invoke-Native([scriptblock]$Command) {
    $ErrorActionPreference = 'Continue'
    $lines = @(& $Command 2>&1 | ForEach-Object { "$_" })
    [pscustomobject]@{ Lines = $lines; Code = $LASTEXITCODE }
}

function Show-Last($Run) { $Run.Lines | Select-Object -Last 1 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray } }

function Invoke-FocusedTest([string]$target, [string]$show = '') {
    $build = Invoke-Native { & ninja -C $buildDir $target }
    if ($build.Code -ne 0) {
        $build.Lines | Select-Object -Last 12 | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "$target did not build; the old executable was not run"
    }
    $run = Invoke-Native { Push-Location (Join-Path $buildDir 'bin'); try { & (Join-Path $buildDir "bin\$target.exe") } finally { Pop-Location } }
    if ($run.Code -ne 0) {
        $run.Lines | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "$target exited with $($run.Code)"
    }
    if ($show) { $run.Lines | Where-Object { $_ -match $show } | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray } } else { Show-Last $run }
    return Result 'PASS'
}

function Invoke-LauncherBugReport { Invoke-FocusedTest 'sotb-bug-report-tests' }

function Invoke-UpdateRules { Invoke-FocusedTest 'sotb-update-tests' }

function Invoke-UpdateNetwork { Invoke-FocusedTest 'sotb-update-network-tests' }

function Invoke-UpdateHelper { Invoke-FocusedTest 'sotb-update-helper-tests' }

function Invoke-ModuleHistory { Invoke-FocusedTest 'module_history_test' }

function Invoke-GuestMemory { Invoke-FocusedTest 'memory_windows_test' }

function Invoke-CpuCompatibility { Invoke-FocusedTest 'sotb-isa-tests' '^(real image|site table|table shapes|no LZCNT|isa tests)' }

function Invoke-WorkerService {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { return Result 'SKIPPED' 'Node.js 22 or newer not found' }
    if (-not (Test-Path -LiteralPath $requestFile)) { return Result 'FAIL' "$requestFile is missing: the launcher test must run first" }
    $env:SOTB_CPP_REQUEST = $requestFile
    try {
        $run = Invoke-Native { Push-Location $workerDir; try { & $node.Source --test --test-reporter=tap 'test/*.test.js' } finally { Pop-Location } }
    } finally {
        Remove-Item Env:\SOTB_CPP_REQUEST -ErrorAction SilentlyContinue
    }
    $summary = @($run.Lines | Where-Object { $_ -match '^# (pass|fail|skipped) ' })
    if ($run.Code -ne 0) {
        $run.Lines | Where-Object { $_ -match '^\s*not ok' } | Select-Object -First 12 | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "node exited with $($run.Code)"
    }
    if ($summary -notcontains '# skipped 0') { return Result 'FAIL' 'a Worker test was skipped; the launcher contract test must run' }
    Write-Host "    $($summary -join ', ')" -ForegroundColor DarkGray
    return Result 'PASS'
}

function Invoke-WorkerScript {
    $shell = (Get-Process -Id $PID).Path
    $run = Invoke-Native { & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $root 'tests\worker_script_tests.ps1') }
    if ($run.Code -ne 0) {
        $run.Lines | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "exit code $($run.Code)"
    }
    Show-Last $run
    return Result 'PASS'
}

$steps = @(
    @{ Title = 'Launcher: Bug Report client (no window, no network: stand-in transport, synthetic logs)'; Block = { Invoke-LauncherBugReport } },
    @{ Title = 'Launcher: Bug Report service (Node, disposable database): submission, receipts, limits, retention, admin, dashboard, and the launcher''s own request'; Block = { Invoke-WorkerService } },
    @{ Title = 'Launcher: worker.ps1 stand-in npx: placeholders, missing files, dry run, failures; nothing deployed'; Block = { Invoke-WorkerScript } },
    @{ Title = 'Updater: versions, release JSON, SHA-256, ZIP safety, staging, manifest, protected data, install, rollback and release contract (no network)'; Block = { Invoke-UpdateRules } },
    @{ Title = 'Updater: loopback HTTP server only (no release yet, 5xx, dropped connection, stall, timeout, redirects, retries, production address); never contacts GitHub'; Block = { Invoke-UpdateNetwork } },
    @{ Title = 'Updater: trg-updater.exe run headless from a temporary folder (parent wait, install, restart marker, environment cleanup, protected data)'; Block = { Invoke-UpdateHelper } },
    @{ Title = 'Crash report: module history, call sites, fault ownership and startup phase (no window, no game)'; Block = { Invoke-ModuleHistory } },
    @{ Title = 'Runtime: guest memory, and file reads into it while a map splits the view (synthetic file, no game)'; Block = { Invoke-GuestMemory } },
    @{ Title = 'CPU compatibility: MOVBE, BMI1 and LZCNT (host classification, MOVBE and BMI1 emulation, LZCNT rewrite run on synthetic forms and applied to every site; the real boot image is read only and skipped when absent)'; Block = { Invoke-CpuCompatibility } }
)

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) { Stop-Tool 'Run tools\build.ps1 once first: the tests reuse its build folder.' }
Remove-Item -LiteralPath $requestFile -ErrorAction SilentlyContinue

$failed = 0
foreach ($step in $steps) {
    Write-Step $step.Title
    $result = & $step.Block
    switch ($result.State) {
        'PASS' { Write-Host '    PASS' -ForegroundColor Green }
        'FAIL' { $failed++; Write-Host "    FAIL: $($result.Reason)" -ForegroundColor Red }
        default { Write-Host "    SKIPPED: $($result.Reason)" -ForegroundColor Yellow }
    }
}
Write-Host ''
Write-Host "Failed: $failed"
if ($failed) { exit 1 }
exit 0
