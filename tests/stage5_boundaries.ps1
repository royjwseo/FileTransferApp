param([string]$Exe = "$PSScriptRoot/../FileTransferApp/x64/Release/FileTransferApp.exe")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/stage3_validation.ps1" -Exe $Exe -HelpersOnly
Add-Type -Path "$PSScriptRoot/Stage4Support.cs"
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Security.Cryptography;
public static class Stage5Prefix {
    public static string Hash(string path, long count) {
        using(var file=File.OpenRead(path)) using(var hash=SHA256.Create()) {
            byte[] buffer=new byte[65536];
            while(count>0) {
                int n=file.Read(buffer,0,(int)Math.Min(count,buffer.Length));
                if(n==0) throw new EndOfStreamException();
                hash.TransformBlock(buffer,0,n,buffer,0); count-=n;
            }
            hash.TransformFinalBlock(new byte[0],0,0);
            return BitConverter.ToString(hash.Hash).Replace("-","").ToLowerInvariant();
        }
    }
}
'@
function Write-ExactFileFrame($peer,[byte]$type,[UInt64]$id,[byte[]]$body) {
    Write-Frame $peer $type ([byte[]]((U64-Bytes $id) + $body)) -NoId
}
foreach($case in @('offset-past-end','wrong-prefix','wrong-proof')) {
    $source = Join-Path $runDir ("$case.bin"); [IO.File]::WriteAllBytes($source,[byte[]]@())
    $c = New-RawConnection
    try {
        $c.App.Send('/file "' + $source + '"'); $request = Read-Frame $c.Peer
        if($case -eq 'wrong-proof') {
            Write-Frame $c.Peer 7 @()
            Assert ((Read-Frame $c.Peer).Type -eq 2) 'Missing BEGIN'; Assert ((Read-Frame $c.Peer).Type -eq 4) 'Missing END'
            Write-ExactFileFrame $c.Peer 5 $request.RequestId ([Text.Encoding]::ASCII.GetBytes(('0' * 128)))
        } else {
            [UInt64]$offset = if($case -eq 'offset-past-end') { 4294967297 } else { 0 }
            $prefix = if($case -eq 'wrong-prefix') { '0' * 64 } else { SHA256-Bytes @() }
            Write-ExactFileFrame $c.Peer 7 $request.RequestId ([byte[]]((U64-Bytes $offset) + [Text.Encoding]::ASCII.GetBytes($prefix)))
        }
        $errorFrame = Read-Frame $c.Peer
        Assert ($errorFrame.Type -eq 9) 'Invalid offset/digest/proof accepted'
        $c.App.WaitText('File transfer not completed')
        Assert (!$c.App.Output.Contains('[Transfer completed]')) 'Invalid proof reported success'
        $c.App.Send('/quit'); $c.App.WaitExit()
        Pass "actual raw TCP validation: $case"
    } finally {
        [IO.File]::WriteAllText((Join-Path $runDir ($case + '.log')),$c.App.Output)
        $c.Peer.Dispose(); $c.App.Dispose()
    }
}
# >4GiB source and prefix hash are real; only the tail is transferred to a raw
# peer. This tests offset arithmetic, not an authenticated receiver checkpoint.
$source = Join-Path $runDir 'offset-4GiB.bin'
[Stage4Data]::Sparse($source,(4GB + 65536))
$digest = (Get-FileHash $source).Hash.ToLowerInvariant()
[byte[]]$sizeBytes = U64-Bytes ([UInt64](4GB + 65536))
$identity = SHA256-Bytes ([byte[]]([Text.Encoding]::ASCII.GetBytes("FTA5`0") + $sizeBytes + [Text.Encoding]::ASCII.GetBytes($digest + 'offset-4GiB.bin')))
[UInt64]$offset = 4GB + 1
$prefix = [Stage5Prefix]::Hash($source,$offset)
$c = New-RawConnection
try {
    $c.Peer.ReceiveTimeout = 60000
    $c.App.Send("/resume $identity `"$source`"")
    $request = Read-Frame $c.Peer
    Write-ExactFileFrame $c.Peer 7 $request.RequestId ([byte[]]((U64-Bytes $offset) + [Text.Encoding]::ASCII.GetBytes($prefix)))
    Assert ((Read-Frame $c.Peer).Type -eq 2) 'Missing resumed BEGIN'
    [UInt64]$bytes = 0
    do {
        $frame = Read-Frame $c.Peer
        if($frame.Type -eq 3){ $bytes += $frame.Payload.Length }
    } while($frame.Type -eq 3)
    Assert ($bytes -eq 65535 -and $frame.Type -eq 4) '4GiB offset wrapped or truncated'
    Write-Frame $c.Peer 5 @()
    $c.App.WaitText('[Transfer completed]')
    $c.App.Send('/quit'); $c.App.WaitExit()
    Pass '4GiB+1 resume offset with real prefix hashing and 65535-byte tail TCP (raw-peer boundary simulation)'
} finally { $c.Peer.Dispose(); $c.App.Dispose(); Remove-Item -LiteralPath $source }
$source = Join-Path $runDir 'duplex-cancel.bin'; [Stage4Data]::Sparse($source,8MB); Record-SourceHash $source
$pair = New-Pair 'duplex-one-cancel'
try {
    # Complete both approvals before pausing data. Pausing the first stream
    # earlier would also trap the second ACCEPT behind that stream's bytes.
    $pair.Relay.ChunkDelayMs = 5
    Start-Transfer $pair.ClientApp $pair.HostApp $source
    Start-Transfer $pair.HostApp $pair.ClientApp $source
    $pair.ClientApp.WaitText('Sending: file=duplex-cancel.bin')
    $pair.HostApp.WaitText('Sending: file=duplex-cancel.bin')
    $pair.Relay.PauseChunks = $true
    Assert ($pair.Relay.WaitChunk(5000)) 'No paused chunk'
    $pair.ClientApp.Send('/cancel 2'); $pair.ClientApp.WaitText('[Transfer cancelled]')
    $pair.Relay.ReleaseChunks()
    $pair.ClientApp.WaitText('File transfer not completed')
    $pair.HostApp.WaitText('[Transfer completed]')
    Assert-Hash $source (Saved-Path $pair.ClientApp 'duplex-cancel.bin')
    Assert (!(Test-Path (Saved-Path $pair.HostApp 'duplex-cancel.bin'))) 'Cancelled direction committed'
    Stop-Pair $pair
    Pass 'actual duplex: cancelling one direction preserves opposite-direction file and ACK'
} finally { Save-Pair $pair }
Write-Output "Boundary logs: $runDir"
