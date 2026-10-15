param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Release/FileTransferAppGui.exe")
$ErrorActionPreference='Stop';$Exe=(Resolve-Path $Exe).Path
Add-Type -Path "$PSScriptRoot/GuiDriver.cs"
Add-Type -Path "$PSScriptRoot/ChatProbe.cs"
Add-Type -Path "$PSScriptRoot/ScrollProbe.cs" -ReferencedAssemblies System.Drawing
$run=Join-Path $PSScriptRoot ('runs/chat-auto-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $run | Out-Null
function Assert($yes,$message){if(!$yes){throw $message}}
function Pass($message){Write-Output "PASS: $message"}
function App($name,$valid=$true){$dir=Join-Path $run $name;New-Item -ItemType Directory $dir | Out-Null;if(!$valid){[IO.File]::WriteAllText((Join-Path $dir 'gui-settings.txt'),'relative-invalid-path')};$g=[GuiDriver]::new($Exe,$dir,$true,$false);$g.Wait(141,'LAN ready:');Assert ($g.Text(141) -match 'TCP=(\d+)') 'Missing port';return @{G=$g;Dir=$dir;Port=[int]$Matches[1]}}
function Connect($a,$b){$a.G.Set(101,'127.0.0.1');$a.G.Set(102,[string]$b.Port);$a.G.Click(137);$b.G.Wait(138,'unverified');Assert (!$b.G.Enabled(110)) 'Text enabled before consent';$b.G.Click(139);$a.G.Wait(107,'Connected:');$b.G.Wait(107,'Connected:')}
function Hash($source,$target){Assert ((Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $target).Hash) 'Hash mismatch'}
$a=$null;$b=$null
try {
 $a=App 'a';$b=App 'b';Connect $a $b;Pass 'initial peer consent remains required'
 Assert ([ChatProbe]::Missing($a.G.Window,109) -and [ChatProbe]::Missing($a.G.Window,110)) 'Chat controls still in main window'
 Assert ([ChatProbe]::Missing($b.G.Window,116) -and [ChatProbe]::Missing($b.G.Window,117)) 'Manual file approval UI remains in default mode';Pass 'main has no text input/send or manual file approval buttons'
 [ChatProbe]::ShowWindow($a.G.Window,5)|Out-Null;$a.G.Click(142);Start-Sleep -Milliseconds 150;Assert ([ChatProbe]::IsWindowVisible($a.G.Chat)) 'Chat did not open';Pass 'main button opens independent chat window'
 $a.G.Set(109,'shift-enter-marker');[ChatProbe]::ShiftEnter($a.G.Chat,$a.G.Control(109))
 Assert ($a.G.Text(109).Contains("`r`n")) ('Shift+Enter failed to insert newline; input='+$a.G.Text(109))
 Assert (!$b.G.Text(108).Contains('shift-enter-marker')) 'Shift+Enter transmitted text';Pass 'real Shift+Enter inserts newline without transmission'
 $a.G.Set(109,"첫 줄`r`n둘째 줄 😀");$a.G.Enter();$b.G.Wait(108,'둘째 줄 😀');Assert ($a.G.Text(109) -eq '') 'Enter did not clear queued message';Pass 'Enter sends multiline Unicode text over existing protocol'
 for($i=0;$i -lt 12;$i++){$a.G.Set(109,"burst-$i!");$a.G.Enter()}
 $b.G.Wait(108,'burst-11!');$text=$b.G.Text(108);$previous=-1
 for($i=0;$i -lt 12;$i++){$at=$text.IndexOf("burst-$i!");Assert ($at -gt $previous) "Rapid chat missing or reordered message $i";$previous=$at};Pass 'rapid chat messages remain ordered without busy-action drops'
 $a.G.Set(109,'draft kept');[ChatProbe]::Close($a.G.Chat)
 Assert ([ChatProbe]::IsWindow($a.G.Chat) -and ![ChatProbe]::IsWindowVisible($a.G.Chat)) 'Close destroyed chat';Assert ($a.G.Text(107).StartsWith('Connected:')) 'Chat close disconnected network';Pass 'chat close hides window without disconnecting'
 $source=Join-Path $run '자동 수신 😀.bin';[IO.File]::WriteAllBytes($source,[byte[]](0..255));$a.G.Pick($source);$a.G.Click(113);$a.G.Wait(122,'Completed:');$b.G.Wait(123,'Saved:');Hash $source (Join-Path $b.Dir 'received/자동 수신 😀.bin');Pass 'file automatically saved and SHA-256/ACK confirmed with chat closed'
 $id=$a.G.Text(124);$a.G.Click(126);$a.G.Wait(122,'Completed: verified existing commit');Pass 'automatic completion query preserves transfer identity verification'
 $a.G.Click(142);Start-Sleep -Milliseconds 150;Assert ($a.G.Text(109) -eq 'draft kept') 'Draft lost';Assert ($a.G.Text(108).Contains('첫 줄')) 'History lost';Pass 'chat reopen preserves draft and history'
 foreach($size in @(@(460,320),@(900,700),@(650,520))){[ChatProbe]::Resize($a.G.Chat,$size[0],$size[1]);$a.G.Responsive()}
 [ChatProbe]::ShowWindow($a.G.Chat,3)|Out-Null;Start-Sleep -Milliseconds 150;[ChatProbe]::ShowWindow($a.G.Chat,9)|Out-Null
 [ChatProbe]::Focus($a.G.Chat,$a.G.Control(110));$changed=[ScrollProbe]::CompareRepaint($a.G.Chat,(Join-Path $run 'chat-resize'))
 Assert ($changed -eq 0) "Chat repaint changed $changed pixels";Pass 'chat resize/maximize/restore settled pixels match complete repaint'
 $back=Join-Path $run 'reverse.txt';[IO.File]::WriteAllText($back,'reverse');$b.G.Pick($back);$b.G.Click(113);$b.G.Wait(122,'Completed:');Hash $back (Join-Path $a.Dir 'received/reverse.txt');Pass 'automatic file reception works in reverse direction'
 [IO.File]::WriteAllText($source,'different content');$before=(Get-FileHash (Join-Path $b.Dir 'received/자동 수신 😀.bin')).Hash;$a.G.Pick($source);$a.G.Click(113);$a.G.Wait(122,'Failed:');Assert ((Get-FileHash (Join-Path $b.Dir 'received/자동 수신 😀.bin')).Hash -eq $before) 'Collision overwrote file';$b.G.Wait(123,'Destination already exists');Pass 'duplicate final filename fails without overwrite and reports error'
 $changedFolder=Join-Path $run '새 저장 폴더';New-Item -ItemType Directory $changedFolder | Out-Null;$b.G.Folder($changedFolder)
 $a.G.Click(113);$a.G.Wait(122,'Completed:');Hash $source (Join-Path $changedFolder '자동 수신 😀.bin');Pass 'next automatic receive uses newly configured download directory'
 $one=Join-Path $run 'duplex-a.bin';$two=Join-Path $run 'duplex-b.bin';[IO.File]::WriteAllBytes($one,[byte[]](1..255)*1024);[IO.File]::WriteAllBytes($two,[byte[]](0..255)*1024)
 $a.G.Pick($one);$b.G.Pick($two);$a.G.Click(113);$b.G.Click(113);$a.G.Wait(122,'duplex-a.bin');$b.G.Wait(122,'duplex-b.bin');$a.G.Wait(122,'Completed:');$b.G.Wait(122,'Completed:');Hash $one (Join-Path $changedFolder 'duplex-a.bin');Hash $two (Join-Path $a.Dir 'received/duplex-b.bin');Pass 'both default GUIs can send and automatically receive files concurrently'
 $a.G.Close();$b.G.Close()
}finally{foreach($app in @($a,$b)){if($app){try{[IO.File]::WriteAllText((Join-Path $app.Dir 'ui.log'),$app.G.Text(108))}catch{};$app.G.Dispose()}}}
$a=$null;$b=$null
try {
 $a=App 'valid';$b=App 'invalid' $false;Connect $a $b
 $source=Join-Path $run 'invalid-folder.bin';[IO.File]::WriteAllText($source,'deny');$a.G.Pick($source);$a.G.Click(113);$a.G.Wait(122,'Failed:');$b.G.Wait(108,'choose a valid download folder')
 Assert (!(Test-Path (Join-Path $b.Dir 'received/invalid-folder.bin'))) 'Invalid settings fell back to received';Pass 'invalid download setting rejects automatic receive without fallback'
 $a.G.Close();$b.G.Close()
}finally{foreach($app in @($a,$b)){if($app){$app.G.Dispose()}}}
Write-Output "Chat/automatic receive artifacts: $run"
