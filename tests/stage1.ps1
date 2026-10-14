param([string]$Exe = "$PSScriptRoot/../x64/Debug/FileTransferApp.exe", [switch]$HelpersOnly)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
# Each invocation owns its processes and files; existing received files are untouched.
$runDir = Join-Path $PSScriptRoot ('runs/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $runDir | Out-Null
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Text;
using System.Threading;
using System.IO;
public sealed class TestApp : IDisposable {
    public readonly Process Process;
    readonly StringBuilder output = new StringBuilder();
    readonly Thread stdout, stderr;
    public TestApp(string exe, string cwd) : this(exe, cwd, "") { }
    public TestApp(string exe, string cwd, string arguments) {
        Process = new Process { StartInfo = new ProcessStartInfo(exe) {
            Arguments = arguments,
            WorkingDirectory = cwd, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardInput = true, RedirectStandardOutput = true,
            RedirectStandardError = true } };
        Process.Start();
        stdout = new Thread(() => Read(Process.StandardOutput));
        stderr = new Thread(() => Read(Process.StandardError));
        stdout.IsBackground = stderr.IsBackground = true;
        stdout.Start(); stderr.Start();
    }
    void Read(StreamReader reader) {
        int c;
        while ((c = reader.Read()) != -1) lock(output) output.Append((char)c);
    }
    public string Output { get { lock(output) return output.ToString(); } }
    public void Send(string line) { Process.StandardInput.WriteLine(line); Process.StandardInput.Flush(); }
    public void WaitText(string text, int timeout = 5000) {
        var watch = Stopwatch.StartNew();
        while (watch.ElapsedMilliseconds < timeout) {
            if (Output.Contains(text)) return;
            Thread.Sleep(10);
        }
        throw new Exception("Timed out waiting for: " + text + "\n" + Output);
    }
    public void WaitExit(int timeout = 5000) {
        if (!Process.WaitForExit(timeout)) throw new Exception("Exit timed out\n" + Output);
        stdout.Join(); stderr.Join();
        if (Process.ExitCode != 0) throw new Exception("Exit code " + Process.ExitCode + "\n" + Output);
    }
    public void Dispose() {
        if (!Process.HasExited) { Process.Kill(); Process.WaitForExit(); }
        stdout.Join(); stderr.Join(); Process.Dispose();
    }
}
'@

function New-App([string]$name,[string]$Arguments = '') {
    $dir = Join-Path $runDir $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    return [TestApp]::new($Exe,$dir,$Arguments)
}
function New-RawConnection {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,9000)
    $listener.Start()
    $app = $null
    try {
        $app = New-App ([Guid]::NewGuid().ToString('N'))
        $app.Send('2'); $app.Send('127.0.0.1')
        $pending = $listener.AcceptTcpClientAsync()
        if (!$pending.Wait(5000)) { throw 'Connection timed out' }
        $peer = $pending.Result
        $peer.ReceiveTimeout = 5000
        $peer.SendTimeout = 5000
        $app.WaitText('Commands:')
        return @{ App=$app; Peer=$peer }
    } catch {
        if ($app) { $app.Dispose() }
        throw
    } finally { $listener.Stop() }
}
function Read-Exact($stream,[int]$size) {
    $data = New-Object byte[] $size
    $offset = 0
    while ($offset -lt $size) {
        $n = $stream.Read($data,$offset,$size-$offset)
        if ($n -eq 0) { throw 'Unexpected EOF' }
        $offset += $n
    }
    return ,$data
}
function Read-Frame($peer) {
    $stream = $peer.GetStream()
    $h = Read-Exact $stream 5
    $length = ([int]$h[1] -shl 24) -bor ([int]$h[2] -shl 16) -bor ([int]$h[3] -shl 8) -bor [int]$h[4]
    $payload = Read-Exact $stream $length
    [UInt64]$id = 0
    $proof = $null
    if ($h[0] -ge 2 -and $h[0] -le 11) {
        Assert ($payload.Length -ge 8) 'File message missing request ID'
        $id = Read-U64 $payload
        $payload = [byte[]]@($payload | Select-Object -Skip 8)
        if ($h[0] -in @(2,6) -and $payload.Length -ge 137) {
            $proof = [Text.Encoding]::ASCII.GetString($payload,0,128)
            $payload = [byte[]]@($payload | Select-Object -Skip 129)
        }
        if ($h[0] -eq 6) {
            $script:outgoingIds[$peer.GetHashCode()] = $id
            $script:outgoingProofs[$peer.GetHashCode()] = $proof
        }
        if ($h[0] -eq 7) { $payload = [byte[]]@() }
        if ($h[0] -in @(5,11)) { $proof = [Text.Encoding]::ASCII.GetString($payload); $payload = [byte[]]@() }
    }
    return @{ Type=$h[0]; Payload=$payload; RequestId=$id; Proof=$proof }
}
$script:outgoingIds = @{}
$script:outgoingProofs = @{}
$script:rawDigest = $null
function SHA256-Bytes([byte[]]$bytes) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($bytes))).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose() }
}
function U64-Bytes([UInt64]$value) {
    [byte[]]$bytes = New-Object byte[] 8
    for ($i = 0; $i -lt 8; $i++) { $bytes[$i] = ($value -shr ((7-$i)*8)) -band 255 }
    return ,$bytes
}
function Read-U64([byte[]]$bytes) {
    [UInt64]$value = 0
    for ($i = 0; $i -lt 8; $i++) { $value = ($value -shl 8) -bor $bytes[$i] }
    return $value
}
function Write-Frame($peer,[byte]$type,[byte[]]$payload,[UInt64]$RequestId = 0,[switch]$NoId) {
    if ($type -ge 2 -and $type -le 11 -and !$NoId) {
        if ($type -in @(2,6) -and $payload.Length -ge 9) {
            $digest = if ($script:rawDigest) { $script:rawDigest } elseif ((Read-U64 $payload) -eq 0) { SHA256-Bytes @() } else { '0' * 64 }
            $identityBytes = [byte[]]([Text.Encoding]::ASCII.GetBytes("FTA5`0") + $payload[0..7] + [Text.Encoding]::ASCII.GetBytes($digest) + $payload[8..($payload.Length-1)])
            $identity = SHA256-Bytes $identityBytes
            $payload = [byte[]]([Text.Encoding]::ASCII.GetBytes($identity + $digest) + [byte]0 + $payload)
        }
        if ($type -eq 7 -and !$payload.Length) {
            $payload = [byte[]]((U64-Bytes 0) + [Text.Encoding]::ASCII.GetBytes((SHA256-Bytes @())))
        }
        if ($type -in @(5,11) -and !$payload.Length) {
            $proof = $script:outgoingProofs[$peer.GetHashCode()]
            if (!$proof) { $proof = '0' * 128 }
            $payload = [Text.Encoding]::ASCII.GetBytes($proof)
        }
        if (!$RequestId) {
            if ($type -in @(5,7,8,9) -and $script:outgoingIds.ContainsKey($peer.GetHashCode())) {
                $RequestId = $script:outgoingIds[$peer.GetHashCode()]
            } else { $RequestId = 1 }
        }
        $payload = [byte[]]((U64-Bytes $RequestId) + $payload)
    }
    $n = $payload.Length
    [byte[]]$header = @($type, (($n -shr 24) -band 255), (($n -shr 16) -band 255), (($n -shr 8) -band 255), ($n -band 255))
    $stream = $peer.GetStream()
    $stream.Write($header,0,5)
    if ($n) { $stream.Write($payload,0,$n) }
}
function Assert($condition,[string]$message) { if (!$condition) { throw $message } }
function Pass([string]$name) { Write-Output "PASS: $name" }

if ($HelpersOnly) { return }

$small = Join-Path $runDir 'small.txt'
[IO.File]::WriteAllText($small,'stage1 test payload')
$large = Join-Path $runDir 'large.bin'
$f = [IO.File]::Create($large)
$f.SetLength(32MB); $f.Dispose()

# Two actual exe instances: duplex text, accept/save/ACK, reject, simultaneous
# requests, then /quit while both outgoing requests are awaiting approval.
$hostApp = New-App 'host'
$clientApp = $null
try {
    $hostApp.Send('1')
    Start-Sleep -Milliseconds 300
    $clientApp = New-App 'client'
    $clientApp.Send('2'); $clientApp.Send('127.0.0.1')
    $hostApp.WaitText('Commands:'); $clientApp.WaitText('Commands:')
    # Several receive timeouts must not disconnect a healthy idle peer.
    Start-Sleep -Milliseconds 800
    $hostApp.Send('hello-from-host'); $clientApp.Send('hello-from-client')
    $hostApp.WaitText('[Received TEXT] hello-from-client')
    $clientApp.WaitText('[Received TEXT] hello-from-host')
    Pass 'localhost duplex text with two app instances'
    $hostApp.Send('/file "' + $small + '"')
    $clientApp.WaitText('[Incoming file request] small.txt')
    $clientApp.Send('/accept')
    $hostApp.WaitText('[Transfer completed] small.txt')
    $saved = Join-Path $runDir 'client/received/small.txt'
    Assert ((Get-FileHash $small).Hash -eq (Get-FileHash $saved).Hash) 'SHA-256 mismatch'
    Pass 'existing approval, file save, ACK and SHA-256 regression'
    $clientApp.Send('/file "' + $small + '"')
    $hostApp.WaitText('[Incoming file request] small.txt')
    $hostApp.Send('/reject')
    $clientApp.WaitText('File rejected, cancelled, or timed out')
    Pass 'existing rejection regression'
    # Wait for the worker to publish completion before issuing another command.
    $clientApp.WaitText('File transfer not completed')
    $hostApp.Send('/file "' + $large + '"')
    $clientApp.Send('/file "' + $large + '"')
    $hostApp.WaitText('[Incoming file request] large.bin')
    $clientApp.WaitText('[Incoming file request] large.bin')
    $hostApp.Send('/quit'); $clientApp.Send('/quit')
    $hostApp.WaitExit(); $clientApp.WaitExit()
    Pass 'simultaneous requests and /quit during approval wait'
} finally {
    [IO.File]::WriteAllText((Join-Path $runDir 'host.log'),$hostApp.Output)
    $hostApp.Dispose()
    if ($clientApp) {
        [IO.File]::WriteAllText((Join-Path $runDir 'client.log'),$clientApp.Output)
        $clientApp.Dispose()
    }
}

foreach ($mode in @('quit','eof','reset','bad-frame')) {
    $c = New-RawConnection
    try {
        $c.App.Send('/file "' + $small + '"')
        Assert ((Read-Frame $c.Peer).Type -eq 6) 'Expected FILE_REQUEST'
        $c.App.WaitText('Waiting for file approval...')
        $watch = [Diagnostics.Stopwatch]::StartNew()
        switch ($mode) {
            'quit' { $c.App.Send('/quit') }
            'eof' { $c.Peer.Close() }
            'reset' {
                $c.Peer.Client.LingerState = [Net.Sockets.LingerOption]::new($true,0)
                $c.Peer.Close()
            }
            'bad-frame' { Write-Frame $c.Peer 255 @() }
        }
        $c.App.WaitText('File transfer not completed',3000)
        if ($mode -ne 'quit') {
            $c.App.Send('/file "' + $large + '"')
            $c.App.Send('/quit')
        }
        $c.App.WaitExit(3000)
        Assert ($watch.ElapsedMilliseconds -lt 3000) 'Cancellation took too long'
        Pass "approval cancellation: $mode (under 3 seconds)"
    } finally {
        [IO.File]::WriteAllText((Join-Path $runDir ($mode + '.log')),$c.App.Output)
        $c.Peer.Dispose(); $c.App.Dispose()
    }
}

# A peer accepts but stops reading. The upload exceeds socket buffers.
$c = New-RawConnection
try {
    $c.Peer.ReceiveBufferSize = 1024
    $c.App.Send('/file "' + $large + '"')
    Assert ((Read-Frame $c.Peer).Type -eq 6) 'Expected FILE_REQUEST'
    Write-Frame $c.Peer 7 @()
    $c.App.WaitText('Sending:')
    Start-Sleep -Milliseconds 500
    Assert (!$c.App.Output.Contains('[Upload finished')) 'Upload did not block as expected'
    $c.App.Send('/quit')
    $c.App.WaitExit(3000)
    Pass '/quit interrupts backpressured file send'
} finally {
    [IO.File]::WriteAllText((Join-Path $runDir 'blocked-send.log'),$c.App.Output)
    $c.Peer.Dispose(); $c.App.Dispose()
}

# Send timeout must also stop a stalled transfer without a local /quit.
$c = New-RawConnection
try {
    $c.Peer.ReceiveBufferSize = 1024
    $c.App.Send('/file "' + $large + '"')
    Assert ((Read-Frame $c.Peer).Type -eq 6) 'Expected FILE_REQUEST'
    Write-Frame $c.Peer 7 @()
    $c.App.WaitText('Sending:')
    $c.App.WaitText('File transfer not completed',7000)
    Assert (!$c.App.Output.Contains('[Upload finished')) 'Stalled upload reported success'
    $c.App.Send('/quit'); $c.App.WaitExit(3000)
    Pass 'stalled send times out and stops the connection'
} finally {
    [IO.File]::WriteAllText((Join-Path $runDir 'send-timeout.log'),$c.App.Output)
    $c.Peer.Dispose(); $c.App.Dispose()
}

$c = New-RawConnection
try {
    [byte[]]$metadata = @(0,0,0,0,0,0,0,10) + [Text.Encoding]::ASCII.GetBytes('partial.txt')
    Write-Frame $c.Peer 6 $metadata
    $c.App.WaitText('[Incoming file request] partial.txt')
    $c.App.Send('/accept')
    Assert ((Read-Frame $c.Peer).Type -eq 7) 'Expected FILE_ACCEPT'
    Write-Frame $c.Peer 2 $metadata
    Write-Frame $c.Peer 3 ([Text.Encoding]::ASCII.GetBytes('abc'))
    $part = Join-Path $c.App.Process.StartInfo.WorkingDirectory 'received/partial.txt.part'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (!(Test-Path $part) -and $watch.ElapsedMilliseconds -lt 3000) {
        Start-Sleep -Milliseconds 10
    }
    Assert (Test-Path $part) 'Expected an active partial file'
    $c.Peer.Close()
    $c.App.WaitText('Peer disconnected.')
    Assert ((Test-Path $part) -and (Test-Path ($part + '.fta'))) 'Owned resume checkpoint was not retained'
    $c.App.Send('/quit'); $c.App.WaitExit()
    Pass 'peer disconnect closes handles and retains authenticated resume checkpoint (stage5 policy)'
} finally { $c.Peer.Dispose(); $c.App.Dispose() }

$c = New-RawConnection
try {
    # A split frame followed by two frames in one write.
    [byte[]]$frame = @(1,0,0,0,5,115,112,108,105,116)
    $stream = $c.Peer.GetStream()
    foreach ($b in $frame) { $stream.WriteByte($b) }
    $c.App.WaitText('[Received TEXT] split')
    [byte[]]$frames = @(1,0,0,0,1,65,1,0,0,0,1,66)
    $stream.Write($frames,0,$frames.Length)
    $c.App.WaitText('[Received TEXT] A'); $c.App.WaitText('[Received TEXT] B')
    $c.App.Send('/quit'); $c.App.WaitExit()
    Pass 'split and coalesced TCP frames; idle receive shutdown'
} finally { $c.Peer.Dispose(); $c.App.Dispose() }
Write-Output "Logs and isolated fixtures: $runDir"
