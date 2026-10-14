param([string]$Exe = "$PSScriptRoot/../x64/Release/FileTransferApp.exe")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/stage3_validation.ps1" -Exe $Exe -HelpersOnly
$large = Join-Path $runDir 'cancel-large.bin'
$f = [IO.File]::Create($large); $f.SetLength(8MB); $f.Dispose()
$small = Join-Path $runDir 'after-cancel.bin'
[IO.File]::WriteAllBytes($small,[byte[]]@(1,3,5,7))
foreach ($side in @('sender','receiver')) {
    $pair = New-Pair ("cancel-$side") 'pause-chunks'
    try {
        Start-Transfer $pair.ClientApp $pair.HostApp $large
        Assert ($pair.Relay.WaitChunk(5000)) 'Missing active chunk'
        $part = (Saved-Path $pair.HostApp 'cancel-large.bin') + '.part'
        Wait-FileState $part $true
        $cancelApp = if ($side -eq 'sender') { $pair.ClientApp } else { $pair.HostApp }
        $cancelApp.Send('/cancel 2')
        $cancelApp.WaitText('[Transfer cancelled] requestId=2')
        $pair.Relay.ReleaseChunks()
        $pair.ClientApp.WaitText('File transfer not completed')
        $pair.HostApp.WaitText('[Transfer cancelled] requestId=2')
        Wait-FileState $part $false
        Assert (!$pair.ClientApp.Output.Contains('[Transfer completed] cancel-large.bin')) 'Cancelled upload succeeded'
        Pass "request cancellation and owned-part cleanup: $side"
        Start-Sleep -Milliseconds 100
        Start-Transfer $pair.ClientApp $pair.HostApp $small
        $pair.ClientApp.WaitText('[Transfer completed] after-cancel.bin')
        Assert-Hash $small (Saved-Path $pair.HostApp 'after-cancel.bin')
        Pass "same connection transfer after cancellation: $side"
        $pair.ClientApp.Send('/cancel 2'); $pair.ClientApp.Send('/cancel 4')
        $pair.ClientApp.WaitText('No matching active transfer')
        $pair.ClientApp.Send('still-connected')
        $pair.HostApp.WaitText('[Received TEXT] still-connected')
        Pass "old and completed cancellation is inert: $side"
        Stop-Pair $pair
    } finally { Save-Pair $pair }
}
Write-Output "Cancellation logs: $runDir"
