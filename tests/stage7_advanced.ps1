param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Release/FileTransferAppGui.exe")
$ErrorActionPreference='Stop';$Exe=(Resolve-Path $Exe).Path
Add-Type -Path "$PSScriptRoot/GuiDriver.cs";Add-Type -Path "$PSScriptRoot/FrameRelay.cs"
$runDir=Join-Path $PSScriptRoot ('runs/gui-advanced-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $runDir | Out-Null
function Assert($condition,[string]$message){if(!$condition){throw $message}}
function Pass([string]$message){Write-Output "PASS: $message"}
function New-Pair([string]$name,[string]$fault='') {
    $probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$probe.Start();$port=$probe.LocalEndpoint.Port;$probe.Stop()
    $hd=Join-Path $runDir ($name+'-host');$cd=Join-Path $runDir ($name+'-client');New-Item -ItemType Directory $hd,$cd | Out-Null
    $h=$null;$c=$null;$r=$null
    try {
        $h=[GuiDriver]::new($Exe,$hd);$h.Set(102,[string]$port);$h.Click(103);$h.Wait(107,'Waiting')
        $clientPort=$port
        if($fault){$r=[FrameRelay]::new($port);$clientPort=$r.Port;$r.HoldAck=$fault -eq 'ack';$r.DropAck=$fault -eq 'drop';$r.PauseChunks=$fault -in @('pause','resume');if($fault -eq 'resume'){$r.PauseAfterBytes=262144}}
        $c=[GuiDriver]::new($Exe,$cd);$c.Set(102,[string]$clientPort);$c.Click(104);$c.Wait(107,'Connected:');$h.Wait(107,'Connected:')
        return @{H=$h;C=$c;R=$r;HD=$hd;CD=$cd;Port=$port;Name=$name}
    }catch{if($r){$r.Dispose()};if($h){$h.Dispose()};if($c){$c.Dispose()};throw}
}
function Finish($p) {
    foreach($app in @($p.H,$p.C)){try{[IO.File]::WriteAllText((Join-Path $runDir ($p.Name+'-'+$app.Process.Id+'.log')),$app.Text(108))}catch{}}
    if($p.R){$p.R.Dispose()};$p.H.Dispose();$p.C.Dispose()
}
function File([string]$name,[int64]$size){$path=Join-Path $runDir $name;$s=[IO.File]::Create($path);$s.SetLength($size);$s.Dispose();return $path}
function Begin($p,[string]$source){$p.C.Pick($source);$p.C.Click(113);$p.H.Wait(115,[IO.Path]::GetFileName($source));$p.H.Click(116)}
function Hash($source,$saved){Assert ((Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $saved).Hash) 'GUI transfer hash mismatch'}
$empty=File 'empty.bin' 0
$large=File 'gui-large.bin' 8MB
$p=New-Pair 'ack-wait' 'ack'
try {
    Begin $p $empty;Assert ($p.R.WaitAck(5000)) 'Missing held ACK';$p.C.Wait(122,'AwaitAck')
    Assert (!$p.C.Text(122).Contains('Completed')) '100% falsely completed';$p.C.Responsive();$p.H.Responsive()
    $p.R.ReleaseAck();$p.C.Wait(122,'Completed:');$p.H.Close();$p.C.Close()
    Pass 'GUI 100% data stays AwaitAck until matching stored ACK; UI remains responsive'
}finally{Finish $p}
foreach($side in @('sender','receiver')) {
    $p=New-Pair ('cancel-'+$side) 'pause'
    try {
        Begin $p $large;Assert ($p.R.WaitChunk(5000)) 'Missing paused data'
        if($side -eq 'sender'){$p.C.Click(118);$p.C.Wait(108,'Cancelled')}else{$p.H.Click(119);$p.H.Wait(108,'Cancelled')}
        $p.R.ReleaseChunks();$p.C.Wait(122,'Cancelled');$p.H.Wait(108,'Cancelled')
        Assert (!(Test-Path (Join-Path $p.HD 'received/gui-large.bin'))) 'Cancelled GUI file committed'
        $p.C.Responsive();$p.H.Responsive();$p.H.Close();$p.C.Close()
        Pass "GUI active $side cancellation and opposite UI responsiveness"
    }finally{Finish $p}
}
$p=New-Pair 'duplex' 'delay'
try {
    $p.R.ChunkDelayMs=5;$p.H.Pick($large);$p.C.Pick($large);$p.H.Click(113);$p.C.Click(113)
    $p.H.Wait(115,'gui-large.bin');$p.C.Wait(115,'gui-large.bin');$p.H.Click(116);$p.C.Click(116)
    $p.H.Wait(122,'Completed:',30000);$p.C.Wait(122,'Completed:',30000)
    Hash $large (Join-Path $p.HD 'received/gui-large.bin');Hash $large (Join-Path $p.CD 'received/gui-large.bin')
    $p.H.Close();$p.C.Close();Pass 'GUI bidirectional simultaneous files with separate ACKs and SHA-256'
}finally{Finish $p}
$p=New-Pair 'recovery' 'resume'
try {
    Begin $p $large;Assert ($p.R.WaitChunk(5000)) 'Resume pause missing'
    $p.C.Wait(122,'requestId=')
    $id=$p.C.Text(124);Assert ($id.Length -eq 64) 'GUI persistent ID missing'
    $p.C.Process.Kill();Assert ($p.C.Process.WaitForExit(3000)) 'Sender kill failed';$p.R.Cut()
    $p.H.Click(105);$p.H.Wait(107,'Disconnected')
    $part=Join-Path $p.HD 'received/gui-large.bin.part';Assert ((Get-Item $part).Length -gt 0) 'No resume checkpoint'
    $p.R.Dispose();$p.R=$null;$p.C.Dispose();$p.H.Click(106);$p.H.Wait(107,'Waiting')
    $p.C=[GuiDriver]::new($Exe,$p.CD);$p.C.Set(102,[string]$p.Port);$p.C.Click(104);$p.C.Wait(107,'Connected:')
    $p.C.Pick($large);$p.C.Set(124,$id);$p.C.Click(125);$p.H.Wait(115,'gui-large.bin');$p.H.Click(116)
    $p.C.Wait(122,'Completed:',30000);Hash $large (Join-Path $p.HD 'received/gui-large.bin')
    $p.H.Close();$p.C.Close();Pass 'GUI abnormal sender exit, reconnect and persistent-ID partial resume'
}finally{Finish $p}
$p=New-Pair 'close-send' 'pause'
try{Begin $p $large;Assert ($p.R.WaitChunk(5000)) 'No backpressure';$p.C.Close();$p.H.Close();Pass 'GUI close during backpressured send joins workers'}finally{Finish $p}
$p=New-Pair 'repeat'
try {
    $p.H.Process.Refresh();$p.C.Process.Refresh();$hb=$p.H.Process.HandleCount;$cb=$p.C.Process.HandleCount
    for($i=0;$i -lt 8;$i++){
        $p.H.Click(105);$p.C.Click(105);$p.H.Wait(107,'Disconnected');$p.C.Wait(107,'Disconnected')
        $p.H.Click(106);$p.H.Wait(107,'Waiting');$p.C.Click(106);$p.C.Wait(107,'Connected:');$p.H.Wait(107,'Connected:')
    }
    $p.H.Process.Refresh();$p.C.Process.Refresh()
    Assert (($p.H.Process.HandleCount-$hb) -le 8 -and ($p.C.Process.HandleCount-$cb) -le 8) 'Repeated GUI connection leaked handles'
    $p.C.Set(101,'invalid');$p.C.Click(104);$p.C.Wait(108,'IPv4');$p.C.Responsive()
    $p.H.Close();$p.C.Close();Pass '8 GUI connection/disconnect cycles: stable handles and invalid-input response'
}finally{Finish $p}
. "$PSScriptRoot/SocketIsolation.ps1"
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();Protect-TestListener $listener
$folder=Join-Path $runDir 'event-flood';New-Item -ItemType Directory $folder | Out-Null
$app=$null;$peer=$null
try {
    $app=[GuiDriver]::new($Exe,$folder);$app.Set(102,[string]$listener.LocalEndpoint.Port);$pending=$listener.AcceptTcpClientAsync();$app.Click(104)
    Assert ($pending.Wait(5000)) 'GUI flood connection missing';$peer=$pending.Result;$app.Wait(107,'Connected:');$listener.Stop()
    $app.Process.Refresh();$before=$app.Process.PrivateMemorySize64
    $payload=[Text.Encoding]::UTF8.GetBytes(('가' * 1000));$n=$payload.Length
    [byte[]]$header=@(1,0,0,(($n -shr 8) -band 255),($n -band 255));$stream=$peer.GetStream()
    for($i=0;$i -lt 1000;$i++){$stream.Write($header,0,5);$stream.Write($payload,0,$payload.Length)}
    $app.Wait(108,'상대:');$app.Responsive();$app.Process.Refresh()
    Assert ($app.Text(108).Length -le 80000) 'GUI transcript is unbounded'
    Assert (($app.Process.PrivateMemorySize64-$before) -lt 32MB) 'GUI event flood memory growth'
    $peer.Close();$app.Close();Pass '1000 UTF-8 TCP text events: bounded transcript/memory and responsive UI'
}finally{if($peer){$peer.Dispose()};$listener.Stop();if($app){$app.Dispose()}}
Write-Output "Advanced GUI artifacts: $runDir"
