param([string]$Exe="$PSScriptRoot/../x64/Debug/FileTransferApp.exe")
$ErrorActionPreference='Stop'
. "$PSScriptRoot/stage3_validation.ps1" -Exe $Exe -HelpersOnly
function Restart-MetadataPair($pair){
 $hostDir=$pair.HostApp.Process.StartInfo.WorkingDirectory;$clientDir=$pair.ClientApp.Process.StartInfo.WorkingDirectory
 Save-Pair $pair
 $probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$probe.Start();$port=$probe.LocalEndpoint.Port;$probe.Stop()
 $h=$null;$c=$null
 try{$h=[TestApp]::new($Exe,$hostDir,"--host --bind 127.0.0.1 --port $port");$h.WaitText('Waiting for connection');$c=[TestApp]::new($Exe,$clientDir,"--connect 127.0.0.1 --port $port");$h.WaitText('Commands:');$c.WaitText('Commands:');return @{HostApp=$h;ClientApp=$c;Relay=$null;Name=$pair.Name;InstanceId=[Guid]::NewGuid().ToString('N')}}
 catch{if($h){$h.Dispose()};if($c){$c.Dispose()};throw}
}
foreach($case in @('central-complete','legacy-complete','legacy-part','central-corrupt','legacy-corrupt')){
 $source=Join-Path $runDir ($case+'.bin');$f=[IO.File]::Create($source);$f.SetLength(8MB);$f.Dispose();Record-SourceHash $source
 $fault=if($case -eq 'legacy-part'){'pause-chunks'}else{''};$pair=New-Pair $case $fault
 try{
  if($fault){$pair.Relay.PauseAfterBytes=262144}
  Start-Transfer $pair.ClientApp $pair.HostApp $source
  if($fault){Assert ($pair.Relay.WaitChunk(10000)) 'No interrupted checkpoint'}else{$pair.ClientApp.WaitText('[Transfer completed]',30000)}
  Assert ($pair.ClientApp.Output -match 'transferId=([a-f0-9]{64})') 'Missing transfer ID';$id=$Matches[1]
  $cwd=$pair.HostApp.Process.StartInfo.WorkingDirectory;$saved=Saved-Path $pair.HostApp ([IO.Path]::GetFileName($source));$folder=[IO.Path]::GetDirectoryName($saved)
  $metadata=[MetadataProbe]::PathFor($cwd,$folder,$id);$legacy=$saved+'.part.fta'
  Assert ((Test-Path $metadata) -and !(Test-Path $legacy)) 'New metadata leaked into download folder'
  $pair=Restart-MetadataPair $pair
  if($case -like 'legacy-*'){Move-Item -LiteralPath $metadata -Destination $legacy}
  if($case -eq 'legacy-corrupt'){[IO.File]::WriteAllBytes($legacy,[byte[]](1..100))}
  if($case -eq 'central-corrupt'){Copy-Item -LiteralPath $metadata -Destination $legacy;[IO.File]::WriteAllBytes($metadata,[byte[]](1..100))}
  $command=if($fault){'/resume'}else{'/status'};$pair.ClientApp.Send("$command $id `"$source`"");$pair.HostApp.WaitText('[Incoming file request]');$pair.HostApp.Send('/accept')
  if($case -like '*corrupt'){
   $pair.ClientApp.WaitText('File transfer not completed');Assert (Test-Path $legacy) 'Rejected metadata was removed';Assert-Hash $source $saved
   if($case -eq 'central-corrupt'){Assert ((Get-Item $metadata).Length -eq 100) 'Corrupt central metadata was replaced by legacy fallback'}else{Assert (!(Test-Path $metadata)) 'Unverified legacy record was migrated'}
  }else{
   $pair.ClientApp.WaitText('[Transfer completed]',30000);Assert-Hash $source $saved;Assert ((Test-Path $metadata) -and !(Test-Path $legacy)) 'Validated metadata did not migrate'
  }
  Stop-Pair $pair;Pass "private metadata storage and recovery: $case"
 }finally{Save-Pair $pair}
}
"Metadata logs: $runDir"
