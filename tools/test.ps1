<#
.SYNOPSIS
  Builds and runs the focused headless Bug Report, crash dump correlation and upload, device-lost reporting, startup diagnostics, updater, crash report, guest memory, CPU compatibility, Vulkan preflight, buffer arena, build identity, pipeline cache, pipeline warm-up, shader cache, audio backlog recovery, controller rebinding, controller prompt, in-game controller prompt assets, launcher placement, launcher startup, launcher UI stall, launcher Escape navigation, Bug Report feedback, GPU command snapshot, Vulkan recording, Vulkan device lost, GPU crash timeline, GPU memory fallback, swapchain state, frame pacing, presentation hardening, shader interface hardening, loading-screen hint layout and resolution guard tests, plus the runtime thread cancellation and save mount diagnostics tests: no window, no game launch, no audio, no Internet, nothing deployed.
.DESCRIPTION
  Needs the build configured once by tools\build.ps1. Only the test target is built, incrementally in the existing build
  folder; the game executable, the launcher and the video core are not rebuilt. Every step runs even after one fails; the
  exit code is non-zero when any failed.

  While a step runs, a progress block (percentage, bar, completed groups, elapsed time, ETA, check count when the suite
  reports one, previous group) is printed at each step start and refreshed on a timer: one carriage-return line with a | / - \
  spinner every 500 ms in a terminal, a RUNNING text line every 15 s when output is redirected. A timing table and the slowest groups follow the last step.

  Steps: the launcher's Bug Report test (builds the request the Worker must accept), the crash dump correlation test, the
  crash dump upload test against a stand-in service (builds the dump request and chunks the Worker must accept), the
  device-lost reporting test, the startup diagnostics test, the Worker's tests in Node on a
  disposable in-memory database, tools\worker.ps1 against a stand-in npx, the updater tests (rules, loopback server, helper in a temporary folder), the module history test of the crash report, the guest memory test, the CPU compatibility test, the Vulkan preflight test, the buffer arena (sparse or fully resident) test, the build identity (unsupported build diagnostics) test, the pipeline cache test, the pipeline warm-up test, the shader cache test, the audio backlog recovery test, the controller rebinding test, the controller prompt test, the launcher placement, startup, UI stall and Escape navigation tests, the Bug Report feedback test, the GPU command snapshot test, the Vulkan recording and synchronization test, the Vulkan device lost test, the GPU crash timeline test, the runtime thread cancellation test, the save mount diagnostics test, the GPU memory fallback test, the swapchain state test, the frame pacing and high frame rate test, the presentation hardening test, the shader interface hardening test, the loading-screen hint layout test and the resolution guard test.
.PARAMETER TimeoutSeconds
  Fails a step that runs longer than this.
.PARAMETER List
  Lists the suites by name and exits; nothing is built or run.
.PARAMETER Filter
  Runs only the suites whose name contains this text (case-insensitive; _ and - are equivalent).
.PARAMETER WindowsOnly
  Every suite here is Windows headless; accepted for the shared test convention.
.EXAMPLE
  .\tools\test.ps1
.EXAMPLE
  .\tools\test.ps1 -List
.EXAMPLE
  .\tools\test.ps1 -Filter resolution_guard_tests
#>
[CmdletBinding()]
param(
    [int]$TimeoutSeconds = 300,
    [switch]$List,
    [string]$Filter = '',
    [switch]$WindowsOnly
)

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

function Invoke-FocusedTest([string]$target, [string]$show = '') {
    $build = Invoke-Native (Get-Command ninja).Source @('-C', $buildDir, $target)
    if ($build.Code -ne 0) {
        $build.Lines | Select-Object -Last 12 | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "$target did not build; the old executable was not run"
    }
    $bin = Join-Path $buildDir 'bin'
    $run = Invoke-Native (Join-Path $bin "$target.exe") @() $bin
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

function Invoke-WorkerScript {
    $shell = (Get-Process -Id $PID).Path
    $run = Invoke-Native $shell @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tests\worker_script_tests.ps1'))
    if ($run.Code -ne 0) {
        $run.Lines | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "exit code $($run.Code)"
    }
    Show-Last $run
    return Result 'PASS'
}

function Invoke-ProgressReporter {
    $shell = (Get-Process -Id $PID).Path
    $run = Invoke-Native $shell @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tests\progress_reporter_tests.ps1'))
    if ($run.Code -ne 0) {
        $run.Lines | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' "exit code $($run.Code)"
    }
    Show-Last $run
    return Result 'PASS'
}

$steps = @(
    @{ Name = 'bug-report-client'; Title = 'Launcher: Bug Report client (no window, no network: stand-in transport, synthetic logs)'; Block = { Invoke-LauncherBugReport } },
    @{ Name = 'crash-dump-correlation'; Title = 'Crash dump correlation: dump numbers stay taken, exact session name and folder, named by its own log, written with it, size, MDMP signature, directories and links refused, local retention of the newest dumps, crash, device lost and normal sessions told apart, no local path sent (synthetic logs and dumps, no real crash)'; Block = { Invoke-CrashDumpCorrelation } },
    @{ Name = 'crash-dump-upload'; Title = 'Crash dump upload: Crash detected and the optional attachment, report with and without a dump, chunks resumed from what the service holds, lost answers and already-stored chunks, refusals stop, failures keep the report, retry under the same Report ID, repeated send with the submission key files one report, no local path sent (stand-in service, no network)'; Block = { Invoke-CrashDumpUpload } },
    @{ Name = 'device-lost-reporting'; Title = 'Device-lost reporting: the device-lost owner''s own lines classify the session (exit code 25 path), reason with operation, result, stage, GPU, driver and bounded fault text, timeline left in the log, missing fault data, first fault owns the session, last startup stage kept, with and without a dump (no Vulkan device)'; Block = { Invoke-DeviceLostReporting } },
    @{ Name = 'startup-diagnostics'; Title = 'Startup diagnostics: one current stage printed once per change, GPU initialization and first frame presented stages, no per-frame growth, empty stages ignored, loader stage order unchanged, the stage a failure happened in reaches the report, module history keeps the stage (no game, no window)'; Block = { Invoke-StartupDiagnostics } },
    @{ Name = 'resolution-guard-tests'; Title = 'Resolution guard: a high-resolution startup crash restores 1440p, a gameplay crash, clean run, pre-patch failure and unknown outcome keep the setting, 1800p and 4K arm the marker, marker arm/read/clear (the aspect ratio kept beside the resolution, older marker files read as 16:9), the gameplay milestone clears it, 1440p, 1800p and 4K stay selectable (pure policy and marker files, no game, no window)'; Block = { Invoke-ResolutionGuard } },
    @{ Name = 'aspect-ratio'; Title = 'Aspect Ratio: the six shapes (16:9 Original, 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3, 5:4) and the aspect_ratio setting (invalid values and a missing key are 16:9, reset, persistence, settings summary), Window size choices at 720, 900, 1080, 1440 and 2160 following the shape, the render target of every Render resolution in every shape (height kept, width aligned to 8, 3840x2160 pixel budget reached by losing height, 16:9 unchanged, 854x480 included), the six display size sites identical and patched on the real CUSA03762, CUSA02871 and CUSA00951 images with nothing else written, altered sites refused, no overlap with the other guest patches, the Resolution Guard and its 1440p-equivalent fallback judged on the real target, the loading hint independent of the shape, and the presenter, SMAA and FSR taking the game''s own image size (real images read only and skipped when absent; no game, no window)'; Block = { Invoke-AspectRatio } },
    @{ Name = 'bug-report-service'; Title = 'Launcher: Bug Report service (Node, disposable database): submission, receipts, limits, retention, admin, dashboard, crash dump announcement, resumable chunked upload, completion checks, minidump download and retention, submission key, and the launcher''s own report and dump requests'; Block = { Invoke-WorkerService } },
    @{ Name = 'worker-script'; Title = 'Launcher: worker.ps1 stand-in npx: placeholders, missing files, dry run, failures; nothing deployed'; Block = { Invoke-WorkerScript } },
    @{ Name = 'updater-rules'; Title = 'Updater: versions, release JSON, SHA-256, ZIP safety, staging, manifest, protected data, install, rollback and release contract (no network)'; Block = { Invoke-UpdateRules } },
    @{ Name = 'updater-network'; Title = 'Updater: loopback HTTP server only (no release yet, 5xx, dropped connection, stall, timeout, redirects, retries, production address, transient status report, cross-origin header stripping, log redaction, destination preserved on failure and cancel, bounded automatic retries of transient failures, cancellation, manual Retry); never contacts GitHub'; Block = { Invoke-UpdateNetwork } },
    @{ Name = 'updater-helper'; Title = 'Updater: trg-updater.exe run headless from a temporary folder (parent wait, install, restart marker, environment cleanup, protected data)'; Block = { Invoke-UpdateHelper } },
    @{ Name = 'module-history'; Title = 'Crash report: module history, call sites, fault ownership and startup phase (no window, no game)'; Block = { Invoke-ModuleHistory } },
    @{ Name = 'guest-memory'; Title = 'Runtime: guest memory, and file reads into it while a map splits the view (synthetic file, no game)'; Block = { Invoke-GuestMemory } },
    @{ Name = 'runtime-thread-cancel'; Title = 'Runtime threads: create, join, detach and exit, scePthreadCancel as a recorded request (invalid and finished handles, running threads left to finish, cancelled sleepers, condition waits and joiners, joins and detaches racing a cancel, cancel storms, no host cancellation, timed wait or polling), mutexes and condition variables (no game, no window)'; Block = { Invoke-RuntimeThreadCancel } },
    @{ Name = 'save-mount-diagnostics'; Title = 'Save data: mount, search, delete and save memory through the real save and file layers, and one bounded failure line for Mount, Mount2 and TransferringMount (call, directory, error, mode, no host path, hostile names) in a synthetic save root; the real user save is read only and skipped when absent'; Block = { Invoke-SaveMountDiagnostics } },
    @{ Name = 'cpu-compatibility'; Title = 'CPU compatibility: MOVBE, BMI1 and LZCNT (host classification, MOVBE and BMI1 emulation, LZCNT rewrite run on synthetic forms and applied to every site; the real boot image is read only and skipped when absent)'; Block = { Invoke-CpuCompatibility } },
    @{ Name = 'renderer-selection'; Title = 'Renderer selection: the Renderer setting, Auto fallback and the sources that must agree (no window, no context)'; Block = { Invoke-RendererSelection } },
    @{ Name = 'loading-hint-layout-tests'; Title = 'Loading screen hint layout: 1920x1080 identity, 1440p 1.333x, 4K 2x, centered bar, proportional placement, ultrawide kept, vtable patch fingerprint validation (pure geometry, no window)'; Block = { Invoke-LoadingHintLayout } },
    @{ Name = 'vulkan-preflight'; Title = 'Vulkan preflight: loader, runtime, device, mandatory extensions and features, optional capabilities, explicit Vulkan and Auto fallback (synthetic capabilities, no Vulkan, no window)'; Block = { Invoke-VulkanPreflight } },
    @{ Name = 'buffer-arena'; Title = 'GPU memory: sparse or fully resident arena decision, layout, overflow, allocation failure rollback and migration (fake arenas, no Vulkan device)'; Block = { Invoke-BufferArena } },
    @{ Name = 'build-identity'; Title = 'Unsupported build diagnostics: supported identity, altered fields, candidates, deterministic report, refusal before guest code (synthetic; the real boot image is read only and skipped when absent)'; Block = { Invoke-BuildIdentity } },
    @{ Name = 'pipeline-cache'; Title = 'Pipeline cache: sealed blobs, host identity (GPU, driver, pipeline cache UUID, format), corruption and truncation, stale temporaries, atomic writes, failure keeps the previous cache, unrelated files kept (temporary folders, no Vulkan, no real cache)'; Block = { Invoke-PipelineCache } },
    @{ Name = 'pipeline-warmup'; Title = 'Pipeline warm-up: startup choice with an on-demand override, bounded worker pool and queue, no duplicate compilation against the game''s own pipelines, failures counted once and never retried, shutdown drain and refusal of new work (fake jobs and a fake renderer, no Vulkan, no window)'; Block = { Invoke-PipelineWarmup } },
    @{ Name = 'shader-cache'; Title = 'Shader cache: background loading note hidden below 500 ms and gone when the load ends, deterministic cache identity, relevant changes invalidate and unrelated ones never enter the key, cleared only on a mismatch, persistent writable cache folder per build, sealed blobs reused, truncated and corrupt ones rejected (synthetic data in temporary folders, no Vulkan, no window, no real cache)'; Block = { Invoke-ShaderCache } },
    @{ Name = 'audio-backlog'; Title = 'Audio backlog recovery: format-aware queue duration, 100 ms threshold and boundary, invalid formats and overflow, steady playback never recovers, device stall and endpoint reset recover, repeated recoveries, log rate limit (pure queue model, no audio device)'; Block = { Invoke-AudioBacklog } },
    @{ Name = 'pad-binding'; Title = 'Controller rebinding: default layout, every action and source, triggers, shoulders, stick clicks, Options and touchpad, persistence and reset in the settings file, invalid and older configs, duplicate policy, capture rules, hotplug (synthetic gamepad samples, no controller, no window)'; Block = { Invoke-PadBinding } },
    @{ Name = 'pad-prompt'; Title = 'Controller prompts: Xbox, DualShock, DualSense, generic and Steam Input detection, Auto/Xbox/PlayStation policy, active controller tracking without flapping, persisted mode, labels, regional Confirm/Back, prompts never change the guest buttons (synthetic metadata, no controller)'; Block = { Invoke-PadPrompt } },
    @{ Name = 'controller-prompts'; Title = 'In-game controller prompts: Auto, Xbox and PlayStation style chosen once at startup (Auto follows the first controller, anything else gives Xbox, explicit modes ignore the controller), invalid setting is Auto, PlayStation installs nothing, Xbox art served over the unchanged pak only after pak size, footer, index and every texture CRC-32 match, all-or-nothing, modified, truncated, wrong-build and damaged-table sources rejected with the original art kept, bounded diagnostics without paths, no per-frame decision, launcher wording, and the runtime file overlay itself (sorted non-overlapping patches, reads across chunks, patch edges, the host end and zeros past it, seek and size, refusals, table limit, host file unchanged; synthetic paks and files, no controller, no window; the installed pak is read only and skipped when absent)'; Block = { Invoke-ControllerPrompts } },
    @{ Name = 'controller-prompts-assets'; Title = 'Xbox prompt assets: generated data tied to the build and pak identity, every texture CRC-32, position, format, mips and tiling against the installed pak, replacement confined to mip payload bytes and to the original footprint, glyph coverage of every prompt the game draws, deterministic regeneration, line art licence and notices, no local paths and no game files in the repository (the installed pak is read only and its checks are skipped when absent)'; Block = { Invoke-ControllerPromptsAssets } },
    @{ Name = 'menu-buttons'; Title = 'Menu Button Layout: the Western default, the saved setting and environment override, multi-build support (CUSA03762 Korea Circle-confirm original, CUSA02871 USA and CUSA00951 Europe Western original), site fingerprint and unique location, Western changing only the six-byte region branch, Original leaving the image untouched, unknown builds and altered, short or duplicated sites written by nothing'; Block = { Invoke-MenuButtons } },
    @{ Name = 'exit-game'; Title = 'Exit Game: Map and Pause allowlist, append guard, input filter, 19-culture localization, confirmation modal Western/Original and Xbox/PlayStation mapping, multi-build (CUSA03762, CUSA02871, CUSA00951) fingerprint patching, fail-closed refusals, and cross-patch non-overlap verification (synthetic, no window)'; Block = { Invoke-ExitGame } },
    @{ Name = 'skip-intro'; Title = 'Skip Intro: on CUSA03762, CUSA02871 and CUSA00951 the loading screen''s own publisher and developer sequence starts finished, so its three boot images (Shadow of the Beast, Sony Computer Entertainment presents, XDEV and Heavy Spectrum) never appear and the same completion path runs on the first frame; the fingerprint is unique, altered, short and missing images are refused with nothing written, the normal loading screen, Press Start, frontend and gamma screen stay untouched, and no frame is hidden and no input injected (real images and synthetic, no game, no window)'; Block = { Invoke-SkipIntro } },
    @{ Name = 'psn-notice'; Title = 'PlayStation Network notice: ConnectionSuccess in SignInPopup and MainMenuScreen is redirected before the locres lookup to an unlocalized key whose text is a single space on CUSA03762, CUSA02871 and CUSA00951, every other key and namespace keeps its lookup, the old literal patches are gone, the NP state and trophy wiring stay intact, and altered or short images and caves are refused (no game, no window)'; Block = { Invoke-PsnNotice } },
    @{ Name = 'launcher-tests'; Title = 'Launcher core: Play page cleanup, About page layout, multi-build SupportedBuildsText, settings persistence, Menu & Subtitles, Skip Intro, Menu Button Layout, render resolution patches, keys and icons (no window)'; Block = { Invoke-LauncherCore } },
    @{ Name = 'gamepad-database'; Title = 'Gamepad database: user/gamecontrollerdb.txt beside the settings file (never a configured path), missing file silent, one bounded summary line, malformed and unreadable files harmless, size cap, loaded once before the first gamepad enumeration, mappings reach later joysticks through SDL, recognized pads unchanged; unmapped joysticks logged once per device identity (name, GUID, VID:PID, no pointer or path), reconnects and hotplug, bounded memory (fake loader and SDL virtual joysticks, no controller, no window)'; Block = { Invoke-GamepadDatabase } },
    @{ Name = 'input-device'; Title = 'Controller activity: button and D-pad onset, stick and trigger thresholds with hysteresis, held input and drift never repeat, rearm after release, independent axes, extreme and zero values, per-controller state with hotplug and disconnect (pure policy, no controller)'; Block = { Invoke-InputDevice } },
    @{ Name = 'cursor-policy'; Title = 'Cursor visibility: last meaningful device (mouse, keyboard, controller), overlay forces the cursor visible, device kept while the overlay is open, overlay close follows the current device, no flapping, launcher and focus rules, window wiring (pure policy, no window, no cursor)'; Block = { Invoke-CursorPolicy } },
    @{ Name = 'launcher-renderer'; Title = 'Launcher renderer policy: Direct3D 11 first on Windows with software as the fallback and d3d11.dll and dxgi.dll pinned for the life of the process (never freed), SOTB_LAUNCHER_RENDERER=software forcing software, other values ignored, Direct3D 11 never asked for elsewhere, each renderer tried once with no fallback loop, a failed pin skips the renderer, complete failure reported once, requested and actual renderer classified and logged once, one creation site outside the frame, teardown order textures, ImGui backends, renderer, window, SDL, launcher closed before the game (pure policy and source wiring, no window, no SDL, no D3D11)'; Block = { Invoke-LauncherRenderer } },
    @{ Name = 'launcher-pacing'; Title = 'Launcher frame cap and minimized behavior: 60 Hz deadline pacing with an exact period (first, early, on-time, slightly late, boundaries, large stall and clock jumps re-anchor with no catch-up burst, no negative wait, three days without drift), high-refresh displays never exceed 60 Hz, independent of the game frame rate and the renderer, sleeps instead of spinning, minimized idles at 50 ms with events and background work kept and no draw, restore draws once and rebases, repeated minimize and restore (synthetic timestamps, no window, no sleeping)'; Block = { Invoke-LauncherPacing } },
    @{ Name = 'launcher-performance'; Title = 'Launcher performance telemetry: render rate, UI-thread CPU, build, submit and present timing in 5 s windows after a warm-up, runaway-loop, present-stall and high-CPU warnings once per incident and at most once a minute, renderer in every line, formatting bounded and sanitized, minimize discards the window, no per-frame logging or CPU reads, deterministic, a seven-day session without overflow (synthetic timestamps, no window)'; Block = { Invoke-LauncherPerformance } },
    @{ Name = 'launcher-placement'; Title = 'Launcher placement: centered on the primary at the display scale, a saved window kept while it fits and its title bar can be reached, moved and shrunk into its display when it sticks out of the top or no longer fits, centered with its size kept after its display was removed, moved or its coordinates went negative, monitors left of and above the primary, primary changed, resolution reduced, usable area smaller than the launcher, DPI and the title bar reserve, maximized and fullscreen kept, one bounded placement line with display, scale and final position, minimized keeps the saved state, chosen once while hidden (synthetic displays, no window)'; Block = { Invoke-LauncherPlacement } },
    @{ Name = 'launcher-startup'; Title = 'Launcher startup: created hidden, placement, renderer, ImGui, fonts and a complete launcher frame before the window shows, shown once and that frame presented again, no visible move or resize, failures and a first-frame Play or Quit never show it, renderer failure path, window created, renderer ready, first frame and running stages once each in order with no per-frame stages, existing stages kept (source wiring and the stage store, no window)'; Block = { Invoke-LauncherStartup } },
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
    @{ Name = 'progress-reporter'; Title = 'Test progress: percentage, bar, durations, ETA, check counts, previous group, timing summary, redirected output and a synthetic long group (synthetic time, no sleeping)'; Block = { Invoke-ProgressReporter } }
)

if ($Filter) {
    $needle = ($Filter -replace '_', '-').ToLowerInvariant()
    $steps = @($steps | Where-Object { (($_.Name -replace '_', '-').ToLowerInvariant()) -like "*$needle*" })
    if ($steps.Count -eq 0) {
        Write-Host "No suite matches '$Filter'."
        Write-Host 'Run tools\test.ps1 -List to see the suite names.'
        exit 2
    }
}
if ($List) {
    foreach ($step in $steps) { Write-Output ('{0}  {1}' -f $step.Name, $step.Title) }
    exit 0
}
if ($Filter) { Write-Host ("Filter: {0} ({1} suite(s))" -f $Filter, $steps.Count) }

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

$failed = 0
$finishedRun = $false
try {
    foreach ($step in $steps) {
        Start-ProgressGroup $script:Progress $step.Name $script:Clock.Elapsed.TotalSeconds
        Write-Step $step.Title
        $result = & $step.Block
        Clear-ProgressLine $script:Progress
        switch ($result.State) {
            'PASS' { Write-Host '    PASS' -ForegroundColor Green }
            'FAIL' { $failed++; Write-Host "    FAIL: $($result.Reason)" -ForegroundColor Red }
            default { Write-Host "    SKIPPED: $($result.Reason)" -ForegroundColor Yellow }
        }
        Complete-ProgressGroup $script:Progress $(if ($result.State -in 'PASS', 'FAIL') { $result.State } else { 'SKIPPED' }) $script:Clock.Elapsed.TotalSeconds
    }
    $finishedRun = $true
} finally {
    Clear-ProgressLine $script:Progress
    Write-Host ''
    Format-TimingSummary $script:Progress $script:Clock.Elapsed.TotalSeconds | ForEach-Object { Write-Host $_ }
    if (-not $finishedRun) { Write-Host 'Run interrupted before the last group finished.' -ForegroundColor Yellow }
}
Write-Host ''
Write-Host "Failed: $failed"
if ($failed) { exit 1 }
exit 0
