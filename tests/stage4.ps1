param([string]$Exe = "$PSScriptRoot/../x64/Release/FileTransferApp.exe",
    [string]$FaultExe = "$PSScriptRoot/runs/stage4-fault-build/bin/FileTransferApp.exe")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/stage3_validation.ps1" -Exe $Exe -HelpersOnly
Add-Type -Path "$PSScriptRoot/Stage4Support.cs"
$productionExe = $Exe
$FaultExe = (Resolve-Path $FaultExe).Path
$measurements = @()
function Meta([UInt64]$size,[string]$name) {
    return ,([byte[]]((U64-Bytes $size) + [Text.Encoding]::ASCII.GetBytes($name)))
}
function Remove-TestFile([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    $root = [IO.Path]::GetFullPath($runDir).TrimEnd('\') + '\'
    Assert ($full.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)) 'Cleanup outside owned run directory'
    Remove-Item -LiteralPath $full
}
function Finish($sender,[string]$name,[int]$timeout = 30000) {
    $sender.WaitText('[Transfer completed] ' + $name,$timeout)
    Start-Sleep -Milliseconds 50
}
function Check-Progress($app,[string]$label,[string]$name,[UInt64]$size) {
    $line = [regex]::Escape($label + ' file=' + $name) + ' requestId=[1-9][0-9]* bytes=' +
        $size + '/' + $size + ' progress=100\.0% rate=[0-9.]+ MiB/s elapsed=[0-9.]+s'
    Assert ([regex]::IsMatch($app.Output,$line)) "Missing final 64-bit progress for $name"
}
function Save-Raw($c,[string]$name) {
    [IO.File]::WriteAllText((Join-Path $runDir ($name + '.log')),$c.App.Output)
    $c.Peer.Dispose(); $c.App.Dispose()
}

# These only validate 64-bit metadata/approval, NOT transfer the declared bytes.
foreach ($size in @([UInt64]4294967295,[UInt64]4294967296,[UInt64]4294967297,[UInt64]1099511627776)) {
    $c = New-RawConnection
    try {
        Write-Frame $c.Peer 6 (Meta $size 'boundary.bin') -RequestId 1
        $c.App.WaitText("boundary.bin ($size bytes)")
        $c.App.Send('/reject')
        $decision = Read-Frame $c.Peer
        Assert ($decision.Type -eq 8 -and $decision.RequestId -eq 1) '64-bit metadata truncated'
        $c.App.Send('/quit'); $c.App.WaitExit()
        Pass "64-bit metadata simulation: $size bytes"
    } finally { Save-Raw $c "metadata-$size" }
}
foreach ($size in @([UInt64]1099511627777,[UInt64]::MaxValue)) {
    $c = New-RawConnection
    try {
        Write-Frame $c.Peer 6 (Meta $size 'overflow.bin') -RequestId 1
        $failureFrame = Read-Frame $c.Peer
        Assert ($failureFrame.Type -eq 9 -and $failureFrame.RequestId -eq 1) 'Out-of-range size accepted'
        Assert ($c.Peer.GetStream().ReadByte() -eq -1) 'Overflow did not stop connection'
        Assert (!(Test-Path (Join-Path $c.App.Process.StartInfo.WorkingDirectory 'received'))) 'Overflow touched disk'
        $c.App.Send('/quit'); $c.App.WaitExit()
        Pass "size upper-bound refusal: $size"
    } finally { Save-Raw $c "overflow-$size" }
}
$c = New-RawConnection
try {
    $meta = Meta ([UInt64]4294967297) 'short-large.bin'
    Write-Frame $c.Peer 6 $meta -RequestId 1
    $c.App.WaitText('[Incoming file request]')
    $c.App.Send('/accept'); Assert ((Read-Frame $c.Peer).Type -eq 7) 'Missing accept'
    Write-Frame $c.Peer 2 $meta -RequestId 1
    Write-Frame $c.Peer 3 @(42) -RequestId 1
    Write-Frame $c.Peer 4 @() -RequestId 1
    Assert ((Read-Frame $c.Peer).Type -eq 9) 'Large short transfer falsely ACKed'
    Assert ($c.Peer.GetStream().ReadByte() -eq -1) 'Short transfer did not stop'
    $c.App.Send('/quit'); $c.App.WaitExit()
    Assert (!(Test-Path (Join-Path $c.App.Process.StartInfo.WorkingDirectory 'received/short-large.bin.part'))) 'Part remained'
    Pass '4 GiB+1 declaration with one byte refuses END and deletes owned part (simulation)'
} finally { Save-Raw $c 'short-large' }

$pair = New-Pair 'large-direct' -Direct
try {
    foreach ($size in @((1GB + 65536),(4GB + 65536))) {
        Assert ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($runDir)).AvailableFreeSpace -gt ($size + 2GB)) 'Insufficient test disk space'
        $name = "actual-$size.bin"
        $source = Join-Path $runDir $name
        [Stage4Data]::Sparse($source,$size)
        Record-SourceHash $source
        $sender = $pair.ClientApp; $receiver = $pair.HostApp
        $sender.Process.Refresh(); $receiver.Process.Refresh()
        $sm = [Stage4Monitor]::new($sender.Process); $rm = [Stage4Monitor]::new($receiver.Process)
        $watch = [Diagnostics.Stopwatch]::StartNew()
        try {
            Start-Transfer $sender $receiver $source -SkipHash
            Finish $sender $name 180000
            $watch.Stop()
        } finally { $sm.Dispose(); $rm.Dispose() }
        $saved = Saved-Path $receiver $name
        Assert-Hash $source $saved
        Check-Progress $sender 'Sending:' $name $size
        Check-Progress $receiver 'Saving:' $name $size
        $lines = @([regex]::Matches($sender.Output,'Sending: file=' + [regex]::Escape($name))).Count
        $receiveLines = @([regex]::Matches($receiver.Output,'Saving: file=' + [regex]::Escape($name))).Count
        Assert ($lines -le ([Math]::Ceiling($watch.Elapsed.TotalSeconds * 2) + 4)) 'Progress output not rate limited'
        Assert ($receiveLines -le ([Math]::Ceiling($watch.Elapsed.TotalSeconds * 2) + 4)) 'Receive progress output not rate limited'
        Assert ($sm.PeakPrivateBytes -lt 256MB -and $rm.PeakPrivateBytes -lt 256MB) 'Application memory exceeded 256 MiB envelope'
        $measurements += [pscustomobject]@{ Case='actual-direct'; Bytes=$size; Seconds=$watch.Elapsed.TotalSeconds; MiBps=($size/1MB/$watch.Elapsed.TotalSeconds); SenderPrivate=$sm.PeakPrivateBytes; ReceiverPrivate=$rm.PeakPrivateBytes; SenderWorkingSet=$sm.PeakWorkingSet; ReceiverWorkingSet=$rm.PeakWorkingSet; SenderHandles=$sm.PeakHandles; ReceiverHandles=$rm.PeakHandles; Samples=($sm.Samples+$rm.Samples); ProgressLines=$lines }
        Remove-TestFile $saved; Remove-TestFile $source
        Pass "actual direct TCP streaming, SHA-256, progress, memory: $size bytes"
    }
    Assert ([Math]::Abs($measurements[1].ReceiverPrivate - $measurements[0].ReceiverPrivate) -lt 32MB) 'Receive memory grew with file size'
    Assert ([Math]::Abs($measurements[1].SenderPrivate - $measurements[0].SenderPrivate) -lt 32MB) 'Send memory grew with file size'
    Pass '1 GiB versus 4 GiB transfer memory does not scale with file size'
    Stop-Pair $pair
} finally { Save-Pair $pair; $measurements | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $runDir 'measurements.json') }

$pair = New-Pair 'duplex-large' -Direct
try {
    $a = Join-Path $runDir 'duplex-a.bin'; $b = Join-Path $runDir 'duplex-b.bin'
    [Stage4Data]::Sparse($a,256MB); [Stage4Data]::Sparse($b,256MB)
    Record-SourceHash $a; Record-SourceHash $b
    $pair.ClientApp.Send('/file "' + $a + '"'); $pair.HostApp.Send('/file "' + $b + '"')
    $pair.HostApp.WaitText('[Incoming file request] duplex-a.bin')
    $pair.ClientApp.WaitText('[Incoming file request] duplex-b.bin')
    $pair.HostApp.Send('/accept'); $pair.ClientApp.Send('/accept')
    Finish $pair.HostApp 'duplex-b.bin' 60000; Finish $pair.ClientApp 'duplex-a.bin' 60000
    Assert-Hash $a (Saved-Path $pair.HostApp 'duplex-a.bin')
    Assert-Hash $b (Saved-Path $pair.ClientApp 'duplex-b.bin')
    Stop-Pair $pair
    Remove-TestFile $a; Remove-TestFile $b
    Remove-TestFile (Saved-Path $pair.HostApp 'duplex-a.bin'); Remove-TestFile (Saved-Path $pair.ClientApp 'duplex-b.bin')
    Pass 'simultaneous direct 256 MiB each direction, ACK and SHA-256, no deadlock'
} finally { Save-Pair $pair }

$small = Join-Path $runDir 'small.bin'; [Stage4Data]::Sparse($small,1MB)
$zero = Join-Path $runDir 'zero.bin'; [IO.File]::WriteAllBytes($zero,[byte[]]@())
$pair = New-Pair 'progress-ack' 'hold-ack'
try {
    Start-Transfer $pair.ClientApp $pair.HostApp $zero
    Assert ($pair.Relay.WaitAck(5000)) 'No zero-byte ACK'
    Check-Progress $pair.ClientApp 'Sending:' 'zero.bin' 0
    Check-Progress $pair.HostApp 'Saving:' 'zero.bin' 0
    Assert (!$pair.ClientApp.Output.Contains('[Transfer completed]')) '100% before ACK returned success'
    $pair.Relay.ReleaseAck(); Finish $pair.ClientApp 'zero.bin'
    Stop-Pair $pair
    Pass 'zero-byte 100% data progress remains separate from delayed ACK success'
} finally { Save-Pair $pair }
$pair = New-Pair 'slow-receiver'
try {
    $source = Join-Path $runDir 'slow.bin'; [Stage4Data]::Sparse($source,8MB)
    $pair.Relay.ChunkDelayMs = 20
    $sm = [Stage4Monitor]::new($pair.ClientApp.Process)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try { Start-Transfer $pair.ClientApp $pair.HostApp $source; Finish $pair.ClientApp 'slow.bin' 30000 }
    finally { $sm.Dispose(); $watch.Stop() }
    Assert-Hash $source (Saved-Path $pair.HostApp 'slow.bin')
    Assert ($watch.Elapsed.TotalSeconds -gt 2) 'Slow receiver did not apply backpressure'
    Assert ($sm.PeakPrivateBytes -lt 256MB) 'Backpressure grew sender memory'
    $measurements += [pscustomobject]@{ Case='slow-relay'; Bytes=8MB; Seconds=$watch.Elapsed.TotalSeconds; MiBps=(8/$watch.Elapsed.TotalSeconds); SenderPrivate=$sm.PeakPrivateBytes; SenderHandles=$sm.PeakHandles }
    Stop-Pair $pair
    Pass 'slow receiver applies bounded TCP backpressure without indefinite wait'
} finally { Save-Pair $pair }

# Same-process repetitions, distinct names. Warm up allocator/CRT before baseline.
$pair = New-Pair 'repeated' -Direct
try {
    $beforePrivate = @(); $beforeHandles = @()
    for ($i = 0; $i -lt 12; $i++) {
        $source = Join-Path $runDir ("repeat-$i.bin"); [Stage4Data]::Sparse($source,1MB)
        Start-Transfer $pair.ClientApp $pair.HostApp $source; Finish $pair.ClientApp ([IO.Path]::GetFileName($source))
        Assert-Hash $source (Saved-Path $pair.HostApp ([IO.Path]::GetFileName($source)))
        if ($i -eq 0) {
            Check-Progress $pair.ClientApp 'Sending:' 'repeat-0.bin' 1MB
            Check-Progress $pair.HostApp 'Saving:' 'repeat-0.bin' 1MB
        }
        if ($i -eq 1) {
            foreach ($app in @($pair.ClientApp,$pair.HostApp)) { $app.Process.Refresh(); $beforePrivate += $app.Process.PrivateMemorySize64; $beforeHandles += $app.Process.HandleCount }
        }
    }
    $index = 0
    foreach ($app in @($pair.ClientApp,$pair.HostApp)) {
        $app.Process.Refresh()
        Assert (($app.Process.PrivateMemorySize64 - $beforePrivate[$index]) -lt 8MB) 'Repeated-transfer memory growth'
        Assert (($app.Process.HandleCount - $beforeHandles[$index]) -le 3) 'Repeated-transfer handle growth'
        $measurements += [pscustomobject]@{ Case='repeated'; Direction=$index; PrivateBefore=$beforePrivate[$index]; PrivateAfter=$app.Process.PrivateMemorySize64; HandlesBefore=$beforeHandles[$index]; HandlesAfter=$app.Process.HandleCount; Transfers=12 }
        $index++
    }
    Stop-Pair $pair
    Pass '12 same-connection transfers: bounded private bytes and stable handle counts'
} finally { Save-Pair $pair }

# Explicitly compiled test executable only. Faults never exist in production.
$Exe = $FaultExe
foreach ($mode in @('read','write','space','flush','partial-io')) {
    $keys = @('FTA_TEST_READ_FAIL_AFTER','FTA_TEST_WRITE_FAIL_AFTER','FTA_TEST_FREE_BYTES','FTA_TEST_FLUSH_FAIL','FTA_TEST_SEND_SLICE','FTA_TEST_WRITE_SLICE')
    $oldValues = @{}
    foreach ($key in $keys) { $oldValues[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
    $pair = $null
    try {
        switch ($mode) {
            'read' { $env:FTA_TEST_READ_FAIL_AFTER = '131072' }
            'write' { $env:FTA_TEST_WRITE_FAIL_AFTER = '131072' }
            'space' { $env:FTA_TEST_FREE_BYTES = '0' }
            'flush' { $env:FTA_TEST_FLUSH_FAIL = '1' }
            'partial-io' { $env:FTA_TEST_SEND_SLICE = '97'; $env:FTA_TEST_WRITE_SLICE = '17' }
        }
        $pair = New-Pair ('fault-' + $mode) -Direct
        foreach ($key in $keys) { [Environment]::SetEnvironmentVariable($key,$oldValues[$key],'Process') }
        Start-Transfer $pair.ClientApp $pair.HostApp $small
        if ($mode -eq 'partial-io') {
            Finish $pair.ClientApp 'small.bin' 30000
            Assert-Hash $small (Saved-Path $pair.HostApp 'small.bin')
        } else {
            $pair.ClientApp.WaitText('File transfer not completed',10000)
            Assert (!$pair.ClientApp.Output.Contains('[Transfer completed]')) 'I/O fault reported success'
            Wait-FileState ((Saved-Path $pair.HostApp 'small.bin') + '.part') $false
            Assert (!(Test-Path (Saved-Path $pair.HostApp 'small.bin'))) 'I/O fault committed file'
        }
        Stop-Pair $pair
        Pass "test-build actual TCP fault/short I/O: $mode"
    } finally {
        foreach ($key in $keys) { [Environment]::SetEnvironmentVariable($key,$oldValues[$key],'Process') }
        if ($pair) { Save-Pair $pair }
    }
}
$Exe = $productionExe
$measurements | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $runDir 'measurements.json')
Write-Output "Logs, measurements and bounded fixtures: $runDir"
