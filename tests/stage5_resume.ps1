param([string]$Exe = "$PSScriptRoot/../FileTransferApp/x64/Release/FileTransferApp.exe")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/stage3_validation.ps1" -Exe $Exe -HelpersOnly
Add-Type -Path "$PSScriptRoot/Stage4Support.cs"
function Transfer-Id($app) {
    $match = [regex]::Match($app.Output,'transferId=([a-f0-9]{64})')
    Assert $match.Success 'Missing persistent transfer identity'
    return $match.Groups[1].Value
}
function Restart-Pair($pair,[string]$fault = '') {
    $hostDir = $pair.HostApp.Process.StartInfo.WorkingDirectory
    $clientDir = $pair.ClientApp.Process.StartInfo.WorkingDirectory
    Save-Pair $pair # kills both test-owned processes, preserving receiver directory
    $probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $probe.Start(); $port = $probe.LocalEndpoint.Port; $probe.Stop()
    $hostApp = $null; $clientApp = $null; $relay = $null
    try {
        $hostApp = [TestApp]::new($Exe,$hostDir,"--host --bind 127.0.0.1 --port $port")
        $hostApp.WaitText('Waiting for connection')
        if ($fault) {
            $relay = [FrameRelay]::new($port); $port = $relay.Port
            $relay.PauseChunks = $fault -eq 'pause'
            $relay.PauseAfterBytes = 131072
        }
        $clientApp = [TestApp]::new($Exe,$clientDir,"--connect 127.0.0.1 --port $port")
        $hostApp.WaitText('Commands:'); $clientApp.WaitText('Commands:')
        return @{HostApp=$hostApp; ClientApp=$clientApp; Relay=$relay; Name=$pair.Name; InstanceId=[Guid]::NewGuid().ToString('N')}
    } catch {
        if($relay){$relay.Dispose()};if($clientApp){$clientApp.Dispose()};if($hostApp){$hostApp.Dispose()};throw
    }
}
function Resume-Transfer($pair,[string]$source,[string]$id,[string]$command = '/resume') {
    $pair.ClientApp.Send("$command $id `"$source`"")
    $pair.HostApp.WaitText('[Incoming file request]',60000)
    $pair.HostApp.Send('/accept')
}
foreach ($case in @('restart','restart-twice','tamper','manifest-tamper','foreign-part','new-request','source-change','wrong-id','large')) {
    $size = if($case -eq 'large') { 1GB + 65536 } else { 8MB }
    $source = Join-Path $runDir ("resume-$case.bin")
    [Stage4Data]::Sparse($source,$size); Record-SourceHash $source
    $pair = New-Pair ("resume-$case") 'pause-chunks'
    try {
        $pair.Relay.PauseAfterBytes = 262144
        Start-Transfer $pair.ClientApp $pair.HostApp $source -SkipHash
        Assert ($pair.Relay.WaitChunk(60000)) 'Transfer did not reach pause boundary'
        $part = (Saved-Path $pair.HostApp ([IO.Path]::GetFileName($source))) + '.part'
        $id = Transfer-Id $pair.ClientApp
        $locked = $false
        try { $writer = [IO.File]::OpenWrite($source); $writer.Dispose() }
        catch [IO.IOException] { $locked = $true }
        Assert $locked 'Source was writable while a send owned it'
        $pair = Restart-Pair $pair
        Assert ((Get-Item $part).Length -eq 262144) 'Incorrect checkpoint offset'
        $metadata=[MetadataProbe]::PathFor($pair.HostApp.Process.StartInfo.WorkingDirectory,[IO.Path]::GetDirectoryName($part),$id)
        Assert (Test-Path $metadata) 'Missing durable ownership metadata'
        if($case -in @('manifest-tamper','foreign-part','new-request')) {
            if($case -eq 'manifest-tamper') { [IO.File]::WriteAllBytes($metadata,[byte[]](1..100)) }
            if($case -eq 'foreign-part') {
                Move-Item -LiteralPath $part -Destination ($part + '.saved')
                [IO.File]::WriteAllText($part,'unowned replacement')
            }
            if($case -eq 'new-request') { Start-Transfer $pair.ClientApp $pair.HostApp $source }
            else { Resume-Transfer $pair $source $id }
            $pair.ClientApp.WaitText('File transfer not completed')
            Assert (Test-Path $part) 'Rejected recovery deleted existing data'
            if($case -eq 'foreign-part') { Assert ([IO.File]::ReadAllText($part) -eq 'unowned replacement') 'Foreign data changed' }
        } elseif($case -eq 'source-change') {
            $stream = [IO.File]::OpenWrite($source); $stream.WriteByte(99); $stream.Dispose()
            $pair.ClientApp.Send("/resume $id `"$source`"")
            $pair.ClientApp.WaitText('Source changed or transferId mismatch')
            Assert ((Get-Item $part).Length -eq 262144) 'Changed source touched receiver'
        } elseif($case -eq 'wrong-id') {
            $pair.ClientApp.Send('/resume ' + ('0' * 64) + ' "' + $source + '"')
            $pair.ClientApp.WaitText('Source changed or transferId mismatch')
        } else {
            if($case -eq 'tamper') {
                $stream = [IO.File]::OpenWrite($part); $stream.WriteByte(99); $stream.Dispose()
            }
            if($case -eq 'restart-twice') {
                $pair = Restart-Pair $pair 'pause'
                Resume-Transfer $pair $source $id
                Assert ($pair.Relay.WaitChunk(30000)) 'Resumed transfer did not reach second pause'
                $pair = Restart-Pair $pair
                Assert ((Get-Item $part).Length -eq 393216) 'Second checkpoint offset incorrect'
            }
            Resume-Transfer $pair $source $id
            if($case -eq 'tamper') {
                $pair.ClientApp.WaitText('File transfer not completed')
                $pair.HostApp.WaitText('Resume prefix SHA-256 mismatch')
                Assert (!(Test-Path ($part.Substring(0,$part.Length-5)))) 'Tampered prefix committed'
            } else {
                $pair.ClientApp.WaitText('[Transfer completed]',180000)
                Assert-Hash $source (Saved-Path $pair.HostApp ([IO.Path]::GetFileName($source)))
                Assert ($pair.ClientApp.Output.Contains('[Resume offset] ')) 'Missing resumed progress offset'
            }
        }
        Stop-Pair $pair
        Pass "actual TCP persistent resume: $case size=$size"
        if($case -eq 'large') {
            Remove-Item -LiteralPath $source
            Remove-Item -LiteralPath (Saved-Path $pair.HostApp ([IO.Path]::GetFileName($source)))
        }
    } finally { Save-Pair $pair }
}
$source = Join-Path $runDir 'completion.bin'; [IO.File]::WriteAllBytes($source,[byte[]](1..100)); Record-SourceHash $source
$pair = New-Pair 'completion-recovery' 'drop-ack'
try {
    Start-Transfer $pair.ClientApp $pair.HostApp $source
    Assert ($pair.Relay.WaitAck(5000)) 'Receiver did not commit'
    Assert (!$pair.ClientApp.Output.Contains('[Transfer completed]')) 'Lost ACK falsely completed'
    $id = Transfer-Id $pair.ClientApp
    $saved = Saved-Path $pair.HostApp 'completion.bin'
    $stamp = (Get-Item $saved).LastWriteTimeUtc
    $pair = Restart-Pair $pair
    for($i=1;$i -le 3;$i++) {
        $pair.ClientApp.Send("/status $id `"$source`"")
        Wait-Count $pair.HostApp '[Incoming file request]' $i
        $pair.HostApp.Send('/accept')
        Wait-Count $pair.ClientApp '[Transfer completed]' $i
    }
    Assert ((Get-Item $saved).LastWriteTimeUtc -eq $stamp) 'Completion query rewrote final file'
    Assert-Hash $source $saved
    $writer = [IO.File]::OpenWrite($saved); $writer.WriteByte(199); $writer.Dispose()
    $pair.ClientApp.Send("/status $id `"$source`"")
    Wait-Count $pair.HostApp '[Incoming file request]' 4
    $pair.HostApp.Send('/accept')
    $pair.ClientApp.WaitText('File transfer not completed')
    Assert ([regex]::Matches($pair.ClientApp.Output,'\[Transfer completed\]').Count -eq 3) 'Modified final file was falsely reconfirmed'
    Assert (Test-Path $saved) 'Modified final file was deleted'
    Stop-Pair $pair
    Pass 'ACK loss: restart, three idempotent completion queries, modified final rejects confirmation'
} finally { Save-Pair $pair }
$pair = New-Pair 'integrity-corruption'
try {
    $pair.Relay.CorruptChunk = $true
    Start-Transfer $pair.ClientApp $pair.HostApp $source
    $pair.ClientApp.WaitText('File transfer not completed')
    $pair.HostApp.WaitText('SHA-256 mismatch')
    Assert (!(Test-Path (Saved-Path $pair.HostApp 'completion.bin'))) 'Corrupt file committed'
    $pair.ClientApp.Send('integrity-connection-survives'); $pair.HostApp.WaitText('[Received TEXT] integrity-connection-survives')
    Stop-Pair $pair
    Pass 'actual TCP corruption rejects final hash; connection remains usable'
} finally { Save-Pair $pair }
for($i=0;$i -lt 10;$i++) {
    $pair = New-Pair ("cancel-commit-$i") 'hold-ack'
    try {
        Start-Transfer $pair.ClientApp $pair.HostApp $source
        Assert ($pair.Relay.WaitAck(5000)) 'No stored ACK to race'
        $pair.ClientApp.Send('/cancel 2')
        $pair.ClientApp.WaitText('[Cancel requested; awaiting outcome]')
        $pair.Relay.ReleaseAck()
        $pair.ClientApp.WaitText('[Transfer completed]')
        Assert-Hash $source (Saved-Path $pair.HostApp 'completion.bin')
        Stop-Pair $pair
    } finally { Save-Pair $pair }
}
Pass 'cancel after commit preserves matching ACK success (10 actual process pairs)'
Write-Output "Stage5 resume logs: $runDir"
