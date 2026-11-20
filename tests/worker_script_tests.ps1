# Static and stand-in checks of tools\worker.ps1: a copy of the Worker folder in a temporary tree, and an npx that only
# records its arguments. Nothing reaches Cloudflare, Wrangler or the network.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$shell = (Get-Process -Id $PID).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) "sotb-worker-script-tests-$PID"
$failures = 0
$checks = 0

function Check([bool]$condition, [string]$what) {
    $script:checks++
    if (-not $condition) { $script:failures++; Write-Host "FAIL $what" }
}

function New-Tree([string]$Name, [string]$NamespaceId) {
    $tree = Join-Path $temp $Name
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $tree 'tools')
    Copy-Item -LiteralPath (Join-Path $root 'tools\worker.ps1') -Destination (Join-Path $tree 'tools')
    $worker = Join-Path $tree 'server\shadow-bug-reports'
    $null = New-Item -ItemType Directory -Force -Path $worker
    foreach ($name in 'src', 'package.json', 'wrangler.toml') {
        Copy-Item -Recurse -LiteralPath (Join-Path $root "server\shadow-bug-reports\$name") -Destination $worker
    }
    if ($NamespaceId) {
        $config = Join-Path $worker 'wrangler.toml'
        (Get-Content -Raw -LiteralPath $config) -replace '(?m)^namespace_id = ".*"$', "namespace_id = `"$NamespaceId`"" |
            Set-Content -NoNewline -LiteralPath $config
    }
    return $tree
}

function Invoke-Worker([string]$Tree, [string[]]$Arguments = @(), [int]$NpxCode = 0, [switch]$NoNpx) {
    $log = Join-Path $Tree 'npx.log'
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    $env:SOTB_FAKE_NPX_LOG = $log
    $env:SOTB_FAKE_NPX_CODE = "$NpxCode"
    $savedPath = $env:PATH
    if ($NoNpx) { $env:PATH = Join-Path $env:SystemRoot 'System32' }
    $outFile = Join-Path $Tree 'out.txt'
    $errFile = Join-Path $Tree 'err.txt'
    $process = Start-Process -FilePath $shell -ArgumentList (@('-NoProfile', '-NonInteractive', '-File', (Join-Path $Tree 'tools\worker.ps1')) + $Arguments) `
        -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $env:PATH = $savedPath
    [pscustomobject]@{
        Code = $process.ExitCode
        Out = [System.IO.File]::ReadAllText($outFile)
        Err = [System.IO.File]::ReadAllText($errFile)
        Cwd = if (Test-Path -LiteralPath "$log.cwd") { [System.IO.File]::ReadAllText("$log.cwd").Trim() } else { $null }
        Npx = if (Test-Path -LiteralPath $log) { [System.IO.File]::ReadAllText($log).Trim() } else { $null }
    }
}

try {
    $shim = Join-Path $temp 'bin'
    $null = New-Item -ItemType Directory -Force -Path $shim
    Set-Content -Encoding ASCII -LiteralPath (Join-Path $shim 'npx.cmd') -Value @('@echo off', '>>"%SOTB_FAKE_NPX_LOG%" echo %*', '>>"%SOTB_FAKE_NPX_LOG%.cwd" echo %CD%', 'exit /b %SOTB_FAKE_NPX_CODE%')
    $env:PATH = "$shim;$env:PATH"

    $shipped = Invoke-Worker (New-Tree 'shipped' $null) @('-DryRun')
    Check ($shipped.Code -eq 0 -and $shipped.Npx -eq 'wrangler deploy --dry-run') 'the shipped wrangler.toml passes the checks and reaches Wrangler'
    Check ((Get-Content -Raw -LiteralPath (Join-Path $root 'server\shadow-bug-reports\wrangler.toml')) -match '(?m)^namespace_id = "5006"$') 'the shipped namespace_id is 5006'

    $placeholder = Invoke-Worker (New-Tree 'placeholder' 'SET_UNIQUE_SHADOW_RATE_LIMIT_NAMESPACE_ID')
    Check ($placeholder.Code -eq 1 -and $placeholder.Err -match 'namespace_id' -and $null -eq $placeholder.Npx -and $placeholder.Out -notmatch 'deployed') 'placeholder: fails, Wrangler never runs'
    $placeholderDry = Invoke-Worker (New-Tree 'placeholder-dry' 'REPLACE_ME') @('-DryRun')
    Check ($placeholderDry.Code -eq 1 -and $null -eq $placeholderDry.Npx -and $placeholderDry.Out -notmatch 'dry run') 'placeholder: a dry run fails the same way'

    $dry = Invoke-Worker (New-Tree 'dry' '4711') @('-DryRun')
    Check ($dry.Code -eq 0 -and $dry.Npx -eq 'wrangler deploy --dry-run') 'dry run: only wrangler deploy --dry-run is invoked'
    Check ($dry.Out -match 'nothing uploaded' -and $dry.Out -notmatch 'Worker deployed') 'dry run: says nothing was uploaded and never says deployed'

    $real = Invoke-Worker (New-Tree 'real' '4711')
    Check ($real.Code -eq 0 -and $real.Npx -eq 'wrangler deploy') 'deploy: only wrangler deploy is invoked'
    Check ($real.Out -match 'Worker deployed' -and $real.Out -notmatch 'nothing uploaded') 'deploy: says the Worker was deployed'

    $broken = Invoke-Worker (New-Tree 'broken' '4711') @() 3
    Check ($broken.Code -eq 3 -and $broken.Out -notmatch 'Worker deployed' -and $broken.Err -match 'exited with code 3') 'a Wrangler failure is passed on and not reported as deployed'

    $missing = New-Tree 'missing-module' '4711'
    Remove-Item -LiteralPath (Join-Path $missing 'server\shadow-bug-reports\src\limits.js')
    $lost = Invoke-Worker $missing
    Check ($lost.Code -eq 1 -and $lost.Err -match 'limits\.js' -and $null -eq $lost.Npx) 'a missing imported module stops the deploy before Wrangler'

    $noEntry = New-Tree 'no-entry' '4711'
    Remove-Item -LiteralPath (Join-Path $noEntry 'server\shadow-bug-reports\src\worker.js')
    Check ((Invoke-Worker $noEntry).Code -eq 1) 'a missing entry point stops the deploy'

    $noConfig = New-Tree 'no-config' '4711'
    Remove-Item -LiteralPath (Join-Path $noConfig 'server\shadow-bug-reports\wrangler.toml')
    Check ((Invoke-Worker $noConfig).Code -eq 1) 'a missing wrangler.toml stops the deploy'

    $noBinding = New-Tree 'no-binding' '4711'
    $config = Join-Path $noBinding 'server\shadow-bug-reports\wrangler.toml'
    (Get-Content -Raw -LiteralPath $config) -replace '(?m)^namespace_id = ".*"$', '' | Set-Content -NoNewline -LiteralPath $config
    Check ((Invoke-Worker $noBinding).Code -eq 1) 'a configuration without the rate-limit binding stops the deploy'

    $worker = 'server\shadow-bug-reports'
    $noFolder = New-Tree 'no-folder' '4711'
    Remove-Item -Recurse -Force -LiteralPath (Join-Path $noFolder $worker)
    $r = Invoke-Worker $noFolder
    Check ($r.Code -eq 1 -and $r.Err -match '^worker: missing folder' -and $null -eq $r.Npx) 'a missing Worker folder stops the deploy'

    $noPackage = New-Tree 'no-package' '4711'
    Remove-Item -LiteralPath (Join-Path $noPackage "$worker\package.json")
    $r = Invoke-Worker $noPackage
    Check ($r.Code -eq 1 -and $r.Err -match 'package\.json' -and $null -eq $r.Npx) 'a missing package.json stops the deploy'

    $noMain = New-Tree 'no-main' '4711'
    $toml = Join-Path $noMain "$worker\wrangler.toml"
    (Get-Content -Raw -LiteralPath $toml) -replace '(?m)^main = ".*"$', '' | Set-Content -NoNewline -LiteralPath $toml
    $r = Invoke-Worker $noMain
    Check ($r.Code -eq 1 -and $r.Err -match 'no main entry' -and $null -eq $r.Npx) 'a configuration without main stops the deploy'

    $r = Invoke-Worker $noEntry
    Check ($r.Err -match 'entry point' -and $null -eq $r.Npx) 'a missing entry point is named and Wrangler never runs'

    $noImport = New-Tree 'no-import' '4711'
    Remove-Item -LiteralPath (Join-Path $noImport "$worker\src\api.js")
    $r = Invoke-Worker $noImport
    Check ($r.Code -eq 1 -and $r.Err -match 'api\.js' -and $null -eq $r.Npx) 'another missing import stops the deploy'

    foreach ($bad in 'abc', '12', '0123', '1234567890') {
        $r = Invoke-Worker (New-Tree "bad-$bad" $bad)
        Check ($r.Code -eq 1 -and $r.Err -match 'namespace_id' -and $null -eq $r.Npx) "namespace_id '$bad' is refused"
    }
    foreach ($marker in 'REPLACE_ME', 'SET_UNIQUE_X', 'TODO', 'CHANGEME', 'PLACEHOLDER', 'YOUR_ID') {
        $r = Invoke-Worker (New-Tree "marker-$marker" $marker) @('-DryRun')
        Check ($r.Code -eq 1 -and $null -eq $r.Npx) "namespace_id '$marker' is refused"
    }
    $other = New-Tree 'other-placeholder' '4711'
    $toml = Join-Path $other "$worker\wrangler.toml"
    (Get-Content -Raw -LiteralPath $toml) -replace '(?m)^name = ".*"$', 'name = "YOUR_WORKER_NAME"' | Set-Content -NoNewline -LiteralPath $toml
    $r = Invoke-Worker $other
    Check ($r.Code -eq 1 -and $r.Err -match 'placeholder' -and $null -eq $r.Npx) 'another placeholder in wrangler.toml stops the deploy'

    $r = Invoke-Worker (New-Tree 'no-npx' '4711') @() 0 -NoNpx
    Check ($r.Code -eq 1 -and $r.Err -match 'npx not found' -and $null -eq $r.Npx) 'a missing npx is reported'

    $cwdTree = New-Tree 'cwd' '4711'
    $r = Invoke-Worker $cwdTree
    Check ($r.Cwd -eq (Join-Path $cwdTree $worker)) 'Wrangler runs inside the Worker folder'
    Check ($r.Out -match 'SOTB_BUG_REPORT_ADMIN_TOKEN' -and $r.Out -notmatch 'secret put') 'the next-step note names the secret and sets nothing'

    $failed = Invoke-Worker (New-Tree 'exit-7' '4711') @('-DryRun') 7
    Check ($failed.Code -eq 7 -and $failed.Out -notmatch 'nothing uploaded') 'a Wrangler failure keeps its exit code in a dry run'

    $lines = Get-Content -LiteralPath (Join-Path $root 'tools\worker.ps1') | Where-Object { $_.TrimStart() -notmatch '^#' }
    Check (-not ($lines -match 'secret\s+(put|bulk)|CLOUDFLARE_API_TOKEN|account_id|Bearer')) 'the script sets no secret and holds no credential'
    Check (-not ($lines -match 'wrangler\s+(delete|rollback|versions)|--force')) 'the script only deploys'
} finally {
    Remove-Item Env:\SOTB_FAKE_NPX_LOG, Env:\SOTB_FAKE_NPX_CODE -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force -LiteralPath $temp -ErrorAction SilentlyContinue
}

Write-Host "worker script: $checks checks, $failures failed"
if ($failures) { exit 1 }
exit 0
