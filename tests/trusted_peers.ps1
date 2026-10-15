param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Debug/FileTransferAppGui.exe")
$ErrorActionPreference='Stop';$Exe=(Resolve-Path $Exe).Path
Add-Type -Path "$PSScriptRoot/GuiDriver.cs"
$run=Join-Path $PSScriptRoot ('runs/trust-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $run | Out-Null
function Assert($ok,$message){if(!$ok){throw $message}}
function Pass($message){"PASS: $message"}
function App($name){$dir=Join-Path $run $name;[void][IO.Directory]::CreateDirectory($dir);$g=[GuiDriver]::new($Exe,$dir,$true,$false);$g.Wait(141,'LAN ready:');Assert ($g.Text(141) -match 'LAN ready: ([a-f0-9]{32}) TCP=(\d+)') 'Startup';return @{G=$g;Dir=$dir;ID=$Matches[1];Port=[int]$Matches[2]}}
function Select-Loopback($a){
 $u=[Net.Sockets.UdpClient]::new();try{$b=[Text.Encoding]::UTF8.GetBytes(('FTA-DISC|1|R|'+('f'*32)+'|43210|Registration fixture'));$u.Client.SetSocketOption([Net.Sockets.SocketOptionLevel]::IP,[Net.Sockets.SocketOptionName]::MulticastInterface,[Net.IPAddress]::Loopback.GetAddressBytes());[void]$u.Send($b,$b.Length,'239.255.90.8',9001)}finally{$u.Dispose()}
 $w=[Diagnostics.Stopwatch]::StartNew();while($w.ElapsedMilliseconds -lt 5000){$rows=$a.G.Devices();for($i=0;$i -lt $rows.Length;$i++){if($rows[$i].Contains('[ffffffff]')){$a.G.SelectDevice($i);return}};Start-Sleep -Milliseconds 20};throw 'Loopback selection missing'
}
function Register($a,$key){Select-Loopback $a;$a.G.Set(147,$key);$a.G.Click(145);$a.G.Wait(141,'Registered PC saved: 127.0.0.1')}
function Direct($a,$b){$a.G.Set(101,'127.0.0.1');$a.G.Set(102,[string]$b.Port);$a.G.Click(137)}
function Connected($a,$b){$a.G.Wait(107,'Connected:');$b.G.Wait(107,'Connected:');Assert ($b.G.Text(138) -eq '연결 요청 없음') 'Automatic connection requested approval'}
function Disconnect($a,$b){$a.G.Click(105);$a.G.Wait(107,'Disconnected');$b.G.Wait(107,'Disconnected')}
function Raw($a){$c=[Net.Sockets.TcpClient]::new('127.0.0.1',$a.Port);$c.GetStream().ReadTimeout=4000;return $c}
function Write-Line($c,$line){$b=[Text.Encoding]::UTF8.GetBytes($line+"`n");$c.GetStream().Write($b,0,$b.Length)}
function Read-Line($c){$s=$c.GetStream();$line='';while($line.Length -lt 512){$b=$s.ReadByte();if($b -lt 0){throw 'Unexpected connection close'};if($b -eq 10){return $line};$line+=[char]$b};throw 'Oversized line'}
function Mac($key,$data){$bytes=New-Object byte[] 32;for($i=0;$i -lt 32;$i++){$bytes[$i]=[Convert]::ToByte($key.Substring(2*$i,2),16)};$h=[Security.Cryptography.HMACSHA256]::new($bytes);try{return ([BitConverter]::ToString($h.ComputeHash([Text.Encoding]::UTF8.GetBytes($data)))).Replace('-','').ToLowerInvariant()}finally{$h.Dispose()}}
$a=$null;$b=$null;$raw=$null
try{
 $a=App 'a';$b=App 'b'
 Select-Loopback $a;$a.G.Set(147,'bad');$a.G.Click(145);$a.G.Wait(108,'64자리');Assert (!(Test-Path (Join-Path $a.Dir 'gui-settings.txt.trusted'))) 'Invalid key persisted';Pass 'invalid key rejected without registration'
 Register $a ''; $key=$a.G.Text(147);Assert ($key -cmatch '^[a-f0-9]{64}$') 'Generated key invalid';Pass 'empty key generates cryptographic 256-bit registration key'
 $file=Join-Path $a.Dir 'gui-settings.txt.trusted';Assert ((Test-Path $file) -and !([Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($file)).Contains($key))) 'Plaintext key on disk';Pass 'registration persisted without plaintext key using DPAPI'
 Register $b $key;Direct $a $b;Connected $a $b;Pass 'matching registered keys connect without receiver approval'
 $a.G.Set(109,'authenticated chat');$a.G.Click(110);$b.G.Wait(108,'authenticated chat');$b.G.Set(109,'authenticated reply');$b.G.Click(110);$a.G.Wait(108,'authenticated reply');Pass 'authenticated session supports bidirectional chat'
 $source=Join-Path $run 'trusted.bin';[IO.File]::WriteAllBytes($source,[byte[]](1..255));$a.G.Pick($source);$a.G.Click(113);$a.G.Wait(122,'Completed:');$b.G.Wait(123,'Saved:');Assert ((Get-FileHash $source).Hash -eq (Get-FileHash (Join-Path $b.Dir 'received/trusted.bin')).Hash) 'File mismatch';Pass 'authenticated session automatically receives file with SHA-256 and completion ACK'
 Disconnect $a $b;$b.G.Close();$b.G.Dispose();$b=App 'b';Direct $a $b;Connected $a $b;Disconnect $a $b;Pass 'registered key survives receiver process restart'
 Register $b ('e'*64);Direct $a $b;$a.G.Wait(141,'key mismatch');Assert (!$a.G.Enabled(110) -and !$b.G.Enabled(110)) 'Wrong key activated session';Pass 'wrong shared key cannot activate session or fall back to manual approval'
 $b.G.Wait(141,'closed');Register $b $key
 $raw=Raw $b;Write-Line $raw ('FTA-C1|'+('1'*32)+'|'+('2'*32)+'|Spoof registered PC');$b.G.Wait(138,'Spoof registered PC');Assert (!$b.G.Enabled(110)) 'Legacy identity spoof activated session';$b.G.Click(140);[void](Read-Line $raw);$raw.Dispose();$raw=$null;Pass 'registered IP with legacy or spoofed discovery identity still requires manual approval'
 $greeting='FTA-C2|'+('1'*32)+'|'+('2'*32)+'|Raw registered PC'
 $raw=Raw $b;Write-Line $raw $greeting;$challenge=(Read-Line $raw).Split('|');$transcript=$greeting+'|'+$challenge[1]+'|'+$challenge[2];Assert ($challenge[3] -eq (Mac $key ('server|'+$transcript))) 'Receiver proof invalid';$proof=Mac $key ('client|'+$transcript);Write-Line $raw ('FTA-P2|'+$proof);$accepted=(Read-Line $raw).Split('|');Assert ($accepted[0] -eq 'FTA-A2' -and $accepted[3] -eq (Mac $key ('accepted|'+$transcript))) 'Accepted proof invalid';$raw.Dispose();$raw=$null;$b.G.Wait(107,'Disconnected');Pass 'raw challenge-response independently verifies directional HMAC proofs'
 $raw=Raw $b;Write-Line $raw $greeting;$fresh=(Read-Line $raw).Split('|');Assert ($fresh[2] -ne $challenge[2]) 'Challenge repeated';Write-Line $raw ('FTA-P2|'+$proof);$b.G.Wait(141,'key mismatch or replay');Assert (!$b.G.Enabled(110)) 'Replayed proof activated session';$raw.Dispose();$raw=$null;Pass 'old proof replay fails against fresh receiver challenge'
 Select-Loopback $b;$b.G.Click(146);$b.G.Wait(141,'Registered PC removed:');$raw=Raw $b;Write-Line $raw $greeting;$b.G.Wait(141,'PC is not registered');Assert (!$b.G.Enabled(110)) 'Removed key still authorized';$raw.Dispose();$raw=$null;Pass 'registration removal immediately blocks authenticated auto-connect'
 Select-Loopback $a;$a.G.Click(146);$a.G.Wait(141,'Registered PC removed:');Direct $a $b;$b.G.Wait(138,'unverified');Assert (!$b.G.Enabled(110)) 'Unregistered connection bypassed consent';$b.G.Click(139);Connected $a $b;Disconnect $a $b;Pass 'unregistered peers retain explicit manual connection approval'
 Register $b $key;$b.G.Close();$b.G.Dispose();[IO.File]::WriteAllText((Join-Path $b.Dir 'gui-settings.txt.trusted'),'corrupt');$b=App 'b';$b.G.Wait(108,'Registered peers unavailable');$raw=Raw $b;Write-Line $raw $greeting;$b.G.Wait(141,'PC is not registered');$raw.Dispose();$raw=$null;Pass 'corrupt protected store grants no automatic authorization'
}finally{if($raw){$raw.Dispose()};foreach($x in @($a,$b)){if($x){try{[IO.File]::WriteAllText((Join-Path $x.Dir 'ui.log'),$x.G.Text(108))}catch{};$x.G.Dispose()}};"Run directory: $run"}
