param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Debug/FileTransferAppGui.exe",[switch]$HelpersOnly)
$ErrorActionPreference='Stop';$Exe=(Resolve-Path $Exe).Path
Add-Type -Path "$PSScriptRoot/GuiDriver.cs";Add-Type -Path "$PSScriptRoot/FrameRelay.cs";Add-Type -Path "$PSScriptRoot/MetadataProbe.cs"
$run=Join-Path $PSScriptRoot ('runs/n1-'+[Guid]::NewGuid().ToString('N'));[void][IO.Directory]::CreateDirectory($run)
function Assert($ok,$message){if(!$ok){throw $message}}
function Pass($text){"PASS: $text"}
function App($name,$server=$false,$display=''){
 $dir=Join-Path $run $name;[void][IO.Directory]::CreateDirectory($dir);$g=[GuiDriver]::new($Exe,$dir,$true,$false,$true);$g.Wait(141,'N:1 ready:')
 if($display){$before=$g.Text(141);$g.SetRole($server);$g.Set(150,$display);$g.Click(151);$w=[Diagnostics.Stopwatch]::StartNew();while($w.ElapsedMilliseconds -lt 8000){$status=$g.Text(141);if($status -ne $before -and $status.StartsWith('N:1 ready:') -and $status.Contains(('role='+$(if($server){'server'}else{'client'})))){break};Start-Sleep -Milliseconds 20};Assert ($w.ElapsedMilliseconds -lt 8000) 'Profile apply did not restart listener'}
 $g.Wait(157,'PeerId:');$id=[IO.File]::ReadAllText((Join-Path $dir 'gui-settings.txt.peer-id'));Assert ($id -cmatch '^[a-f0-9]{32}$') 'Persistent ID invalid'
 Assert ($g.Text(141) -match 'TCP=(\d+)') 'Listener port missing';$port=[int]$Matches[1];Assert ($g.Text(141) -match 'N:1 ready: ([a-f0-9]{32})') 'Instance ID missing';return @{G=$g;Dir=$dir;ID=$id;Port=$port;Instance=$Matches[1]}
}
function Count($a,$count){$w=[Diagnostics.Stopwatch]::StartNew();while($w.ElapsedMilliseconds -lt 8000){if($a.G.List(152).Length -eq $count){return};Start-Sleep -Milliseconds 30};throw ('Session count expected '+$count+': '+($a.G.List(152)-join ';')+' '+$a.G.Text(108))}
function Connect($c,$s,$relay=$null){$c.G.Set(101,'127.0.0.1');$c.G.Set(102,[string]$(if($relay){$relay.Port}else{$s.Port}));$c.G.Click(137);$s.G.Wait(138,'unverified');$s.G.Click(139);$c.G.Wait(107,'Connected:')}
function Select-Peer($a,$b){$a.G.SelectSession($b.ID);$a.G.UseChat($b.ID)}
function Send($a,$b,$text){Select-Peer $a $b;$a.G.Set(109,$text);$a.G.Click(110)}
function ChatWait($a,$b,$text){$a.G.UseChat($b.ID);$w=[Diagnostics.Stopwatch]::StartNew();while($w.ElapsedMilliseconds -lt 5000){if($a.G.ChatText().Contains($text)){return};Start-Sleep -Milliseconds 20};throw "Peer chat missing: $text"}
function Folder($name){$p=Join-Path $run $name;[void][IO.Directory]::CreateDirectory($p);return $p}
function Hash($source,$saved){Assert ((Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $saved).Hash) 'Stored SHA-256 mismatch'}
if($HelpersOnly){return}
$s=$null;$a=$null;$b=$null;$c=$null;$ra=$null;$rb=$null
try{
 $s=App 'server' $true 'server-name';Pass 'server mode starts listener with persisted role and PeerId'
 $a=App 'client-a' $false 'same-name';$b=App 'client-b' $false 'same-name';$c=App 'client-c' $false 'client-c'
 Assert ($a.ID -ne $b.ID) 'Duplicate persistent identity';Pass 'duplicate display names retain distinct persistent PeerIds'
 $ra=[FrameRelay]::new($s.Port,$true);$rb=[FrameRelay]::new($s.Port,$true)
 Connect $a $s $ra;Connect $b $s $rb;Connect $c $s;Count $s 3;Pass 'three clients connect with independent manual approvals'
 foreach($x in @($a,$b,$c)){Send $s $x ('to-'+$x.ID);ChatWait $x $s ('to-'+$x.ID);Send $x $s ('from-'+$x.ID);ChatWait $s $x ('from-'+$x.ID)}
 Pass 'server and each client exchange bidirectional messages'
 $s.G.UseChat($a.ID);Assert (!$s.G.ChatText().Contains('from-'+$b.ID)) 'Other client chat leaked';Pass 'per-peer chat histories remain isolated'
 $customA=Folder 'custom-a';$customB=Folder 'custom-b';Select-Peer $s $a;$s.G.Folder($customA,$false,155);Select-Peer $s $b;$s.G.Folder($customB,$false,155);Pass 'server independently configures each selected client destination'
 $sourceA=Join-Path (Folder 'source-a') 'same.bin';$sourceB=Join-Path (Folder 'source-b') 'same.bin';$bytes=New-Object byte[] (8MB);$bytes[0]=17;[IO.File]::WriteAllBytes($sourceA,$bytes);$bytes[0]=29;[IO.File]::WriteAllBytes($sourceB,$bytes)
 Select-Peer $a $s;$a.G.Pick($sourceA);Select-Peer $b $s;$b.G.Pick($sourceB);$ra.ChunkDelayMs=50;$rb.ChunkDelayMs=50;$ra.PauseChunks=$true;$rb.PauseChunks=$true;$ra.PauseAfterBytes=262144;$rb.PauseAfterBytes=262144;$a.G.Click(113);$b.G.Click(113)
 Assert ($ra.WaitChunk(10000) -and $rb.WaitChunk(10000)) 'Both client transfers did not overlap';$ra.ReleaseChunks();$rb.ReleaseChunks();Pass 'two clients simultaneously stream same-named files with independent request namespaces'
 $changedA=Folder 'next-a';Select-Peer $s $a;$s.G.Folder($changedA,$false,155)
 $sourceC=Join-Path $run 'server-to-c.bin';[IO.File]::WriteAllBytes($sourceC,[byte[]](1..255));Select-Peer $s $c;$s.G.Pick($sourceC);$s.G.Click(113);$s.G.Wait(122,'Completed:');Hash $sourceC (Join-Path $c.Dir 'received/server-to-c.bin')
 Send $c $s 'chat-while-a-and-b-stream';ChatWait $s $c 'chat-while-a-and-b-stream';Pass 'server sends to C and receives chat while A and B file workers stream'
 $ra.ReleaseChunks();$rb.ReleaseChunks();$a.G.Wait(122,'Completed:');$b.G.Wait(122,'Completed:');Assert ($a.G.Text(122) -match 'requestId=2\b' -and $b.G.Text(122) -match 'requestId=2\b') 'Fixture did not exercise identical request IDs across clients';Hash $sourceA (Join-Path $customA 'same.bin');Hash $sourceB (Join-Path $customB 'same.bin');Assert (!(Test-Path (Join-Path $changedA 'same.bin'))) 'Active receive changed destination';Pass 'accepted destinations remain pinned while per-peer settings change mid-transfer'
 Assert (!(Get-ChildItem $customA -Filter '*.fta')) 'Metadata in download folder';Pass 'FTA metadata remains in application storage'
 $idA=$a.G.Text(124);Select-Peer $a $s;$a.G.Click(126);$a.G.Wait(122,'Failed:');Assert (!(Test-Path (Join-Path $changedA 'same.bin'))) 'Completion lookup used wrong folder';Pass 'path change does not redirect completion metadata to another directory'
 Select-Peer $s $a;$s.G.Folder($customA,$false,155);Select-Peer $a $s;$a.G.Click(126);$a.G.Wait(122,'Completed:');Hash $sourceA (Join-Path $customA 'same.bin');Pass 'restoring original peer folder permits hash-verified completion lookup'
 Select-Peer $b $s;$b.G.Click(113);$b.G.Wait(122,'Failed:');Hash $sourceB (Join-Path $customB 'same.bin');Pass 'existing file is preserved when the same client sends a duplicate name'
 Select-Peer $a $s;$a.G.Click(105);Count $s 2;Send $b $s 'b-survives-a-disconnect';ChatWait $s $b 'b-survives-a-disconnect';Pass 'disconnecting A preserves B and C communication'
 $a.G.Close();$a.G.Dispose();$oldId=$a.ID;$a=App 'client-a' $false 'renamed-a';Assert ($a.ID -eq $oldId) 'PeerId changed on restart';$ra.Dispose();$ra=$null;Connect $a $s;Count $s 3;Select-Peer $s $a;$s.G.Wait(154,$customA);Pass 'renamed restarted client retains server folder mapping by persistent PeerId'
 Select-Peer $s $c;$s.G.Click(156);$expected=Join-Path $s.Dir ('received/peers/'+$c.ID);$s.G.Wait(154,$expected);Pass 'default peer folder is derived from PeerId rather than display name'
 $s.G.Close();foreach($x in @($a,$b,$c)){$x.G.Wait(107,'Disconnected')};Pass 'server shutdown closes all peer sessions'
 $s.G.Dispose();$oldServerId=$s.ID;$s=App 'server';Assert ($s.ID -eq $oldServerId -and $s.G.Text(141).Contains('role=server')) 'Server profile not restored';Connect $a $s;Select-Peer $s $a;$s.G.Wait(154,$customA);Pass 'server restart restores role, identity and per-client destination settings'
}finally{foreach($relay in @($ra,$rb)){if($relay){$relay.Dispose()}};foreach($x in @($a,$b,$c,$s)){if($x){try{[IO.File]::WriteAllText((Join-Path $x.Dir 'ui.log'),$x.G.Text(108))}catch{};try{$x.G.Dispose()}catch{}}};"N1 artifacts: $run"}
