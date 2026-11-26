<#
.SYNOPSIS
  Prepares a complete local release in dist\: the Windows x64 ZIP and the Linux x86_64 AppImage, each with its .sha256,
  and dist\release-notes-v<version>.md with their checksums. Local only: nothing is published, tagged, pushed or uploaded,
  and neither the game nor the launcher is started.
.DESCRIPTION
  1. The complete headless gate, tools\test.ps1 -Full (skipped with -SkipTests). When it fails, nothing is packaged.
  2. tools\package.ps1 for Windows, then package.ps1 -VerifyOnly on the staged folder.
  3. tools/package-release-linux.sh through wsl.exe in an Ubuntu 20.04 distribution (glibc 2.31, the AppImage's
     baseline), prepared once by packaging/linux/ci-deps.sh (docs/LINUX.md). It builds, runs the Linux suites, packages
     and verifies the AppImage. Nothing is installed or changed in WSL.
  4. Every artifact checked against its .sha256 file, and docs\releases\v<version>.md copied to dist with the checksums
     filled in. A run that fails leaves no release notes behind.
  The version is the project VERSION in CMakeLists.txt, the only place it is written.
.PARAMETER WslDistro
  The Ubuntu 20.04 WSL distribution to build the AppImage in; by default the first one with glibc 2.31 and the tools.
.EXAMPLE
  .\tools\package-release.ps1
#>
[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$SkipWindows,
    [switch]$SkipLinux,
    [switch]$Clean,
    [string]$WslDistro = ''
)

. (Join-Path $PSScriptRoot '_common.ps1')

$root = $script:RepoRoot
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^project\(shadow_of_the_beast_pc VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { Stop-Tool 'project VERSION not found in CMakeLists.txt' }
$version = $Matches[1]
$dist = Join-Path $root 'dist'
$windowsName = "ShadowOfTheBeastPC-v$version-windows-x64"
$windowsFile = "$windowsName.zip"
$linuxFile = "ShadowOfTheBeastPC-v$version-x86_64.AppImage"
$notesSource = Join-Path $root "docs\releases\v$version.md"
$notesOut = Join-Path $dist "release-notes-v$version.md"
$shell = (Get-Process -Id $PID).Path
$failures = New-Object System.Collections.Generic.List[string]

function Invoke-Child([string]$Script, [string[]]$Arguments = @()) {
    & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $Script) @Arguments | Out-Host
    return $LASTEXITCODE
}

# The SHA-256 of an artifact when its .sha256 file is '<sha256>  <file name>' and matches it; otherwise a problem.
function Get-CheckedSha256([string]$File) {
    $path = Join-Path $dist $File
    $checksum = "$path.sha256"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return @{ Problem = "missing: $File" } }
    if (-not (Test-Path -LiteralPath $checksum -PathType Leaf)) { return @{ Problem = "missing: $File.sha256" } }
    $line = (Get-Content -LiteralPath $checksum -TotalCount 1)
    if ($line -cnotmatch '^([0-9a-f]{64})  (.+)$' -or $Matches[2] -ne $File) { return @{ Problem = "$File.sha256 is not '<sha256>  $File'" } }
    $recorded = $Matches[1]
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $recorded) { return @{ Problem = "$File.sha256 does not match the file" } }
    return @{ Sha256 = $actual }
}

# An Ubuntu 20.04 distribution whose glibc is the baseline and that has the build and packaging tools.
function Find-FocalDistro {
    $probe = '. /etc/os-release; [ "$ID $VERSION_ID" = "ubuntu 20.04" ] && [ "$(getconf GNU_LIBC_VERSION)" = "glibc 2.31" ] && command -v clang cmake ninja patchelf rsync desktop-file-validate >/dev/null && echo FOCAL_READY'
    $names = @(& wsl.exe -l -q 2>$null | ForEach-Object { ("$_" -replace "`0", '').Trim() } | Where-Object { $_ })
    foreach ($name in @($names | Where-Object { $_ -match 'Focal' }) + @($names | Where-Object { $_ -notmatch 'Focal' })) {
        $answer = (& wsl.exe -d $name --exec /bin/bash -c $probe 2>$null | Out-String) -replace "`0", ''
        if ($answer -match 'FOCAL_READY') { return $name }
    }
    return ''
}

Write-Step "Shadow of the Beast PC $version - release packages (local only)"
New-Item -ItemType Directory -Force $dist | Out-Null
Remove-Item -LiteralPath $notesOut -ErrorAction SilentlyContinue

if (-not $SkipTests) {
    Write-Step 'Headless release gate (tools\test.ps1 -Full)'
    $code = Invoke-Child 'test.ps1' @('-Full')
    if ($code -ne 0) { $failures.Add("tools\test.ps1 -Full failed (exit $code); nothing was packaged") }
}

if ($failures.Count -eq 0 -and -not $SkipWindows) {
    Write-Step "Windows x64 ZIP (tools\package.ps1)"
    $code = Invoke-Child 'package.ps1' @(if ($Clean) { '-Clean' })
    if ($code -ne 0) { $failures.Add("tools\package.ps1 failed (exit $code)") }
    else {
        $code = Invoke-Child 'package.ps1' @('-VerifyOnly', (Join-Path $dist $windowsName))
        if ($code -ne 0) { $failures.Add("tools\package.ps1 -VerifyOnly failed (exit $code)") }
    }
}

if ($failures.Count -eq 0 -and -not $SkipLinux) {
    Write-Step 'Linux x86_64 AppImage (tools/package-release-linux.sh in Ubuntu 20.04 WSL)'
    $distro = if ($WslDistro) { $WslDistro } elseif (Get-Command wsl.exe -ErrorAction SilentlyContinue) { Find-FocalDistro } else { '' }
    if (-not $distro) {
        $failures.Add('No Ubuntu 20.04 WSL distribution with glibc 2.31 and the build tools. Prepare one as docs/LINUX.md describes (packaging/linux/ci-deps.sh), or name it with -WslDistro.')
    } else {
        Write-Host "  WSL distribution: $distro"
        $repo = ((& wsl.exe -d $distro --exec /usr/bin/wslpath -a -u ($root.Replace('\', '/')) | Out-String) -replace "`0", '').Trim()
        $linuxArgs = @(if ($Clean) { '--clean' })
        & wsl.exe -d $distro --exec /bin/bash "$repo/tools/package-release-linux.sh" @linuxArgs
        if ($LASTEXITCODE -ne 0) { $failures.Add("tools/package-release-linux.sh failed in $distro (exit $LASTEXITCODE)") }
    }
}

$sha = @{}
foreach ($pair in @(@('WINDOWS', $windowsFile, $SkipWindows), @('LINUX', $linuxFile, $SkipLinux))) {
    if ($failures.Count) { break }
    $result = Get-CheckedSha256 $pair[1]
    if ($result.Problem) { if (-not $pair[2]) { $failures.Add($result.Problem) } }
    else { $sha[$pair[0]] = $result.Sha256 }
}

if ($failures.Count -eq 0 -and -not $SkipWindows -and -not $SkipLinux) {
    if (-not (Test-Path -LiteralPath $notesSource -PathType Leaf)) { $failures.Add("missing release notes: $notesSource") }
    else {
        $notes = (Get-Content -LiteralPath $notesSource -Raw -Encoding UTF8).Replace('@WINDOWS_SHA256@', $sha['WINDOWS']).Replace('@LINUX_SHA256@', $sha['LINUX'])
        foreach ($file in $windowsFile, $linuxFile) { if (-not $notes.Contains("``$file``")) { $failures.Add("the release notes do not name $file") } }
        if ($notes -match '@[A-Z0-9_]+@') { $failures.Add("the release notes still hold $($Matches[0])") }
        if ($failures.Count -eq 0) {
            [IO.File]::WriteAllText($notesOut, ($notes -replace "`r`n", "`n"), (New-Object Text.UTF8Encoding $false))
            Write-Ok "Wrote $notesOut"
        }
    }
}

Write-Host ''
if ($failures.Count) {
    Write-Host 'RELEASE NOT PREPARED' -ForegroundColor Red
    foreach ($failure in $failures) { Write-Host "  $failure" -ForegroundColor Red }
    exit 1
}
Write-Host 'RELEASE PREPARED (nothing was published)' -ForegroundColor Green
foreach ($key in $sha.Keys | Sort-Object -Descending) {
    $file = if ($key -eq 'WINDOWS') { $windowsFile } else { $linuxFile }
    Write-Host "  $(Join-Path $dist $file)  $($sha[$key])"
}
if (Test-Path -LiteralPath $notesOut) { Write-Host "  $notesOut" }
exit 0
