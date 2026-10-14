param([string]$Exe = "$PSScriptRoot/../x64/Release/FileTransferApp.exe")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/stage3_validation.ps1" -Exe $Exe -HelpersOnly
. "$PSScriptRoot/SocketIsolation.ps1"
foreach ($automatic in @($false,$true)) {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $listener.Start(); Protect-TestListener $listener; $port = $listener.LocalEndpoint.Port
    $argsText = "--connect 127.0.0.1 --port $port"
    if ($automatic) { $argsText += ' --reconnect' }
    $app = New-App ("reconnect-$automatic") $argsText
    $peer = $null
    try {
        $pending = $listener.AcceptTcpClientAsync(); Assert ($pending.Wait(5000)) 'Initial connection missing'
        $peer = $pending.Result; $app.WaitText('Commands:')
        $app.Process.Refresh(); $handlesBefore = $app.Process.HandleCount
        for ($i = 1; $i -le 4; $i++) {
            $peer.Close()
            Wait-Count $app 'Peer disconnected.' $i
            $pending = $listener.AcceptTcpClientAsync()
            if (!$automatic) { $app.Send('/reconnect') }
            Assert ($pending.Wait(5000)) 'Reconnect did not arrive'
            $peer = $pending.Result
            Wait-Count $app '[Reconnected]' $i
            Write-Frame $peer 1 ([Text.Encoding]::ASCII.GetBytes("new-session-$i"))
            $app.WaitText("[Received TEXT] new-session-$i")
        }
        $app.Process.Refresh()
        Assert (($app.Process.HandleCount - $handlesBefore) -le 3) 'Reconnect leaked handles'
        $app.Send('/quit'); $app.WaitExit(3000)
        Pass "four same-process sessions with stable handles; automatic=$automatic"
    } finally {
        [IO.File]::WriteAllText((Join-Path $runDir ("reconnect-$automatic.log")),$app.Output)
        if ($peer) { $peer.Dispose() }; $listener.Stop(); $app.Dispose()
    }
}
$probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$probe.Start(); $port = $probe.LocalEndpoint.Port; $probe.Stop()
$app = New-App 'server-reconnect' "--host --bind 127.0.0.1 --port $port --reconnect"
$peer = $null
try {
    $app.WaitText('Waiting for connection')
    $peer = [Net.Sockets.TcpClient]::new('127.0.0.1',$port); $app.WaitText('Commands:')
    $peer.Close(); $app.WaitText('Peer disconnected.')
    Wait-Count $app 'Waiting for connection' 2
    $peer = [Net.Sockets.TcpClient]::new('127.0.0.1',$port)
    $app.WaitText('[Reconnected]')
    Write-Frame $peer 1 ([Text.Encoding]::ASCII.GetBytes('server-new-session'))
    $app.WaitText('[Received TEXT] server-new-session')
    $peer.Close(); Wait-Count $app 'Peer disconnected.' 2
    $app.WaitText('[Reconnect attempt] 1/3')
    $watch = [Diagnostics.Stopwatch]::StartNew(); $app.Send('/quit'); $app.WaitExit(3000)
    Assert ($watch.ElapsedMilliseconds -lt 3000) 'Quit failed to interrupt retry'
    Pass 'server bounded accept retry and explicit quit interrupts reconnect'
} finally {
    [IO.File]::WriteAllText((Join-Path $runDir 'server-reconnect.log'),$app.Output)
    if ($peer) { $peer.Dispose() }; $app.Dispose()
}
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); Protect-TestListener $listener; $port = $listener.LocalEndpoint.Port
$app = New-App 'bounded-retries' "--connect 127.0.0.1 --port $port --reconnect"
$peer = $null
try {
    $pending = $listener.AcceptTcpClientAsync(); Assert ($pending.Wait(5000)) 'Missing connection'
    $peer = $pending.Result; $app.WaitText('Commands:')
    $listener.Stop(); $peer.Close()
    $app.WaitText('Reconnect attempts exhausted',10000)
    Assert ([regex]::Matches($app.Output,'\[Reconnect attempt\]').Count -eq 3) 'Retry count unbounded'
    $app.Send('/quit'); $app.WaitExit()
    Pass 'unavailable endpoint stops after exactly three exponential retries'
} finally {
    [IO.File]::WriteAllText((Join-Path $runDir 'bounded-retries.log'),$app.Output)
    if ($peer) { $peer.Dispose() }; $listener.Stop(); $app.Dispose()
}
Write-Output "Reconnect logs: $runDir"
