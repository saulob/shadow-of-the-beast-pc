<#
.SYNOPSIS
  Shadow's headless validation, the one entry point: no window, no game launch, no audio, no Internet, nothing deployed.
  The default run is every focused Windows group with the supported-build parity and version contracts; -Full is the
  complete matrix and release gate, Windows and Linux.
.DESCRIPTION
  Needs the build configured once by tools\build.ps1. Only test targets are built, incrementally in the existing build
  folder; the game executable, the launcher and the video core are not rebuilt. Every group runs even after one fails and
  ends with one line GROUP|name|PASS, FAIL, SKIPPED or BLOCKED (with checks=N when the suite counts them, parity when it
  is a parity blocker, and the reason). A group that ends without a result is a FAIL. The exit code is non-zero when any
  group failed; under -Full a BLOCKED group (missing real evidence, missing WSL tools) fails the run too.

  Parity: sotb-build-parity-tests checks the three supported builds of the launcher's table (CUSA03762 1.00 Korea,
  CUSA02871 1.00 USA, CUSA00951 1.00 Europe) against every build-keyed runtime table, applies every patch installer
  together to each build's prepared boot image (build\image, read only), and reports which public features this
  platform's shipped programs compile and wire in. A build without a prepared boot image is BLOCKED, never PASS.

  Linux (-Full, or -Filter naming a Linux group): tools/test-linux.sh runs in the first WSL distribution with clang, cmake
  and ninja, in build/cmake/linux. linux-tests builds the Linux program (never started) and runs every headless suite of
  the Linux build; linux-parity runs the same contract there. platform-parity then compares the two contracts (version,
  supported builds, public features, per-build feature results) and prints PLATFORM_PARITY|windows_vs_linux. Without WSL or
  the Linux libraries that is BLOCKED; with -SkipLinux it is SKIPPED; it is PASS only when the Linux program built, its
  suites passed and both contracts ran, passed and match. PLATFORM_CAPABILITY lines (Direct3D 11) are not compared.

  While a step runs, a progress block (percentage, bar, completed groups, elapsed time, ETA, check count when the suite
  reports one, previous group) is printed at each step start and refreshed on a timer: one carriage-return line with a | / - \
  spinner every 500 ms in a terminal, a RUNNING text line every 15 s when output is redirected. A timing table, the
  slowest groups and the build and platform parity matrix follow the last step.
.PARAMETER TimeoutSeconds
  Fails a step that runs longer than this.
.PARAMETER Full
  Runs the complete matrix: every registered group, Windows and Linux, with the Windows/Linux comparison. Real boot images
  of the three supported builds are required.
.PARAMETER SkipLinux
  Does not run the Linux groups through WSL; platform parity is reported SKIPPED, never PASS.
.PARAMETER List
  Lists the registered groups (when they run, whether they block parity, their platform) and exits; nothing is built or run.
.PARAMETER Filter
  Runs only the groups whose name contains this text (case-insensitive; _ and - are equivalent), from the whole registry,
  plus the groups they need.
.PARAMETER WindowsOnly
  The same as -SkipLinux.
.EXAMPLE
  .\tools\test.ps1
.EXAMPLE
  .\tools\test.ps1 -Full
.EXAMPLE
  .\tools\test.ps1 -List
.EXAMPLE
  .\tools\test.ps1 -Filter build-parity
.EXAMPLE
  .\tools\test.ps1 -Filter platform-parity -SkipLinux
#>
[CmdletBinding()]
param(
    [int]$TimeoutSeconds = 300,
    [switch]$Full,
    [switch]$SkipLinux,
    [switch]$List,
    [string]$Filter = '',
    [switch]$WindowsOnly
)
if ($WindowsOnly) { $SkipLinux = [switch]$true }

. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_progress.ps1')
$root = $script:RepoRoot
$buildDir = Join-Path $root 'build\cmake\clang64'
$requestFile = Join-Path $buildDir 'bin\bug-report-request.json'
$dumpContractFiles = @('bug-report-dump-request.json', 'bug-report-dump-upload.json') | ForEach-Object { Join-Path $buildDir "bin\$_" }
$workerDir = Join-Path $root 'server\shadow-bug-reports'

function Result([string]$State, [string]$Reason = '') { [pscustomobject]@{ State = $State; Reason = $Reason } }

function ConvertTo-ArgumentString([string[]]$Arguments) {
    ($Arguments | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' } else { $_ } }) -join ' '
}

# Runs a program with stdout and stderr merged into Lines. While it runs, the group's progress is refreshed on a timer,
# and "@@sotb-progress" lines from instrumented suites feed the check count; those lines are not part of Lines.
function Invoke-Native([string]$File, [string[]]$Arguments = @(), [string]$WorkingDirectory = '') {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $File
    $info.Arguments = ConvertTo-ArgumentString $Arguments
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    if ($WorkingDirectory) { $info.WorkingDirectory = $WorkingDirectory }
    $info.EnvironmentVariables['SOTB_TEST_PROGRESS'] = '1'
    $lines = New-Object System.Collections.Generic.List[string]
    try { $process = [System.Diagnostics.Process]::Start($info) }
    catch { return [pscustomobject]@{ Lines = @("could not start ${File}: $($_.Exception.Message)"); Code = -1 } }
    try {
        $readers = @($process.StandardOutput, $process.StandardError)
        $pending = @($readers[0].ReadLineAsync(), $readers[1].ReadLineAsync())
        while ($pending[0] -or $pending[1]) {
            $active = @($pending | Where-Object { $_ })
            $index = [System.Threading.Tasks.Task]::WaitAny([System.Threading.Tasks.Task[]]$active, 500)
            if ($index -ge 0) {
                $slot = [Array]::IndexOf($pending, $active[$index])
                $line = $pending[$slot].Result
                if ($null -eq $line) { $pending[$slot] = $null }
                else {
                    $pending[$slot] = $readers[$slot].ReadLineAsync()
                    $child = ConvertFrom-ChildProgress $line
                    if ($child) { $null = Set-ProgressChecks $script:Progress $script:Clock.Elapsed.TotalSeconds $child.Checks $child.Expected $child.Estimate }
                    else { $lines.Add($line) }
                }
            }
            $null = Update-Progress $script:Progress $script:Clock.Elapsed.TotalSeconds
        }
        $process.WaitForExit()
        $final = Get-FinalCheckCount $lines.ToArray()
        if ($null -ne $final) { $script:Progress.Checks = $final }
        return [pscustomobject]@{ Lines = $lines.ToArray(); Code = $process.ExitCode }
    } finally {
        $process.Dispose()
    }
}

function Show-Last($Run) { $Run.Lines | Select-Object -Last 1 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray } }

# Builds one test target and runs it from bin; a target that does not build is never run from an older executable.
function Invoke-TestTarget([string]$Target) {
    $build = Invoke-Native (Get-Command ninja).Source @('-C', $buildDir, $Target)
    if ($build.Code -ne 0) {
        $build.Lines | Select-Object -Last 12 | ForEach-Object { Write-Host "    $_" }
        return [pscustomobject]@{ Problem = "$Target did not build; the old executable was not run"; Lines = @(); Code = -1 }
    }
    $bin = Join-Path $buildDir 'bin'
    $run = Invoke-Native (Join-Path $bin "$Target.exe") @() $bin
    [pscustomobject]@{ Problem = ''; Lines = @($run.Lines); Code = $run.Code }
}

function Invoke-FocusedTest([string]$target, [string]$show = '') {
    $run = Invoke-TestTarget $target
    if ($run.Problem) { return Result 'FAIL' $run.Problem }
    if ($run.Code -ne 0) {
        $run.Lines | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "$target exited with $($run.Code)"
    }
    if ($show) { $run.Lines | Where-Object { $_ -match $show } | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray } } else { Show-Last $run }
    return Result 'PASS'
}

function Invoke-LauncherBugReport { Invoke-FocusedTest 'sotb-bug-report-tests' }

function Invoke-CrashDumpCorrelation { Invoke-FocusedTest 'sotb-crash-dump-correlation-tests' '^(  symbolic link|crash dump correlation tests)' }

function Invoke-CrashDumpUpload { Invoke-FocusedTest 'sotb-crash-dump-upload-tests' '^crash dump upload tests' }

function Invoke-SimulateCrash { Invoke-FocusedTest 'sotb-simulate-crash-tests' '^simulate crash tests' }

function Invoke-DeviceLostReporting { Invoke-FocusedTest 'sotb-device-lost-reporting-tests' '^device lost reporting tests' }

function Invoke-StartupDiagnostics { Invoke-FocusedTest 'sotb-startup-diagnostics-tests' '^startup diagnostics tests' }

function Invoke-ResolutionGuard { Invoke-FocusedTest 'sotb-resolution-guard-tests' '^resolution_guard_tests' }

function Invoke-UpdateRules { Invoke-FocusedTest 'sotb-update-tests' }

function Invoke-UpdateNetwork { Invoke-FocusedTest 'sotb-update-network-tests' }

function Invoke-UpdateHelper { Invoke-FocusedTest 'sotb-update-helper-tests' }

function Invoke-ModuleHistory { Invoke-FocusedTest 'module_history_test' }

function Invoke-GuestMemory { Invoke-FocusedTest 'memory_windows_test' }

function Invoke-CpuCompatibility { Invoke-FocusedTest 'sotb-isa-tests' '^(real image|site table|table shapes|no LZCNT|isa tests)' }

function Invoke-RendererSelection { Invoke-FocusedTest 'sotb-renderer-selection-tests' '^renderer selection tests' }

function Invoke-LoadingHintLayout { Invoke-FocusedTest 'sotb-loading-hint-layout-tests' '^loading_hint_layout_tests' }

function Invoke-VulkanPreflight { Invoke-FocusedTest 'sotb-vulkan-preflight-tests' '^vulkan preflight tests' }

function Invoke-BufferArena { Invoke-FocusedTest 'sotb-buffer-arena-tests' '^(BDA page table|buffer arena tests)' }

function Invoke-BuildIdentity { Invoke-FocusedTest 'sotb-build-identity-tests' '^(real image|build identity tests)' }

function Invoke-UnsupportedBuildDiagnostics { Invoke-FocusedTest 'sotb-build-diagnostics-tests' '^(real build|unsupported build diagnostics tests)' }

function Invoke-PipelineCache { Invoke-FocusedTest 'sotb-pipeline-cache-tests' '^pipeline cache tests' }

function Invoke-PipelineWarmup { Invoke-FocusedTest 'sotb-pipeline-warmup-tests' '^pipeline warm-up tests' }

function Invoke-ShaderCache { Invoke-FocusedTest 'sotb-shader-cache-tests' '^shader cache tests' }

function Invoke-AudioBacklog { Invoke-FocusedTest 'sotb-audio-backlog-tests' '^audio backlog tests' }

function Invoke-PadBinding { Invoke-FocusedTest 'sotb-pad-binding-tests' '^pad binding tests' }

function Invoke-PadPrompt { Invoke-FocusedTest 'sotb-pad-prompt-tests' '^pad prompt tests' }

function Invoke-ControllerPrompts {
    $policy = Invoke-FocusedTest 'sotb-controller-prompts-tests' '^(real pak|controller prompts tests)'
    if ($policy.State -ne 'PASS') { return $policy }
    $policyChecks = $script:Progress.Checks
    $overlay = Invoke-FocusedTest 'sotb-prompt-overlay-tests' '^runtime overlay tests'
    if ($null -ne $policyChecks -and $null -ne $script:Progress.Checks) { $script:Progress.Checks = $policyChecks + $script:Progress.Checks }
    return $overlay
}

function Invoke-ControllerPromptsAssets {
    $python = Get-Python
    if (-not $python) { return Result 'SKIPPED' 'Python 3 was not found' }
    $run = Invoke-Native $python.Exe (@($python.Args) + @('-I', (Join-Path $root 'tests\controller_prompts_assets_tests.py'))) $root
    if ($run.Code -ne 0) {
        $run.Lines | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "controller_prompts_assets_tests.py exited with $($run.Code)"
    }
    $run.Lines | Where-Object { $_ -match '^(SKIPPED|controller prompts assets tests)' } | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    return Result 'PASS'
}

function Invoke-MenuButtons { Invoke-FocusedTest 'sotb-menu-buttons-tests' '^(Menu button layout|menu buttons|checks,)' }

function Invoke-ExitGame { Invoke-FocusedTest 'sotb-exit-game-tests' '^exit_game_tests' }

function Invoke-SkipIntro { Invoke-FocusedTest 'skip_intro_test' 'checks,' }

function Invoke-PsnNotice { Invoke-FocusedTest 'psn_notice_test' 'checks,' }

function Invoke-LauncherCore { Invoke-FocusedTest 'sotb-launcher-tests' 'checks,' }

function Invoke-GamepadDatabase { Invoke-FocusedTest 'sotb-gamepad-database-tests' '^gamepad database tests' }

function Invoke-InputDevice { Invoke-FocusedTest 'sotb-input-device-tests' '^input device tests' }

function Invoke-CursorPolicy { Invoke-FocusedTest 'sotb-cursor-policy-tests' '^cursor policy tests' }

function Invoke-LauncherRenderer { Invoke-FocusedTest 'sotb-launcher-renderer-tests' '^launcher renderer tests' }

function Invoke-LauncherPacing { Invoke-FocusedTest 'sotb-launcher-pacing-tests' '^launcher pacing tests' }

function Invoke-LauncherPerformance { Invoke-FocusedTest 'sotb-launcher-performance-tests' '^launcher performance tests' }

function Invoke-LauncherPlacement { Invoke-FocusedTest 'sotb-launcher-placement-tests' '^launcher placement tests' }

function Invoke-LauncherStartup { Invoke-FocusedTest 'sotb-launcher-startup-tests' '^launcher startup tests' }

function Invoke-LauncherHang { Invoke-FocusedTest 'sotb-launcher-hang-tests' '^launcher hang tests' }

function Invoke-LauncherNavigation { Invoke-FocusedTest 'sotb-launcher-navigation-tests' '^launcher navigation tests' }

function Invoke-LauncherSidebar { Invoke-FocusedTest 'sotb-launcher-sidebar-tests' '^launcher sidebar tests' }

function Invoke-TrueUltrawide { Invoke-FocusedTest 'sotb-true-ultrawide-tests' '^(not prepared|true ultrawide tests)' }

function Invoke-AspectRatio { Invoke-FocusedTest 'sotb-aspect-ratio-tests' '^(not prepared|aspect ratio tests)' }

function Invoke-BugReportFeedback { Invoke-FocusedTest 'sotb-bug-report-feedback-tests' '^bug report feedback tests' }

function Invoke-CommandSnapshot { Invoke-FocusedTest 'sotb-command-snapshot-tests' '^command snapshot tests' }

function Invoke-GpuRecording { Invoke-FocusedTest 'sotb-gpu-recording-tests' '^gpu recording tests' }

function Invoke-DeviceLost { Invoke-FocusedTest 'sotb-device-lost-tests' '^device lost tests' }

function Invoke-RuntimeThreadCancel { Invoke-FocusedTest 'runtime_sync_test' '^runtime: ' }

function Invoke-SaveMountDiagnostics { Invoke-FocusedTest 'savedata_test' '^savedata: ' }

function Invoke-GpuCrashTimeline { Invoke-FocusedTest 'sotb-gpu-crash-timeline-tests' '^gpu crash timeline tests' }

function Invoke-GpuMemoryFallback { Invoke-FocusedTest 'sotb-gpu-memory-fallback-tests' '^gpu memory fallback tests' }

function Invoke-SwapchainState { Invoke-FocusedTest 'sotb-swapchain-state-tests' '^swapchain state tests' }

function Invoke-FramePacing { Invoke-FocusedTest 'sotb-frame-pacing-tests' '^frame pacing tests' }

function Invoke-PresentationHardening { Invoke-FocusedTest 'sotb-presentation-hardening-tests' '^presentation hardening tests' }

function Invoke-NvidiaHardening { Invoke-FocusedTest 'sotb-nvidia-hardening-tests' '^nvidia hardening tests' }

function Invoke-WorkerService {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { return Result 'SKIPPED' 'Node.js 22 or newer not found' }
    if (-not (Test-Path -LiteralPath $requestFile)) { return Result 'FAIL' "$requestFile is missing: the launcher test must run first" }
    $env:SOTB_CPP_REQUEST = $requestFile
    try {
        $run = Invoke-Native $node.Source @('--test', '--test-reporter=tap', 'test/*.test.js') $workerDir
    } finally {
        Remove-Item Env:\SOTB_CPP_REQUEST -ErrorAction SilentlyContinue
    }
    $summary = @($run.Lines | Where-Object { $_ -match '^# (pass|fail|skipped) ' })
    $passed = $summary | Where-Object { $_ -match '^# pass (\d+)' } | Select-Object -First 1
    if ($passed -and $passed -match '^# pass (\d+)') { $script:Progress.Checks = [long]$Matches[1] }
    if ($run.Code -ne 0) {
        $run.Lines | Where-Object { $_ -match '^\s*not ok' } | Select-Object -First 12 | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "node exited with $($run.Code)"
    }
    if ($summary -notcontains '# skipped 0') { return Result 'FAIL' 'a Worker test was skipped; the launcher contract test must run' }
    Write-Host "    $($summary -join ', ')" -ForegroundColor DarkGray
    return Result 'PASS'
}

function Invoke-TestScript([string]$Script, [string[]]$Arguments = @()) {
    $shell = (Get-Process -Id $PID).Path
    Invoke-Native $shell (@('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root $Script)) + $Arguments)
}

function Invoke-ScriptGroup([string]$Script) {
    $run = Invoke-TestScript $Script
    if ($run.Code -ne 0) {
        $run.Lines | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "exit code $($run.Code)"
    }
    Show-Last $run
    return Result 'PASS'
}

function Invoke-WorkerScript { Invoke-ScriptGroup 'tests\worker_script_tests.ps1' }

function Invoke-ProgressReporter { Invoke-ScriptGroup 'tests\progress_reporter_tests.ps1' }

function Invoke-FFmpegBootstrap { Invoke-ScriptGroup 'tests\ffmpeg_bootstrap_tests.ps1' }

# ---- parity contracts: the same sotb-build-parity-tests on Windows and, through WSL, on Linux

$script:Contracts = @{}

function Get-ResultLine([string[]]$Lines, [string]$Group) {
    $line = @($Lines | Where-Object { "$_".StartsWith("GROUP|$Group|") }) | Select-Object -Last 1
    if (-not $line) { return $null }
    $parts = "$line".Split('|')
    [pscustomobject]@{ State = $parts[2]; Detail = (@($parts | Select-Object -Skip 3) -join '|') }
}

function Get-ContractRows([string[]]$Lines, [string]$Kind) {
    @($Lines | Where-Object { "$_".StartsWith("$Kind|") } | ForEach-Object { , "$_".Split('|') })
}

function Show-ContractRun([string[]]$Lines) {
    foreach ($line in $Lines) {
        if ("$line" -notmatch '^(FAIL |BUILD_PARITY\||FEATURE_PARITY\||IMAGE_IDENTITY\||PLATFORM\||PLATFORM_CAPABILITY\||FEATURE_MISSING\||GROUP\||LINUX_|VERSION\|)') { continue }
        if ("$line" -match '^FEATURE_PARITY\|[^|]+\|PASS') { continue }
        $color = if ("$line" -match '^FAIL |\|FAIL(\||$)') { 'Red' } elseif ("$line" -match 'BLOCKED') { 'Yellow' } else { 'DarkGray' }
        Write-Host "    $line" -ForegroundColor $color
    }
}

# A contract run passes when it printed both results and both passed; real evidence missing for a build is BLOCKED.
function Get-ContractResult([string[]]$Lines, [int]$Code, [string]$Platform) {
    $parity = Get-ResultLine $Lines 'build-parity'
    $features = Get-ResultLine $Lines 'platform-features'
    if (-not $parity) { return Result 'FAIL' "the $Platform contract printed no GROUP|build-parity result (exit code $Code)" }
    if (-not $features) { return Result 'FAIL' "the $Platform contract printed no GROUP|platform-features result (exit code $Code)" }
    $reasons = @()
    if ($parity.State -eq 'FAIL') {
        $builds = @(Get-ContractRows $Lines 'BUILD_PARITY' | Where-Object { $_[4] -eq 'FAIL' } | ForEach-Object { "$($_[1]) $($_[2])" })
        $reasons += "supported-build parity failed$(if ($builds.Count) { " for $($builds -join ', ')" })"
    }
    if ($features.State -ne 'PASS') { $reasons += "platform features: $($features.Detail)" }
    if ($reasons.Count) { return Result 'FAIL' ($reasons -join '; ') }
    if ($parity.State -eq 'BLOCKED') {
        $builds = @(Get-ContractRows $Lines 'BUILD_PARITY' | Where-Object { $_[4] -eq 'BLOCKED' } | ForEach-Object { "$($_[1]) $($_[2])" })
        return Result 'BLOCKED' "no prepared boot image (real evidence) for $($builds -join ', '); install those builds from the launcher"
    }
    if ($parity.State -ne 'PASS' -or $Code -ne 0) { return Result 'FAIL' "unexpected result $($parity.State), exit code $Code" }
    return Result 'PASS'
}

function Invoke-BuildParity {
    $run = Invoke-TestTarget 'sotb-build-parity-tests'
    if ($run.Problem) { return Result 'FAIL' $run.Problem }
    if ($run.Lines | Where-Object { "$_".StartsWith('VERSION|') }) { $script:Contracts['windows'] = $run.Lines }
    Show-ContractRun $run.Lines
    return Get-ContractResult $run.Lines $run.Code 'Windows'
}

$script:Linux = $null

# The WSL distribution the Linux groups run in, found once per run.
function Resolve-Linux {
    if ($script:Linux) { return $script:Linux }
    $found = [pscustomobject]@{ Distro = ''; Root = ''; Problem = '' }
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        $found.Problem = 'WSL is not installed (install a distribution with clang, cmake and ninja, or pass -SkipLinux)'
    } else {
        foreach ($name in @(& wsl.exe -l -q 2>$null | ForEach-Object { ("$_" -replace "`0", '').Trim() } | Where-Object { $_ })) {
            $null = & wsl.exe -d $name --exec bash -c 'command -v clang >/dev/null && command -v cmake >/dev/null && command -v ninja >/dev/null' 2>$null
            if ($LASTEXITCODE -eq 0) { $found.Distro = $name; break }
        }
        if (-not $found.Distro) {
            $found.Problem = 'no WSL distribution has clang, cmake and ninja (install them, or pass -SkipLinux)'
        } else {
            $found.Root = ("$(& wsl.exe -d $found.Distro --exec wslpath -u ($root.Replace('\', '/')) 2>$null | Select-Object -First 1)" -replace "`0", '').Trim()
            if (-not $found.Root) { $found.Problem = "WSL distribution $($found.Distro) cannot see this checkout" }
        }
    }
    $script:Linux = $found
    return $found
}

# Runs tools/test-linux.sh. Outcome is set when it could not test anything (skipped, blocked or broken).
function Invoke-LinuxRunner([string]$Mode) {
    if ($SkipLinux) { return [pscustomobject]@{ Outcome = (Result 'SKIPPED' 'skipped with -SkipLinux'); Lines = @(); Code = -1 } }
    $linux = Resolve-Linux
    if ($linux.Problem) { return [pscustomobject]@{ Outcome = (Result 'BLOCKED' $linux.Problem); Lines = @(); Code = -1 } }
    Write-Host "    WSL distribution: $($linux.Distro)" -ForegroundColor DarkGray
    $run = Invoke-Native 'wsl.exe' @('-d', $linux.Distro, '--exec', 'bash', "$($linux.Root)/tools/test-linux.sh", $Mode)
    $outcome = $null
    $blocked = @($run.Lines | Where-Object { "$_".StartsWith('LINUX_BLOCKED|') }) | Select-Object -First 1
    $broken = @($run.Lines | Where-Object { "$_".StartsWith('LINUX_FAIL|') }) | Select-Object -First 1
    if ($broken) { $outcome = Result 'FAIL' ("$broken" -replace '^LINUX_FAIL\|', '') }
    elseif ($blocked) { $outcome = Result 'BLOCKED' ("$blocked" -replace '^LINUX_BLOCKED\|', '') }
    elseif ($run.Code -eq 3) { $outcome = Result 'BLOCKED' "tools/test-linux.sh exited with $($run.Code)" }
    if ($outcome) { $run.Lines | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" } }
    return [pscustomobject]@{ Outcome = $outcome; Lines = @($run.Lines); Code = $run.Code }
}

# The Linux program and every planned suite must each have reported PASS; a suite with no result is a failure.
function Get-LinuxTestsResult([string[]]$Lines, [int]$Code) {
    $plan = @(Get-ContractRows $Lines 'LINUX_PLAN') | Select-Object -First 1
    if (-not $plan) { return Result 'FAIL' "the Linux runner announced no suites (exit code $Code)" }
    $names = @("$($plan[2])".Split(' ') | Where-Object { $_ })
    if ($names.Count -eq 0 -or "$($names.Count)" -ne $plan[1]) { return Result 'FAIL' 'the Linux runner announced an inconsistent plan' }
    $problems = @()
    $program = @(Get-ContractRows $Lines 'LINUX_EXECUTABLE') | Select-Object -First 1
    if (-not $program) { $problems += 'the Linux program was not built' }
    elseif ($program[2] -ne 'PASS') { $problems += "$($program[1]): $(@($program | Select-Object -Skip 3) -join ' ')" }
    $results = @(Get-ContractRows $Lines 'LINUX_RESULT')
    foreach ($name in $names) {
        $mine = @($results | Where-Object { $_[1] -eq $name })
        if ($mine.Count -ne 1) { $problems += "$name reported $($mine.Count) results" }
        elseif ($mine[0][2] -ne 'PASS') { $problems += "${name}: $(@($mine[0] | Select-Object -Skip 3) -join ' ')" }
    }
    if ($problems.Count) { return Result 'FAIL' ($problems -join '; ') }
    if ($Code -ne 0) { return Result 'FAIL' "tools/test-linux.sh exited with $Code" }
    $script:Progress.Checks = $names.Count
    return Result 'PASS'
}

function Invoke-LinuxTests {
    $run = Invoke-LinuxRunner '--full'
    if ($run.Outcome) { return $run.Outcome }
    foreach ($line in $run.Lines) {
        if ("$line" -match '^LINUX_(SUITE|PLAN)\|') { continue }
        $color = if ("$line" -match '\|FAIL(\||$)') { 'Red' } elseif ("$line" -match '^LINUX_') { 'DarkGray' } else { 'Gray' }
        Write-Host "    $line" -ForegroundColor $color
    }
    return Get-LinuxTestsResult $run.Lines $run.Code
}

function Invoke-LinuxParity {
    $run = Invoke-LinuxRunner '--parity'
    if ($run.Outcome) { return $run.Outcome }
    if ($run.Lines | Where-Object { "$_".StartsWith('VERSION|') }) { $script:Contracts['linux'] = $run.Lines }
    Show-ContractRun $run.Lines
    return Get-ContractResult $run.Lines $run.Code 'Linux'
}

# The contract lines both platforms must print identically, keyed by what they describe.
function Read-Contract([string[]]$Lines) {
    $table = [ordered]@{}
    foreach ($line in $Lines) {
        $p = "$line".Split('|')
        if ($p[0] -eq 'VERSION' -and $p.Count -ge 2) { $table['VERSION'] = $p[1] }
        elseif ($p[0] -eq 'SUPPORTED_BUILD' -and $p.Count -ge 4) { $table["SUPPORTED_BUILD|$($p[1])|$($p[2])"] = $p[3] }
        elseif ($p[0] -eq 'FEATURE' -and $p.Count -ge 3) { $table["FEATURE|$($p[1])"] = $p[2] }
        elseif ($p[0] -eq 'BUILD_FEATURE' -and $p.Count -ge 6) { $table["BUILD_FEATURE|$($p[1])|$($p[2])|$($p[3])"] = "$($p[4])|$($p[5])" }
        elseif ($p[0] -eq 'BUILD_PARITY' -and $p.Count -ge 5) { $table["BUILD_PARITY|$($p[1])|$($p[2])"] = "$($p[3])|$($p[4])" }
        elseif ($p[0] -eq 'FEATURE_PARITY' -and $p.Count -ge 3) { $table["FEATURE_PARITY|$($p[1])"] = $p[2] }
    }
    return $table
}

function Compare-Contracts($Windows, $Linux) {
    $differences = New-Object System.Collections.Generic.List[string]
    foreach ($key in $Windows.Keys) {
        if (-not $Linux.Contains($key)) { $differences.Add("$key only on Windows ($($Windows[$key]))") }
        elseif ($Windows[$key] -ne $Linux[$key]) { $differences.Add("${key}: Windows $($Windows[$key]), Linux $($Linux[$key])") }
    }
    foreach ($key in $Linux.Keys) {
        if (-not $Windows.Contains($key)) { $differences.Add("$key only on Linux ($($Linux[$key]))") }
    }
    return $differences.ToArray()
}

function Invoke-PlatformParity {
    $windows = $script:Contracts['windows']
    $linux = $script:Contracts['linux']
    $outcome = $null
    if (-not $windows) { $outcome = Result 'FAIL' 'the Windows parity contract did not run' }
    elseif (-not $linux) {
        $state = $script:States['linux-parity']
        if ($SkipLinux) { $outcome = Result 'SKIPPED' 'skipped with -SkipLinux' }
        elseif ($state -and $state.State -eq 'FAIL') { $outcome = Result 'FAIL' "the Linux contract did not run: $($state.Reason)" }
        elseif ($state) { $outcome = Result 'BLOCKED' "the Linux contract did not run: $($state.Reason)" }
        else { $outcome = Result 'BLOCKED' 'the Linux contract did not run' }
    } else {
        $differences = @(Compare-Contracts (Read-Contract $windows) (Read-Contract $linux))
        foreach ($difference in $differences) { Write-Host "    PLATFORM_DIFF|$difference" -ForegroundColor Red }
        $unproven = @('build-parity', 'linux-tests', 'linux-parity' | Where-Object { -not $script:States.Contains($_) -or $script:States[$_].State -ne 'PASS' })
        $outcome = if ($differences.Count) { Result 'FAIL' "$($differences.Count) Windows/Linux contract differences" }
        elseif (@($unproven | Where-Object { -not $script:States.Contains($_) -or $script:States[$_].State -eq 'FAIL' }).Count) { Result 'FAIL' "the contracts match, but $($unproven -join ', ') did not pass" }
        elseif ($unproven.Count) { Result 'BLOCKED' "the contracts match, but $($unproven -join ', ') did not pass" }
        else { Result 'PASS' }
    }
    Write-Host "PLATFORM_PARITY|windows_vs_linux|$($outcome.State)$(if ($outcome.Reason) { "|$($outcome.Reason)" })"
    return $outcome
}

function Get-CanonicalVersion {
    $match = [regex]::Match((Get-Content -Raw -LiteralPath (Join-Path $root 'CMakeLists.txt')), '(?m)^project\([^)]*\bVERSION\s+([0-9][0-9A-Za-z.+-]*)')
    if ($match.Success) { $match.Groups[1].Value } else { '' }
}

function Invoke-VersionContract {
    $configure = Invoke-Native (Get-Command ninja).Source @('-C', $buildDir, 'build.ninja')
    if ($configure.Code -ne 0) {
        $configure.Lines | Select-Object -Last 12 | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' 'CMake could not regenerate the build, so the generated version header may be stale'
    }
    $run = Invoke-TestScript 'tests\version_contract_tests.ps1' @('-GeneratedHeader', (Join-Path $buildDir 'generated\sotb_version.h'))
    $run.Lines | Where-Object { "$_" -match '^(FAIL |VERSION_LITERAL\||GROUP\|)' } | ForEach-Object {
        Write-Host "    $_" -ForegroundColor $(if ("$_" -match '^FAIL |\|FAIL') { 'Red' } else { 'DarkGray' })
    }
    $group = Get-ResultLine $run.Lines 'version'
    if (-not $group) { return Result 'FAIL' "the version contract printed no GROUP|version result (exit code $($run.Code))" }
    if ($group.State -ne 'PASS' -or $run.Code -ne 0) { return Result 'FAIL' 'the version contract failed' }
    return Result 'PASS'
}

# The registry. Tier 'full' groups run only with -Full or a -Filter that names them; Parity marks a parity blocker;
# Platform is where the group runs; Needs lists the groups whose results it reads.
$steps = @(
    @{ Name = 'version'; Title = 'Version contract: one canonical version (project VERSION in CMakeLists.txt, 0.1.0) behind the generated runtime header, the Windows version resource, the launcher, the updater and the parity contract on both platforms; no literal port version left in production files, every mention of the former version classified (reads files; CMake regenerates the build files first)'; Parity = $true; Block = { Invoke-VersionContract } },
    @{ Name = 'build-parity'; Title = 'Supported-build parity and the Windows feature contract: CUSA03762 1.00 Korea, CUSA02871 1.00 USA and CUSA00951 1.00 Europe from the launcher''s table, every build-keyed runtime table, unknown builds refused, every patch installer applied together on each build''s prepared boot image in the loader''s order and cave layout (the unsupported-build diagnostics'' read-only check agreeing with each, patch destinations, cave slices, no overlap, shifted images and foreign build keys refused, every render target and Resolution Guard fallback), Korea/USA image identity and the Europe differences, and the public features this platform''s programs compile (boot images read only; no game, no window)'; Parity = $true; Block = { Invoke-BuildParity } },
    @{ Name = 'bug-report-client'; Title = 'Launcher: Bug Report client (no window, no network: stand-in transport, synthetic logs): report, preview, privacy filter, receipts, My Reports history, statuses, message revisions and New reply, unread first, Awaiting player test answers tied to their request, Send a new report, refresh wiring, the background log scan'; Block = { Invoke-LauncherBugReport } },
    @{ Name = 'crash-dump-correlation'; Title = 'Crash dump correlation: dump numbers stay taken, exact session name and folder, named by its own log, written with it, size, MDMP signature, directories and links refused, local retention of the newest dumps, crash, device lost and normal sessions told apart, no local path sent (synthetic logs and dumps, no real crash)'; Block = { Invoke-CrashDumpCorrelation } },
    @{ Name = 'crash-dump-simulate'; Title = 'Crash dump simulated crash trigger: SOTB_SIMULATE_CRASH armed only by exactly 1, called once after the log, the fault handlers, the crash dump writer and the launcher''s return, no abort, exit, TerminateProcess or RaiseException (parsing and source wiring; nothing crashes)'; Block = { Invoke-SimulateCrash } },
    @{ Name = 'crash-dump-upload'; Title = 'Crash dump upload: Crash detected and the optional attachment, report with and without a dump, chunks resumed from what the service holds, lost answers and already-stored chunks, refusals stop, failures keep the report, retry under the same Report ID, repeated send with the submission key files one report, no local path sent (stand-in service, no network)'; Block = { Invoke-CrashDumpUpload } },
    @{ Name = 'device-lost-reporting'; Title = 'Device-lost reporting: the device-lost owner''s own lines classify the session (exit code 25 path), reason with operation, result, stage, GPU, driver and bounded fault text, timeline left in the log, missing fault data, first fault owns the session, last startup stage kept, with and without a dump (no Vulkan device)'; Block = { Invoke-DeviceLostReporting } },
    @{ Name = 'startup-diagnostics'; Title = 'Startup diagnostics: one current stage printed once per change, GPU initialization and first frame presented stages, no per-frame growth, empty stages ignored, loader stage order unchanged, the stage a failure happened in reaches the report, module history keeps the stage (no game, no window)'; Block = { Invoke-StartupDiagnostics } },
    @{ Name = 'resolution-guard-tests'; Title = 'Resolution guard: a high-resolution startup crash restores 1440p, a gameplay crash, clean run, pre-patch failure and unknown outcome keep the setting, 1800p and 4K arm the marker, marker arm/read/clear (the aspect ratio kept beside the resolution, older marker files read as 16:9), the gameplay milestone clears it, 1440p, 1800p and 4K stay selectable (pure policy and marker files, no game, no window)'; Parity = $true; Block = { Invoke-ResolutionGuard } },
    @{ Name = 'aspect-ratio'; Title = 'Aspect Ratio: the six shapes (16:9 Original, 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3, 5:4) and the aspect_ratio setting (a missing file or key, an empty, invalid, label, index or legacy value are all 16:9 Original in the settings, the loader''s reader and the summary, reset, persistence), Window size choices at 720, 900, 1080, 1440 and 2160 following the shape, the render target of every Render resolution in every shape (height kept, width aligned to 8, 3840x2160 pixel budget reached by losing height, 16:9 unchanged, 854x480 included), the six display size sites identical and patched on the real CUSA03762, CUSA02871 and CUSA00951 images with nothing else written, altered sites refused, no overlap with the other guest patches, the Resolution Guard and its 1440p-equivalent fallback judged on the real target, the loading hint independent of the shape, and the presenter, SMAA and FSR taking the game''s own image size (real images read only and skipped when absent; no game, no window)'; Parity = $true; Block = { Invoke-AspectRatio } },
    @{ Name = 'true-ultrawide'; Title = 'True ultrawide: Hor+ for constrained perspective views (gameplay and the title) and the centered 16:9 safe area of the game''s widgets, switched together by the True Ultrawide setting (on by default, 21:9 and 32:9 only; safe area geometry and its 750 ms hold after a widened view, a synthetic game layer hook run for real, the SGameLayerManager vtable slot checked on relocated real images); the projection function FMinimalViewInfo::CalculateProjectionMatrixGivenView read from its own bytes (FOV at +0x18 is horizontal, x scale 1/tan and y scale aspect/tan, the constrained rectangle built from the aspect ratio field, view rectangle at +0x90, two callers), the field of view math (the vertical view of the camera is kept and the horizontal one grows with the width), only 21:9 and 32:9 hooked and never 16:9, 16:10, 4:3 or 5:4, only constrained perspective views widened in any phase (gameplay 61.5, title 39.5, a default 90) and everything else left alone, idempotent, the hook on CUSA03762, CUSA02871 and CUSA00951 (one identical function, a 6-byte site and a cave stub, nothing else written, any altered byte refused, no overlap with the other guest patches in the image or the cave and all of them applying together), the HUD components anchored to the real screen edges (left, right and centre groups at the 16:9 size, the side glows, the wrapper run for real on 21:9, 32:9 and 16:9 canvases with the transforms balanced and everything restored, full-screen effects left alone, the cutscene skip fade redirected through a cave stub to cover the whole real canvas at 21:9 and 32:9 and untouched at 16:9, the hook on the three builds with fingerprints, refusals and no overlap), the score and mana overlay classified with the map, the title, the map, the options and Wisdom of Shadows told by the game''s own screen stack (the frontend manager''s per-frame function hooked on the three builds with fingerprints, refusals and no overlap, the screen values proven from the registration order, the hook run for real on a synthetic stack: the title, the map and the two menus always in the centered safe area at 21:9 and 32:9 from the frame they open, the menus with no side fill, the pause unchanged, no clock or camera involved, the loading screen clearing them), the side fill as two separate modes (title and map: the vignette of their own background texture (full and half alpha) on the sides and on the thin strip the background leaves at the right edge, read only under the pixel itself with no edge repeat, no blur, no black, frame pixels straddling the darkened span vignetted column by column so no seam is left (21:9 and 32:9 in several windows), switched off the moment the screen ends; title, loading, gameplay, map and ten more levels in a row with no fill on any gameplay frame past the hold; loading: blurred copy with the centre untouched, the loading hook run for real on a synthetic image, the mark and its 200 ms hold, the central fraction, the shader and presenter wiring), the loader wiring, the launcher toggle (default on, disabled outside 21:9 and 32:9) and no diagnostics left (real images read only and skipped when absent; no game, no window)'; Parity = $true; Block = { Invoke-TrueUltrawide } },
    @{ Name = 'bug-report-service'; Title = 'Launcher: Bug Report service (Node, disposable database): submission, receipts, limits, retention, admin, dashboard, player tests (revisions, stale and repeated answers, feedback filter), message revisions, databases from the previous Worker, crash dump announcement, resumable chunked upload, completion checks, minidump download and retention, submission key, the launcher''s own report and dump requests, and the launcher contract field by field (every supported build, Windows and Linux, any port version, stable refusal codes with the protocol, nothing echoed, accepted reports listed on /admin)'; Needs = @('bug-report-client', 'crash-dump-upload'); Block = { Invoke-WorkerService } },
    @{ Name = 'worker-script'; Title = 'Launcher: worker.ps1 stand-in npx: placeholders, missing files, dry run, failures; nothing deployed'; Block = { Invoke-WorkerScript } },
    @{ Name = 'updater-rules'; Title = 'Updater: versions, release JSON, SHA-256, ZIP safety, staging, manifest, protected data, install, rollback and release contract (no network)'; Block = { Invoke-UpdateRules } },
    @{ Name = 'updater-network'; Title = 'Updater: loopback HTTP server only (no release yet, 5xx, dropped connection, stall, timeout, redirects, retries, production address, transient status report, cross-origin header stripping, log redaction, destination preserved on failure and cancel, bounded automatic retries of transient failures, cancellation, manual Retry); never contacts GitHub'; Block = { Invoke-UpdateNetwork } },
    @{ Name = 'updater-helper'; Title = 'Updater: trg-updater.exe run headless from a temporary folder (parent wait, install, restart marker, environment cleanup, protected data)'; Block = { Invoke-UpdateHelper } },
    @{ Name = 'module-history'; Title = 'Crash report: module history, call sites, fault ownership and startup phase (no window, no game)'; Block = { Invoke-ModuleHistory } },
    @{ Name = 'guest-memory'; Title = 'Runtime: guest memory, and file reads into it while a map splits the view (synthetic file, no game)'; Block = { Invoke-GuestMemory } },
    @{ Name = 'runtime-thread-cancel'; Title = 'Runtime threads: create, join, detach and exit, scePthreadCancel as a recorded request (invalid and finished handles, running threads left to finish, cancelled sleepers, condition waits and joiners, joins and detaches racing a cancel, cancel storms, no host cancellation, timed wait or polling), mutexes and condition variables (no game, no window)'; Block = { Invoke-RuntimeThreadCancel } },
    @{ Name = 'save-mount-diagnostics'; Title = 'Save data: mount, search, delete and save memory through the real save and file layers, and one bounded failure line for Mount, Mount2 and TransferringMount (call, directory, error, mode, no host path, hostile names) in a synthetic save root; the real user save is read only and skipped when absent'; Block = { Invoke-SaveMountDiagnostics } },
    @{ Name = 'cpu-compatibility'; Title = 'CPU compatibility: MOVBE, BMI1 and LZCNT (host classification, MOVBE and BMI1 emulation, LZCNT rewrite run on synthetic forms and applied to every site; the real boot image is read only and skipped when absent)'; Parity = $true; Block = { Invoke-CpuCompatibility } },
    @{ Name = 'renderer-selection'; Title = 'Renderer selection: the Renderer setting, Auto fallback and the sources that must agree (no window, no context)'; Block = { Invoke-RendererSelection } },
    @{ Name = 'loading-hint-layout-tests'; Title = 'Loading screen hint layout: 1920x1080 identity, 1440p 1.333x, 4K 2x, centered bar, proportional placement, ultrawide kept, vtable patch fingerprint validation (pure geometry, no window)'; Parity = $true; Block = { Invoke-LoadingHintLayout } },
    @{ Name = 'vulkan-preflight'; Title = 'Vulkan preflight: loader, runtime, device, mandatory extensions and features, optional capabilities, explicit Vulkan and Auto fallback (synthetic capabilities, no Vulkan, no window)'; Block = { Invoke-VulkanPreflight } },
    @{ Name = 'buffer-arena'; Title = 'GPU memory: sparse or fully resident arena decision, layout, overflow, allocation failure rollback and migration (fake arenas, no Vulkan device)'; Block = { Invoke-BufferArena } },
    @{ Name = 'build-identity'; Title = 'Unsupported build diagnostics: supported identity, altered fields, candidates, deterministic report, refusal before guest code (synthetic; the real boot image is read only and skipped when absent)'; Parity = $true; Block = { Invoke-BuildIdentity } },
    @{ Name = 'unsupported-build-diagnostics'; Title = 'Unsupported-build diagnostics: source classification (supported, unsupported version, unknown TITLE_ID with Shadow evidence, different game, incomplete, malformed), param.sfo metadata, base and CATEGORY gp update packages, package and folder sources with equal results and archives read only by footer and index head, the SELF/ELF layout and the launcher''s own image builder (malformed, truncated, oversized, overlapping and wrapping executables refused before mapping), modules, the loader''s patch checks run read-only and equal to the installers, exploratory site search (moved, ambiguous, missing, bounded), known metadata with a different eboot SHA-256 refused and diagnosed, nothing installed or changed, the bounded log block kept whole by Bug Report''s filter, every per-build parity feature covered (synthetic sources; the prepared builds are read only and skipped when absent)'; Parity = $true; Needs = @('build-identity'); Block = { Invoke-UnsupportedBuildDiagnostics } },
    @{ Name = 'pipeline-cache'; Title = 'Pipeline cache: sealed blobs, host identity (GPU, driver, pipeline cache UUID, format), corruption and truncation, stale temporaries, atomic writes, failure keeps the previous cache, unrelated files kept (temporary folders, no Vulkan, no real cache)'; Block = { Invoke-PipelineCache } },
    @{ Name = 'pipeline-warmup'; Title = 'Pipeline warm-up: startup choice with an on-demand override, bounded worker pool and queue, no duplicate compilation against the game''s own pipelines, failures counted once and never retried, shutdown drain and refusal of new work (fake jobs and a fake renderer, no Vulkan, no window)'; Block = { Invoke-PipelineWarmup } },
    @{ Name = 'shader-cache'; Title = 'Shader cache: background loading note hidden below 500 ms and gone when the load ends, deterministic cache identity, relevant changes invalidate and unrelated ones never enter the key, cleared only on a mismatch, persistent writable cache folder per build, sealed blobs reused, truncated and corrupt ones rejected (synthetic data in temporary folders, no Vulkan, no window, no real cache)'; Block = { Invoke-ShaderCache } },
    @{ Name = 'audio-backlog'; Title = 'Audio backlog recovery: format-aware queue duration, 100 ms threshold and boundary, invalid formats and overflow, steady playback never recovers, device stall and endpoint reset recover, repeated recoveries, log rate limit (pure queue model, no audio device)'; Block = { Invoke-AudioBacklog } },
    @{ Name = 'pad-binding'; Title = 'Controller rebinding: default layout, every action and source, triggers, shoulders, stick clicks, Options and touchpad, persistence and reset in the settings file, invalid and older configs, duplicate policy, capture rules, hotplug (synthetic gamepad samples, no controller, no window)'; Block = { Invoke-PadBinding } },
    @{ Name = 'pad-prompt'; Title = 'Controller prompts: Xbox, DualShock, DualSense, generic and Steam Input detection, Auto/Xbox/PlayStation policy, active controller tracking without flapping, persisted mode, labels, regional Confirm/Back, prompts never change the guest buttons (synthetic metadata, no controller)'; Block = { Invoke-PadPrompt } },
    @{ Name = 'controller-prompts'; Title = 'In-game controller prompts: Auto, Xbox and PlayStation style chosen once at startup (Auto follows the first controller, anything else gives Xbox, explicit modes ignore the controller), invalid setting is Auto, PlayStation installs nothing, Xbox art served over the unchanged pak only after pak size, footer, index and every texture CRC-32 match, all-or-nothing, modified, truncated, wrong-build and damaged-table sources rejected with the original art kept, bounded diagnostics without paths, no per-frame decision, launcher wording, and the runtime file overlay itself (sorted non-overlapping patches, reads across chunks, patch edges, the host end and zeros past it, seek and size, refusals, table limit, host file unchanged; synthetic paks and files, no controller, no window; the installed pak is read only and skipped when absent)'; Parity = $true; Block = { Invoke-ControllerPrompts } },
    @{ Name = 'controller-prompts-assets'; Title = 'Xbox prompt assets: generated data tied to the build and pak identity, every texture CRC-32, position, format, mips and tiling against the installed pak, replacement confined to mip payload bytes and to the original footprint, glyph coverage of every prompt the game draws, deterministic regeneration, line art licence and notices, no local paths and no game files in the repository (the installed pak is read only and its checks are skipped when absent)'; Parity = $true; Block = { Invoke-ControllerPromptsAssets } },
    @{ Name = 'menu-buttons'; Title = 'Menu Button Layout: the Western default, the saved setting and environment override, multi-build support (CUSA03762 Korea Circle-confirm original, CUSA02871 USA and CUSA00951 Europe Western original), site fingerprint and unique location, Western changing only the six-byte region branch, Original leaving the image untouched, unknown builds and altered, short or duplicated sites written by nothing'; Parity = $true; Block = { Invoke-MenuButtons } },
    @{ Name = 'exit-game'; Title = 'Exit Game: Map and Pause allowlist, append guard, input filter, 19-culture localization, confirmation modal Western/Original and Xbox/PlayStation mapping, multi-build (CUSA03762, CUSA02871, CUSA00951) fingerprint patching, fail-closed refusals, and cross-patch non-overlap verification (synthetic, no window)'; Parity = $true; Block = { Invoke-ExitGame } },
    @{ Name = 'skip-intro'; Title = 'Skip Intro: on CUSA03762, CUSA02871 and CUSA00951 the loading screen''s own publisher and developer sequence starts finished, so its three boot images (Shadow of the Beast, Sony Computer Entertainment presents, XDEV and Heavy Spectrum) never appear and the same completion path runs on the first frame; the fingerprint is unique, altered, short and missing images are refused with nothing written, the normal loading screen, Press Start, frontend and gamma screen stay untouched, and no frame is hidden and no input injected (real images and synthetic, no game, no window)'; Parity = $true; Block = { Invoke-SkipIntro } },
    @{ Name = 'psn-notice'; Title = 'PlayStation Network notice: ConnectionSuccess in SignInPopup and MainMenuScreen is redirected before the locres lookup to an unlocalized key whose text is a single space on CUSA03762, CUSA02871 and CUSA00951, every other key and namespace keeps its lookup, the old literal patches are gone, the NP state and trophy wiring stay intact, and altered or short images and caves are refused (no game, no window)'; Parity = $true; Block = { Invoke-PsnNotice } },
    @{ Name = 'launcher-tests'; Title = 'Launcher core: Play page cleanup, About page layout, multi-build SupportedBuildsText, settings persistence, Menu & Subtitles, Skip Intro, Menu Button Layout, render resolution patches, keys, icons, header art and palette (no window)'; Parity = $true; Block = { Invoke-LauncherCore } },
    @{ Name = 'gamepad-database'; Title = 'Gamepad database: user/gamecontrollerdb.txt beside the settings file (never a configured path), missing file silent, one bounded summary line, malformed and unreadable files harmless, size cap, loaded once before the first gamepad enumeration, mappings reach later joysticks through SDL, recognized pads unchanged; unmapped joysticks logged once per device identity (name, GUID, VID:PID, no pointer or path), reconnects and hotplug, bounded memory (fake loader and SDL virtual joysticks, no controller, no window)'; Block = { Invoke-GamepadDatabase } },
    @{ Name = 'input-device'; Title = 'Controller activity: button and D-pad onset, stick and trigger thresholds with hysteresis, held input and drift never repeat, rearm after release, independent axes, extreme and zero values, per-controller state with hotplug and disconnect (pure policy, no controller)'; Block = { Invoke-InputDevice } },
    @{ Name = 'cursor-policy'; Title = 'Cursor visibility: last meaningful device (mouse, keyboard, controller), overlay forces the cursor visible, device kept while the overlay is open, overlay close follows the current device, no flapping, launcher and focus rules, window wiring (pure policy, no window, no cursor)'; Block = { Invoke-CursorPolicy } },
    @{ Name = 'launcher-renderer'; Parity = $true; Title = 'Launcher renderer policy: Direct3D 11 first on Windows with software as the fallback and d3d11.dll and dxgi.dll pinned for the life of the process (never freed), SOTB_LAUNCHER_RENDERER=software forcing software, other values ignored, Direct3D 11 never asked for elsewhere, each renderer tried once with no fallback loop, an attempt validated through window, pin, renderer, first frame (texture upload, clear, draw, present) and ImGui backends, a failed attempt released once at any stage before the next, a failed pin skips the renderer, each failed attempt and complete failure reported once with a bounded stage and reason and the player told, fallback reason, attempt stage names the renderer, software recovery request neither saved nor seen by the game, launcher renderer separate from the game Renderer setting and Vulkan, requested and actual renderer classified and logged once, one creation site outside the frame, release order textures, ImGui backends, renderer, window, SDL, launcher closed before the game (pure policy and source wiring, no window, no SDL, no D3D11)'; Block = { Invoke-LauncherRenderer } },
    @{ Name = 'launcher-pacing'; Title = 'Launcher frame cap and minimized behavior: 60 Hz deadline pacing with an exact period (first, early, on-time, slightly late, boundaries, large stall and clock jumps re-anchor with no catch-up burst, no negative wait, three days without drift), high-refresh displays never exceed 60 Hz, independent of the game frame rate and the renderer, sleeps instead of spinning, minimized idles at 50 ms with events and background work kept and no draw, restore draws once and rebases, repeated minimize and restore (synthetic timestamps, no window, no sleeping)'; Block = { Invoke-LauncherPacing } },
    @{ Name = 'launcher-performance'; Title = 'Launcher performance telemetry: render rate, UI-thread CPU, build, submit and present timing in 5 s windows after a warm-up, runaway-loop, present-stall and high-CPU warnings once per incident and at most once a minute, renderer in every line, formatting bounded and sanitized, minimize discards the window, no per-frame logging or CPU reads, deterministic, a seven-day session without overflow (synthetic timestamps, no window)'; Block = { Invoke-LauncherPerformance } },
    @{ Name = 'launcher-placement'; Title = 'Launcher placement: centered on the primary at the display scale, a saved window kept while it fits and its title bar can be reached, moved and shrunk into its display when it sticks out of the top or no longer fits, centered with its size kept after its display was removed, moved or its coordinates went negative, monitors left of and above the primary, primary changed, resolution reduced, usable area smaller than the launcher, DPI and the title bar reserve, maximized and fullscreen kept, one bounded placement line with display, scale and final position, minimized keeps the saved state, chosen once while hidden (synthetic displays, no window)'; Block = { Invoke-LauncherPlacement } },
    @{ Name = 'launcher-startup'; Parity = $true; Title = 'Launcher startup: created hidden, placement, renderer, ImGui, fonts and a complete launcher frame before the window shows, shown once and that frame presented again, no visible move or resize, failures and a first-frame Play or Quit never show it and never start the game, the reason shown in a system message box, renderer failure path and each attempt step with its own stage, renderer selection, window created, renderer ready, first frame and running stages in order with no per-frame stages, existing stages kept, Vulkan untouched until the visible launcher has drawn 3 frames over 0.5 s, then the graphics card list asked once (skipped in software recovery) with the saved card shown meanwhile and kept when Vulkan finds none (source wiring, the stage store and the graphics card policy, no window, no Vulkan)'; Block = { Invoke-LauncherStartup } },
    @{ Name = 'launcher-hang'; Title = 'Launcher UI stalls: normal, 60 Hz paced and minimized loops stay quiet, a long frame build, a held event pump and a long present before telemetry warm-up are reported once per incident and at most once a minute, a later present stall left to launcher_present_stall, native file dialogs excused, recovery re-arms, stage and minimized state in a bounded line, thirty days of uptime, no watchdog thread and no termination (synthetic timestamps, no window)'; Block = { Invoke-LauncherHang } },
    @{ Name = 'launcher-navigation'; Title = 'Launcher Escape: never quits from a page, repeated presses change nothing, the Quit button still quits, controller Back unchanged, key capture cancelled without binding and keeping the old key, confirmation dialog closed as Cancel, a text field keeps its draft, controller capture cancelled before any button, one action per press (TRG and ImGui with synthetic input, no window)'; Block = { Invoke-LauncherNavigation } },
    @{ Name = 'launcher-sidebar'; Title = 'Launcher sidebar scrolling at 100, 150 and 225 percent: scroll range and clamps, the current page revealed when it changes or the window is resized and the scroll left alone otherwise, only the page list scrolls with the launcher''s scrollbar in place of its right padding, wheel over the sidebar scrolls it and wheel over the page scrolls the page, clicks on scrolled rows, arrows, Tab and D-pad keeping the focused page in view, badge on its row, clipping, opening on the last page, maximize and restore, no scrollbar when every page fits (TRG and ImGui with synthetic input, no window)'; Block = { Invoke-LauncherSidebar } },
    @{ Name = 'bug-report-feedback'; Title = 'Bug Report feedback: idle, sending, uploading the crash dump, sent, sent with the dump failed and failed states, Send refused while busy, Report ID kept on success, dump retry only after a dump failure and never filing the report again, invalid dump keeps a plain success, rate limit, unreachable and service errors named without paths, credentials or HTTP details, draft kept after a failure (pure state and source wiring, no network)'; Block = { Invoke-BugReportFeedback } },
    @{ Name = 'command-snapshot'; Title = 'GPU command snapshot: owned copy at submission, isolation from guest rewrites, ordering, size bounds and overflow, lifetime until the last consumer, release, shutdown drain (synthetic command buffers, no GPU)'; Block = { Invoke-CommandSnapshot } },
    @{ Name = 'gpu-recording'; Title = 'Vulkan recording and synchronization: replay rendering scope, staging size, alignment and reuse rules, recording chunk bounds, bounded device fault report, device lost reporting paths (no Vulkan device)'; Block = { Invoke-GpuRecording } },
    @{ Name = 'device-lost'; Title = 'Vulkan device lost: first fault owns the incident, repeated and competing losses, refused submissions and sparse binds, waits that end, GPU worker exit, stop token and shutdown, optional bounded device fault query, deterministic reason, opt-in simulation (fake callbacks, no Vulkan device)'; Block = { Invoke-DeviceLost } },
    @{ Name = 'gpu-crash-timeline'; Title = 'GPU crash timeline: bounded lock-free ring of recent GPU operations (order, wrap, payloads, timelines, concurrent producers and snapshots, no heap), deterministic text, one dump by the first device-lost owner, repeated and competing losses, unchanged reason (fake events, no Vulkan device)'; Block = { Invoke-GpuCrashTimeline } },
    @{ Name = 'gpu-memory-fallback'; Title = 'GPU memory fallback: preferred, other device-local, then system memory types for arena backing, only out of device memory moves on, valid types once each, telemetry, device lost hand-off (fake memory types, no real allocation)'; Block = { Invoke-GpuMemoryFallback } },
    @{ Name = 'swapchain-state'; Title = 'Swapchain state: acquire and present results classified (success, suboptimal, out of date, surface lost, failure), one recreation for a persistently suboptimal surface, a clean frame or a resize, VSync, HDR or display change allows another, out of date and a lost surface never held back, zero-size and minimized windows never recreate, same extent keeps the swapchain, shutdown, counters, reason text, no impossible state (pure state machine, no Vulkan, no window)'; Block = { Invoke-SwapchainState } },
    @{ Name = 'frame-pacing'; Title = 'Frame pacing and high frame rate: deadline limiter (first, on-time, early, late, slightly missed, large stall re-anchors with no catch-up burst, clock jumps and a clock going backwards, minimize and resize stalls), exact period with no drift over hours and days, sleep decision with no busy wait, 60 and 90 FPS and every rate''s cadence against VSync and the display, invalid rates fall back, gameplay timing not multiplied, present mode choice, log gate (synthetic timestamps, no sleeping)'; Block = { Invoke-FramePacing } },
    @{ Name = 'presentation-hardening'; Title = 'Presentation hardening: acquire, present and swapchain creation lost-device results go to the one global owner, no recreation, retry or second owner after the loss, suboptimal and out of date are not a loss, failing presentation operation in the GPU crash timeline, ordinary presentations sampled so they do not flood the ring, swapchain, presenter, window and present thread wiring (fake results, no Vulkan device, no window)'; Block = { Invoke-PresentationHardening } },
    @{ Name = 'nvidia-hardening'; Title = 'Shader interface hardening: one clip distance rule for vertex and fragment stages, pipelines without a pixel shader, retired shader binaries (pure rules and source wiring, no Vulkan, no compilation)'; Block = { Invoke-NvidiaHardening } },
    @{ Name = 'guest-patches'; Title = 'Guest patches: the offline Social Features policy patches and their byte checks (synthetic, no game)'; Tier = 'full'; Block = { Invoke-FocusedTest 'guest_patches_test' '^guest patches:' } },
    @{ Name = 'import-slots'; Title = 'Import slots: the import trap ABI probe, its classification and linked data in the executable segment (synthetic, no game)'; Tier = 'full'; Block = { Invoke-FocusedTest 'import_slots_test' } },
    @{ Name = 'runtime-services'; Title = 'Runtime services: every added import resolves, IME and the offline service contracts through the real runtime sources (no game, no window)'; Tier = 'full'; Block = { Invoke-FocusedTest 'runtime_services_test' } },
    @{ Name = 'ngs2'; Title = 'Ngs2: export resolution, system lifecycle and arguments (no audio device)'; Tier = 'full'; Block = { Invoke-FocusedTest 'ngs2_test' } },
    @{ Name = 'ajm'; Title = 'Ajm: ATRAC9 contexts, instances and batches through their exports (no audio device)'; Tier = 'full'; Block = { Invoke-FocusedTest 'ajm_test' '^ajm:' } },
    @{ Name = 'runtime-log'; Title = 'Runtime log: writing with closed standard streams (no window)'; Tier = 'full'; Block = { Invoke-FocusedTest 'log_test' } },
    @{ Name = 'avplayer-threads'; Title = 'AvPlayer threads: workers join themselves and the demuxer joins the decoders without hanging (no movie, no audio)'; Tier = 'full'; Block = { Invoke-FocusedTest 'avplayer_thread_test' } },
    @{ Name = 'image-descriptor'; Title = 'GPU image descriptors: resource decoding (no GPU)'; Tier = 'full'; Block = { Invoke-FocusedTest 'image_descriptor_test' } },
    @{ Name = 'hdr-format'; Title = 'HDR formats: format remap, component swap, round trip and overflow (no GPU)'; Tier = 'full'; Block = { Invoke-FocusedTest 'hdr_format_test' } },
    @{ Name = 'blend-min-max'; Title = 'Vulkan blend min/max rules (no GPU)'; Tier = 'full'; Block = { Invoke-FocusedTest 'blend_min_max_test' } },
    @{ Name = 'identity-lut'; Title = 'SPIR-V identity lookup table (no GPU, no compilation)'; Tier = 'full'; Block = { Invoke-FocusedTest 'identity_lut_test' } },
    @{ Name = 'window-fit'; Title = 'Windowed-mode fit: fits kept, oversized windows shrunk by one factor, centring (pure geometry, no window)'; Tier = 'full'; Block = { Invoke-FocusedTest 'sotb-window-tests' } },
    @{ Name = 'title-bar'; Title = 'Window title bar text (no window)'; Tier = 'full'; Block = { Invoke-FocusedTest 'sotb-title-bar-tests' } },
    @{ Name = 'trophies'; Title = 'Trophies: error codes, trophy data parsing, the store and the view (synthetic files, no window)'; Tier = 'full'; Block = { Invoke-FocusedTest 'sotb-trophy-tests' } },
    @{ Name = 'launcher-ui'; Title = 'Launcher UI: the real Gameplay rows through TRG''s Launcher with ImGui and synthetic mouse events (no window, no renderer)'; Tier = 'full'; Block = { Invoke-FocusedTest 'sotb-launcher-ui-tests' } },
    @{ Name = 'ffmpeg-bootstrap'; Title = 'FFmpeg bootstrap: the pinned prefix and the build script checks as tools\build.ps1 drives them, and the Linux dependency policy (nasm and the other FFmpeg build tools required only when the pinned FFmpeg must be built, the setup script named when they are missing; nothing built, downloaded or installed)'; Tier = 'full'; Block = { Invoke-FFmpegBootstrap } },
    @{ Name = 'linux-tests'; Title = 'Linux program and headless suites (WSL: tools/test-linux.sh builds shadow_of_the_beast for Linux x86_64 with Clang in build/cmake/linux, checks its ELF header, then builds and runs every headless test target of the Linux build; every announced suite must report; no game, no window, no audio)'; Tier = 'full'; Parity = $true; Platform = 'linux'; Block = { Invoke-LinuxTests } },
    @{ Name = 'linux-parity'; Title = 'Linux supported-build parity and feature contract (WSL: the same sotb-build-parity-tests built from the Linux configuration of the production program and run on the same tables and boot images; no game, no window)'; Tier = 'full'; Parity = $true; Platform = 'linux'; Block = { Invoke-LinuxParity } },
    @{ Name = 'platform-parity'; Title = 'Windows/Linux parity: the two contracts compared (version, supported builds, public features, per-build feature results); PASS only when the Linux program built, its suites passed and both contracts ran, passed and match'; Tier = 'full'; Parity = $true; Platform = 'windows+linux'; Needs = @('build-parity', 'linux-tests', 'linux-parity'); Block = { Invoke-PlatformParity } },
    @{ Name = 'progress-reporter'; Title = 'Test progress: percentage, bar, durations, ETA, check counts, previous group, timing summary, redirected output and a synthetic long group (synthetic time, no sleeping)'; Block = { Invoke-ProgressReporter } }
)

function Get-Tier($Step) { if ($Step['Tier']) { $Step['Tier'] } else { 'default' } }
function Get-Platform($Step) { if ($Step['Platform']) { $Step['Platform'] } else { 'windows' } }

# The groups to run, in registry order: the filter's matches (from the whole registry) or the tier, plus what they need.
function Select-Steps($All) {
    $chosen = New-Object System.Collections.Generic.HashSet[string]
    if ($Filter) {
        $needle = ($Filter -replace '_', '-').ToLowerInvariant()
        foreach ($step in $All) { if ((($step.Name -replace '_', '-').ToLowerInvariant()) -like "*$needle*") { $null = $chosen.Add($step.Name) } }
    } else {
        foreach ($step in $All) { if ($Full -or (Get-Tier $step) -ne 'full') { $null = $chosen.Add($step.Name) } }
    }
    $pending = New-Object System.Collections.Generic.Queue[string]
    foreach ($name in $chosen) { $pending.Enqueue($name) }
    while ($pending.Count) {
        $name = $pending.Dequeue()
        $step = $All | Where-Object { $_.Name -eq $name } | Select-Object -First 1
        foreach ($need in @($step['Needs'] | Where-Object { $_ })) { if ($chosen.Add($need)) { $pending.Enqueue($need) } }
    }
    return @($All | Where-Object { $chosen.Contains($_.Name) })
}

$unknownNeeds = @($steps | ForEach-Object { $_['Needs'] } | Where-Object { $_ -and -not ($steps.Name -contains $_) })
if ($unknownNeeds.Count) { Stop-Tool "the registry needs unknown groups: $($unknownNeeds -join ', ')" }
$registered = $steps
$steps = @(Select-Steps $registered)
if ($steps.Count -eq 0) {
    Write-Host "No suite matches '$Filter'."
    Write-Host 'Run tools\test.ps1 -List to see the suite names.'
    exit 2
}
if ($List) {
    Write-Output ('{0,-28} {1,-14} {2,-8} {3,-14} {4}' -f 'GROUP', 'RUNS', 'GATE', 'PLATFORM', 'DESCRIPTION')
    $steps = @(if ($Filter) { $steps } else { $registered })
    foreach ($step in $steps) {
        $runs = if ((Get-Tier $step) -eq 'full') { '-Full only' } else { 'default, -Full' }
        $gate = if ($step['Parity']) { 'parity' } else { 'focused' }
        $title = if ($step.Title.Length -gt 110) { $step.Title.Substring(0, 107) + '...' } else { $step.Title }
        if ($step['Needs']) { $title += " [needs $($step['Needs'] -join ', ')]" }
        Write-Output ('{0,-28} {1,-14} {2,-8} {3,-14} {4}' -f $step.Name, $runs, $gate, (Get-Platform $step), $title)
    }
    Write-Output ''
    Write-Output ('{0} of {1} registered groups. tools\test.ps1 -Full runs all of them, Windows and Linux.' -f $steps.Count, $registered.Count)
    exit 0
}
$mode = if ($Filter) { "filter $Filter" } elseif ($Full) { 'full matrix' } else { 'default (focused Windows groups; -Full adds the full-only groups and Linux)' }
Write-Host ("Mode: {0}; {1} of {2} groups{3}" -f $mode, $steps.Count, $registered.Count, $(if ($SkipLinux) { '; Linux skipped' } else { '' }))

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) { Stop-Tool 'Run tools\build.ps1 once first: the tests reuse its build folder.' }
Remove-Item -LiteralPath $requestFile -ErrorAction SilentlyContinue
$dumpContractFiles | ForEach-Object { Remove-Item -LiteralPath $_ -ErrorAction SilentlyContinue }

$script:Clock = [System.Diagnostics.Stopwatch]::StartNew()
$interactive = -not [Console]::IsOutputRedirected -and $Host.Name -eq 'ConsoleHost'
$script:Progress = New-ProgressReporter @($steps | ForEach-Object { $_.Name }) 0 $interactive {
    param([string]$Text, [bool]$NoNewline)
    if ($NoNewline) { Write-Host $Text -NoNewline } else { Write-Host $Text }
}

function Format-GroupLine($Step, $Result, $Checks) {
    $fields = @('GROUP', $Step.Name, $Result.State)
    if ($null -ne $Checks) { $fields += "checks=$Checks" }
    if ($Step['Parity']) { $fields += 'parity' }
    if ($Result.Reason) { $fields += ("$($Result.Reason)" -replace '\|', '; ') }
    $fields -join '|'
}

function Get-StepState([string]$Name) {
    if ($script:States.Contains($Name)) { return $script:States[$Name].State }
    if ($registered | Where-Object { $_.Name -eq $Name -and (Get-Tier $_) -eq 'full' }) { return 'NOT RUN (-Full)' }
    return 'NOT RUN'
}

# A build passes on a platform when that platform's contract passed it and ships every public feature.
function Get-BuildCell($Lines, [string]$Title, [string]$Version, [string]$Missing) {
    if (-not $Lines) { return $Missing }
    $row = @(Get-ContractRows $Lines 'BUILD_PARITY' | Where-Object { $_[1] -eq $Title -and $_[2] -eq $Version }) | Select-Object -First 1
    if (-not $row) { return 'MISSING' }
    $features = Get-ResultLine $Lines 'platform-features'
    if ($row[4] -eq 'PASS' -and (-not $features -or $features.State -ne 'PASS')) { return 'FAIL*' }
    return $row[4]
}

function Get-FeatureParityCell([string]$Title, [string]$Version) {
    $windows = $script:Contracts['windows']
    $rows = @(Get-ContractRows $windows 'BUILD_FEATURE' | Where-Object { $_[1] -eq $Title -and $_[2] -eq $Version })
    if (-not $rows.Count) { return 'MISSING' }
    $failing = @($rows | Where-Object { $_[4] -eq 'FAIL' } | ForEach-Object { $_[3] })
    if ($failing.Count) { return "FAIL ($($failing -join ', '))" }
    $linux = $script:Contracts['linux']
    if ($linux) {
        $mine = @($rows | ForEach-Object { $_ -join '|' })
        $theirs = @(Get-ContractRows $linux 'BUILD_FEATURE' | Where-Object { $_[1] -eq $Title -and $_[2] -eq $Version } | ForEach-Object { $_ -join '|' })
        if ((($mine | Sort-Object) -join "`n") -ne (($theirs | Sort-Object) -join "`n")) { return 'FAIL (Windows and Linux differ)' }
    }
    if ($rows | Where-Object { $_[4] -eq 'BLOCKED' }) { return 'BLOCKED (no real evidence)' }
    return "PASS ($($rows.Count) features)"
}

function Write-Cell([string]$Text, [string]$Value) {
    $color = if ($Value -like 'PASS*') { 'Green' } elseif ($Value -like 'FAIL*' -or $Value -eq 'MISSING') { 'Red' } else { 'Yellow' }
    Write-Host $Text -ForegroundColor $color
}

function Write-ParityMatrix {
    $names = @($steps | ForEach-Object { $_.Name })
    if (-not ($names | Where-Object { $_ -in 'version', 'build-parity', 'linux-tests', 'linux-parity', 'platform-parity' })) { return }
    Write-Host ''
    $linuxMissing = Get-StepState 'linux-parity'
    if ($names -contains 'build-parity') {
        Write-Host 'Supported builds:'
        Write-Host ('  {0,-24} {1,-16} {2,-16} {3}' -f 'Build', 'Windows', 'Linux', 'Feature parity')
        $rows = @(Get-ContractRows $script:Contracts['windows'] 'SUPPORTED_BUILD')
        if (-not $rows.Count) { Write-Cell '  no SUPPORTED_BUILD row: the Windows parity contract did not report' 'FAIL' }
        foreach ($row in $rows) {
            $windowsCell = Get-BuildCell $script:Contracts['windows'] $row[1] $row[2] 'NOT RUN'
            $linuxCell = Get-BuildCell $script:Contracts['linux'] $row[1] $row[2] $linuxMissing
            $parityCell = Get-FeatureParityCell $row[1] $row[2]
            $worst = @($windowsCell, $linuxCell, $parityCell | Where-Object { $_ -like 'FAIL*' -or $_ -eq 'MISSING' }).Count
            Write-Cell ('  {0,-24} {1,-16} {2,-16} {3}' -f "$($row[1]) $($row[2]) $($row[3])", $windowsCell, $linuxCell, $parityCell) $(if ($worst) { 'FAIL' } elseif ("$windowsCell$linuxCell$parityCell" -match 'BLOCKED|NOT RUN|SKIPPED') { 'BLOCKED' } else { 'PASS' })
        }
        foreach ($line in @($script:Contracts['windows'] | Where-Object { "$_" -match '^FEATURE_PARITY\|[^|]+\|(FAIL|BLOCKED)' })) { Write-Cell "  $line" 'FAIL' }
        if ("$(@($script:Contracts['windows']) + @($script:Contracts['linux']))" -match 'GROUP\|platform-features\|FAIL') {
            Write-Host '  * the build passes on that platform''s tables, but the platform does not ship every public feature' -ForegroundColor Yellow
        }
        Write-Host ''
    }
    Write-Host 'Platform parity:'
    $platform = if ($script:States.Contains('platform-parity')) {
        $result = $script:States['platform-parity']
        "$($result.State)$(if ($result.Reason) { " ($($result.Reason))" })"
    } elseif ($SkipLinux) { 'SKIPPED (-SkipLinux)' } else { Get-StepState 'platform-parity' }
    Write-Cell ('  {0,-24} {1}' -f 'Windows vs Linux', $platform) $platform
    Write-Host ''
    Write-Host 'Version:'
    $canonical = Get-CanonicalVersion
    $versionState = Get-StepState 'version'
    $runtime = @()
    foreach ($platformName in 'windows', 'linux') {
        $line = @($script:Contracts[$platformName] | Where-Object { "$_".StartsWith('VERSION|') }) | Select-Object -First 1
        if ($line) {
            $value = "$line".Split('|')[1]
            $runtime += "$platformName $value"
            if ($value -ne $canonical) { $versionState = 'FAIL' }
        }
    }
    $versionText = "$versionState$(if ($runtime.Count) { " (contract: $($runtime -join ', '))" })"
    if ($versionState -eq 'FAIL' -and $script:States.Contains('version') -and $script:States['version'].State -eq 'PASS') { $script:failed++ }
    Write-Cell ('  {0,-24} {1}' -f $canonical, $versionText) $versionText
    $blockers = @($steps | Where-Object { $_['Parity'] -and $script:States.Contains($_.Name) -and $script:States[$_.Name].State -in 'FAIL', 'BLOCKED' } | ForEach-Object { "$($_.Name) ($($script:States[$_.Name].State))" })
    Write-Host ''
    Write-Cell "Parity blockers: $(if ($blockers.Count) { $blockers -join ', ' } else { 'none' })" $(if ($blockers.Count) { 'FAIL' } else { 'PASS' })
}

$script:States = [ordered]@{}
$failed = 0
$finishedRun = $false
try {
    foreach ($step in $steps) {
        Start-ProgressGroup $script:Progress $step.Name $script:Clock.Elapsed.TotalSeconds
        Write-Step $step.Title
        try { $result = @(& $step.Block) | Where-Object { $_ -and $_.PSObject.Properties['State'] } | Select-Object -Last 1 }
        catch { $result = Result 'FAIL' "the group stopped: $($_.Exception.Message)" }
        if (-not $result) { $result = Result 'FAIL' 'the group ended without a result' }
        Clear-ProgressLine $script:Progress
        switch ($result.State) {
            'PASS' { Write-Host '    PASS' -ForegroundColor Green }
            'FAIL' { $failed++; Write-Host "    FAIL: $($result.Reason)" -ForegroundColor Red }
            'BLOCKED' { if ($Full) { $failed++ }; Write-Host "    BLOCKED: $($result.Reason)" -ForegroundColor Yellow }
            default { Write-Host "    SKIPPED: $($result.Reason)" -ForegroundColor Yellow }
        }
        $script:States[$step.Name] = $result
        Write-Host (Format-GroupLine $step $result $script:Progress.Checks)
        Complete-ProgressGroup $script:Progress $result.State $script:Clock.Elapsed.TotalSeconds
    }
    $finishedRun = $true
} finally {
    Clear-ProgressLine $script:Progress
    Write-Host ''
    Format-TimingSummary $script:Progress $script:Clock.Elapsed.TotalSeconds | ForEach-Object { Write-Host $_ }
    if (-not $finishedRun) { Write-Host 'Run interrupted before the last group finished.' -ForegroundColor Yellow }
}
foreach ($step in @($steps | Where-Object { -not $script:States.Contains($_.Name) })) {
    $failed++
    Write-Host (Format-GroupLine $step (Result 'FAIL' 'no result: the run stopped before this group') $null) -ForegroundColor Red
}
Write-ParityMatrix
$counts = @('PASS', 'FAIL', 'SKIPPED', 'BLOCKED' | ForEach-Object { $state = $_; '{0} {1}' -f @($script:States.Values | Where-Object { $_.State -eq $state }).Count, $state })
Write-Host ''
Write-Host "Groups: $($counts -join ', ')$(if (-not $Full -and -not $Filter) { '; tools\test.ps1 -Full runs the complete Windows and Linux matrix' })"
foreach ($name in @($script:States.Keys | Where-Object { $script:States[$_].State -in 'SKIPPED', 'BLOCKED' })) {
    Write-Host "  $($script:States[$name].State): $name ($($script:States[$name].Reason))" -ForegroundColor Yellow
}
Write-Host ''
Write-Host "Failed: $failed"
if ($failed) { exit 1 }
exit 0
