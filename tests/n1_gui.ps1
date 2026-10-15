param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Debug/FileTransferAppGui.exe")
$ErrorActionPreference='Stop';. "$PSScriptRoot/n_to_1.ps1" -Exe $Exe -HelpersOnly
Add-Type -Path "$PSScriptRoot/ChatProbe.cs";Add-Type -Path "$PSScriptRoot/ScrollProbe.cs" -ReferencedAssemblies System.Drawing
foreach($server in @($false,$true)){
 $a=$null
 try{
  $a=App ('scroll-'+$server) $server ('scroll-'+$server);$g=$a.G;$h=$g.Window;[void][ChatProbe]::ShowWindow($h,5);$g.Resize(900,500)
  Assert ($g.Text(141).Contains('role='+$(if($server){'server'}else{'client'}))) 'Role not applied';Pass "N1 GUI profile controls initialize server=$server"
  [ScrollProbe]::Bar($h,6);$before=@{};foreach($id in @(149,150,152,154)){$before[$id]=[ScrollProbe]::Y($h,$id)}
  for($i=0;$i -lt 40;$i++){[ScrollProbe]::Wheel($h,-120);[ScrollProbe]::Wheel($h,120)};Assert (([ScrollProbe]::State($h)).pos -eq 0) 'Wheel drift';Pass "N1 GUI rapid wheel cycles return to top server=$server"
  [ScrollProbe]::Bar($h,7);$state=[ScrollProbe]::State($h);Assert ($state.pos -eq [Math]::Max(0,$state.max-[int]$state.page+1)) 'Bottom clamp wrong';foreach($id in @(149,150,152,154)){Assert (([ScrollProbe]::Y($h,$id)) -eq $before[$id]-$state.pos) 'New peer controls applied scroll offset incorrectly'};Pass "N1 GUI bottom range and new control coordinates agree server=$server"
  Assert (([ScrollProbe]::CompareRepaint($h,(Join-Path $a.Dir 'bottom'))) -eq 0) 'Settled bottom pixel mismatch';Pass "N1 GUI settled bottom pixels match repaint server=$server"
  foreach($height in @(460,1200,500)){[ScrollProbe]::Wheel($h,-120);$g.Resize(900,$height);$state=[ScrollProbe]::State($h);Assert ($state.pos -le [Math]::Max(0,$state.max-[int]$state.page+1)) 'Resize range wrong'};Pass "N1 GUI resize during scroll clamps position server=$server"
  [ScrollProbe]::Restore($h);$g.Responsive();Assert (!$g.Process.HasExited) 'Restore exited GUI';Pass "N1 GUI minimize and restore stays responsive server=$server"
  [ScrollProbe]::Bar($h,6);[ScrollProbe]::Wheel($h,-120);Assert (([ScrollProbe]::CompareRepaint($h,(Join-Path $a.Dir 'middle'))) -eq 0) 'Settled middle pixel mismatch';Pass "N1 GUI settled middle pixels match repaint server=$server"
  [ScrollProbe]::Bar($h,6);$g.Set(150,'editable-after-scroll');Assert ($g.Text(150) -eq 'editable-after-scroll') 'Name input not usable';Pass "N1 GUI profile input remains usable after scroll server=$server"
  $g.Close()
 }finally{if($a){try{$a.G.Dispose()}catch{}}}
}
"N1 GUI artifacts: $run"
