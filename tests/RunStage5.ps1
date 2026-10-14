param([Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$FaultExe)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path; $FaultExe = (Resolve-Path $FaultExe).Path
$resultsRoot = Join-Path $PSScriptRoot ('runs/suite-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $resultsRoot | Out-Null
$results = @()
foreach($test in @('stage1','stage2','stage3','stage3_validation','stage4','stage5_cancel','stage5_reconnect','stage5_resume','stage5_boundaries')) {
    $argumentsList = @('-NoProfile','-ExecutionPolicy','RemoteSigned','-File',"$PSScriptRoot/$test.ps1",'-Exe',$Exe)
    if($test -eq 'stage4'){ $argumentsList += @('-FaultExe',$FaultExe) }
    $started = Get-Date
    $ErrorActionPreference = 'Continue'
    $output = & powershell @argumentsList 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $output | Out-File -LiteralPath (Join-Path $resultsRoot ($test + '.log')) -Encoding utf8
    $count = @($output | Where-Object { $_.ToString().StartsWith('PASS:') }).Count
    $results += @{test=$test; exitCode=$code; pass=$count; seconds=((Get-Date)-$started).TotalSeconds}
    $results | ConvertTo-Json | Set-Content (Join-Path $resultsRoot 'results.json') -Encoding utf8
    Write-Output "$test exit=$code pass=$count results=$resultsRoot"
    if($code -ne 0) {
        Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-CodeIntegrity/Operational';StartTime=$started} -ErrorAction SilentlyContinue |
            Select-Object TimeCreated,Id,Message | ConvertTo-Json -Depth 4 |
            Set-Content (Join-Path $resultsRoot ($test + '-CodeIntegrity.json')) -Encoding utf8
        throw "Test failed or execution blocked: $test. See recorded log and events. No push authorized."
    }
}
Write-Output ('Total PASS: ' + (($results | ForEach-Object { $_['pass'] } | Measure-Object -Sum).Sum))
