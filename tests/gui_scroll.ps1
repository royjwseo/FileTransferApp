param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Release/FileTransferAppGui.exe")
$ErrorActionPreference='Stop'
Add-Type -Path "$PSScriptRoot/GuiDriver.cs"
Add-Type -Path "$PSScriptRoot/ScrollProbe.cs" -ReferencedAssemblies System.Drawing
$run=Join-Path $PSScriptRoot ('runs/scroll-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $run | Out-Null
function Check($ok,$text){if(!$ok){throw $text};Write-Output "PASS: $text"}
foreach($legacy in @($true,$false)){
 $dir=Join-Path $run ([string]$legacy);New-Item -ItemType Directory $dir | Out-Null
 $app=[GuiDriver]::new((Resolve-Path $Exe).Path,$dir,$true,$legacy)
 try {
  $h=$app.Window;[ScrollProbe]::Display($h);$app.Resize(900,500)
  [ScrollProbe]::Bar($h,6);$base=[ScrollProbe]::Y($h,101)
  for($i=0;$i -lt 40;$i++){[ScrollProbe]::Wheel($h,-120);[ScrollProbe]::Wheel($h,120)}
  Check (([ScrollProbe]::State($h)).pos -eq 0) "wheel cycles return to top legacy=$legacy"
  $s=[ScrollProbe]::State($h);[ScrollProbe]::Bar($h,3);$page=[ScrollProbe]::State($h)
  [ScrollProbe]::Bar($h,2)
  Check ($page.pos -eq [Math]::Min([int]$s.page,[Math]::Max(0,$s.max-[int]$s.page+1)) -and ([ScrollProbe]::State($h)).pos -eq 0) "page down and up legacy=$legacy"
  [ScrollProbe]::Bar($h,1);$line=[ScrollProbe]::State($h);[ScrollProbe]::Bar($h,0)
  Check ($line.pos -gt 0 -and ([ScrollProbe]::State($h)).pos -eq 0) "line down and up legacy=$legacy"
  [ScrollProbe]::Bar($h,7);$s=[ScrollProbe]::State($h)
  Check ($s.pos -eq [Math]::Max(0,$s.max-[int]$s.page+1)) "bottom range clamp legacy=$legacy"
  Check (([ScrollProbe]::Y($h,101)) -eq $base-$s.pos) "single scroll offset legacy=$legacy"
  for($i=0;$i -lt 20;$i++){[ScrollProbe]::Bar($h,6);[ScrollProbe]::Bar($h,7)}
  Check (([ScrollProbe]::State($h)).pos -eq $s.pos) "repeated endpoints stable legacy=$legacy"
  $changes=[ScrollProbe]::CompareRepaint($h,(Join-Path $dir 'bottom'))
  Check ($changes -eq 0) "settled pixels agree with full repaint ($changes changed pixels) legacy=$legacy"
  [ScrollProbe]::Bar($h,6);[ScrollProbe]::Wheel($h,-120);$s=[ScrollProbe]::State($h)
  Check ($s.pos -gt 0 -and ([ScrollProbe]::Y($h,101)) -eq $base-$s.pos) "wheel down coordinate consistency legacy=$legacy"
  $changes=[ScrollProbe]::CompareRepaint($h,(Join-Path $dir 'middle'))
  Check ($changes -eq 0) "middle pixels agree with full repaint ($changes changed pixels) legacy=$legacy"
  foreach($height in @(460,700,1200,500)){[ScrollProbe]::Wheel($h,-120);$app.Resize(900,$height);$s=[ScrollProbe]::State($h);Check ($s.pos -le [Math]::Max(0,$s.max-[int]$s.page+1)) "resize clamp height=$height legacy=$legacy"}
  [ScrollProbe]::Restore($h);$app.Responsive();Check (!$app.Process.HasExited) "minimize restore responsive legacy=$legacy"
  $changes=[ScrollProbe]::CompareRepaint($h,(Join-Path $dir 'restored'))
  Check ($changes -eq 0) "restored pixels agree with full repaint ($changes changed pixels) legacy=$legacy"
  [ScrollProbe]::Bar($h,6);$app.Set(101,'127.0.0.2');Check ($app.Text(101) -eq '127.0.0.2') "input usable after scrolling legacy=$legacy"
  $app.Close()
 } finally {$app.Dispose()}
}
Write-Output "Scroll artifacts: $run"
