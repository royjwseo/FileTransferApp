param([string]$Exe = "$PSScriptRoot/../x64/Release/FileTransferApp.exe")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/stage1.ps1" -Exe $Exe -HelpersOnly

function Meta([UInt64]$size,[string]$name) {
    return ,([byte[]]((U64-Bytes $size) + [Text.Encoding]::ASCII.GetBytes($name)))
}
function Save-Case($c,[string]$name) {
    [IO.File]::WriteAllText((Join-Path $runDir ($name + '.log')),$c.App.Output)
    $c.Peer.Dispose(); $c.App.Dispose()
}
function Start-Outgoing($c,[string]$name = 'empty.bin') {
    $path = Join-Path $runDir $name
    [IO.File]::WriteAllBytes($path,[byte[]]@())
    $c.App.Send('/file "' + $path + '"')
    $f = Read-Frame $c.Peer
    Assert ($f.Type -eq 6 -and $f.RequestId -gt 0 -and ($f.RequestId % 2) -eq 0) 'Wrong outgoing request ID'
    return $f.RequestId
}
function Send-ToEnd($c,[UInt64]$id) {
    Write-Frame $c.Peer 7 @() -RequestId $id
    $begin = Read-Frame $c.Peer
    $end = Read-Frame $c.Peer
    Assert ($begin.Type -eq 2 -and $begin.RequestId -eq $id) 'Wrong BEGIN ID'
    Assert ($end.Type -eq 4 -and $end.RequestId -eq $id -and !$end.Payload.Length) 'Wrong END ID'
    $c.App.WaitText('[Upload finished, waiting for ACK]')
}
function Expect-Fatal($c,[UInt64]$id) {
    $f = Read-Frame $c.Peer
    Assert ($f.Type -eq 9 -and $f.RequestId -eq $id) 'Expected matching FILE_ERROR'
    Assert ($c.Peer.GetStream().ReadByte() -eq -1) 'Invalid transition did not stop connection'
    $c.App.Send('/quit'); $c.App.WaitExit()
}
function Wait-Count($app,[string]$text,[int]$count,[int]$timeout = 5000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $timeout) {
        if ([regex]::Matches($app.Output,[regex]::Escape($text)).Count -ge $count) { return }
        Start-Sleep -Milliseconds 10
    }
    throw "Missing occurrence $count of $text`n$($app.Output)"
}

foreach ($mode in @('early-ack','duplicate-accept','accept-and-ack','reject-after-end')) {
    $c = New-RawConnection
    try {
        $id = Start-Outgoing $c
        switch ($mode) {
            'early-ack' { Write-Frame $c.Peer 5 @() -RequestId $id }
            'duplicate-accept' {
                # One TCP write makes both transitions observable before worker wakes.
                [byte[]]$wire = @()
                foreach ($type in @(7,7)) {
                    $body = [byte[]]((U64-Bytes $id) + (U64-Bytes 0) + [Text.Encoding]::ASCII.GetBytes((SHA256-Bytes @())))
                    $wire += [byte[]]@($type,0,0,0,$body.Length) + $body
                }
                $c.Peer.GetStream().Write($wire,0,$wire.Length)
            }
            'accept-and-ack' {
                [byte[]]$wire = @()
                foreach ($type in @(7,5)) {
                    $content = if($type -eq 7) { [byte[]]((U64-Bytes 0) + [Text.Encoding]::ASCII.GetBytes((SHA256-Bytes @()))) }
                        else { [Text.Encoding]::ASCII.GetBytes($script:outgoingProofs[$c.Peer.GetHashCode()]) }
                    $body = [byte[]]((U64-Bytes $id) + $content)
                    $wire += [byte[]]@($type,0,0,0,$body.Length) + $body
                }
                $c.Peer.GetStream().Write($wire,0,$wire.Length)
            }
            'reject-after-end' { Send-ToEnd $c $id; Write-Frame $c.Peer 8 @() -RequestId $id }
        }
        Expect-Fatal $c $id
        Assert (!$c.App.Output.Contains('[Transfer completed]')) 'Invalid transition reported success'
        Pass "sender rejects same-ID invalid transition: $mode"
    } finally { Save-Case $c $mode }
}

$c = New-RawConnection
try {
    $id = Start-Outgoing $c
    foreach ($type in @(7,8,5,9)) {
        [byte[]]$body = @()
        if ($type -eq 9) { $body = [Text.Encoding]::ASCII.GetBytes('stale failure') }
        Write-Frame $c.Peer $type $body -RequestId ($id+100)
    }
    $c.App.WaitText('[Ignored stale file response]')
    Assert (!$c.App.Output.Contains('[Upload finished')) 'Wrong-ID ACCEPT started transfer'
    Send-ToEnd $c $id
    Write-Frame $c.Peer 1 ([Text.Encoding]::ASCII.GetBytes('ack-wait-barrier'))
    $c.App.WaitText('[Received TEXT] ack-wait-barrier')
    Assert (!$c.App.Output.Contains('[Transfer completed]')) 'END alone reported completion'
    $c.App.Send('/file "' + (Join-Path $runDir 'empty.bin') + '"')
    $c.App.WaitText('File transfer in progress')
    Write-Frame $c.Peer 5 @() -RequestId $id
    $c.App.WaitText('[Transfer completed] empty.bin')
    Pass 'wrong-ID responses ignored; END keeps worker busy until matching ACK'
    Write-Frame $c.Peer 5 @() -RequestId $id
    Write-Frame $c.Peer 8 @() -RequestId $id
    Write-Frame $c.Peer 9 ([Text.Encoding]::ASCII.GetBytes('old failure')) -RequestId $id
    Write-Frame $c.Peer 1 ([Text.Encoding]::ASCII.GetBytes('duplicates-barrier'))
    $c.App.WaitText('[Received TEXT] duplicates-barrier')
    Assert ([regex]::Matches($c.App.Output,'\[Transfer completed\]').Count -eq 1) 'Duplicate completion'
    Pass 'completed request duplicate ACK, REJECT and ERROR have no effect'
    Start-Sleep -Milliseconds 100
    $next = Start-Outgoing $c 'next.bin'
    Assert ($next -gt $id) 'Request ID reused'
    foreach ($type in @(7,8,5,9)) {
        [byte[]]$body = @()
        if ($type -eq 9) { $body = [Text.Encoding]::ASCII.GetBytes('old failure') }
        Write-Frame $c.Peer $type $body -RequestId $id
    }
    Send-ToEnd $c $next
    Write-Frame $c.Peer 5 @() -RequestId $next
    $c.App.WaitText('[Transfer completed] next.bin')
    $c.App.Send('/quit'); $c.App.WaitExit()
    Pass 'old responses cannot approve, reject, complete or fail the next request'
} finally { Save-Case $c 'stale-and-ack' }

foreach ($mode in @('peer-error','disconnect','ack-timeout')) {
    $c = New-RawConnection
    try {
        $id = Start-Outgoing $c
        Send-ToEnd $c $id
        switch ($mode) {
            'peer-error' { Write-Frame $c.Peer 9 ([Text.Encoding]::ASCII.GetBytes('save failed')) -RequestId $id }
            'disconnect' { $c.Peer.Close() }
            'ack-timeout' {
                $c.Peer.ReceiveTimeout = 20000
                $failureFrame = Read-Frame $c.Peer
                Assert ($failureFrame.Type -eq 9 -and $failureFrame.RequestId -eq $id) 'ACK timeout missing ID'
            }
        }
        $c.App.WaitText('File transfer not completed',20000)
        Assert (!$c.App.Output.Contains('[Transfer completed]')) 'Missing ACK reported success'
        if ($mode -ne 'disconnect') {
            Write-Frame $c.Peer 5 @() -RequestId $id
            Write-Frame $c.Peer 1 ([Text.Encoding]::ASCII.GetBytes('still-connected'))
            $c.App.WaitText('[Received TEXT] still-connected')
            Assert (!$c.App.Output.Contains('[Transfer completed]')) 'Late ACK resurrected failed request'
        }
        $c.App.Send('/quit'); $c.App.WaitExit()
        Pass "ACK wait terminates safely: $mode"
    } finally { Save-Case $c $mode }
}

foreach ($mode in @('wrong-begin-id','wrong-chunk-id','wrong-end-id','chunk-before-begin',
    'end-before-begin','empty-chunk','duplicate-request','replayed-request','missing-id','zero-id','wrong-parity')) {
    $c = New-RawConnection
    $folder = Join-Path $c.App.Process.StartInfo.WorkingDirectory 'received'
    try {
        $meta = Meta 1 'incoming.bin'
        if ($mode -notin @('missing-id','zero-id','wrong-parity')) {
            Write-Frame $c.Peer 6 $meta -RequestId 1
            $c.App.WaitText('[Incoming file request]')
            if ($mode -eq 'duplicate-request') {
                Write-Frame $c.Peer 6 $meta -RequestId 1
            } elseif ($mode -eq 'replayed-request') {
                $c.App.Send('/reject')
                Assert ((Read-Frame $c.Peer).RequestId -eq 1) 'Wrong reject ID'
                Write-Frame $c.Peer 6 $meta -RequestId 1
            } else {
                $c.App.Send('/accept')
                $accepted = Read-Frame $c.Peer
                Assert ($accepted.Type -eq 7 -and $accepted.RequestId -eq 1) 'Wrong accept ID'
                switch ($mode) {
                    'wrong-begin-id' { Write-Frame $c.Peer 2 $meta -RequestId 3 }
                    'chunk-before-begin' { Write-Frame $c.Peer 3 @(42) -RequestId 1 }
                    'end-before-begin' { Write-Frame $c.Peer 4 @() -RequestId 1 }
                    default {
                        Write-Frame $c.Peer 2 $meta -RequestId 1
                        switch ($mode) {
                            'wrong-chunk-id' { Write-Frame $c.Peer 3 @(42) -RequestId 3 }
                            'wrong-end-id' { Write-Frame $c.Peer 4 @() -RequestId 3 }
                            'empty-chunk' { Write-Frame $c.Peer 3 @() -RequestId 1 }
                        }
                    }
                }
            }
        } else {
            switch ($mode) {
                'missing-id' { Write-Frame $c.Peer 6 @(1,2,3) -NoId }
                'zero-id' { Write-Frame $c.Peer 6 ([byte[]]((U64-Bytes 0) + $meta)) -NoId }
                'wrong-parity' { Write-Frame $c.Peer 6 $meta -RequestId 2 }
            }
        }
        if ($mode -in @('missing-id','zero-id')) {
            Assert ($c.Peer.GetStream().ReadByte() -eq -1) 'Malformed ID was accepted'
            $c.App.Send('/quit'); $c.App.WaitExit()
        } else {
            [UInt64]$errorId = 1
            if ($mode -like 'wrong-*-id') { $errorId = 3 }
            if ($mode -eq 'wrong-parity') { $errorId = 2 }
            Expect-Fatal $c $errorId
        }
        Assert (!(Test-Path (Join-Path $folder 'incoming.bin'))) 'Invalid transfer committed'
        Assert (!(Test-Path (Join-Path $folder 'incoming.bin.part'))) 'Invalid transfer leaked owned part'
        Pass "receiver rejects invalid ID/state with safe cleanup: $mode"
    } finally { Save-Case $c $mode }
}

foreach ($mode in @('await-begin-timeout','receiving-timeout','incoming-error')) {
    $c = New-RawConnection
    try {
        $meta = Meta 10 'timeout.bin'
        Write-Frame $c.Peer 6 $meta -RequestId 1
        $c.App.WaitText('[Incoming file request]')
        $c.App.Send('/accept'); $accept = Read-Frame $c.Peer
        Assert ($accept.Type -eq 7 -and $accept.RequestId -eq 1) 'Wrong accept'
        $part = Join-Path $c.App.Process.StartInfo.WorkingDirectory 'received/timeout.bin.part'
        if ($mode -ne 'await-begin-timeout') {
            Write-Frame $c.Peer 2 $meta -RequestId 1
            Write-Frame $c.Peer 3 @(1,2,3) -RequestId 1
            $c.App.WaitText('[Receiving]')
        }
        if ($mode -eq 'incoming-error') {
            Write-Frame $c.Peer 9 ([Text.Encoding]::ASCII.GetBytes('sender cancelled')) -RequestId 1
            $c.App.WaitText('[Peer file error] sender cancelled')
        } else {
            $c.Peer.ReceiveTimeout = 20000
            $failureFrame = Read-Frame $c.Peer
            Assert ($failureFrame.Type -eq 9 -and $failureFrame.RequestId -eq 1) 'Receive timeout missing ID'
        }
        Assert (!(Test-Path $part)) 'Timeout/error leaked part'
        # New request succeeds on the same connection after cleanup.
        Write-Frame $c.Peer 6 (Meta 0 'recovered.bin') -RequestId 3
        $c.App.WaitText('[Incoming file request] recovered.bin')
        $c.App.Send('/accept'); $accept = Read-Frame $c.Peer
        Assert ($accept.Type -eq 7 -and $accept.RequestId -eq 3) 'Receiver did not recover'
        Write-Frame $c.Peer 2 (Meta 0 'recovered.bin') -RequestId 3
        Write-Frame $c.Peer 4 @() -RequestId 3
        $ack = Read-Frame $c.Peer
        Assert ($ack.Type -eq 5 -and $ack.RequestId -eq 3) 'Wrong recovery ACK'
        $c.App.Send('/quit'); $c.App.WaitExit()
        Pass "receiver cleanup and same-connection recovery: $mode"
    } finally { Save-Case $c $mode }
}

# Exercise both 60-second approval deadlines together, with text traffic so the
# receiver cannot rely only on idle recv timeouts to expire requests.
$c = New-RawConnection
try {
    $id = Start-Outgoing $c
    Write-Frame $c.Peer 6 (Meta 0 'pending.bin') -RequestId 1
    $c.App.WaitText('[Incoming file request] pending.bin')
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt 62) {
        Write-Frame $c.Peer 1 ([Text.Encoding]::ASCII.GetBytes('tick'))
        Start-Sleep -Milliseconds 200
    }
    $ids = @()
    for ($i = 0; $i -lt 2; $i++) {
        $failureFrame = Read-Frame $c.Peer
        Assert ($failureFrame.Type -eq 9) 'Missing approval timeout error'
        $ids += $failureFrame.RequestId
    }
    Assert ($ids -contains $id -and $ids -contains 1) 'Approval timeouts assigned to wrong request'
    $c.App.WaitText('File transfer not completed')
    $c.App.Send('/accept'); $c.App.WaitText('No pending file request')
    Write-Frame $c.Peer 7 @() -RequestId $id
    Write-Frame $c.Peer 1 ([Text.Encoding]::ASCII.GetBytes('late-accept-barrier'))
    $c.App.WaitText('[Received TEXT] late-accept-barrier')
    Assert (!$c.App.Output.Contains('[Upload finished')) 'Late ACCEPT resurrected timeout'
    $c.App.Send('/quit'); $c.App.WaitExit()
    Pass 'outgoing approval timeout; late ACCEPT ignored'
    Pass 'incoming approval timeout under continuous text; late user approval refused'
} finally { Save-Case $c 'approval-timeouts' }

# Both actual applications upload simultaneously; each direction has its own ID
# namespace and state, and both SendFile workers must receive their own ACK.
$hostApp = New-App 'duplex-host'
$clientApp = $null
try {
    $hostApp.Send('1'); Start-Sleep -Milliseconds 300
    $clientApp = New-App 'duplex-client'
    $clientApp.Send('2'); $clientApp.Send('127.0.0.1')
    $hostApp.WaitText('Commands:'); $clientApp.WaitText('Commands:')
    $a = Join-Path $runDir 'host-data.bin'; $b = Join-Path $runDir 'client-data.bin'
    [byte[]]$data = New-Object byte[] 1MB
    [Random]::new(31).NextBytes($data); [IO.File]::WriteAllBytes($a,$data)
    [Random]::new(32).NextBytes($data); [IO.File]::WriteAllBytes($b,$data)
    $hostApp.Send('/file "' + $a + '"'); $clientApp.Send('/file "' + $b + '"')
    $hostApp.WaitText('[Incoming file request] client-data.bin')
    $clientApp.WaitText('[Incoming file request] host-data.bin')
    $hostApp.Send('/accept'); $clientApp.Send('/accept')
    $hostApp.WaitText('[Transfer completed] host-data.bin')
    $clientApp.WaitText('[Transfer completed] client-data.bin')
    Assert ((Get-FileHash $a).Hash -eq (Get-FileHash (Join-Path $runDir 'duplex-client/received/host-data.bin')).Hash) 'Host upload corrupted'
    Assert ((Get-FileHash $b).Hash -eq (Get-FileHash (Join-Path $runDir 'duplex-host/received/client-data.bin')).Hash) 'Client upload corrupted'
    $hostApp.Send('/quit'); $clientApp.Send('/quit')
    $hostApp.WaitExit(); $clientApp.WaitExit()
    Pass 'simultaneous bidirectional uploads complete with separate ACKs and SHA-256'
} finally {
    [IO.File]::WriteAllText((Join-Path $runDir 'duplex-host.log'),$hostApp.Output)
    $hostApp.Dispose()
    if ($clientApp) {
        [IO.File]::WriteAllText((Join-Path $runDir 'duplex-client.log'),$clientApp.Output)
        $clientApp.Dispose()
    }
}
Write-Output "Logs and isolated fixtures: $runDir"
