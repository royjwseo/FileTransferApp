param([string]$Exe = "$PSScriptRoot/../x64/Release/FileTransferApp.exe",[switch]$AckCloseOnly,[switch]$HelpersOnly)
$validationHelpersOnly = $HelpersOnly.IsPresent
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/stage1.ps1" -Exe $Exe -HelpersOnly
Add-Type -Path "$PSScriptRoot/FrameRelay.cs"
$script:sourceHashes = @{}

function Record-SourceHash([string]$path) {
    $script:sourceHashes[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

function Wait-Count($app,[string]$text,[int]$count,[int]$timeout = 5000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $timeout) {
        if ([regex]::Matches($app.Output,[regex]::Escape($text)).Count -ge $count) { return }
        Start-Sleep -Milliseconds 10
    }
    throw "Missing occurrence $count of $text`n$($app.Output)"
}
function Wait-FileState([string]$path,[bool]$exists,[int]$timeout = 5000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ((Test-Path -LiteralPath $path) -ne $exists -and $watch.ElapsedMilliseconds -lt $timeout) {
        Start-Sleep -Milliseconds 10
    }
    Assert ((Test-Path -LiteralPath $path) -eq $exists) "Unexpected file existence ($exists): $path"
}
function Assert-Hash([string]$source,[string]$saved) {
    Assert ($script:sourceHashes.ContainsKey($source)) 'Missing pre-transfer SHA-256'
    $before = $script:sourceHashes[$source]
    Assert ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -eq $before) 'Source changed during transfer'
    Assert ((Get-FileHash -LiteralPath $saved -Algorithm SHA256).Hash -eq $before) 'SHA-256 mismatch'
    Assert (!(Test-Path -LiteralPath ($saved + '.part'))) 'Committed file retained part'
}
function New-Pair([string]$name,[string]$fault = '',[switch]$Direct) {
    $probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $probe.Start(); $port = $probe.LocalEndpoint.Port; $probe.Stop()
    $hostApp = $null; $clientApp = $null; $relay = $null
    try {
        $hostApp = New-App ($name + '-host') "--host --bind 127.0.0.1 --port $port"
        $hostApp.WaitText('Waiting for connection on port')
        if (!$Direct) {
            $relay = [FrameRelay]::new($port)
            $relay.HoldAck = $fault -eq 'hold-ack'
            $relay.DropAck = $fault -eq 'drop-ack'
            $relay.PauseChunks = $fault -eq 'pause-chunks'
            $port = $relay.Port
        }
        $clientApp = New-App ($name + '-client') ("--connect 127.0.0.1 --port " + $port)
        $hostApp.WaitText('Commands:'); $clientApp.WaitText('Commands:')
        return @{ HostApp=$hostApp; ClientApp=$clientApp; Relay=$relay; Name=$name; InstanceId=[Guid]::NewGuid().ToString('N') }
    } catch {
        if ($relay) { $relay.Dispose() }
        if ($hostApp) { $hostApp.Dispose() }
        if ($clientApp) { $clientApp.Dispose() }
        throw
    }
}
function Save-Pair($pair) {
    try {
        foreach ($app in @($pair.HostApp,$pair.ClientApp)) {
            $logName = $pair.Name + '-' + $pair.InstanceId + '-' +
                [IO.Path]::GetFileName($app.Process.StartInfo.WorkingDirectory) + '.log'
            [IO.File]::WriteAllText((Join-Path $runDir $logName),$app.Output)
        }
    } finally {
        if ($pair.Relay) { $pair.Relay.Dispose() }
        $pair.HostApp.Dispose(); $pair.ClientApp.Dispose()
    }
}
function Stop-Pair($pair) {
    $pair.HostApp.Send('/quit'); $pair.ClientApp.Send('/quit')
    $pair.HostApp.WaitExit(); $pair.ClientApp.WaitExit()
}
function Start-Transfer($sender,$receiver,[string]$path,[switch]$SkipHash) {
    $name = [IO.Path]::GetFileName($path)
    if (!$SkipHash) { Record-SourceHash $path }
    $sender.Send('/file "' + $path + '"')
    $receiver.WaitText('[Incoming file request] ' + $name)
    $receiver.Send('/accept')
}
function Saved-Path($app,[string]$name) {
    return Join-Path $app.Process.StartInfo.WorkingDirectory ('received/' + $name)
}

if ($validationHelpersOnly) { return }

# A valid ACK followed immediately by FIN must not race cancellation and undo
# already confirmed storage. Repeated real process runs exercise the scheduling.
$empty = Join-Path $runDir 'ack-close.bin'
[IO.File]::WriteAllBytes($empty,[byte[]]@())
for ($attempt = 0; $attempt -lt 20; $attempt++) {
    $c = New-RawConnection
    try {
        $c.App.Send('/file "' + $empty + '"')
        $req = Read-Frame $c.Peer
        Assert ($req.Type -eq 6) 'Missing request'
        Write-Frame $c.Peer 7 @() -RequestId $req.RequestId
        $begin = Read-Frame $c.Peer; $end = Read-Frame $c.Peer
        Assert ($begin.Type -eq 2 -and $end.Type -eq 4) 'Missing BEGIN/END'
        Write-Frame $c.Peer 5 @() -RequestId $req.RequestId
        $c.Peer.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
        Start-Sleep -Milliseconds 100
        $c.App.Send('/quit'); $c.App.WaitExit()
        # The receiver's disconnect log can interleave between the sender's
        # completion marker and filename. This case has one outgoing request.
        Assert ($c.App.Output.Contains('[Transfer completed]')) 'ACK followed by FIN lost confirmed success'
        Assert (!$c.App.Output.Contains('File transfer not completed')) 'Valid ACK was cancelled by following FIN'
    } finally {
        [IO.File]::WriteAllText((Join-Path $runDir ("ack-close-$attempt.log")),$c.App.Output)
        $c.Peer.Dispose(); $c.App.Dispose()
    }
}
Pass 'ACK followed by immediate FIN preserves success (20 process runs)'
if ($AckCloseOnly) { Write-Output "Logs and isolated fixtures: $runDir"; return }

# CLI parsing: invalid options must terminate before connecting or listening.
foreach ($arguments in @('--port 9000','--host --port 0','--host --port 65536',
    '--host --port -1','--host --port 9x','--host --port','--host --host',
    '--host --connect 127.0.0.1','--connect 127.0.0.1 --bind 127.0.0.1',
    '--host --port 9000 --port 9001','--connect invalid','--host --bind invalid',
    '--unknown','--version --host')) {
    $app = New-App ([Guid]::NewGuid().ToString('N')) $arguments
    try {
        Assert ($app.Process.WaitForExit(3000)) "CLI did not reject: $arguments"
        Assert ($app.Process.ExitCode -ne 0) "CLI accepted: $arguments"
    } finally { $app.Dispose() }
}
Pass 'CLI rejects invalid, missing, duplicated and conflicting options (14 cases)'
foreach ($arguments in @('--help','--version')) {
    $app = New-App ([Guid]::NewGuid().ToString('N')) $arguments
    try {
        $app.WaitExit()
        if ($arguments -eq '--version') { Assert ($app.Output.Contains('protocol=4 requestId64-parity')) 'Missing protocol version' }
        else { Assert ($app.Output.Contains('--host')) 'Missing usage' }
    } finally { $app.Dispose() }
}
Pass 'CLI help and stable wire-version identity'

$pair = New-Pair 'cli-duplex'
try {
    $pair.ClientApp.Send('first-command'); $pair.HostApp.Send('host-first-command')
    $pair.HostApp.WaitText('[Received TEXT] first-command')
    $pair.ClientApp.WaitText('[Received TEXT] host-first-command')
    Stop-Pair $pair
    Pass 'custom loopback bind and ports; CLI preserves the first stdin command'
} finally { Save-Pair $pair }

# Random data in bounded chunks; 100 MiB regression for the former file-size limit.
$large = Join-Path $runDir 'large-100MiB.bin'
$stream = [IO.File]::Create($large)
try {
    [byte[]]$block = New-Object byte[] 1MB
    $random = [Random]::new(20261014)
    for ($i = 0; $i -lt 100; $i++) { $random.NextBytes($block); $stream.Write($block,0,$block.Length) }
} finally { $stream.Dispose() }
$zero = Join-Path $runDir 'zero.bin'
[IO.File]::WriteAllBytes($zero,[byte[]]@())
$pair = New-Pair 'boundary-files'
try {
    Start-Transfer $pair.HostApp $pair.ClientApp $zero
    $pair.HostApp.WaitText('[Transfer completed] zero.bin')
    Assert-Hash $zero (Saved-Path $pair.ClientApp 'zero.bin')
    Pass 'A to B actual applications: zero-byte save, ACK and SHA-256'
    Start-Transfer $pair.ClientApp $pair.HostApp $large
    $pair.ClientApp.WaitText('[Transfer completed] large-100MiB.bin',30000)
    Assert-Hash $large (Saved-Path $pair.HostApp 'large-100MiB.bin')
    Stop-Pair $pair
    Pass 'B to A actual applications: 100 MiB regression save, ACK and SHA-256'
} finally { Save-Pair $pair }

foreach ($mode in @('hold-ack','drop-ack')) {
    $pair = New-Pair $mode $mode
    try {
        Start-Transfer $pair.ClientApp $pair.HostApp $zero
        Assert ($pair.Relay.WaitAck(5000)) 'Receiver did not produce ACK'
        $saved = Saved-Path $pair.HostApp 'zero.bin'
        Assert-Hash $zero $saved
        Assert (!$pair.ClientApp.Output.Contains('[Transfer completed]')) 'Completion before ACK forwarding'
        if ($mode -eq 'hold-ack') {
            Start-Sleep -Milliseconds 1000
            Assert (!$pair.ClientApp.Output.Contains('[Transfer completed]')) 'Held ACK produced success'
            $pair.Relay.ReleaseAck()
            $pair.ClientApp.WaitText('[Transfer completed] zero.bin')
            Pass 'delayed real receiver ACK completes only after relay releases it'
        } else {
            $pair.ClientApp.WaitText('File transfer not completed',20000)
            Assert (!$pair.ClientApp.Output.Contains('[Transfer completed]')) 'Lost ACK produced success'
            Assert-Hash $zero $saved
            $pair.Relay.DropAck = $false
            $pair.Relay.ReplayAck()
            $pair.ClientApp.WaitText('[Ignored stale file response]')
            Assert (!$pair.ClientApp.Output.Contains('[Transfer completed]')) 'Late ACK resurrected failed request'
            $fresh = Join-Path $runDir 'fresh-after-ack-loss.bin'
            [IO.File]::WriteAllBytes($fresh,[byte[]]@(1,3,5,7,9))
            Record-SourceHash $fresh
            $pair.ClientApp.Send('/file "' + $fresh + '"')
            $pair.HostApp.WaitText('[Incoming file request] fresh-after-ack-loss.bin')
            $pair.Relay.ReplayAck()
            Wait-Count $pair.ClientApp '[Ignored stale file response]' 2
            $pair.HostApp.Send('/accept')
            $pair.ClientApp.WaitText('[Transfer completed] fresh-after-ack-loss.bin')
            Assert-Hash $fresh (Saved-Path $pair.HostApp 'fresh-after-ack-loss.bin')
            Assert-Hash $zero $saved
            Pass 'lost real receiver ACK: timeout, committed file preserved, late/replayed ACK ignored, next request succeeds'
        }
        Stop-Pair $pair
    } finally { Save-Pair $pair }
}

foreach ($mode in @('cut-connection','kill-sender','quit-sender')) {
    $pair = New-Pair $mode 'pause-chunks'
    try {
        Start-Transfer $pair.ClientApp $pair.HostApp $large
        Assert ($pair.Relay.WaitChunk(5000)) 'No outgoing chunk to pause'
        $part = (Saved-Path $pair.HostApp 'large-100MiB.bin') + '.part'
        Wait-FileState $part $true
        Assert (!$pair.ClientApp.Output.Contains('[Upload finished')) 'Sender finished before fault injection'
        switch ($mode) {
            'cut-connection' {
                $pair.Relay.Cut()
                $pair.ClientApp.WaitText('File transfer not completed',3000)
            }
            'kill-sender' {
                $pair.ClientApp.Process.Kill()
                Assert ($pair.ClientApp.Process.WaitForExit(3000)) 'Killed sender did not exit'
                $pair.Relay.ReleaseChunks()
            }
            'quit-sender' {
                $pair.ClientApp.Send('/quit'); $pair.ClientApp.WaitExit(3000)
                $pair.Relay.ReleaseChunks()
            }
        }
        $pair.HostApp.WaitText('Peer disconnected.',5000)
        Wait-FileState $part $false
        Assert (!(Test-Path -LiteralPath (Saved-Path $pair.HostApp 'large-100MiB.bin'))) 'Interrupted transfer committed'
        $pair.HostApp.Send('/quit'); $pair.HostApp.WaitExit()
        Pass "interrupted actual upload cleans receiver-owned part: $mode"
    } finally { Save-Pair $pair }
    # Restart both executables after the failed session, retaining the receiver
    # directory, then transfer and hash a new file on a new connection.
    $reconnected = New-Pair $mode
    try {
        $newSource = Join-Path $runDir ($mode + '-reconnected.bin')
        [IO.File]::WriteAllBytes($newSource,[byte[]]@(10,20,30,40))
        Start-Transfer $reconnected.ClientApp $reconnected.HostApp $newSource
        $reconnected.ClientApp.WaitText('[Transfer completed] ' + [IO.Path]::GetFileName($newSource))
        Assert-Hash $newSource (Saved-Path $reconnected.HostApp ([IO.Path]::GetFileName($newSource)))
        Stop-Pair $reconnected
        Pass "new connection and transfer after failure: $mode"
    } finally { Save-Pair $reconnected }
}

$pair = New-Pair 'save-failure'
try {
    $blocker = Join-Path $pair.HostApp.Process.StartInfo.WorkingDirectory 'received'
    [IO.File]::WriteAllText($blocker,'preserve this blocker')
    Start-Transfer $pair.ClientApp $pair.HostApp $zero
    $pair.ClientApp.WaitText('File transfer not completed')
    Assert (!$pair.ClientApp.Output.Contains('[Transfer completed]')) 'Storage failure produced success'
    Assert ([IO.File]::ReadAllText($blocker) -eq 'preserve this blocker') 'Storage blocker changed'
    Assert (!(Test-Path -LiteralPath (Join-Path $blocker 'zero.bin.part'))) 'Storage failure left part'
    $pair.HostApp.Send('/quit'); $pair.ClientApp.Send('/quit')
    $pair.HostApp.WaitExit(); $pair.ClientApp.WaitExit()
    Pass 'actual receiver save failure propagates to actual sender without false success'
} finally { Save-Pair $pair }
# Only remove the exact test-owned blocker after verifying its content above.
Remove-Item -LiteralPath $blocker
$pair = New-Pair 'save-failure'
try {
    Start-Transfer $pair.ClientApp $pair.HostApp $zero
    $pair.ClientApp.WaitText('[Transfer completed] zero.bin')
    Assert-Hash $zero (Saved-Path $pair.HostApp 'zero.bin')
    Stop-Pair $pair
    Pass 'reconnect and successful save after receiver storage failure is corrected'
} finally { Save-Pair $pair }
Write-Output "Logs and isolated fixtures: $runDir"
