# The port version has one source, project(VERSION) in CMakeLists.txt; the runtime header, the Windows version resource,
# the launcher, the updater and the parity contract on both platforms derive from it. Reads files only.
param([string]$Expected = '0.1.0', [string]$GeneratedHeader = '')

$root = Split-Path $PSScriptRoot -Parent
$checks = 0
$failures = New-Object System.Collections.Generic.List[string]
function Check([bool]$Condition, [string]$Text) {
    $script:checks++
    if (-not $Condition) { $script:failures.Add($Text); Write-Host "FAIL $Text" }
}
function Read-Source([string]$Path) { Get-Content -Raw -LiteralPath (Join-Path $root $Path) }

$cmake = Read-Source 'CMakeLists.txt'
$declared = [regex]::Matches($cmake, '(?m)^project\([^)]*\bVERSION\s+([0-9][0-9A-Za-z.+-]*)')
Check ($declared.Count -eq 1) 'CMakeLists.txt declares the project version exactly once'
$version = if ($declared.Count) { $declared[0].Groups[1].Value } else { '' }
Check ($version -eq $Expected) "the canonical version is $Expected (CMakeLists.txt declares '$version')"
Check ($cmake.Contains('set(SOTB_PORT_VERSION "${PROJECT_VERSION}")')) 'SOTB_PORT_VERSION derives from PROJECT_VERSION'
Check ($cmake.Contains('generated/sotb_version.h') -and $cmake.Contains('#define SOTB_VERSION_STRING \"${PROJECT_VERSION}\"')) `
    'the generated runtime header derives from PROJECT_VERSION'
Check ($cmake.IndexOf('sotb_version.h') -lt $cmake.IndexOf('if (NOT WIN32)')) 'the version header is generated on every platform'

$rc = Read-Source 'src\shadow_of_the_beast.rc'
foreach ($line in 'FILEVERSION SOTB_VERSION_NUMBERS', 'PRODUCTVERSION SOTB_VERSION_NUMBERS', '"FileVersion", SOTB_FILE_VERSION_STRING',
                  '"ProductVersion", SOTB_VERSION_STRING', '#include "sotb_version.h"') {
    Check ($rc.Contains($line)) "the Windows version resource uses $line"
}
Check ($rc -notmatch '"\d+\.\d+\.\d+') 'the Windows version resource has no literal version'

$launcher = Read-Source 'launcher\CMakeLists.txt'
Check ([regex]::Matches($launcher, 'SOTB_PORT_VERSION="\$\{(PROJECT_VERSION|SOTB_PORT_VERSION)\}"').Count -ge 4) `
    'the launcher, its core and the updater tests receive the canonical version'
Check ($launcher -notmatch 'SOTB_PORT_VERSION="\d') 'no launcher target is given a literal version'

$update = Read-Source 'launcher\update_config.cpp'
Check ($update.Contains('config.current_version = SOTB_PORT_VERSION;')) 'the updater compares releases against the canonical version'
Check ($update.Contains('std::string("ShadowOfTheBeastPC-Updater/") + SOTB_PORT_VERSION')) 'the updater user agent carries the canonical version'
Check ($update.Contains('"ShadowOfTheBeastPC-v" + version.CoreString() + "-windows-x64.zip"')) 'release asset names come from the release version'
Check ($update.Contains('"ShadowOfTheBeastPC-v" + version.CoreString() + "-x86_64.AppImage"')) 'the Linux release asset name comes from the release version'
foreach ($file in 'launcher\launcher.cpp', 'launcher\settings.cpp') {
    Check ((Read-Source $file).Contains('SOTB_PORT_VERSION')) "$file shows the canonical version"
}
Check ((Read-Source 'tests\build_parity_tests.cpp').Contains('std::printf("VERSION|%s\n", SOTB_VERSION_STRING);')) `
    'the parity contract reports the generated version on Windows and Linux'

if ($GeneratedHeader) {
    $header = if (Test-Path -LiteralPath $GeneratedHeader) { Get-Content -Raw -LiteralPath $GeneratedHeader } else { '' }
    Check ([bool]$header) "the generated header $(Split-Path $GeneratedHeader -Leaf) exists"
    $parts = $Expected.Split('.')
    Check ($header.Contains("#define SOTB_VERSION_STRING `"$Expected`"")) "the generated runtime version is $Expected"
    Check ($header.Contains("#define SOTB_FILE_VERSION_STRING `"$Expected.0`"")) "the generated file version is $Expected.0"
    Check ($header.Contains("#define SOTB_VERSION_NUMBERS $($parts -join ','),0")) "the generated version numbers are $($parts -join ','),0"
}

# A literal port version in production code would not follow the canonical one. Third-party code, the updater tests'
# later-release examples (asset names of a release after this one) and the Bug Report tests' lists of accepted version
# names are not declarations of this version.
$git = Get-Command git -ErrorAction SilentlyContinue
Check ([bool]$git) 'git is available to list the tracked files'
if ($git) {
    $production = '^(CMakeLists\.txt|src/|launcher/|gpu/shim/|gpu/CMakeLists\.txt|tools/|scripts/|packaging/|README\.md)'
    $thirdParty = '(^|/)third_party/|^gpu/shadps4/'
    $literal = '(VERSION[[:space:]]+|SOTB_[A-Za-z0-9_]*VERSION[A-Za-z0-9_]*[[:space:]]*=?[[:space:]]*"|ShadowOfTheBeastPC-v|"v?)[0-9]+\.[0-9]+\.[0-9]+'
    $otherVersions = @(
        @{ Path = 'launcher/launcher.cpp'; Text = 'kTrgLauncherVersion'; Kind = 'TRG Launcher component version' },
        @{ Path = 'src/shadow_of_the_beast.manifest'; Text = 'assemblyIdentity'; Kind = 'side-by-side assembly identity' }
    )
    foreach ($hit in @(& $git.Source -C $root grep -n -I -E $literal -- . 2>$null)) {
        $path, $number, $text = $hit -split ':', 3
        if ($path -match $thirdParty -or $path -notmatch $production) { continue }
        if ($path -eq 'CMakeLists.txt' -and $text -match '^\s*(cmake_minimum_required|project)\(') { continue }
        $other = $otherVersions | Where-Object { $_.Path -eq $path -and $text.Contains($_.Text) } | Select-Object -First 1
        if ($other) { Write-Host "VERSION_LITERAL|$($path):$number|$($other.Kind)"; continue }
        Check $false "$($path):$number declares a literal version: $($text.Trim())"
    }
    foreach ($hit in @(& $git.Source -C $root grep -n -I -F '0.2.0' -- . 2>$null)) {
        $path, $number, $text = $hit -split ':', 3
        if ($path -match $thirdParty) { continue }
        $kind = if ($path -match '^tests/update(_network)?_tests\.cpp$' -and $text -match 'ParseVersion\("v?0\.2\.0|v0\.2\.0[-/]') {
            'later-release example'
        } elseif ($path -match '^(tests/bug_report_tests\.cpp|server/shadow-bug-reports/test/launcher_contract\.test\.js)$' -and
                  $text -match '0\.1\.0' -and $text -match '1\.0\.0-rc\.1') {
            'accepted-version sample'
        } elseif ($path -eq 'tests/version_contract_tests.ps1') { 'this check' } else { 'active declaration' }
        Write-Host "VERSION_LITERAL|0.2.0|$($path):$number|$kind"
        Check ($kind -ne 'active declaration') "$($path):$number still says 0.2.0: $($text.Trim())"
    }
}

$state = if ($failures.Count) { 'FAIL' } else { 'PASS' }
Write-Host "GROUP|version|$state|checks=$checks|version=$version"
Write-Host "version contract tests: $checks checks, $($failures.Count) failures"
if ($failures.Count) { exit 1 }
exit 0
