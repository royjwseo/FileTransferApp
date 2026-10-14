param([string]$Exe = "$PSScriptRoot/../FileTransferApp/x64/Release/FileTransferApp.exe")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/stage3_validation.ps1" -Exe $Exe -HelpersOnly
Add-Type -Path "$PSScriptRoot/Stage4Support.cs"
$utf8 = [Text.UTF8Encoding]::new($false,$true)
function Send-NameRequest($peer,[byte[]]$name) {
    $digest = SHA256-Bytes @()
    $identity = SHA256-Bytes ([byte[]]([Text.Encoding]::ASCII.GetBytes("FTA5`0") + (U64-Bytes 0) + [Text.Encoding]::ASCII.GetBytes($digest) + $name))
    $body = [byte[]]((U64-Bytes 1) + [Text.Encoding]::ASCII.GetBytes($identity + $digest) + [byte]0 + (U64-Bytes 0) + $name)
    Write-Frame $peer 6 $body -NoId
}
$sourceFolder = Join-Path $runDir '입력 파일'
New-Item -ItemType Directory $sourceFolder | Out-Null
$names = @('사진 2026.jpg','보고서 최종.pdf','report_한글 01.pdf','résumé_日本語_😀.txt',
    '공백 (최종) & #.pdf','ASCII-123.txt',"caf$([char]0xe9).txt","cafe$([char]0x301).txt")
$pair = New-Pair 'unicode-roundtrip' -Direct
try {
    $requestCount = 0
    foreach($name in $names) {
        $source = Join-Path $sourceFolder $name
        [IO.File]::WriteAllBytes($source,[byte[]](1..255))
        Record-SourceHash $source
        $pair.ClientApp.Send('/file "' + $source + '"')
        $requestCount++; Wait-Count $pair.HostApp '[Incoming file request]' $requestCount
        $pair.HostApp.Send('/accept')
        $pair.ClientApp.WaitText('[Transfer completed] ' + $name,30000)
        $saved = Saved-Path $pair.HostApp $name
        Assert-Hash $source $saved
        Assert ([string]::Equals((Get-Item -LiteralPath $saved).Name,$name,[StringComparison]::Ordinal)) 'Unicode filename was normalized or changed'
        Pass "actual UTF-8 TCP / UTF-16 save and SHA-256: $name"
    }
    # Same name but different content: no replacement of either final or part.
    $name = $names[0]; $saved = Saved-Path $pair.HostApp $name
    $before = (Get-FileHash -LiteralPath $saved).Hash
    $source = Join-Path $sourceFolder $name; [IO.File]::WriteAllBytes($source,[byte[]]@(99))
    $pair.ClientApp.Send('/file "' + $source + '"')
    $requestCount++; Wait-Count $pair.HostApp '[Incoming file request]' $requestCount
    $pair.HostApp.Send('/accept'); $pair.ClientApp.WaitText('File transfer not completed')
    Assert ((Get-FileHash -LiteralPath $saved).Hash -eq $before) 'Unicode destination overwritten'
    Pass 'Unicode same-name different-content collision refuses replacement'
    $name = '소유하지 않은.bin'; $part = (Saved-Path $pair.HostApp $name) + '.part'
    [IO.File]::WriteAllText($part,'keep unowned bytes')
    $source = Join-Path $sourceFolder $name; [IO.File]::WriteAllBytes($source,[byte[]]@(1))
    $pair.ClientApp.Send('/file "' + $source + '"')
    $requestCount++; Wait-Count $pair.HostApp '[Incoming file request]' $requestCount
    $pair.HostApp.Send('/accept'); Wait-Count $pair.ClientApp 'File transfer not completed' 2
    Assert ([IO.File]::ReadAllText($part) -eq 'keep unowned bytes') 'Unowned Unicode part changed'
    Pass 'unowned Unicode part preserved on collision'
    $name = $names[1]; $source = Join-Path $sourceFolder $name
    $identities = [regex]::Matches($pair.ClientApp.Output,'transferId=([a-f0-9]{64})')
    $id = $identities[1].Groups[1].Value
    foreach($command in @('/status','/resume')) {
        $pair.ClientApp.Send("$command $id `"$source`"")
        $requestCount++; Wait-Count $pair.HostApp '[Incoming file request]' $requestCount
        $pair.HostApp.Send('/accept')
        Wait-Count $pair.ClientApp '[verified existing commit]' $(if($command -eq '/status'){1}else{2})
        Assert-Hash $source (Saved-Path $pair.HostApp $name)
        Pass "Unicode persistent identity and completed-file query: $command"
    }
    # Reverse direction exercises Unicode source lookup and requestId parity.
    $name = '반대 방향 사진.png'; $source = Join-Path $sourceFolder $name
    [IO.File]::WriteAllBytes($source,[byte[]]@(3,5,7)); Record-SourceHash $source
    Start-Transfer $pair.HostApp $pair.ClientApp $source
    $pair.HostApp.WaitText('[Transfer completed] ' + $name)
    Assert-Hash $source (Saved-Path $pair.ClientApp $name)
    Pass 'Unicode reverse-direction actual TCP and ACK'
    Stop-Pair $pair
} finally { Save-Pair $pair }
$invalid = @(
    @{label='overlong UTF-8';bytes=[byte[]]@(0xc0,0xaf)},
    @{label='UTF-8 surrogate';bytes=[byte[]]@(0xed,0xa0,0x80)},
    @{label='beyond Unicode scalar range';bytes=[byte[]]@(0xf4,0x90,0x80,0x80)},
    @{label='truncated UTF-8';bytes=[byte[]]@(0xe3,0x81)},
    @{label='stray continuation';bytes=[byte[]]@(0x80)},
    @{label='embedded NUL';bytes=[byte[]]@(65,0,66)}
)
foreach($name in @('../한글.pdf','..\사진.jpg','C:\한글.pdf','/절대.pdf','\\서버\공유.pdf',
    'NUL.pdf','con .txt','COM¹.pdf','LPT².txt','CONIN$.txt','CONOUT$.txt','CLOCK$.txt',
    '대체:스트림.pdf','잘못된?.jpg','마지막.','마지막 ','.숨김',' 시작.txt',
    "줄$([char]10)바꿈.pdf","표시$([char]0x202e)역전.pdf",(('가' * 97) + '.pdf'))) {
    $invalid += @{label=$name;bytes=$utf8.GetBytes($name)}
}
foreach($case in $invalid) {
    $c = New-RawConnection
    try {
        Send-NameRequest $c.Peer $case.bytes
        Assert ((Read-Frame $c.Peer).Type -eq 9) 'Unsafe or malformed filename accepted'
        Assert ($c.Peer.GetStream().ReadByte() -eq -1) 'Malformed request did not stop connection'
        Assert (!(Test-Path (Join-Path $c.App.Process.StartInfo.WorkingDirectory 'received'))) 'Invalid filename touched disk'
        $c.App.Send('/quit'); $c.App.WaitExit()
        Pass "raw TCP rejects before disk I/O: $($case.label)"
    } finally { $c.Peer.Dispose(); $c.App.Dispose() }
}
foreach($name in @((('가' * 96) + '.pdf'),('😀' * 50))) {
    $c = New-RawConnection
    try {
        Send-NameRequest $c.Peer ($utf8.GetBytes($name)); $c.App.WaitText('[Incoming file request]')
        $c.App.Send('/reject'); Assert ((Read-Frame $c.Peer).Type -eq 8) 'Valid UTF-16 length boundary rejected'
        $c.App.Send('/quit'); $c.App.WaitExit()
        Pass '100 UTF-16 unit boundary accepted for approval (no disk creation)'
    } finally { $c.Peer.Dispose(); $c.App.Dispose() }
}
$name = '대용량 동영상.mp4'; $source = Join-Path $sourceFolder $name
Assert ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($runDir)).AvailableFreeSpace -gt 3GB) 'Insufficient space for large Unicode test'
[Stage4Data]::Sparse($source,(1GB + 65536)); Record-SourceHash $source
$pair = New-Pair 'unicode-large' -Direct
try {
    Start-Transfer $pair.ClientApp $pair.HostApp $source -SkipHash
    $pair.ClientApp.WaitText('[Transfer completed] ' + $name,180000)
    Assert-Hash $source (Saved-Path $pair.HostApp $name)
    Stop-Pair $pair
    Pass 'Unicode 1GiB+64KiB actual TCP transfer, stored ACK and SHA-256'
    Remove-Item -LiteralPath $source
    Remove-Item -LiteralPath (Saved-Path $pair.HostApp $name)
} finally { Save-Pair $pair }
Write-Output "Unicode logs and isolated fixtures: $runDir"
