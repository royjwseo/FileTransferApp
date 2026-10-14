param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Release/FileTransferAppGui.exe",[string]$Cli="$PSScriptRoot/../x64/Release/FileTransferApp.exe")
$ErrorActionPreference='Stop';$Exe=(Resolve-Path $Exe).Path;$Cli=(Resolve-Path $Cli).Path
Add-Type -Path "$PSScriptRoot/GuiDriver.cs";Add-Type -Path "$PSScriptRoot/FrameRelay.cs"
$guiExe=$Exe
. "$PSScriptRoot/stage1.ps1" -Exe $Cli -HelpersOnly
$Exe=$guiExe
$runDir=Join-Path $PSScriptRoot ('runs/gui-download-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $runDir | Out-Null
function Assert($yes,[string]$message){if(!$yes){throw $message}}
function Pass([string]$message){"PASS: $message"}
function Folder([string]$name){$p=Join-Path $runDir $name;New-Item -ItemType Directory $p | Out-Null;return $p}
function Hash($a,$b){Assert ((Get-FileHash -LiteralPath $a).Hash -eq (Get-FileHash -LiteralPath $b).Hash) 'SHA-256 mismatch'}
$default=Folder 'default';$app=[GuiDriver]::new($Exe,$default,$false)
try {
    $app.Wait(127,[GuiDriver]::Downloads());Assert ($app.Text(127) -eq [GuiDriver]::Downloads()) 'Windows Downloads mismatch';
    Assert (!(Test-Path (Join-Path $default 'received'))) 'GUI fell back to cwd received';Pass 'first startup resolves Windows Downloads without CLI received fallback'
    Assert (!$app.Enabled(110) -and !$app.Enabled(113) -and !$app.Enabled(116)) 'Disconnected buttons enabled'
    $app.Resize(1100,1000);$app.Resize(860,600);$app.Resize(860,850);$app.Responsive();Pass 'resizable GUI, minimum layout and disconnected button states'
    $unicode=Folder '다운로드 폴더 日本語 😀';$app.Folder($unicode);Pass 'native folder dialog accepts Unicode path'
    $app.Folder('',$true);Assert ($app.Text(127) -eq $unicode) 'Cancel changed download folder';Pass 'folder dialog cancellation preserves setting'
    $app.Close()
}finally{$app.Dispose()}
$app=[GuiDriver]::new($Exe,$default,$false)
try{$app.Wait(127,$unicode);$app.Close();Pass 'Unicode folder setting persists across process restart'}finally{$app.Dispose()}
foreach($kind in @('missing','relative','file','no-write')) {
    $cwd=Folder ('invalid-'+$kind);$target=Join-Path $cwd 'absent'
    if($kind -eq 'relative'){$target='relative-path'}
    if($kind -eq 'file'){$target=Join-Path $cwd 'file.txt';[IO.File]::WriteAllText($target,'not directory')}
    $acl=$null
    if($kind -eq 'no-write'){
        $target=Join-Path $cwd 'locked';New-Item -ItemType Directory $target | Out-Null
        $acl=Get-Acl -LiteralPath $target;$deny=Get-Acl -LiteralPath $target
        $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User
        $rule=[Security.AccessControl.FileSystemAccessRule]::new($sid,[Security.AccessControl.FileSystemRights]::CreateFiles,[Security.AccessControl.AccessControlType]::Deny)
        $deny.AddAccessRule($rule);Set-Acl -LiteralPath $target -AclObject $deny
    }
    [IO.File]::WriteAllText((Join-Path $cwd 'gui-settings.txt'),$target,[Text.UTF8Encoding]::new($false));$app=$null
    try {$app=[GuiDriver]::new($Exe,$cwd,$false);$app.Wait(108,'Download folder error:');$app.Responsive();$app.Close();Pass "invalid setting rejects $kind with responsive UI"}
    finally{if($app){$app.Dispose()};if($acl){Set-Acl -LiteralPath $target -AclObject $acl}}
}
$hd=Folder 'host';$cd=Folder 'client';$h=$null;$c=$null;$relay=$null
try {
    $probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$probe.Start();$port=$probe.LocalEndpoint.Port;$probe.Stop()
    $h=[GuiDriver]::new($Exe,$hd);$h.Set(102,[string]$port);$h.Click(103);$h.Wait(107,'Waiting')
    $relay=[FrameRelay]::new($port);$relay.PauseChunks=$true;$relay.PauseAfterBytes=262144
    $c=[GuiDriver]::new($Exe,$cd);$c.Set(102,[string]$relay.Port);$c.Click(104);$c.Wait(107,'Connected:');$h.Wait(107,'Connected:')
    Assert ($c.Enabled(110) -and !$h.Enabled(116)) 'Connected buttons incorrect';Pass 'connected message/send/approval button states'
    $source=Join-Path $runDir '변경 중.dat';$f=[IO.File]::Create($source);$f.SetLength(4MB);$f.Dispose()
    $c.Pick($source);$c.Click(113);$h.Wait(115,'변경 중.dat');$h.Click(116);Assert ($relay.WaitChunk(5000)) 'No paused chunk'
    $changed=Folder '다른 저장 폴더';$h.Folder($changed);$relay.ReleaseChunks();$c.Wait(122,'Completed:');Hash $source (Join-Path $hd 'received/변경 중.dat')
    Assert (!(Test-Path (Join-Path $changed '변경 중.dat'))) 'Active transfer migrated folders';Pass 'changing default during active transfer keeps pinned original destination'
    $id=$c.Text(124);$c.Click(126);$h.Wait(115,'변경 중.dat');$h.Click(116);$c.Wait(122,'Failed:');
    Assert (!(Test-Path (Join-Path $changed '변경 중.dat'))) 'Wrong folder reused transfer';Pass 'completion lookup after folder change refuses unrelated metadata'
    $oldFile=Join-Path $hd 'received/변경 중.dat';$oldMeta=$oldFile+'.part.fta'
    $newFile=Join-Path $changed '변경 중.dat';$newMeta=$newFile+'.part.fta'
    Move-Item -LiteralPath $oldFile -Destination $newFile;Move-Item -LiteralPath $oldMeta -Destination $newMeta
    $c.Click(125);$h.Wait(115,'변경 중.dat');$h.Click(116);$c.Wait(122,'Failed:')
    Assert (Test-Path -LiteralPath $newFile) 'Rejected recovery removed moved file';Pass 'moving data and sealed metadata to another directory fails directory identity check'
    Move-Item -LiteralPath $newFile -Destination $oldFile;Move-Item -LiteralPath $newMeta -Destination $oldMeta
    $h.Folder((Join-Path $hd 'received'));$c.Click(125);$h.Wait(115,'변경 중.dat');$h.Click(116);$c.Wait(122,'Completed: verified existing commit');Pass 'restoring original folder permits persistent-ID recovery'
    $other=Join-Path $runDir 'collision.txt';[IO.File]::WriteAllText($other,'new content');$saved=Join-Path $hd 'received/collision.txt';[IO.File]::WriteAllText($saved,'keep original')
    $c.Pick($other);$c.Click(113);$h.Wait(115,'collision.txt');$h.Click(116);$c.Wait(122,'Failed:');Assert ([IO.File]::ReadAllText($saved) -eq 'keep original') 'Existing file overwritten';Pass 'configured receive directory refuses existing-file overwrite'
    $h.Close();$c.Close()
}finally{if($relay){$relay.Dispose()};if($h){$h.Dispose()};if($c){$c.Dispose()}}
$hd=Folder 'partial-host';$cd=Folder 'partial-client';$h=$null;$c=$null;$relay=$null
try {
    $probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$probe.Start();$port=$probe.LocalEndpoint.Port;$probe.Stop()
    $h=[GuiDriver]::new($Exe,$hd);$h.Set(102,[string]$port);$h.Click(103);$h.Wait(107,'Waiting')
    $relay=[FrameRelay]::new($port);$relay.PauseChunks=$true;$relay.PauseAfterBytes=262144
    $c=[GuiDriver]::new($Exe,$cd);$c.Set(102,[string]$relay.Port);$c.Click(104);$c.Wait(107,'Connected:')
    $source=Join-Path $runDir '부분 복구.dat';$f=[IO.File]::Create($source);$f.SetLength(4MB);$f.Dispose()
    $c.Pick($source);$c.Click(113);$h.Wait(115,'부분 복구.dat');$h.Click(116);Assert ($relay.WaitChunk(5000)) 'No partial checkpoint chunk'
    $c.Wait(122,'requestId=');$id=$c.Text(124);$c.Process.Kill();Assert ($c.Process.WaitForExit(3000)) 'Sender kill failed';$relay.Cut();$h.Click(105);$h.Wait(107,'Disconnected')
    $original=Join-Path $hd 'received';$part=Join-Path $original '부분 복구.dat.part';$offset=(Get-Item -LiteralPath $part).Length;Assert ($offset -gt 0 -and $offset -lt 4MB) 'No real partial checkpoint'
    $relay.Dispose();$relay=$null;$c.Dispose();$c=$null
    $different=Folder '부분 복구 다른 폴더';$h.Folder($different);$h.Click(106);$h.Wait(107,'Waiting')
    $c=[GuiDriver]::new($Exe,$cd);$c.Set(102,[string]$port);$c.Click(104);$c.Wait(107,'Connected:');$c.Pick($source);$c.Set(124,$id)
    $c.Click(125);$h.Wait(115,'부분 복구.dat');$h.Click(116);$c.Wait(122,'Failed:');Assert ((Get-Item -LiteralPath $part).Length -eq $offset) 'Wrong-folder resume modified original partial'
    $h.Folder($original);$c.Click(125);$h.Wait(115,'부분 복구.dat');$h.Click(116);$c.Wait(122,'Completed:',30000);Hash $source (Join-Path $original '부분 복구.dat')
    Pass 'interrupted partial transfer survives folder change; wrong-folder resume rejected and original-folder resume completes with SHA-256'
    $h.Close();$c.Close()
}finally{if($relay){$relay.Dispose()};if($h){$h.Dispose()};if($c){$c.Dispose()}}
$cliDir=Folder 'cli-peer';$guiDir=Folder 'gui-peer';$peer=$null;$g=$null
try {
    $probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$probe.Start();$port=$probe.LocalEndpoint.Port;$probe.Stop()
    $peer=[TestApp]::new($Cli,$cliDir,"--host --port $port");$peer.WaitText('Waiting for connection')
    $g=[GuiDriver]::new($Exe,$guiDir);$g.Set(102,[string]$port);$g.Click(104);$g.Wait(107,'Connected:');$peer.WaitText('Commands:')
    $peer.Send('CLI Unicode 안녕');$g.Wait(108,'CLI Unicode 안녕');$g.Set(109,'GUI reply');$g.Click(110);$peer.WaitText('GUI reply')
    $a=Join-Path $runDir 'cli-to-gui.txt';[IO.File]::WriteAllText($a,'from CLI');$peer.Send('/file '+$a);$g.Wait(115,'cli-to-gui.txt');$g.Click(116);$peer.WaitText('[Transfer completed]');Hash $a (Join-Path $guiDir 'received/cli-to-gui.txt')
    $b=Join-Path $runDir 'gui-to-cli.txt';[IO.File]::WriteAllText($b,'from GUI');$g.Pick($b);$g.Click(113);$peer.WaitText('gui-to-cli.txt');$peer.Send('/accept');$g.Wait(122,'Completed:');Hash $b (Join-Path $cliDir 'received/gui-to-cli.txt')
    Pass 'GUI/CLI bidirectional Unicode text and SHA-256 file transfers; CLI received path preserved'
    $g.Close();$peer.Send('/quit');$peer.WaitExit()
}finally{if($g){$g.Dispose()};if($peer){$peer.Dispose()}}
"Download GUI artifacts: $runDir"
