<#
.SYNOPSIS
  Builds shadow_of_the_beast.exe and its runtime DLLs (MSYS2 CLANG64, CMake + Ninja). -Run launches it.
.DESCRIPTION
  Reuses what is current and updates only what is stale: the video core dependencies (gpu\deps.txt), the pinned ANGLE
  runtime (gpu\angle-runtime.txt, cached in cache\deps), CMake and, once the game is installed, its boot image. Every DLL
  the program loads ends up beside it. The launcher installs the game from the Play page (Install game...).
.PARAMETER Run
  Launches the launcher that was just built and stays attached until it closes. The build stops immediately if it fails.
.PARAMETER Clean
  Deletes build\cmake first (the installed game, the boot image and the logs are kept).
.PARAMETER CheckImports
  Diagnostic: with -Run, lists the PS4 imports the runtime lacks instead of running the game.
.PARAMETER VerboseRuntime
  With -Run, streams the loader output into the terminal as well as the log.
.PARAMETER TimeoutSeconds
  Diagnostic for automated tests only: with -Run, kills the loader after this many seconds and skips the launcher.
.PARAMETER LoaderArgs
  Extra arguments for shadow_of_the_beast.exe (with -Run).
.EXAMPLE
  .\tools\build.ps1
  .\tools\build.ps1 -Run
#>
[CmdletBinding()]
param(
    [switch]$Run,
    [switch]$Clean,
    [switch]$CheckImports,
    [switch]$VerboseRuntime,
    [int]$TimeoutSeconds = 0,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$LoaderArgs = @()
)

. (Join-Path $PSScriptRoot '_common.ps1')
$version = 'CUSA03762-1.00'
$config = 'RelWithDebInfo'
$root = $script:RepoRoot
$buildDir = Join-Path $root 'build\cmake\clang64'
$loader = Join-Path $buildDir 'bin\shadow_of_the_beast.exe'
$view = Join-Path $root "game\prepared\$version"
$marker = Join-Path $view '.sotb_view.json'
$image = Join-Path $root "build\image\$version"

function Test-ImageCurrent {
    $newest = (Get-Item -LiteralPath $marker).LastWriteTime
    foreach ($source in 'launcher\boot_image.cpp', 'launcher\boot_image.h', 'launcher\game_profile.h') {
        $time = (Get-Item -LiteralPath (Join-Path $root $source)).LastWriteTime
        if ($time -gt $newest) { $newest = $time }
    }
    foreach ($name in 'boot.bin', 'content.bin', 'image.json') {
        $file = Join-Path $image $name
        if (-not (Test-Path -LiteralPath $file) -or (Get-Item -LiteralPath $file).LastWriteTime -lt $newest) { return $false }
    }
    return $true
}

function Get-LogsDirectory {
    $index = [array]::LastIndexOf($LoaderArgs, '--logs')
    if ($index -lt 0 -or $index + 1 -ge $LoaderArgs.Count) { return Join-Path $root 'build\logs' }
    $dir = $LoaderArgs[$index + 1]
    if (-not [System.IO.Path]::IsPathRooted($dir)) { $dir = Join-Path (Split-Path $loader -Parent) $dir }
    return [System.IO.Path]::GetFullPath($dir)
}

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
Write-Step "Toolchain clang64 ($($tc.Bin)), $config"

Sync-GpuDependencies
$angleRuntime = Sync-AngleRuntime $tc.Bin
Write-Step 'FFmpeg (gpu\ffmpeg.txt)'
$ffmpeg = (Sync-FFmpeg).Replace('\', '/')

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Write-Step 'Cleaning build\cmake'
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}
$cache = Join-Path $buildDir 'CMakeCache.txt'
$configured = (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja')) -and (Test-Path -LiteralPath $cache) -and
    (Select-String -Path $cache -SimpleMatch "CMAKE_BUILD_TYPE:STRING=$config" -Quiet)
# Another FFmpeg prefix needs a fresh configuration: pkg-config results stay cached otherwise.
$sameFFmpeg = $configured -and (Select-String -Path $cache -SimpleMatch "SOTB_FFMPEG_PREFIX:PATH=$ffmpeg" -Quiet)
if (-not $configured -or -not $sameFFmpeg) {
    Write-Step 'Configuring'
    $fresh = if ($configured) { @('--fresh') } else { @() }
    & cmake (@('-S', $root, '-B', $buildDir, '-G', 'Ninja') + $fresh + @("-DCMAKE_BUILD_TYPE=$config", '-DCMAKE_C_COMPILER=clang', "-DSOTB_FFMPEG_PREFIX=$ffmpeg"))
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'CMake configuration failed' }
}
Write-Step 'Building'
& ninja -C $buildDir shadow_of_the_beast
if ($LASTEXITCODE -ne 0) { Stop-Tool 'Build failed' }
if (-not (Test-Path -LiteralPath $loader)) { Stop-Tool "Missing $loader" }
Install-GameRuntime $loader $angleRuntime $tc.Bin (Join-Path $ffmpeg 'bin')

if (-not (Test-Path -LiteralPath $marker)) {
    Write-Warn 'The game is not installed yet: use Install game... in the launcher.'
}
elseif (-not (Test-ImageCurrent)) {
    Write-Step 'Updating the boot image'
    & $loader --prepare-image $view $image | Out-Host
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'Boot image build failed' }
}
Write-Host ''
Write-Host "Built $loader" -ForegroundColor Green
if (-not $Run) { return }

$running = @(Get-Process shadow_of_the_beast -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $loader })
if ($running.Count -gt 0) {
    Write-Warn "shadow_of_the_beast.exe is already running (PID $($running.Id -join ', ')); not launching another copy."
    return
}

$arguments = @()
if ($CheckImports) { $arguments += '--check-imports' }
if ($TimeoutSeconds -gt 0) { $arguments += '--sotb-skip-launcher' }
$arguments += $LoaderArgs
$processArguments = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
$quiet = -not ($VerboseRuntime -or $CheckImports -or $TimeoutSeconds -gt 0)
$logsDir = Get-LogsDirectory
Write-Step 'Starting shadow_of_the_beast.exe'
Write-Host "Runtime logs: $logsDir"
if ($quiet) { Write-Host 'Run output goes to the log only; -VerboseRuntime streams it here.' }

Push-Location (Split-Path $loader -Parent)
try {
    if ($TimeoutSeconds -gt 0) {
        $process = Start-Process -FilePath $loader -ArgumentList $processArguments -NoNewWindow -PassThru
        $null = $process.Handle
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            Write-Warn "Stopping shadow_of_the_beast.exe after $TimeoutSeconds seconds"
            $process.Kill()
            $process.WaitForExit()
        }
        $code = $process.ExitCode
    }
    else {
        $start = @{ FilePath = $loader; NoNewWindow = $true; Wait = $true; PassThru = $true }
        if ($quiet) { $start.RedirectStandardOutput = 'NUL'; $start.RedirectStandardError = '\\.\NUL' }
        if ($processArguments.Count -gt 0) { $start.ArgumentList = $processArguments }
        $code = (Start-Process @start).ExitCode
    }
}
finally {
    Pop-Location
}
Write-Host "shadow_of_the_beast.exe exit code $code"
if ($code -ne 0) { Write-Warn "Abnormal exit; see the runtime logs in $logsDir" }
exit $code
