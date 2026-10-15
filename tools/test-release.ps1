param([string[]]$Configurations=@('Debug','Release'))
$ErrorActionPreference='Stop'
. "$PSScriptRoot/ReleaseCommon.ps1"
$msbuild=Find-MSBuild
$run=Join-Path $RepositoryRoot ('tests/runs/release-validation-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $run | Out-Null
$summary=@()
Push-Location $RepositoryRoot
try {
 foreach($cfg in $Configurations) {
  if($cfg -notin @('Debug','Release')){throw "Unsupported configuration: $cfg"}
  & $msbuild FileTransferApp.slnx /m /t:Rebuild "/p:Configuration=$cfg" /p:Platform=x64 /v:minimal *> "$run/build-$cfg.log"
  if($LASTEXITCODE -ne 0){throw "Build failed: $cfg. See $run"}
  & $msbuild FileTransferApp/FileTransferApp.vcxproj /m "/p:Configuration=$cfg" /p:Platform=x64 "/p:ForceImportBeforeCppTargets=$RepositoryRoot/tests/Stage4Faults.props" "/p:OutDir=$run/fault/$cfg/bin/" "/p:IntDir=$run/fault/$cfg/obj/" /v:minimal *> "$run/fault-$cfg.log"
  if($LASTEXITCODE -ne 0){throw "Fault build failed: $cfg"}
  $cli="x64/$cfg/FileTransferApp.exe";$gui="FileTransferApp/x64/$cfg/FileTransferAppGui.exe"
  foreach($test in @('stage1','stage2','stage3','stage3_validation','stage4','stage5_cancel','stage5_reconnect','stage5_resume','stage5_boundaries','stage6_unicode','stage7_gui','stage7_advanced','stage7_1_download','stage8_lan','gui_scroll','chat_auto_receive','auto_receive_wire','trusted_peers','metadata_storage')) {
   $exe=if($test -in @('stage1','stage2','stage3','stage3_validation','stage4','stage5_cancel','stage5_reconnect','stage5_resume','stage5_boundaries','stage6_unicode','metadata_storage')){$cli}else{$gui}
   $argsList=@('-NoProfile','-ExecutionPolicy','RemoteSigned','-File',"tests/$test.ps1",'-Exe',$exe)
   if($test -eq 'stage4'){$argsList+=@('-FaultExe',"$run/fault/$cfg/bin/FileTransferApp.exe")}
   if($test -eq 'stage7_1_download'){$argsList+=@('-Cli',$cli)}
   $started=Get-Date
   $ErrorActionPreference='Continue'
   & powershell @argsList *> "$run/$test-$cfg.log"
   $ErrorActionPreference='Stop'
   $exitCode=$LASTEXITCODE
   $expected=@{stage1=12;stage2=20;stage3=27;stage3_validation=16;stage4=19;stage5_cancel=6;stage5_reconnect=4;stage5_resume=12;stage5_boundaries=5;stage6_unicode=43;stage7_gui=10;stage7_advanced=8;stage7_1_download=17;stage8_lan=25;gui_scroll=32;chat_auto_receive=16;auto_receive_wire=15;trusted_peers=14;metadata_storage=5}[$test]
   $count=@(Select-String -LiteralPath "$run/$test-$cfg.log" -Pattern '^PASS:').Count

   $summary += [PSCustomObject]@{configuration=$cfg;test=$test;exitCode=$exitCode;pass=$count;expected=$expected}
   $summary | ConvertTo-Json | Set-Content "$run/results.json" -Encoding UTF8
   Write-Output "$cfg $test exit=$exitCode PASS=$count/$expected"
   if($exitCode -ne 0 -or $count -ne $expected){
    Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-CodeIntegrity/Operational';StartTime=$started} -ErrorAction SilentlyContinue | Select-Object TimeCreated,Id,Message | ConvertTo-Json | Set-Content "$run/CodeIntegrity-$test-$cfg.json" -Encoding UTF8
    Write-Warning "Validation failed/blocked: $cfg $test. Continuing independent suites; packaging remains blocked. Logs: $run"
   }
  }
 }
 if(@($summary | Where-Object {$_.exitCode -ne 0 -or $_.pass -ne $_.expected}).Count){throw "Validation incomplete/failed. No package or release allowed. Logs: $run"}
 Write-Output ('Total PASS: '+(($summary | Measure-Object pass -Sum).Sum))
 Write-Output "Validation logs: $run"
} finally {Pop-Location}
