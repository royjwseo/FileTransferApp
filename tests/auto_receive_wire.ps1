param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Release/FileTransferAppGui.exe")
$ErrorActionPreference='Stop';$Exe=(Resolve-Path $Exe).Path
Add-Type -Path "$PSScriptRoot/GuiDriver.cs"
Add-Type -Path "$PSScriptRoot/ChatProbe.cs"
$run=Join-Path $PSScriptRoot ('runs/auto-wire-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $run | Out-Null
function Assert($yes,$text){if(!$yes){throw $text}}
function Pass($text){"PASS: $text"}
function U64([UInt64]$n){[byte[]]$b=New-Object byte[] 8;for($i=0;$i -lt 8;$i++){$b[$i]=($n -shr (56-8*$i)) -band 255};return ,$b}
function Read64([byte[]]$b){[UInt64]$v=0;for($i=0;$i -lt 8;$i++){$v=($v -shl 8) -bor $b[$i]};return $v}
function Digest([byte[]]$data){$h=[Security.Cryptography.SHA256]::Create();try{return [BitConverter]::ToString($h.ComputeHash($data)).Replace('-','').ToLowerInvariant()}finally{$h.Dispose()}}
function Spec($name,[UInt64]$size,[string]$hash,[byte]$mode=0){$n=[Text.Encoding]::UTF8.GetBytes($name);$id=Digest ([byte[]]([Text.Encoding]::ASCII.GetBytes("FTA5`0")+(U64 $size)+[Text.Encoding]::ASCII.GetBytes($hash)+$n));return @{ID=$id;Hash=$hash;Bytes=[byte[]]([Text.Encoding]::ASCII.GetBytes($id+$hash)+$mode+(U64 $size)+$n)}}
function Exact($s,$n){[byte[]]$b=New-Object byte[] $n;$at=0;while($at -lt $n){$got=$s.Read($b,$at,$n-$at);if(!$got){throw 'EOF'};$at+=$got};return ,$b}
function Send($peer,[byte]$type,[UInt64]$id,[byte[]]$payload){[byte[]]$b=(U64 $id)+$payload;$n=$b.Length;[byte[]]$wire=@($type,(($n -shr 24)-band 255),(($n -shr 16)-band 255),(($n -shr 8)-band 255),($n -band 255))+$b;$peer.GetStream().Write($wire,0,$wire.Length)}
function Frame($peer){$s=$peer.GetStream();$h=Exact $s 5;$n=([int]$h[1]-shl 24)-bor([int]$h[2]-shl 16)-bor([int]$h[3]-shl 8)-bor[ int]$h[4];$b=Exact $s $n;Assert ($b.Length -ge 8) 'Missing request ID';return @{Type=[int]$h[0];ID=(Read64 $b);Body=[byte[]]@($b|Select-Object -Skip 8)}}
function App($name){$dir=Join-Path $run $name;New-Item -ItemType Directory $dir | Out-Null;$g=[GuiDriver]::new($Exe,$dir,$true,$false);$g.Wait(141,'LAN ready:');Assert ($g.Text(141)-match 'TCP=(\d+)') 'Port missing';return @{G=$g;Dir=$dir;Port=[int]$Matches[1]}}
function Connect($a){$p=[Net.Sockets.TcpClient]::new('127.0.0.1',$a.Port);$p.ReceiveTimeout=5000;$p.SendTimeout=5000;$s=$p.GetStream();$line=[Text.Encoding]::ASCII.GetBytes(('FTA-C1|'+('1'*32)+'|'+('2'*32)+"|Auto test`n"));$s.Write($line,0,$line.Length);$a.G.Wait(138,'Auto test');$a.G.Click(139);$reply='';while(!$reply.EndsWith("`n")){$c=$s.ReadByte();if($c -lt 0){throw 'Gate closed'};$reply+=[char]$c;Assert ($reply.Length -le 256) 'Unbounded greeting'};Assert ($reply.StartsWith('FTA-A1|')) 'Connection not approved';$a.G.Wait(107,'Connected:');return $p}
function Expect($p,$type,$id){$f=Frame $p;Assert ($f.Type -eq $type -and $f.ID -eq $id) "Expected type=$type id=$id, got type=$($f.Type) id=$($f.ID)";return $f}
$a=$null;$p=$null
try {
 $a=App 'receive';$p=Connect $a;[UInt64]$id=0;$empty=Digest @()
 foreach($name in @('../escape.bin','..\escape.bin','C:\escape.bin','NUL.txt','stream:evil.bin')){
  $id+=2;$spec=Spec $name 0 $empty;Send $p 6 $id $spec.Bytes;$f=Expect $p 9 $id;Pass "automatic receiver rejects unsafe filename: $name";$p.Dispose();$a.G.Wait(107,'Disconnected');$p=Connect $a;$id=0
 }
 $id+=2;$spec=Spec 'oversized.bin' ([UInt64]1099511627777) $empty;Send $p 6 $id $spec.Bytes;$f=Expect $p 9 $id;Pass 'automatic receiver enforces existing 1 TiB limit before acceptance';$p.Dispose();$a.G.Wait(107,'Disconnected');$p=Connect $a;$id=0
 $free=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($a.Dir)).AvailableFreeSpace
 Assert ($free -lt 1099511627776) 'Cannot perform bounded insufficient-space case on this volume'
 $id+=2;$spec=Spec 'no-space.bin' ([UInt64]($free+1)) $empty;Send $p 6 $id $spec.Bytes;$f=Expect $p 9 $id;Assert ([Text.Encoding]::ASCII.GetString($f.Body).Contains('Insufficient disk space')) 'Wrong space error';Pass 'disk space is checked before ready response without allocating declared data'
 $id+=2;$spec=Spec 'cancel.bin' 256 (Digest ([byte[]](0..255)));Send $p 6 $id $spec.Bytes;$ready=Expect $p 7 $id;Send $p 10 $id @();$f=Expect $p 10 $id;Assert (!(Test-Path (Join-Path $a.Dir 'received/cancel.bin'))) 'Cancellation committed file';Pass 'automatic AwaitBegin receive remains cancellable with matching request ID'
 $id+=2;$spec=Spec 'bad-hash.bin' 3 (Digest ([byte[]]@(1,2,3)));Send $p 6 $id $spec.Bytes;$ready=Expect $p 7 $id;Send $p 2 $id $spec.Bytes;Send $p 3 $id ([byte[]]@(9,9,9));Send $p 4 $id @();$f=Expect $p 9 $id;Assert (!(Test-Path (Join-Path $a.Dir 'received/bad-hash.bin'))) 'Bad hash committed file';Pass 'automatic receiver rejects mismatched SHA-256 and never sends success ACK'
 # Resume uses the same owned partial object after a real peer disconnect.
 [byte[]]$data=0..255;$hash=Digest $data;$spec=Spec 'resume.bin' $data.Length $hash;$id+=2;Send $p 6 $id $spec.Bytes;$ready=Expect $p 7 $id;Send $p 2 $id $spec.Bytes;Send $p 3 $id ([byte[]]$data[0..99]);$a.G.Wait(123,'Receiving data')
 $a.G.Click(142);Start-Sleep -Milliseconds 100;[ChatProbe]::Close($a.G.Chat);Assert ($a.G.Text(107).StartsWith('Connected:')) 'Closing chat interrupted active file receive';Pass 'closing chat during active receive leaves file session connected'
 $p.Dispose();$p=$null;$a.G.Wait(107,'Disconnected')
 $part=Join-Path $a.Dir 'received/resume.bin.part';Assert ((Get-Item $part).Length -eq 100) 'Partial checkpoint mismatch';Pass 'peer disconnect preserves owned automatic receive checkpoint'
 $p=Connect $a;$resume=Spec 'resume.bin' $data.Length $hash 1;Send $p 6 2 $resume.Bytes;$ready=Expect $p 7 2;Assert ((Read64 $ready.Body)-eq 100) 'Resume offset lost';Send $p 2 2 $resume.Bytes;Send $p 3 2 ([byte[]]$data[100..255]);Send $p 4 2 @();$ack=Expect $p 5 2;Assert ([Text.Encoding]::ASCII.GetString($ack.Body)-eq $spec.ID+$hash) 'ACK identity mismatch';Assert ((Get-FileHash (Join-Path $a.Dir 'received/resume.bin')).Hash.ToLowerInvariant()-eq $hash) 'Resumed data corrupt';Pass 'automatic recovery validates prefix, resumes exact offset and returns storage ACK'
 $query=Spec 'resume.bin' $data.Length $hash 2;Send $p 6 4 $query.Bytes;$complete=Expect $p 11 4;Assert ([Text.Encoding]::ASCII.GetString($complete.Body)-eq $spec.ID+$hash) 'Completion identity mismatch';Pass 'automatic completion query returns verified identity without BEGIN/CHUNK'
 Send $p 6 4 $query.Bytes;$f=Expect $p 9 4;Pass 'completed request ID replay remains rejected'
 $a.G.Responsive();$p.Dispose();$p=$null;$a.G.Close()
}finally{if($p){$p.Dispose()};if($a){try{[IO.File]::WriteAllText((Join-Path $a.Dir 'ui.log'),$a.G.Text(108))}catch{};$a.G.Dispose()}}
$a=$null;$p=$null;$acl=$null
try {
 $a=App 'write-denied';$folder=Join-Path $a.Dir 'received';$acl=Get-Acl -LiteralPath $folder;$deny=Get-Acl -LiteralPath $folder;$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User;$rule=[Security.AccessControl.FileSystemAccessRule]::new($sid,[Security.AccessControl.FileSystemRights]::CreateFiles,[Security.AccessControl.AccessControlType]::Deny);$deny.AddAccessRule($rule);Set-Acl -LiteralPath $folder -AclObject $deny
 $p=Connect $a;$spec=Spec 'denied.bin' 0 (Digest @());Send $p 6 2 $spec.Bytes;$f=Expect $p 9 2;Assert (!(Test-Path (Join-Path $folder 'denied.bin'))) 'Denied write committed file';$a.G.Responsive();Pass 'write permission revoked after settings validation returns error and leaves UI responsive'
}finally{if($acl){Set-Acl -LiteralPath $folder -AclObject $acl};if($p){$p.Dispose()};if($a){$a.G.Dispose()}}
Write-Output "Automatic wire artifacts: $run"
