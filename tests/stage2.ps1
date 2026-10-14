param([string]$Exe = "$PSScriptRoot/../x64/Debug/FileTransferApp.exe")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/stage1.ps1" -Exe $Exe -HelpersOnly

function Metadata([UInt64]$size,[string]$name) {
    [byte[]]$bytes = New-Object byte[] 8
    for ($i = 0; $i -lt 8; $i++) { $bytes[$i] = ($size -shr ((7-$i)*8)) -band 255 }
    return ,([byte[]]($bytes + [Text.Encoding]::ASCII.GetBytes($name)))
}
function Approve($c,[byte[]]$metadata) {
    Write-Frame $c.Peer 6 $metadata
    $c.App.WaitText('[Incoming file request]')
    $c.App.Send('/accept')
    Assert ((Read-Frame $c.Peer).Type -eq 7) 'Expected FILE_ACCEPT'
}
function Expect-Error($c,[switch]$ConnectionAlive) {
    $frame = Read-Frame $c.Peer
    Assert ($frame.Type -eq 9) 'Expected FILE_ERROR'
    Assert ($frame.Payload.Length -gt 0) 'Missing failure reason'
    if ($ConnectionAlive) {
        Write-Frame $c.Peer 1 ([Text.Encoding]::ASCII.GetBytes('storage-failure-barrier'))
        $c.App.WaitText('[Received TEXT] storage-failure-barrier')
    } else {
        Assert ($c.Peer.GetStream().ReadByte() -eq -1) 'Connection should stop after FILE_ERROR'
    }
    $c.App.Send('/quit'); $c.App.WaitExit()
}
function Save-Log($c,[string]$name) {
    [IO.File]::WriteAllText((Join-Path $runDir ($name + '.log')),$c.App.Output)
    $c.Peer.Dispose(); $c.App.Dispose()
}

foreach ($mode in @('unapproved','pending','rejected','wrong-name','wrong-size',
    'truncated','over-limit','traversal','reserved','trailing-dot','existing-final','existing-part',
    'directory-blocked','duplicate-begin','chunk-overflow','short-end','commit-collision')) {
    $c = New-RawConnection
    $cwd = $c.App.Process.StartInfo.WorkingDirectory
    $folder = Join-Path $cwd 'received'
    $final = Join-Path $folder 'approved.txt'
    $part = $final + '.part'
    [byte[]]$meta = Metadata 10 'approved.txt'
    $original = 'keep existing bytes untouched'
    try {
        if ($mode -eq 'commit-collision') { $script:rawDigest = SHA256-Bytes ([byte[]](1..10)) }
        switch ($mode) {
            'pending' {
                Write-Frame $c.Peer 6 $meta
                $c.App.WaitText('[Incoming file request]')
            }
            'rejected' {
                Write-Frame $c.Peer 6 $meta
                $c.App.WaitText('[Incoming file request]')
                $c.App.Send('/reject')
                Assert ((Read-Frame $c.Peer).Type -eq 8) 'Expected FILE_REJECT'
            }
            'unapproved' { }
            default { Approve $c $meta }
        }
        switch ($mode) {
            'wrong-name' { $meta = Metadata 10 'different.txt' }
            'wrong-size' { $meta = Metadata 11 'approved.txt' }
            'truncated' { $meta = [byte[]]@(0,1,2) }
            'over-limit' { $meta = Metadata ([UInt64]1099511627777) 'approved.txt' }
            'traversal' { $meta = Metadata 10 '../escape.txt' }
            'reserved' { $meta = Metadata 10 'CON.txt' }
            'trailing-dot' { $meta = Metadata 10 'approved.' }
            'existing-final' {
                New-Item -ItemType Directory $folder | Out-Null
                [IO.File]::WriteAllText($final,$original)
            }
            'existing-part' {
                New-Item -ItemType Directory $folder | Out-Null
                [IO.File]::WriteAllText($part,$original)
            }
            'directory-blocked' { [IO.File]::WriteAllText($folder,$original) }
        }
        Write-Frame $c.Peer 2 $meta
        if ($mode -in @('duplicate-begin','chunk-overflow','short-end','commit-collision')) {
            $watch = [Diagnostics.Stopwatch]::StartNew()
            while (!(Test-Path $part) -and $watch.ElapsedMilliseconds -lt 3000) {
                Start-Sleep -Milliseconds 10
            }
            Assert (Test-Path $part) 'Approved temporary file was not created'
            switch ($mode) {
                'duplicate-begin' { Write-Frame $c.Peer 2 $meta }
                'chunk-overflow' { Write-Frame $c.Peer 3 ([byte[]](1..11)) }
                'short-end' {
                    Write-Frame $c.Peer 3 ([byte[]](1..3))
                    Write-Frame $c.Peer 4 @()
                }
                'commit-collision' {
                    [IO.File]::WriteAllText($final,$original)
                    Write-Frame $c.Peer 3 ([byte[]](1..10))
                    Write-Frame $c.Peer 4 @()
                }
            }
        }
        Expect-Error $c -ConnectionAlive:($mode -in @('existing-final','existing-part','directory-blocked','commit-collision'))
        if ($mode -in @('existing-final','commit-collision')) {
            Assert ([IO.File]::ReadAllText($final) -eq $original) 'Existing destination changed'
        } else { Assert (!(Test-Path $final)) 'Unexpected final file' }
        if ($mode -eq 'existing-part') {
            Assert ([IO.File]::ReadAllText($part) -eq $original) 'Unowned temporary file changed'
        } else { Assert (!(Test-Path $part)) 'Owned temporary file was not removed' }
        Assert (!(Test-Path (Join-Path $folder 'different.txt.part'))) 'Mismatched file was created'
        Assert (!(Test-Path (Join-Path $cwd 'escape.txt'))) 'Traversal created a file'
        if ($mode -in @('unapproved','pending','rejected','wrong-name','wrong-size','truncated',
            'over-limit','traversal','reserved','trailing-dot')) {
            Assert (!(Test-Path $folder)) 'Invalid FILE_BEGIN touched the receive directory'
        }
        if ($mode -eq 'directory-blocked') {
            Assert ([IO.File]::ReadAllText($folder) -eq $original) 'Directory blocker changed'
        }
        Pass "receive validation, ownership and error notice: $mode"
    } finally { $script:rawDigest = $null; Save-Log $c $mode }
}

foreach ($size in @(0,1MB)) {
    $c = New-RawConnection
    try {
        [byte[]]$data = New-Object byte[] $size
        [Random]::new(2026).NextBytes($data)
        $script:rawDigest = SHA256-Bytes $data
        [byte[]]$meta = Metadata $size 'normal.bin'
        Approve $c $meta
        Write-Frame $c.Peer 2 $meta
        for ($offset = 0; $offset -lt $size; $offset += 65536) {
            [byte[]]$chunk = New-Object byte[] ([Math]::Min(65536,$size-$offset))
            [Array]::Copy($data,$offset,$chunk,0,$chunk.Length)
            Write-Frame $c.Peer 3 $chunk
        }
        Write-Frame $c.Peer 4 @()
        $ack = Read-Frame $c.Peer
        Assert ($ack.Type -eq 5) 'Expected FILE_ACK after commit'
        Assert ($ack.RequestId -eq 1 -and $ack.Payload.Length -eq 0) 'Wrong ACK request ID or payload'
        $source = Join-Path $runDir ("source-$size.bin")
        [IO.File]::WriteAllBytes($source,$data)
        $saved = Join-Path $c.App.Process.StartInfo.WorkingDirectory 'received/normal.bin'
        Assert ((Get-FileHash $source).Hash -eq (Get-FileHash $saved).Hash) 'SHA-256 mismatch'
        Assert (!(Test-Path ($saved + '.part'))) 'Temporary file remained after success'
        $c.App.Send('/quit'); $c.App.WaitExit()
        Assert (Test-Path $saved) 'Cleanup removed committed file'
        Pass "approved normal transfer, commit and SHA-256: $size bytes"
    } finally { $script:rawDigest = $null; Save-Log $c "normal-$size" }
}

$c = New-RawConnection
try {
    $source = Join-Path $runDir 'outgoing.txt'
    [IO.File]::WriteAllText($source,'outgoing')
    $c.App.Send('/file "' + $source + '"')
    Assert ((Read-Frame $c.Peer).Type -eq 6) 'Expected outgoing request'
    Write-Frame $c.Peer 9 ([Text.Encoding]::ASCII.GetBytes('Cannot save file'))
    $c.App.WaitText('[Peer file error] Cannot save file')
    $c.App.WaitText('File transfer not completed')
    $c.App.Send('/quit'); $c.App.WaitExit()
    Pass 'sender displays peer failure and cancels approval wait'
} finally { Save-Log $c 'peer-error' }
Write-Output "Logs and isolated fixtures: $runDir"
