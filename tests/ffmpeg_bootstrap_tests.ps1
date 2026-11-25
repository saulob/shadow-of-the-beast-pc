# Headless checks of the FFmpeg bootstrap as tools\build.ps1 drives it: the shell tests run through MSYS2's login bash,
# exactly as Sync-FFmpeg starts the script, then the Linux dependency policy; the rest reads the build scripts. Nothing is
# built or downloaded.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools\_common.ps1')
$failures = 0
function Check([string]$Name, [bool]$Condition) {
    if ($Condition) { Write-Host "  [ok]   $Name" -ForegroundColor Green } else { Write-Host "  [FAIL] $Name" -ForegroundColor Red; $script:failures++ }
}

foreach ($file in 'tools\_common.ps1', 'tools\build.ps1', 'tools\setup.ps1') {
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root $file), [ref]$null, [ref]$errors)
    Check "$file parses" ($errors.Count -eq 0)
}

$bash = Join-Path (Get-Msys2Root) 'usr\bin\bash.exe'
$saved = @{ MSYSTEM = $env:MSYSTEM; CHERE_INVOKING = $env:CHERE_INVOKING }
$env:MSYSTEM = 'CLANG64'
$env:CHERE_INVOKING = '1'
try {
    $unix = ($root -replace '\\', '/') -replace '^([A-Za-z]):', { '/' + $_.Groups[1].Value.ToLower() }
    & $bash -lc "bash -n '$unix/tools/build-ffmpeg.sh'"
    Check 'build-ffmpeg.sh passes bash -n' ($LASTEXITCODE -eq 0)
    $output = & $bash -lc "bash '$unix/tests/ffmpeg_bootstrap_tests.sh'" 2>&1
    $output | ForEach-Object { Write-Host "  $_" }
    Check 'bootstrap scenarios (tests/ffmpeg_bootstrap_tests.sh)' ($LASTEXITCODE -eq 0)
    foreach ($script in 'tools/build-ffmpeg-linux.sh', 'tools/build.sh', 'tools/test-linux.sh', 'packaging/linux/ci-deps.sh') {
        & $bash -lc "bash -n '$unix/$script'"
        Check "$script passes bash -n" ($LASTEXITCODE -eq 0)
    }
    $output = & $bash -lc "bash '$unix/tests/linux_dependency_tests.sh'" 2>&1
    $output | ForEach-Object { Write-Host "  $_" }
    Check 'Linux dependency policy (tests/linux_dependency_tests.sh)' ($LASTEXITCODE -eq 0)
} finally {
    foreach ($name in $saved.Keys) {
        if ($null -eq $saved[$name]) { Remove-Item "env:$name" -ErrorAction SilentlyContinue } else { Set-Item "env:$name" $saved[$name] }
    }
}

$build = Get-Content -LiteralPath (Join-Path $root 'tools\build.ps1') -Raw
$gpu = Get-Content -LiteralPath (Join-Path $root 'gpu\CMakeLists.txt') -Raw
Check 'CMake receives only the pinned prefix' ($build -match '-DSOTB_FFMPEG_PREFIX=\$ffmpeg' -and $build -notmatch 'msys64[\\/]clang64["'']?\s*\)')
Check 'a changed prefix reconfigures; --fresh only for an existing configuration' ($build -match '\$fresh = if \(\$configured\)' -and ([regex]::Matches($build, '--fresh')).Count -eq 1)
Check 'a current FFmpeg is not rebuilt: the shell script decides' ($build -match 'Sync-FFmpeg')
Check 'the FFmpeg DLLs are staged next to the exe' ($build -match "Install-GameRuntime \`$loader \`$angleRuntime \`$tc\.Bin \(Join-Path \`$ffmpeg 'bin'\)")
Check 'CMake refuses a system FFmpeg' ($gpu -match 'SOTB_FFMPEG_PREFIX is not set' -and $gpu -match 'cmake_path\(IS_PREFIX SOTB_FFMPEG_PREFIX')

if ($failures -gt 0) { Write-Host "$failures failure(s)" -ForegroundColor Red; exit 1 }
Write-Host 'ffmpeg bootstrap checks passed' -ForegroundColor Green
