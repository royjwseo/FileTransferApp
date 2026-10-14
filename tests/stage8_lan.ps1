param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Debug/FileTransferAppGui.exe")
$ErrorActionPreference='Stop';$Exe=(Resolve-Path $Exe).Path
Add-Type -Path "$PSScriptRoot/GuiDriver.cs"
$runDir=Join-Path $PSScriptRoot ('runs/lan-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $runDir | Out-Null
function Assert($yes,[string]$message){if(!$yes){throw $message}}
function Pass([string]$message){"PASS: $message"}
function App([string]$name){$dir=Join-Path $runDir $name;New-Item -ItemType Directory $dir | Out-Null;$g=[GuiDriver]::new($Exe,$dir,$true,$false);$g.Wait(141,'LAN ready:');$text=$g.Text(141);Assert ($text -match 'LAN ready: ([a-f0-9]{32}) TCP=(\d+)') 'LAN startup missing';return @{G=$g;ID=$Matches[1];Port=[int]$Matches[2];Dir=$dir}}
function Wait-Devices($a,[int]$count,[int]$timeout=12000){$watch=[Diagnostics.Stopwatch]::StartNew();while($watch.ElapsedMilliseconds -lt $timeout){if($a.G.Devices().Count -eq $count){return};Start-Sleep -Milliseconds 50};throw "Device count mismatch: $($a.G.Devices() -join ';')"}
function Select-Peer($a,$b){$rows=$a.G.Devices();$index=-1;for($i=0;$i -lt $rows.Length;$i++){if($rows[$i].Contains($b.ID.Substring(0,8))){$index=$i;break}};Assert ($index -ge 0) 'Expected peer missing';$a.G.SelectDevice($index)}
function Udp([byte[]]$bytes){$u=[Net.Sockets.UdpClient]::new();try{[void]$u.Send($bytes,$bytes.Length,'127.0.0.1',9001)}finally{$u.Dispose()}}
function Packet([string]$text){Udp ([Text.Encoding]::UTF8.GetBytes($text))}
function Disconnect($a,$b){$a.G.Click(105);$a.G.Wait(107,'Disconnected');$b.G.Wait(107,'Disconnected')}
function Connect-Pair($a,$b){Select-Peer $a $b;$a.G.Click(136);$b.G.Wait(138,'unverified');$b.G.Click(139);$a.G.Wait(107,'Connected:');$b.G.Wait(107,'Connected:')}
function Save-Log($a){if($a){try{[IO.File]::WriteAllText((Join-Path $a.Dir 'ui.log'),$a.G.Text(108))}catch{}}}
$a=$null;$b=$null
try {
    $a=App 'a';Start-Sleep -Seconds 3;Wait-Devices $a 0;Pass 'default GUI starts discovery and consent listener without establishing a transfer session'
    Packet "FTA-DISC|1|R|$($a.ID)|$($a.Port)|Self";Start-Sleep -Milliseconds 300;Wait-Devices $a 0;Pass 'own instance discovery response excluded'
    foreach($bad in @('not an app',('FTA-DISC|99|R|'+('a'*32)+'|9000|WrongVersion'),('FTA-DISC|1|R|'+('a'*32)+'|0|BadPort'),('FTA-DISC|1|R|'+('a'*32)+'|9000|../fake|extra'),('FTA-DISC|1|R|'+('a'*32)+'|9000|'+('x'*97)))){Packet $bad}
    Udp ([byte[]]@(70,84,65,45,68,73,83,67,124,49,124,82,124,255));Udp ([byte[]]::new(1024));Start-Sleep -Milliseconds 500;Wait-Devices $a 0;Pass 'malformed, oversized, invalid UTF-8, invalid ports and wrong discovery version rejected'
    $fake='f'*32;for($i=0;$i -lt 4;$i++){Packet "FTA-DISC|1|R|$fake|43210|Fake device"};Wait-Devices $a 1
    Assert ($a.G.Devices()[0].Contains('127.0.0.1:43210')) 'Did not use UDP source address';Pass 'UDP response source IP used and duplicate instance responses coalesced'
    Wait-Devices $a 0 12000;Pass 'silent discovery candidate expires after bounded TTL'
    $b=App 'b';$a.G.Click(135);$b.G.Click(135);Wait-Devices $a 1;Wait-Devices $b 1;Pass 'two actual GUI processes discover each other over UDP multicast'
    Assert (!$a.G.Enabled(110) -and !$b.G.Enabled(110)) 'Discovery enabled file/text session';Pass 'discovery does not auto-connect or enable transfer commands'
    Select-Peer $a $b;$a.G.Click(136);$b.G.Wait(138,'unverified');Assert (!$b.G.Enabled(110)) 'Unapproved session active';$b.G.Click(140);$a.G.Wait(141,'Connection rejected');Pass 'selected peer request prompts separately and rejection closes candidate'
    Select-Peer $a $b;$a.G.Click(136);$b.G.Wait(138,'unverified');$a.G.Wait(141,'timed out',20000);$b.G.Wait(138,'연결 요청 없음',3000);Pass 'unanswered request times out without activating Network'
    Connect-Pair $a $b;Assert ($a.G.Enabled(110) -and $b.G.Enabled(110)) 'Approved session controls not enabled';Pass 'explicit incoming consent hands approved socket to existing TCP protocol'
    $a.G.Click(136);$a.G.Wait(108,'already connected or busy');Assert ($b.G.Text(138) -eq '연결 요청 없음') 'Duplicate incoming session';Pass 'duplicate connection request blocked while session active'
    $a.G.Set(109,'LAN 한글 메시지');$a.G.Click(110);$b.G.Wait(108,'LAN 한글 메시지');$b.G.Set(109,'LAN reply');$b.G.Enter();$a.G.Wait(108,'LAN reply');Pass 'approved connection supports bidirectional Unicode text'
    $source=Join-Path $runDir 'LAN 사진.pdf';[IO.File]::WriteAllBytes($source,[byte[]](1..255));$a.G.Pick($source);$a.G.Click(113);$b.G.Wait(115,'LAN 사진.pdf');$b.G.Click(116);$a.G.Wait(122,'Completed:');
    Assert ((Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath (Join-Path $b.Dir 'received/LAN 사진.pdf')).Hash) 'Approved transfer hash mismatch';Pass 'approved connection retains file approval, stored ACK and SHA-256'
    Disconnect $a $b;$a.G.Click(135);Wait-Devices $a 1;Connect-Pair $a $b;Disconnect $a $b;Pass 'disconnect retains discovery and permits new user-approved connection'
    $a.G.Set(101,'127.0.0.1');$a.G.Set(102,[string]$b.Port);$a.G.Click(137);$b.G.Wait(138,'unverified');$b.G.Click(139);$a.G.Wait(107,'Connected:');$b.G.Wait(107,'Connected:');Disconnect $a $b;Pass 'direct IPv4 request uses the same consent gate'
    Select-Peer $a $b;Select-Peer $b $a;$a.G.Click(136);$b.G.Click(136)
    if([string]::CompareOrdinal($a.ID,$b.ID) -lt 0){$initiator=$a;$receiver=$b}else{$initiator=$b;$receiver=$a}
    $receiver.G.Wait(138,'unverified');$receiver.G.Click(139);$a.G.Wait(107,'Connected:');$b.G.Wait(107,'Connected:');Assert ($initiator.G.Text(138) -eq '연결 요청 없음') 'Both simultaneous requests survived';Disconnect $a $b;Pass 'simultaneous requests converge to one ID-ordered candidate and still require consent'
    $a.G.Process.Refresh();$b.G.Process.Refresh();$ha=$a.G.Process.HandleCount;$hb=$b.G.Process.HandleCount;$ma=$a.G.Process.PrivateMemorySize64;$mb=$b.G.Process.PrivateMemorySize64
    for($i=0;$i -lt 6;$i++){Connect-Pair $a $b;Disconnect $a $b}
    $a.G.Process.Refresh();$b.G.Process.Refresh();Assert (($a.G.Process.HandleCount-$ha) -le 12 -and ($b.G.Process.HandleCount-$hb) -le 12) 'Connection handle growth';Assert (($a.G.Process.PrivateMemorySize64-$ma) -lt 16MB -and ($b.G.Process.PrivateMemorySize64-$mb) -lt 16MB) 'Connection memory growth';Pass 'six approved connect/disconnect cycles remain responsive with bounded handle/memory growth'
    Select-Peer $a $b;$b.G.Close();$a.G.Click(136);$a.G.Wait(141,'Connection request failed',8000);Wait-Devices $a 0 12000;Pass 'offline selected device errors asynchronously and later expires'
    $a.G.Close()
}finally{foreach($x in @($a,$b)){Save-Log $x;if($x){$x.G.Dispose()}}}
# Raw sockets verify consent gate separation from file protocol.
$a=$null;$raw=$null
try {
    $a=App 'raw';$raw=[Net.Sockets.TcpClient]::new('127.0.0.1',$a.Port);$s=$raw.GetStream();$s.ReadTimeout=3000
    $line=[Text.Encoding]::UTF8.GetBytes(('FTA-C1|'+('1'*32)+'|'+('2'*32)+'|Raw device' + "`n"));$s.Write($line,0,$line.Length);$a.G.Wait(138,'Raw device');
    [byte[]]$illegal=@(1,0,0,0,1,65);$s.Write($illegal,0,$illegal.Length);$a.G.Wait(141,'before approval');Assert (!$a.G.Enabled(110)) 'Unauthorized commands activated session';Pass 'file/text protocol bytes before user approval are rejected'
    $raw.Dispose();$raw=$null
    $raw=[Net.Sockets.TcpClient]::new('127.0.0.1',$a.Port);$s=$raw.GetStream();$bad=[Text.Encoding]::UTF8.GetBytes("FTA-C99|bad`n");$s.Write($bad,0,$bad.Length);$a.G.Wait(141,'Invalid connection greeting');Pass 'wrong consent version and malformed greeting rejected before session'
    $raw.Dispose();$raw=$null
    $raw=[Net.Sockets.TcpClient]::new('127.0.0.1',$a.Port);$s=$raw.GetStream();$s.ReadTimeout=3000;$s.Write($line,0,$line.Length);$a.G.Wait(138,'Raw device')
    for($i=0;$i -lt 12;$i++){$extra=[Net.Sockets.TcpClient]::new('127.0.0.1',$a.Port);try{$extra.GetStream().Write($line,0,$line.Length)}catch{}finally{$extra.Dispose()}}
    $a.G.Responsive();Assert ($a.G.Text(138).Contains('Raw device')) 'Duplicate requests displaced original consent';$a.G.Click(139)
    $reader=[IO.StreamReader]::new($s,[Text.Encoding]::UTF8,$false,1024,$true);$reply=$reader.ReadLine();Assert ($reply.EndsWith('|'+('2'*32))) 'Original consent nonce lost'
    $a.G.Wait(107,'Connected:');[byte[]]$frame=@(1,0,0,0,1,90);$s.Write($frame,0,$frame.Length);$a.G.Wait(108,'상대: Z')
    Pass 'pending-request flood stays bounded and only original approved socket reaches text protocol'
    $raw.Dispose();$raw=$null;$a.G.Wait(107,'Disconnected')
    $raw=[Net.Sockets.TcpClient]::new('127.0.0.1',$a.Port);$s=$raw.GetStream();$s.Write($line,0,$line.Length);$a.G.Wait(138,'Raw device');$a.G.Close();Pass 'GUI close while consent pending cleans listener and candidate sockets'
}finally{if($raw){$raw.Dispose()};Save-Log $a;if($a){$a.G.Dispose()}}
$a=$null;$server=$null;$peer=$null
try {
    $a=App 'stale-response';$server=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$server.Start();$a.G.Set(101,'127.0.0.1');$a.G.Set(102,[string]$server.LocalEndpoint.Port)
    $accept=$server.AcceptTcpClientAsync();$a.G.Click(137);Assert ($accept.Wait(5000)) 'Missing outgoing request';$peer=$accept.Result;$stream=$peer.GetStream();$stream.ReadTimeout=3000
    $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8,$false,1024,$true);$request=$reader.ReadLine();Assert ($request.StartsWith('FTA-C1|')) 'Missing consent greeting'
    $stale=[Text.Encoding]::UTF8.GetBytes(('FTA-A1|'+('3'*32)+'|'+('0'*32)+"`n"));$stream.Write($stale,0,$stale.Length);$a.G.Wait(141,'Invalid connection approval');Assert (!$a.G.Enabled(110)) 'Stale approval activated session';Pass 'old/mismatched connection nonce cannot approve a new request'
    $a.G.Close()
}finally{if($peer){$peer.Dispose()};if($server){$server.Stop()};Save-Log $a;if($a){$a.G.Dispose()}}
$a=$null;$blocker=$null;$raw=$null
try {
    $blocker=[Net.Sockets.Socket]::new([Net.Sockets.AddressFamily]::InterNetwork,[Net.Sockets.SocketType]::Dgram,[Net.Sockets.ProtocolType]::Udp)
    $blocker.ExclusiveAddressUse=$true;$blocker.Bind([Net.IPEndPoint]::new([Net.IPAddress]::Any,9001));[GuiDriver]::PreventInheritance($blocker.Handle)
    $a=App 'udp-blocked';$a.G.Wait(108,'Discovery unavailable; direct connection remains available')
    $raw=[Net.Sockets.TcpClient]::new('127.0.0.1',$a.Port);$s=$raw.GetStream();$line=[Text.Encoding]::UTF8.GetBytes(('FTA-C1|'+('7'*32)+'|'+('8'*32)+'|UDP blocked peer' + "`n"));$s.Write($line,0,$line.Length)
    $a.G.Wait(138,'UDP blocked peer');$a.G.Click(139);$a.G.Wait(107,'Connected:');$a.G.Close();Pass 'UDP port unavailable leaves consent-based direct TCP listener operational'
}finally{if($raw){$raw.Dispose()};if($blocker){$blocker.Dispose()};Save-Log $a;if($a){$a.G.Dispose()}}
$a=$null
try {
    $a=App 'udp-flood';$a.G.Process.Refresh();$before=$a.G.Process.PrivateMemorySize64
    for($i=1;$i -le 90;$i++){Packet ("FTA-DISC|1|R|"+$i.ToString('x32')+"|43210|Flood "+$i)}
    Start-Sleep -Milliseconds 600;Assert ($a.G.Devices().Count -le 64) 'Unbounded discovery table';$a.G.Responsive();$a.G.Process.Refresh();Assert (($a.G.Process.PrivateMemorySize64-$before) -lt 16MB) 'Discovery snapshot memory growth'
    $a.G.Close();Pass 'UDP candidate table and coalesced GUI snapshots remain bounded under discovery flood'
}finally{Save-Log $a;if($a){$a.G.Dispose()}}
"LAN test artifacts: $runDir"
