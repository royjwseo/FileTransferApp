param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Release/FileTransferAppGui.exe")
$ErrorActionPreference='Stop'
$Exe=(Resolve-Path $Exe).Path
Add-Type -Path "$PSScriptRoot/GuiDriver.cs"
$runDir=Join-Path $PSScriptRoot ('runs/gui-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $runDir | Out-Null
function App([string]$name){$folder=Join-Path $runDir $name;New-Item -ItemType Directory $folder | Out-Null;return [GuiDriver]::new($Exe,$folder)}
function Assert($condition,[string]$message){if(!$condition){throw $message}}
function Pass([string]$text){Write-Output "PASS: $text"}
$one=App 'smoke'
try{$one.Responsive();$one.Close();Pass 'Win32 GUI creation, responsive message loop and normal shutdown'}finally{$one.Dispose()}
$hostApp=App 'host';$clientApp=App 'client'
try {
    $probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$probe.Start();$port=$probe.LocalEndpoint.Port;$probe.Stop()
    foreach($app in @($hostApp,$clientApp)){$app.Set(102,[string]$port)}
    $hostApp.Click(103);$hostApp.Wait(107,'Waiting');$hostApp.Responsive();Pass 'host waits without blocking GUI'
    $clientApp.Click(104);$clientApp.Wait(107,'Connected:');$hostApp.Wait(107,'Connected:')
    Pass 'GUI Host/Client real TCP connection and peer IP'
    $clientApp.Set(109,'안녕하세요 GUI 메시지');$clientApp.Click(110);$hostApp.Wait(108,'안녕하세요 GUI 메시지')
    $hostApp.Set(109,'reply via Enter');$hostApp.Enter();$clientApp.Wait(108,'reply via Enter');Pass 'Unicode bidirectional text and Enter send'
    $source=Join-Path $runDir '한글 사진.pdf';[IO.File]::WriteAllBytes($source,[byte[]](1..255))
    $clientApp.Pick($source);$clientApp.Click(113);$hostApp.Wait(115,'한글 사진.pdf');$hostApp.Click(116)
    $clientApp.Wait(122,'Completed: storage ACK verified');$hostApp.Wait(123,'Saved: hash verified')
    $saved=Join-Path $runDir 'host/received/한글 사진.pdf'
    Assert ((Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $saved).Hash) 'GUI Unicode file SHA-256 mismatch'
    Pass 'native file dialog, Unicode file, approval, saved ACK and SHA-256'
    $clientApp.Click(126);$hostApp.Wait(115,'한글 사진.pdf');$hostApp.Click(116);$clientApp.Wait(108,'verified existing commit')
    Pass 'GUI persistent transfer ID and completion query'
    $other=Join-Path $runDir 'reject.bin';[IO.File]::WriteAllBytes($other,[byte[]]@(1,3,5))
    $clientApp.Pick($other);$clientApp.Click(113);$hostApp.Wait(115,'reject.bin');$hostApp.Click(117);$clientApp.Wait(122,'Failed:')
    Assert (!(Test-Path (Join-Path $runDir 'host/received/reject.bin'))) 'Rejected GUI file saved'
    Pass 'GUI rejects receive request without saving'
    $hostApp.Click(105);$hostApp.Wait(107,'Disconnected');$clientApp.Wait(107,'Disconnected')
    $hostApp.Responsive();$clientApp.Responsive();Pass 'GUI disconnect and responsiveness'
    $hostApp.Click(106);$hostApp.Wait(107,'Waiting');$clientApp.Click(106);$clientApp.Wait(107,'Connected:');$hostApp.Wait(107,'Connected:')
    $hostApp.Close();$clientApp.Close();Pass 'GUI reconnect and close while connected'
}finally{foreach($app in @($hostApp,$clientApp)){try{[IO.File]::WriteAllText((Join-Path $runDir ($app.Process.Id.ToString()+'.log')),$app.Text(108))}catch{};$app.Dispose()}}
$one=App 'waiting-close'
try{$probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$probe.Start();$one.Set(102,[string]$probe.LocalEndpoint.Port);$probe.Stop();$one.Click(103);$one.Wait(107,'Waiting');$one.Close();Pass 'close while accept is pending joins workers'}finally{$one.Dispose()}
Write-Output "GUI artifacts: $runDir"
