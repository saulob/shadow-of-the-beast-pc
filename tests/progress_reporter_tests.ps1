# Checks of tools\_progress.ps1 with synthetic time: nothing sleeps, no process is started. -Show prints the synthetic long group.
param([switch]$Show)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '..\tools\_progress.ps1')
$failures = 0
$checks = 0

function Check([bool]$condition, [string]$what) {
    $script:checks++
    if (-not $condition) { $script:failures++; Write-Host "FAIL $what" }
}

function Equal($actual, $expected, [string]$what) {
    $script:checks++
    if ("$actual" -ne "$expected") { $script:failures++; Write-Host "FAIL $what`n  expected: $expected`n  actual:   $actual" }
}

function New-Capture { ,(New-Object System.Collections.Generic.List[string]) }

function New-Reporter([string[]]$Names, [bool]$Interactive = $false, [double]$Interval = 0, $Sink = $null) {
    if ($null -eq $Sink) { $Sink = New-Capture }
    $writer = { param([string]$Text, [bool]$NoNewline) $Sink.Add($Text) }.GetNewClosure()
    New-ProgressReporter $Names 0 $Interactive $writer $Interval
}

function Names([int]$Count) { 1..$Count | ForEach-Object { "group-$_" } }

function Finish($R, [string]$State, [double]$Start, [double]$End) {
    Start-ProgressGroup $R $R.Names[$R.Done] $Start
    Complete-ProgressGroup $R $State $End
}

# Percentage
Equal (Get-ProgressPercent 0 14) 0 'zero completed'
Equal (Get-ProgressPercent 9 14) 64 '9 of 14'
Equal (Get-ProgressPercent 3 14 0.5 $true) 25 'partial group counts'
Equal (Get-ProgressPercent 3 14 0.5 $false) 21 'partial group ignored when not running'
Equal (Get-ProgressPercent 14 14) 100 'complete'
Equal (Get-ProgressPercent 13 14 0.99 $true) 99 'last running group never reaches 100'
Equal (Get-ProgressPercent 0 0) 0 'zero groups'
Equal (Get-ProgressPercent 5 0) 0 'zero groups with done'
Equal (Get-ProgressPercent -3 10) 0 'negative done'
Equal (Get-ProgressPercent 30 10) 100 'done above total'
Equal (Get-ProgressPercent 1 10 ([double]::NaN) $true) 10 'NaN fraction'
Equal (Get-ProgressPercent 5 29) 17 'percent of 29 groups'
Equal (Get-ProgressPercent 5 20) 25 'percent of 20 groups'
Equal (Get-ProgressPercent 23 30) 76 'percent of 30 groups'

# Bar
Equal (Format-ProgressBar 0) '[------------------------]' 'bar 0'
Equal (Format-ProgressBar 63) '[###############---------]' 'bar 63'
Equal (Format-ProgressBar 21) '[#####-------------------]' 'bar 21'
Equal (Format-ProgressBar 25) '[######------------------]' 'bar 25'
Equal (Format-ProgressBar 100) '[########################]' 'bar 100'
Equal (Format-ProgressBar 99) '[#######################-]' 'bar 99 is not full'
Equal (Format-ProgressBar -20) '[------------------------]' 'bar below 0'
Equal (Format-ProgressBar 400) '[########################]' 'bar above 100'
Equal (Format-ProgressBar 50 10) '[#####-----]' 'bar custom width'
Equal ((Format-ProgressBar 37).Length) 26 'bar width is fixed'

# Durations
Equal (Format-Elapsed 0) '0m 00s' 'elapsed zero'
Equal (Format-Elapsed 4) '0m 04s' 'elapsed 4s'
Equal (Format-Elapsed 59.9) '0m 59s' 'elapsed below a minute'
Equal (Format-Elapsed 60) '1m 00s' 'elapsed minute boundary'
Equal (Format-Elapsed 1902) '31m 42s' 'elapsed 31m 42s'
Equal (Format-Elapsed 3599) '59m 59s' 'elapsed below an hour'
Equal (Format-Elapsed 3600) '1h 00m 00s' 'elapsed hour boundary'
Equal (Format-Elapsed 7384) '2h 03m 04s' 'elapsed long'
Equal (Format-Elapsed -5) '0m 00s' 'elapsed negative'
Equal (Format-Elapsed ([double]::NaN)) '0m 00s' 'elapsed NaN'
Equal (Format-Elapsed ([double]::PositiveInfinity)) '99h 00m 00s' 'elapsed infinite is capped'
Equal (Format-ShortDuration 1.24) '1.2s' 'short duration'
Equal (Format-ShortDuration 59.94) '59.9s' 'short duration below a minute'
Equal (Format-ShortDuration 61) '1m 01s' 'short duration above a minute'
Equal (Format-Count 1284220) '1,284,220' 'count grouping'
Equal (Format-Count 0) '0' 'count zero'
Equal (Format-Count -4) '0' 'count never negative'

# ETA text
Equal (Format-Eta $null) 'calculating' 'eta unknown'
Equal (Format-Eta ([double]::NaN)) 'calculating' 'eta NaN'
Equal (Format-Eta ([double]::PositiveInfinity)) 'calculating' 'eta infinite'
Equal (Format-Eta 14) '~14s' 'eta seconds'
Equal (Format-Eta 0) '~2s' 'eta zero'
Equal (Format-Eta -9) '~2s' 'eta negative'
Equal (Format-Eta 450) '~7m 30s' 'eta minutes and seconds'
Equal (Format-Eta 270) '~4m 30s' 'eta 4m 30s'
Equal (Format-Eta 240) '~4m' 'eta whole minutes'
Equal (Format-Eta 1085) '~18m' 'eta rounds to the minute'
Equal (Format-Eta 3900) '~1h 05m' 'eta hours'
Equal (Format-Eta 1e12) '~99h 00m' 'eta capped'

# Checks text
Equal (Format-ChecksText 1284220 2039231 $false) '1,284,220 / 2,039,231' 'checks exact'
Equal (Format-ChecksText 1284220 2039231 $true) '1,284,220 / ~2,039,231' 'checks estimated'
Equal (Format-ChecksText 12845 $null $false) '12,845' 'checks current only'
Equal (Format-ChecksText $null $null $false) 'unavailable' 'checks unavailable'
Equal (Format-ChecksText $null 500 $false) 'unavailable' 'expected without current'
Equal (Format-ChecksText 900 500 $true) '900' 'estimate exceeded shows current only'

# Group count comes from the list; indexing; previous group
foreach ($count in 1, 5, 20, 29) {
    $r = New-Reporter (Names $count)
    Equal $r.Total $count "total from list ($count)"
}
$r = New-Reporter (Names 5)
Equal ((Format-ProgressBlock $r 0)[0]) '[------------------------] 0% | 0/5 groups' 'no group completed'
Equal ((Format-ProgressBlock $r 0)[-1]) 'Previous: none' 'previous none'
Equal ((Format-ProgressBlock $r 0)[-2]) 'Elapsed: 0m 00s | ETA: calculating' 'waiting block has no group time'
Finish $r 'PASS' 0 128
$block = Format-ProgressBlock $r 130
Equal $block[0] '[####--------------------] 20% | 1/5 groups' 'one of five'
Equal $block[-1] 'Previous: group-1 PASS  2m 08s' 'previous PASS with duration'
Start-ProgressGroup $r 'group-2' 130
$block = Format-ProgressBlock $r 160
Equal $block[1] 'group-2' 'current group name'
Equal $block[2] 'Elapsed: 2m 40s | Group: 0m 30s | ETA: ~8m' 'times with eta from completed groups'
Equal $block[3] 'Checks: unavailable' 'no check data'
Check ($block.Count -eq 5) 'block lines while running'
Complete-ProgressGroup $r 'FAIL' 170
Equal ((Format-ProgressBlock $r 170)[-1]) 'Previous: group-2 FAIL  0m 40s' 'previous FAIL'
Finish $r 'SKIPPED' 170 170
Equal ((Format-ProgressBlock $r 170)[-1]) 'Previous: group-3 SKIPPED  0m 00s' 'previous SKIPPED'
Equal $r.Done 3 'skipped counts as completed'

# ETA
$r = New-Reporter (Names 4)
Check ($null -eq (Get-ProgressEta $r 0)) 'eta before any group'
Start-ProgressGroup $r 'group-1' 0
Check ($null -eq (Get-ProgressEta $r 10)) 'eta while the first group runs'
Complete-ProgressGroup $r 'PASS' 100
Start-ProgressGroup $r 'group-2' 100
Equal ([Math]::Round((Get-ProgressEta $r 130))) 270 'eta from average: rest of this group plus two more'
Equal ([Math]::Round((Get-ProgressEta $r 400))) 140 'eta when the group overruns the average (smoothed)'
$r = New-Reporter (Names 3)
Finish $r 'SKIPPED' 0 0
Check ($null -eq (Get-ProgressEta $r 5)) 'skipped groups carry no timing'
$r = New-Reporter (Names 2)
Finish $r 'PASS' 0 50
Start-ProgressGroup $r 'group-2' 50
Complete-ProgressGroup $r 'PASS' 100
Equal (Get-ProgressEta $r 100) 0 'eta after the last group is zero'
$r = New-Reporter (Names 100)
Finish $r 'PASS' 0 1e9
Start-ProgressGroup $r 'group-2' 1e9
Check ((Get-ProgressEta $r 1e9) -le 99 * 3600) 'eta never above the cap'
$r = New-Reporter (Names 10)
Finish $r 'PASS' 0 60
Start-ProgressGroup $r 'group-2' 60
$null = Set-ProgressChecks $r 60 0 1000 $false
$r.Checks = 500
Equal ([Math]::Round((Get-ProgressEta $r 90))) 510 'eta from measured group progress'
$samples = 0..20 | ForEach-Object { $r.Checks = 100 + 20 * $_; Get-ProgressEta $r (62 + 2 * $_) }
$never = @($samples | Where-Object { $_ -lt 0 -or [double]::IsNaN($_) -or [double]::IsInfinity($_) })
Equal $never.Count 0 'eta never negative, NaN or infinite'
$jump = 0.0; for ($i = 1; $i -lt $samples.Count; $i++) { $jump = [Math]::Max($jump, [Math]::Abs($samples[$i] - $samples[$i - 1])) }
Check ($jump -lt 120) 'eta does not oscillate violently'

# Check variants in the block
$r = New-Reporter (Names 14)
Start-ProgressGroup $r 'group-1' 0
$null = Set-ProgressChecks $r 1 12845
Equal (Format-ProgressBlock $r 1)[3] 'Checks: 12,845' 'block current only'
$null = Set-ProgressChecks $r 1 1284220 2039231 $false
Equal (Format-ProgressBlock $r 1)[3] 'Checks: 1,284,220 / 2,039,231' 'block exact'
$null = Set-ProgressChecks $r 1 1284220 2039231 $true
Equal (Format-ProgressBlock $r 1)[3] 'Checks: 1,284,220 / ~2,039,231' 'block estimated'
Complete-ProgressGroup $r 'PASS' 5
Check ($null -eq $r.Checks -and $null -eq $r.Expected) 'check state cleared between groups'
Equal $r.Results[0].Checks 1284220 'group keeps its last check count'
$child = ConvertFrom-ChildProgress '@@sotb-progress checks=1234'
Equal $child.Checks 1234 'child progress line'
Check ($null -eq $child.Expected) 'child line without total'
$child = ConvertFrom-ChildProgress '@@sotb-progress checks=5 total=10 estimate=1'
Check ($child.Expected -eq 10 -and $child.Estimate) 'child line with estimated total'
Check ($null -eq (ConvertFrom-ChildProgress 'isa tests passed: 5 checks')) 'ordinary line is not progress'
Check ($null -eq (ConvertFrom-ChildProgress '@@sotb-progress checks=abc')) 'malformed progress line ignored'
Equal (Get-FinalCheckCount @('x', 'isa tests passed: 123456 checks')) 123456 'final check count'
Check ($null -eq (Get-FinalCheckCount @('nothing here'))) 'no final check count'

# Completion
$r = New-Reporter (Names 3)
Finish $r 'PASS' 0 1; Finish $r 'PASS' 1 2
Start-ProgressGroup $r 'group-3' 2
Check ((Format-ProgressBlock $r 3)[0] -notmatch '100%') 'last group running is not 100%'
Complete-ProgressGroup $r 'PASS' 4
Equal (Format-ProgressBlock $r 4)[0] '[########################] 100% | 3/3 groups' 'final completion'
Equal (Format-ProgressBlock $r 4)[1] 'complete' 'final block name'
$r = New-Reporter (Names 3)
Finish $r 'PASS' 0 1; Finish $r 'FAIL' 1 2
Check ((Format-ProgressBlock $r 2)[0] -notmatch '100%') 'interrupted run is not 100%'
Check ((Format-TimingSummary $r 2) -contains 'Incomplete: 2/3 groups finished') 'summary flags an incomplete run'
$r = New-Reporter @()
Equal (Format-ProgressBlock $r 0)[0] '[------------------------] 0% | 0/0 groups' 'zero groups'
Check ((Format-TimingSummary $r 0).Count -ge 3) 'zero-group summary'
Complete-ProgressGroup $r 'PASS' 1
Equal $r.Done 0 'completing with no running group is ignored'

# Cadence
$sink = New-Capture
$r = New-Reporter (Names 3) $false 15 $sink
Start-ProgressGroup $r 'group-1' 0
$base = $sink.Count
Check (-not (Update-Progress $r 14.9)) 'no update inside the interval'
Check (Update-Progress $r 15) 'update at the interval'
Check (-not (Update-Progress $r 16)) 'rate limit restarts'
Check (Update-Progress $r 30.5) 'next interval'
Equal ($sink.Count - $base) 2 'redirected updates are single text lines'
Check (-not ($sink | Where-Object { $_ -match "`r" })) 'redirected output has no carriage returns'
Check ($sink[0] -match '^\[[#-]{24}\] \d+% \| \d+/\d+ groups$') 'redirected first line is plain text'
Check ($sink[$base] -eq 'RUNNING group-1 | group 0m 15s | ETA calculating | checks unavailable') 'redirected heartbeat is a RUNNING line'
Check (@($sink | Where-Object { $_ -match '^\[[|/\\-]\]' }).Count -eq 0) 'redirected output has no spinner frames'
$null = Update-Progress $r 31 -Force
Equal ($sink.Count - $base) 3 'forced update'
$sink = New-Capture
$r = New-Reporter (Names 3) $true 0 $sink
Equal $r.Interval 0.5 'interactive default interval'
Equal (New-Reporter (Names 3) $false).Interval 15 'redirected default interval'
Start-ProgressGroup $r 'group-1' 0
Check (-not (Update-Progress $r 0.3)) 'no interactive update inside 500 ms'
Check (Update-Progress $r 0.5) 'interactive update'
Check ($sink[-1].StartsWith("`r")) 'interactive update redraws the line'
Clear-ProgressLine $r
Check ($sink[-1].StartsWith("`r") -and $sink[-1].Trim() -eq '') 'line cleared before normal output'
$last = $sink.Count
Clear-ProgressLine $r
Equal $sink.Count $last 'clearing twice writes nothing'
Check (-not (Update-Progress (New-Reporter (Names 2)) 100)) 'no update without a running group'

# Timing summary
$r = New-Reporter (Names 5)
$durations = 1.2, 258, 2.8, 77, 0.2
for ($i = 0; $i -lt 5; $i++) { Finish $r $(if ($i -eq 2) { 'SKIPPED' } else { 'PASS' }) 0 $durations[$i] }
$r.Results[1].Checks = 1284220
$summary = Format-TimingSummary $r 400
Equal $summary[0] 'Test timing' 'summary title'
$order = @($summary | Where-Object { $_ -match '^group-\d ' } | ForEach-Object { ($_ -split ' ')[0] })
Equal ($order -join ',') 'group-1,group-2,group-3,group-4,group-5' 'summary keeps run order'
Check ($summary -contains ('{0,-36} {1,-8} {2,8}' -f 'group-1', 'PASS', '1.2s')) 'summary row format'
Check (@($summary | Where-Object { $_ -match 'group-2 +PASS +4m 18s +1,284,220 checks$' }).Count -eq 1) 'summary shows check counts when known'
Check (@($summary | Where-Object { $_ -match '^Total +6m 40s$' }).Count -eq 1) 'summary total'
$slow = $summary[($summary.IndexOf('Slowest:') + 1)..($summary.Count - 1)]
Check ($slow[0] -match '^1. group-2 +4m 18s$') 'slowest first'
Check ($slow[1] -match '^2. group-4 +1m 17s$') 'slowest second'
Check ($slow[2] -match '^3. group-1 +0m 01s$') 'slowest third skips skipped groups'
Equal $slow.Count 3 'at most three slowest'
$r2 = New-Reporter (Names 3)
Finish $r2 'PASS' 0 5; Finish $r2 'PASS' 5 10
Check (-not ((Format-TimingSummary $r2 10) -contains 'Slowest:')) 'no slowest list below three groups'
Equal ((Get-SlowestGroups $r 3 | ForEach-Object { $_.Name }) -join ',') 'group-2,group-4,group-1' 'slowest ranking'
$tie = New-Reporter (Names 3)
Finish $tie 'PASS' 0 5; Finish $tie 'PASS' 5 10; Finish $tie 'PASS' 10 15
Equal ((Get-SlowestGroups $tie 3 | ForEach-Object { $_.Name }) -join ',') 'group-1,group-2,group-3' 'ties rank by name'

# Machine-readable output stays outside the reporter
$runner = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\tools\test.ps1')
foreach ($marker in "Write-Host '    PASS'", 'Write-Host "    FAIL: $($result.Reason)"', 'Write-Host "    SKIPPED: $($result.Reason)"', 'Write-Host "Failed: $failed"', 'Write-Step $step.Title') {
    Check ($runner.Contains($marker)) "runner still prints $marker"
}
Check ($runner.IndexOf('Write-Host "Failed: $failed"') -gt $runner.IndexOf('Format-TimingSummary')) 'Failed line stays last'
$names = @((Select-String -InputObject $runner -Pattern "(?m)^    @\{ Name = '([a-z-]+)'; Title" -AllMatches).Matches | ForEach-Object { $_.Groups[1].Value })
Equal $names.Count (@($names | Select-Object -Unique)).Count 'runner group names are unique'
Check ($names -contains 'progress-reporter') 'progress tests are registered in the runner'

# Spinner
foreach ($case in @(@(0, '|'), @(1, '/'), @(2, '-'), @(3, '\'), @(4, '|'), @(7, '\'), @(1000001, '/'), @(-1, '\'))) {
    Equal (Get-ProgressSpinnerFrame $case[0]) $case[1] "spinner frame $($case[0])"
}
$sink = New-Capture
$r = New-Reporter (Names 21) $true 0 $sink
foreach ($n in 1..6) { Finish $r 'PASS' 0 ($n * 4) }
Start-ProgressGroup $r 'guest-memory' 28
$percentBefore = (Format-ProgressBlock $r 30)[0]
$etaBefore = Get-ProgressEta $r 30
$frames = @(0..4 | ForEach-Object { $null = Update-Progress $r (28.5 + 0.5 * $_); $sink[-1].TrimStart("`r").TrimEnd() })
Equal (($frames | ForEach-Object { $_.Substring(0, 3) }) -join ' ') '[|] [/] [-] [\] [|]' 'interactive heartbeat advances one frame per refresh'
Check ($frames[0] -match '^\[\|\] \[#+-+\] \d+% \| 6/21 groups \| guest-memory \| group 0m 00s | ETA ~[0-9ms ]+ | checks unavailable$\| ETA ~\d+s \| checks unavailable$') 'interactive heartbeat format with unavailable checks'
Equal (Format-ProgressBlock $r 30)[0] $percentBefore 'spinner does not change the percentage'
Equal $r.Done 6 'spinner does not change completed groups'
Equal (Get-ProgressEta $r 30) $etaBefore 'spinner does not change the ETA'
$null = Set-ProgressChecks $r 31 820000
Check ((Format-ProgressLine $r 31 200) -match 'checks 820,000$') 'spinner with current checks'
$null = Set-ProgressChecks $r 31 820000 1000000 $false
Check ((Format-ProgressLine $r 31 200) -match 'checks 820,000 / 1,000,000$') 'spinner with exact total'
$null = Set-ProgressChecks $r 31 820000 1000000 $true
Check ((Format-ProgressLine $r 31 200) -match 'checks 820,000 / ~1,000,000$') 'spinner with estimated total'
Check ((Format-ProgressLine $r 31 200) -match '^\[.\] \[#+-+\] \d+% \| 6/21 groups \| guest-memory \| group 0m 03s') 'measured progress coexists with the spinner'
$short = Format-ProgressLine $r 31 40
Check ($short.Length -le 40 -and $short.StartsWith('['))  'heartbeat is truncated to the console width'
$narrow = Format-ProgressLine $r 31 100
Check ($narrow.Length -le 100 -and $narrow -match '^\[.\] \d+% 6/21 guest-memory \| group' -and $narrow.EndsWith('checks 820,000 / ~1,000,000')) 'a narrow console drops the bar, not the check count'
Complete-ProgressGroup $r 'PASS' 40
Start-ProgressGroup $r 'next-group' 40
Equal $r.Tick 0 'new group starts at tick 0'
$null = Update-Progress $r 40.5
Check ($sink[-1] -match '^\r\[\|\]') 'new group starts on the first frame'
$sink2 = New-Capture
$r2 = New-Reporter (Names 3) $true 0 $sink2
Start-ProgressGroup $r2 'group-1' 0
$null = Update-Progress $r2 0.5
Clear-ProgressLine $r2
Complete-ProgressGroup $r2 'FAIL' 61
Check (($sink2[-1].Trim() -eq '') -and -not $r2.LineDirty) 'failure leaves no stale spinner'
Check ((Format-ProgressBlock $r2 61) -join "`n" -notmatch '\[[|/\\-]\] ') 'blocks after a failure carry no spinner'
Check ((Format-TimingSummary $r2 61) -join "`n" -notmatch '\[[|/\\-]\]') 'timing summary has no spinner'
Equal (Format-ProgressBlock $r2 61)[-1] 'Previous: group-1 FAIL  1m 01s' 'previous after failure'
$zero = New-Reporter @() $true 0 (New-Capture)
Check (-not (Update-Progress $zero 5)) 'zero groups produce no heartbeat'
Equal (Format-ProgressBlock $zero 5)[0] '[------------------------] 0% | 0/0 groups' 'zero groups stay safe'
$partial = New-Reporter (Names 3) $true 0 (New-Capture)
Finish $partial 'PASS' 0 1
Start-ProgressGroup $partial 'group-2' 1
$null = Update-Progress $partial 2
Check ((Format-ProgressLine $partial 2 200) -notmatch '100%') 'interrupted run shows no 100% on the heartbeat'
$runner = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\tools\test.ps1')
Check ($runner.Contains('Write-Host "Failed: $failed"') -and $runner.Contains('if ($failed) { exit 1 }')) 'Failed line and exit code unchanged'

# Synthetic long group, redirected: 14 groups, group 4 runs 1,000,000 checks over 20 synthetic seconds
$sink = New-Capture
$r = New-Reporter (Names 14) $false 2 $sink
Finish $r 'PASS' 0 1; Finish $r 'PASS' 1 3; Finish $r 'PASS' 3 6
Start-ProgressGroup $r 'Synthetic long test' 6
$sink.Clear()
for ($t = 7; $t -le 26; $t++) { $null = Set-ProgressChecks $r $t ([long](($t - 6) * 50000)) 1000000 $false }
$lines = @($sink)
Check ($lines.Count -ge 5 -and $lines.Count -le 12) 'a long group produces several rate-limited updates'
$counts = @($lines | ForEach-Object { [long](($_ -replace '.*checks ([\d,]+) /.*', '$1') -replace ',', '') })
$ascending = $true; for ($i = 1; $i -lt $counts.Count; $i++) { if ($counts[$i] -le $counts[$i - 1]) { $ascending = $false } }
Check $ascending 'check progress advances between updates'
Check ($lines[0] -match '^RUNNING Synthetic long test \| group 0m 02s \| ETA ~\d+s \| checks 100,000 / 1,000,000$') 'redirected synthetic heartbeat'
Complete-ProgressGroup $r 'PASS' 26
Check ((Format-ProgressBlock $r 26)[-1] -eq 'Previous: Synthetic long test PASS  0m 20s') 'previous group after the synthetic run'

# Synthetic animation: the running group's checks change only once in the whole window
$sink = New-Capture
$r = New-Reporter (Names 14) $true 0 $sink
$r.Width = 200
Finish $r 'PASS' 0 1; Finish $r 'PASS' 1 3; Finish $r 'PASS' 3 6
Start-ProgressGroup $r 'Synthetic long test' 6
$null = Set-ProgressChecks $r 6 250000 1000000 $false
$sink.Clear()
$frames = @()
for ($step = 1; $step -le 12; $step++) {
    $now = 6 + 0.5 * $step
    $null = if ($step -eq 9) { Set-ProgressChecks $r $now 500000 1000000 $false } else { Update-Progress $r $now }
    if ($sink.Count) { $frames += $sink[-1].TrimStart("`r").TrimEnd(); $sink.Clear() }
}
Equal $frames.Count 12 'one interactive refresh per 500 ms step'
Equal (($frames | ForEach-Object { $_[1] }) -join '') '|/-\|/-\|/-\' 'spinner keeps cycling when nothing else changes'
$held = @($frames[0..7] | ForEach-Object { $_.Substring(4) -replace 'group \d+m \d+s \| ETA ~\d+s', '' } | Select-Object -Unique)
Equal $held.Count 1 'percentage and checks stay put while the spinner moves'
Check ($frames[8] -match 'checks 500,000 / 1,000,000') 'real check progress still replaces the count'
if ($Show) { $frames }
Complete-ProgressGroup $r 'PASS' 12
Clear-ProgressLine $r

if ($failures) { Write-Host "progress reporter tests failed: $failures of $checks checks"; exit 1 }
Write-Host "progress reporter tests passed: $checks checks"
exit 0
