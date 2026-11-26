<#
.SYNOPSIS
  Prepares the complete local release in dist\: the Windows x64 ZIP and the Linux x86_64 AppImage with their .sha256
  files, dist\release-notes-v<version>.md and the VirusTotal manifests dist\virustotal-v<version>.json and .txt. Local
  only: nothing is published, tagged, pushed or uploaded, and neither the game nor the launcher is started.
.DESCRIPTION
  1. The complete headless gate, tools\test.ps1 -Full. When it fails, nothing is packaged.
  2. tools\package.ps1 builds and packages Windows; the staged folder is checked again with package.ps1 -VerifyOnly and
     every file in the ZIP is compared with it.
  3. tools/package-release-linux.sh, through wsl.exe in an Ubuntu 20.04 distribution (glibc 2.31, the AppImage's
     baseline) prepared once by packaging/linux/ci-deps.sh (docs/LINUX.md): build, Linux suites, parity contract,
     AppImage packaging and verification. Nothing is installed or changed in WSL.
  4. The parity contracts of the two release builds compared.
  5. The Linux program read out of the final AppImage with unsquashfs into a temporary folder under build\release\
     (the AppImage is never run) and compared with the program the packager staged. It is not a release file.
  6. Every artifact checked against its .sha256 file, then the release notes and both VirusTotal manifests written with
     the four VirusTotal entries (ZIP, AppImage, game executable, updater) PENDING.
  Fails closed: this version's artifacts already in dist\ are moved to build\package-superseded\ first, and when a step
  fails or the run is interrupted, everything it wrote is moved to build\package-failed\. A run with -SkipTests,
  -SkipWindows or -SkipLinux is not a release: it writes no notes and its artifacts go to build\package-partial\.

  -NotesOnly builds, tests and packages nothing and never moves or deletes an artifact. It checks the artifacts again and
  rewrites only the notes and the two manifests, keeping each VirusTotal URL that names the exact SHA-256 of its file
  (docs\RELEASE_SECURITY.md). The version is the project VERSION in CMakeLists.txt, the only place it is written.
.PARAMETER NotesOnly
  Rewrites the notes and the VirusTotal manifests for the artifacts already in dist\.
.PARAMETER WslDistro
  The Ubuntu 20.04 WSL distribution to build the AppImage in; by default the first one with glibc 2.31 and the tools.
.PARAMETER ExtractDistro
  The WSL distribution whose unsquashfs reads the AppImage; by default the first one that has it.
.EXAMPLE
  .\tools\package-release.ps1
  .\tools\package-release.ps1 -NotesOnly
#>
[CmdletBinding()]
param(
    [switch]$NotesOnly,
    [switch]$SkipTests,
    [switch]$SkipWindows,
    [switch]$SkipLinux,
    [switch]$Clean,
    [string]$WslDistro = '',
    [string]$ExtractDistro = ''
)

. (Join-Path $PSScriptRoot '_common.ps1')

$root = $script:RepoRoot
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^project\(shadow_of_the_beast_pc VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { Stop-Tool 'project VERSION not found in CMakeLists.txt' }
$version = $Matches[1]
$dist = Join-Path $root 'dist'
$release = Join-Path $root 'build\release'
$windowsName = "ShadowOfTheBeastPC-v$version-windows-x64"
$windowsFile = "$windowsName.zip"
$linuxFile = "ShadowOfTheBeastPC-v$version-x86_64.AppImage"
$linuxName = "ShadowOfTheBeastPC-v$version-x86_64"
$stage = Join-Path $dist $windowsName
$notesOut = Join-Path $dist "release-notes-v$version.md"
$scanJson = Join-Path $dist "virustotal-v$version.json"
$scanText = Join-Path $dist "virustotal-v$version.txt"
$windowsContract = Join-Path $release 'feature-contract-windows.txt'
$linuxContract = Join-Path $release 'feature-contract-linux.txt'
$stagedLinuxProgram = Join-Path $release 'linux-program.sha256'
$linuxCheck = Join-Path $release 'linux-program-check'
$shell = (Get-Process -Id $PID).Path
$failures = New-Object System.Collections.Generic.List[string]
$partial = $SkipTests -or $SkipWindows -or $SkipLinux
$builds = @('CUSA03762', 'CUSA02871', 'CUSA00951')

$artifacts = @($notesOut, $scanJson, $scanText, $stage, (Join-Path $dist $windowsFile), (Join-Path $dist "$windowsFile.sha256"),
    (Join-Path $dist "$windowsName.sha256"), (Join-Path $dist "$windowsName-symbols"), (Join-Path $dist $linuxFile),
    (Join-Path $dist "$linuxFile.sha256"), (Join-Path $dist "$linuxName-symbols"))

# Only this version's artifacts, by exact name, leave dist\; nothing else there is touched.
function Move-ArtifactsOutOfDist([string]$Kind) {
    $existing = @($artifacts | Where-Object { Test-Path -LiteralPath $_ })
    if ($existing.Count -eq 0) { return }
    $folder = Join-Path $root ('build\package-{0}\v{1}-{2}' -f $Kind, $version, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    foreach ($path in $existing) { Move-Item -LiteralPath $path -Destination $folder -Force }
    Write-Host "  This version's artifacts were moved out of dist to $folder" -ForegroundColor Yellow
}

function Invoke-Child([string]$Script, [string[]]$Arguments = @()) {
    & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $Script) @Arguments | Out-Host
    return $LASTEXITCODE
}

function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# The SHA-256 of an artifact when its .sha256 file is '<sha256>  <file name>' and matches it; otherwise a problem.
function Get-CheckedSha256([string]$File) {
    $path = Join-Path $dist $File
    $checksum = "$path.sha256"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return @{ Problem = "missing: $File" } }
    if (-not (Test-Path -LiteralPath $checksum -PathType Leaf)) { return @{ Problem = "missing: $File.sha256" } }
    $line = (Get-Content -LiteralPath $checksum -TotalCount 1)
    if ($line -cnotmatch '^([0-9a-f]{64})  (.+)$' -or $Matches[2] -ne $File) { return @{ Problem = "$File.sha256 is not '<sha256>  $File'" } }
    $recorded = $Matches[1]
    if ((Get-Sha256 $path) -ne $recorded) { return @{ Problem = "$File.sha256 does not match the file" } }
    return @{ Sha256 = $recorded }
}

# Every file in the ZIP must be the staged file of the same path, byte for byte, and nothing may be missing.
function Compare-StageWithZip {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $problems = New-Object System.Collections.Generic.List[string]
    $hashes = @{}
    if (-not (Test-Path -LiteralPath $stage -PathType Container)) { $problems.Add("missing staged folder: $stage"); return @{ Problems = $problems; Hashes = $hashes } }
    $base = (Resolve-Path -LiteralPath $stage).Path.TrimEnd('\')
    $staged = @{}
    foreach ($file in Get-ChildItem -LiteralPath $base -Recurse -File -Force) {
        $staged["$windowsName/" + $file.FullName.Substring($base.Length + 1).Replace('\', '/')] = $file.FullName
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $dist $windowsFile))
    try {
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            if ($name -match '^[\\/]|\.\.|:|\\' -or -not $name.StartsWith("$windowsName/")) { $problems.Add("unsafe ZIP entry: $name"); continue }
            if (-not $staged.ContainsKey($name)) { $problems.Add("only in the ZIP: $name"); continue }
            $stream = $entry.Open()
            try { $hashes[$name] = ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant() } finally { $stream.Dispose() }
            if ($hashes[$name] -ne (Get-Sha256 $staged[$name])) { $problems.Add("differs from the staged file: $name") }
        }
    } finally { $archive.Dispose(); $sha.Dispose() }
    # Starting the staged program writes logs\ and user\ beside it; packaging never ships either folder.
    $runtime = "^$([regex]::Escape($windowsName))/(logs|user)/"
    foreach ($name in $staged.Keys) { if (-not $hashes.ContainsKey($name) -and $name -notmatch $runtime) { $problems.Add("missing from the ZIP: $name") } }
    return @{ Problems = $problems; Hashes = $hashes }
}

# The WSL helpers read wsl.exe's own error output as text instead of stopping on it.
function ConvertTo-WslPath([string]$Distro, [string]$Path) {
    $ErrorActionPreference = 'Continue'
    ((& wsl.exe -d $Distro --exec /usr/bin/wslpath -a -u ($Path.Replace('\', '/')) | Out-String) -replace "`0", '').Trim()
}

# An Ubuntu 20.04 distribution whose glibc is the baseline and that has the build and packaging tools, with the SDL3 and
# NASM that packaging/linux/ci-deps.sh pins.
function Find-FocalDistro {
    $ErrorActionPreference = 'Continue'
    $deps = Get-Content -LiteralPath (Join-Path $root 'packaging\linux\ci-deps.sh') -Raw
    if ($deps -notmatch '(?m)^SDL_VERSION=([0-9.]+)') { return '' }
    $sdl = $Matches[1]
    if ($deps -notmatch '(?m)^NASM_VERSION=([0-9.]+)') { return '' }
    $nasm = $Matches[1]
    $probe = '. /etc/os-release; [ "$ID $VERSION_ID" = "ubuntu 20.04" ] && [ "$(getconf GNU_LIBC_VERSION)" = "glibc 2.31" ] && command -v clang cmake ninja patchelf rsync desktop-file-validate >/dev/null' +
        " && [ `"`$(pkg-config --modversion sdl3)`" = `"$sdl`" ] && nasm -v | grep -qF `"version $nasm `" && echo FOCAL_READY"
    foreach ($name in Get-WslDistros) {
        $answer = (& wsl.exe -d $name --exec /bin/bash -c $probe 2>$null | Out-String) -replace "`0", ''
        if ($answer -match 'FOCAL_READY') { return $name }
    }
    return ''
}

function Get-WslDistros {
    $ErrorActionPreference = 'Continue'
    $names = @(& wsl.exe -l -q 2>$null | ForEach-Object { ("$_" -replace "`0", '').Trim() } | Where-Object { $_ })
    return @($names | Where-Object { $_ -match 'Focal' }) + @($names | Where-Object { $_ -notmatch 'Focal' })
}

function Find-ExtractDistro {
    $ErrorActionPreference = 'Continue'
    foreach ($name in Get-WslDistros) {
        $answer = (& wsl.exe -d $name --exec /bin/sh -c 'command -v unsquashfs readelf od sha256sum >/dev/null && echo EXTRACT_READY' 2>$null | Out-String) -replace "`0", ''
        if ($answer -match 'EXTRACT_READY') { return $name }
    }
    return ''
}

# Copies usr/bin/shadow_of_the_beast out of the AppImage in dist\ to $Destination; returns its SHA-256 or a problem.
function Copy-LinuxProgram([string]$Destination) {
    $ErrorActionPreference = 'Continue'
    $distro = if ($ExtractDistro) { $ExtractDistro } elseif (Get-Command wsl.exe -ErrorAction SilentlyContinue) { Find-ExtractDistro } else { '' }
    if (-not $distro) { return @{ Problem = 'no WSL distribution has unsquashfs to read the AppImage (squashfs-tools), or name one with -ExtractDistro' } }
    $repo = ConvertTo-WslPath $distro $root
    $image = ConvertTo-WslPath $distro (Join-Path $dist $linuxFile)
    $out = ConvertTo-WslPath $distro $Destination
    $lines = @(& wsl.exe -d $distro --exec /bin/bash "$repo/tools/package-linux.sh" --extract-program $image $out 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    $reported = @($lines | Where-Object { $_ -match '^PROGRAM_SHA256\|[0-9a-f]{64}$' }) | Select-Object -First 1
    if ($code -ne 0 -or -not $reported) { return @{ Problem = "the Linux program could not be read from $linuxFile in ${distro}: $($lines -join ' ')" } }
    $sha = $reported.Split('|')[1]
    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf) -or (Get-Sha256 $Destination) -ne $sha) { return @{ Problem = 'the copied Linux program does not have the SHA-256 read in WSL' } }
    Write-Ok "shadow_of_the_beast read from $linuxFile with unsquashfs in $distro (the AppImage was not run): $sha"
    return @{ Sha256 = $sha }
}

function Get-WindowsContract {
    $tc = Resolve-Toolchain 'auto'
    $buildDir = Join-Path $root 'build\cmake\clang64'
    $ninja = Join-Path $tc.Bin 'ninja.exe'
    if (-not (Test-Path -LiteralPath $ninja)) { $ninja = (Get-Command ninja).Source }
    $savedPath = $env:PATH
    try {
        $env:PATH = "$($tc.Bin);$env:PATH"
        & $ninja -C $buildDir sotb-build-parity-tests | Out-Host
        if ($LASTEXITCODE -ne 0) { return 'sotb-build-parity-tests did not build on Windows' }
        Push-Location (Join-Path $buildDir 'bin')
        try { $lines = @(& .\sotb-build-parity-tests.exe | ForEach-Object { "$_" }); $code = $LASTEXITCODE } finally { Pop-Location }
    } finally { $env:PATH = $savedPath }
    [IO.File]::WriteAllText($windowsContract, (($lines + "EXIT|$code") -join "`n") + "`n", (New-Object Text.UTF8Encoding $false))
    return ''
}

# Both contracts ran, passed for the three supported builds, and describe the same port.
function Test-ParityContracts {
    $problems = New-Object System.Collections.Generic.List[string]
    $tables = @{}
    foreach ($pair in @(@('Windows', $windowsContract), @('Linux', $linuxContract))) {
        if (-not (Test-Path -LiteralPath $pair[1] -PathType Leaf)) { $problems.Add("no $($pair[0]) parity contract ($($pair[1]))"); continue }
        $lines = @(Get-Content -LiteralPath $pair[1])
        foreach ($required in "GROUP|build-parity|PASS", "GROUP|platform-features|PASS", "EXIT|0", "VERSION|$version") {
            if (-not ($lines | Where-Object { $_ -eq $required -or $_.StartsWith("$required|") })) { $problems.Add("the $($pair[0]) contract lacks $required") }
        }
        foreach ($build in $builds) {
            if (-not ($lines | Where-Object { $_ -match "^BUILD_PARITY\|$build\|1\.00\|[^|]+\|PASS(\||$)" })) { $problems.Add("the $($pair[0]) contract has no PASS for $build 1.00") }
        }
        $tables[$pair[0]] = Read-Contract $lines
    }
    if ($tables.Count -eq 2) {
        foreach ($difference in @(Compare-Contracts $tables['Windows'] $tables['Linux'])) { $problems.Add("Windows/Linux: $difference") }
    }
    return $problems
}

$recordedScans = @{}
function Read-RecordedScans {
    if (-not (Test-Path -LiteralPath $scanJson -PathType Leaf)) { return }
    try { $entries = @(Get-Content -LiteralPath $scanJson -Raw | ConvertFrom-Json | ForEach-Object { $_ }) }
    catch { $failures.Add("cannot read ${scanJson}: $($_.Exception.Message)"); return }
    foreach ($entry in $entries) {
        $fields = @{}
        foreach ($field in 'path', 'sha256', 'virustotal') {
            $property = $entry.PSObject.Properties[$field]
            $fields[$field] = if ($property) { [string]$property.Value } else { '' }
        }
        if ($fields.path) { $recordedScans[$fields.path] = $fields }
    }
}

# A recorded scan counts only when it is the VirusTotal page of the file's exact current SHA-256.
function Get-ScanCell([string]$RelativePath, [string]$Sha256) {
    $entry = $recordedScans[$RelativePath]
    if (-not $entry -or $entry.sha256 -ne $Sha256) { return 'PENDING' }
    if ($entry.virustotal -cmatch ('^https://www\.virustotal\.com/gui/file/' + $Sha256 + '(\?nocache=1)?$')) { return $entry.virustotal }
    return 'PENDING'
}

function Write-ReleaseNotes($Scans) {
    $template = Join-Path $root 'packaging\release-notes.md'
    $caveat = 'VirusTotal is an additional verification signal, not an absolute guarantee of safety.'
    $pending = @($Scans | Where-Object { $_.Scan -eq 'PENDING' }).Count
    $status = if ($pending -eq 0) {
        "Each file below was checked on VirusTotal before publication; each scan link names the file's exact SHA-256.`n`n$caveat"
    } else {
        "**VirusTotal: PENDING.** Not every file below has been checked on VirusTotal yet. These notes must not be published while any row says PENDING.`n`n$caveat"
    }
    $notes = (Get-Content -LiteralPath $template -Raw -Encoding UTF8).Replace('@VERSION@', $version)
    $notes = $notes.Replace('@WINDOWS_ASSET@', $windowsFile).Replace('@LINUX_ASSET@', $linuxFile).Replace('@VIRUSTOTAL_STATUS@', $status)
    foreach ($scan in $Scans) {
        $cell = if ($scan.Scan -eq 'PENDING') { 'PENDING' } else { "[VirusTotal]($($scan.Scan))" }
        $notes = $notes.Replace("@$($scan.Key)_SHA256@", $scan.Sha256).Replace("@$($scan.Key)_VIRUSTOTAL@", $cell)
    }
    if ($notes -match '@[A-Z_0-9]+@') { $failures.Add("release notes: unreplaced placeholder $($Matches[0])"); return }
    $utf8 = New-Object Text.UTF8Encoding $false
    [IO.File]::WriteAllText($notesOut, ($notes -replace "`r`n", "`n"), $utf8)
    $entries = @($Scans | ForEach-Object { [ordered]@{ path = $_.Path; sha256 = $_.Sha256; virustotal = $_.Scan } })
    [IO.File]::WriteAllText($scanJson, ((ConvertTo-Json -InputObject $entries -Depth 3) -replace "`r`n", "`n") + "`n", $utf8)
    [IO.File]::WriteAllText($scanText, ((@($entries | ForEach-Object { "$($_.virustotal)  $($_.sha256)  $($_.path)" }) -join "`n") + "`n"), $utf8)
    Write-Ok "Wrote $notesOut, $scanJson and $scanText"
}

# Checks every artifact of the release and returns the files to scan, with their SHA-256 and VirusTotal state.
function Get-ReleaseScans {
    $sha = @{}
    foreach ($pair in @(@('WINDOWS', $windowsFile), @('LINUX', $linuxFile))) {
        $result = Get-CheckedSha256 $pair[1]
        if ($result['Problem']) { $failures.Add($result['Problem']) } else { $sha[$pair[0]] = $result.Sha256 }
    }
    if ($failures.Count) { return @() }
    $zip = Compare-StageWithZip
    foreach ($problem in $zip.Problems) { $failures.Add("Windows ZIP: $problem") }
    foreach ($pair in @(@('WINDOWS_EXE', 'shadow_of_the_beast.exe'), @('UPDATER_EXE', 'trg-updater.exe'))) {
        $path = Join-Path $stage $pair[1]
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $failures.Add("the staged folder has no $($pair[1])"); continue }
        $sha[$pair[0]] = Get-Sha256 $path
        $inZip = $zip.Hashes["$windowsName/$($pair[1])"]
        if ($inZip -ne $sha[$pair[0]]) { $failures.Add("$($pair[1]) in the ZIP is not the staged one") }
        else { Write-Ok "$($pair[1]): staged and in the ZIP $inZip" }
    }
    $built = (Get-Item -LiteralPath (Join-Path $stage 'shadow_of_the_beast.exe') -ErrorAction SilentlyContinue)
    if ($built -and $built.VersionInfo.ProductVersion -ne $version) { $failures.Add("the staged shadow_of_the_beast.exe is version '$($built.VersionInfo.ProductVersion)', not $version") }
    if ($failures.Count) { return @() }
    $scans = @(
        @{ Key = 'WINDOWS'; Path = (Join-Path $dist $windowsFile) },
        @{ Key = 'LINUX'; Path = (Join-Path $dist $linuxFile) },
        @{ Key = 'WINDOWS_EXE'; Path = (Join-Path $stage 'shadow_of_the_beast.exe') },
        @{ Key = 'UPDATER_EXE'; Path = (Join-Path $stage 'trg-updater.exe') })
    foreach ($scan in $scans) {
        $scan.Path = $scan.Path.Substring($root.Length + 1)
        $scan.Sha256 = $sha[$scan.Key]
        $scan.Scan = Get-ScanCell $scan.Path $scan.Sha256
    }
    return $scans
}

function Write-Scans($Scans) {
    Write-Host ''
    Write-Host 'Files to scan on VirusTotal, each on its own (uploads are manual):'
    foreach ($scan in $Scans) {
        $state = if ($scan.Scan -eq 'PENDING') { 'PENDING' } else { 'validated' }
        Write-Host ("  {0,-9} {1}  {2}" -f $state, $scan.Sha256, (Join-Path $root $scan.Path))
    }
}

if ($NotesOnly) {
    Write-Step "Shadow of the Beast PC $version - release notes and VirusTotal manifests only (nothing is built or moved)"
    Read-RecordedScans
    $before = @{}
    foreach ($file in $windowsFile, $linuxFile) { if (Test-Path -LiteralPath (Join-Path $dist $file)) { $before[$file] = Get-Sha256 (Join-Path $dist $file) } }
    $scans = if ($failures.Count) { @() } else { @(Get-ReleaseScans) }
    if ($failures.Count -eq 0) { Write-ReleaseNotes $scans }
    foreach ($file in $before.Keys) {
        if ((Get-Sha256 (Join-Path $dist $file)) -ne $before[$file]) { $failures.Add("$file changed during -NotesOnly") }
    }
    Write-Host ''
    if ($failures.Count) {
        Write-Host 'NOTES NOT WRITTEN (no artifact was changed, moved or deleted)' -ForegroundColor Red
        foreach ($failure in $failures) { Write-Host "  $failure" -ForegroundColor Red }
        exit 1
    }
    Write-Scans $scans
    $pending = @($scans | Where-Object { $_.Scan -eq 'PENDING' }).Count
    Write-Host ''
    if ($pending) {
        Write-Host "ARTIFACTS READY FOR VIRUSTOTAL ($pending of $($scans.Count) still PENDING; do not publish)" -ForegroundColor Yellow
    } else {
        Write-Host 'READY FOR MANUAL RUNTIME TEST (every VirusTotal URL matches its exact SHA-256)' -ForegroundColor Green
    }
    exit 0
}

Write-Step "Shadow of the Beast PC $version - release packages (local only)"
New-Item -ItemType Directory -Force $dist, $release | Out-Null
Move-ArtifactsOutOfDist 'superseded'
Remove-Item -LiteralPath $windowsContract, $linuxContract, $stagedLinuxProgram -ErrorAction SilentlyContinue

$finished = $false
try {
    if (-not $SkipTests) {
        Write-Step 'Step 1: complete headless release gate (tools\test.ps1 -Full)'
        $code = Invoke-Child 'test.ps1' @('-Full')
        if ($code -ne 0) { $failures.Add("tools\test.ps1 -Full failed (exit $code); nothing was packaged") }
    }

    if ($failures.Count -eq 0 -and -not $SkipWindows) {
        Write-Step 'Steps 2-3: Windows x64 Release build, ZIP and verification (tools\package.ps1)'
        $code = Invoke-Child 'package.ps1' @(if ($Clean) { '-Clean' })
        if ($code -ne 0) { $failures.Add("tools\package.ps1 failed (exit $code)") }
        else {
            $code = Invoke-Child 'package.ps1' @('-VerifyOnly', $stage)
            if ($code -ne 0) { $failures.Add("tools\package.ps1 -VerifyOnly failed for $stage (exit $code)") }
            $zip = Compare-StageWithZip
            foreach ($problem in $zip.Problems) { $failures.Add("Windows ZIP: $problem") }
            if ($zip.Problems.Count -eq 0) { Write-Ok "$windowsFile holds exactly the $($zip.Hashes.Count) staged files" }
            $problem = Get-WindowsContract
            if ($problem) { $failures.Add($problem) }
        }
    }

    if ($failures.Count -eq 0 -and -not $SkipLinux) {
        Write-Step 'Steps 4-7: Linux x86_64 Release build, tests, AppImage and verification (tools/package-release-linux.sh in Ubuntu 20.04 WSL)'
        $distro = if ($WslDistro) { $WslDistro } elseif (Get-Command wsl.exe -ErrorAction SilentlyContinue) { Find-FocalDistro } else { '' }
        if (-not $distro) {
            $failures.Add('No Ubuntu 20.04 WSL distribution with glibc 2.31 and the build tools. Prepare one as docs/LINUX.md describes (packaging/linux/ci-deps.sh), or name it with -WslDistro.')
        } else {
            Write-Host "  WSL distribution: $distro"
            $repo = ConvertTo-WslPath $distro $root
            $linuxArgs = @(if ($Clean) { '--clean' })
            & wsl.exe -d $distro --exec /bin/bash "$repo/tools/package-release-linux.sh" @linuxArgs
            if ($LASTEXITCODE -ne 0) { $failures.Add("tools/package-release-linux.sh failed in $distro (exit $LASTEXITCODE)") }
            else {
                try {
                    $copy = Copy-LinuxProgram (Join-Path $linuxCheck 'shadow_of_the_beast')
                    if ($copy['Problem']) { $failures.Add($copy['Problem']) }
                    elseif (-not (Test-Path -LiteralPath $stagedLinuxProgram) -or (Get-Content -LiteralPath $stagedLinuxProgram -TotalCount 1).Trim() -ne $copy.Sha256) {
                        $failures.Add('the program inside the AppImage is not the one the packager staged')
                    } else { Write-Ok 'The program inside the AppImage is the one the packager staged' }
                } finally { if (Test-Path -LiteralPath $linuxCheck) { Remove-Item -LiteralPath $linuxCheck -Recurse -Force } }
            }
        }
    }

    if ($failures.Count -eq 0 -and -not $SkipWindows -and -not $SkipLinux) {
        Write-Step 'Step 8: Windows/Linux parity of the release builds'
        foreach ($problem in Test-ParityContracts) { $failures.Add("Parity: $problem") }
        if ($failures.Count -eq 0) { Write-Ok "The Windows and Linux contracts match and pass for $($builds -join ', ')" }
    }

    $scans = @()
    if ($failures.Count -eq 0 -and -not $partial) {
        Write-Step 'Steps 9-11: SHA-256 checks, release notes and VirusTotal manifests'
        $scans = @(Get-ReleaseScans)
        if ($failures.Count -eq 0) { Write-ReleaseNotes $scans }
    }
    $finished = $true
} finally {
    if (-not $finished) { $failures.Add('the run was interrupted') }
    if ($failures.Count) { Move-ArtifactsOutOfDist 'failed' }
    elseif ($partial) { Move-ArtifactsOutOfDist 'partial' }
}

Write-Host ''
if ($failures.Count) {
    Write-Host 'RELEASE NOT PREPARED' -ForegroundColor Red
    foreach ($failure in $failures) { Write-Host "  $failure" -ForegroundColor Red }
    exit 1
}
if ($partial) {
    Write-Host 'PARTIAL RUN, NOT A RELEASE: no notes were written and its artifacts were moved out of dist' -ForegroundColor Yellow
    exit 0
}
Write-Host 'RELEASE PREPARED (nothing was published)' -ForegroundColor Green
foreach ($file in "$windowsFile", "$windowsFile.sha256", "$linuxFile", "$linuxFile.sha256") { Write-Host "  $(Join-Path $dist $file)" }
Write-Host "  $notesOut"
Write-Host "  $scanJson"
Write-Host "  $scanText"
Write-Host "  Staged Windows folder: $stage"
Write-Host "  Symbols (not for release): $(Join-Path $dist "$windowsName-symbols"), $(Join-Path $dist "$linuxName-symbols")"
Write-Scans $scans
Write-Host ''
Write-Host 'ARTIFACTS READY FOR VIRUSTOTAL. Do not rebuild or change them; see docs\RELEASE_SECURITY.md for the next steps.' -ForegroundColor Yellow
exit 0
