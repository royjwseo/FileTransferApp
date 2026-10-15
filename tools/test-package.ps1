param([string]$PackageDirectory)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/ReleaseCommon.ps1"
$version=Get-ReleaseVersion
if(!$PackageDirectory){$PackageDirectory=Join-Path $RepositoryRoot 'dist'}
$name="FileTransferApp-v$version-win-x64"
$zip=Join-Path $PackageDirectory "$name.zip"
$checksum=[IO.File]::ReadAllText((Join-Path $PackageDirectory 'SHA256SUMS.txt')).Trim()
$actual=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
if($checksum -ne "$actual  $name.zip"){throw 'Package checksum mismatch'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=[IO.Compression.ZipFile]::OpenRead($zip)
try {
 $expected=@("$name/FileTransferAppGui.exe","$name/LICENSE.txt","$name/README.txt") | Sort-Object
 $entries=@($archive.Entries | ForEach-Object FullName | Sort-Object)
 if(@(Compare-Object $expected $entries).Count){throw 'Unexpected/missing ZIP entries; package rejected'}
}finally{$archive.Dispose()}
$extract=Join-Path ([IO.Path]::GetTempPath()) ('FileTransferApp-package-'+[Guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $zip -DestinationPath $extract
$folder=Join-Path $extract $name
$exe=Join-Path $folder 'FileTransferAppGui.exe'
$source=Join-Path $RepositoryRoot 'FileTransferApp/x64/Release/FileTransferAppGui.exe'
if((Get-FileHash $exe).Hash -ne (Get-FileHash $source).Hash){throw 'Extracted EXE differs from validated Release GUI'}
if([Diagnostics.FileVersionInfo]::GetVersionInfo($exe).ProductVersion -ne $version){throw 'Extracted EXE version mismatch'}
Add-Type 'using System; using System.Runtime.InteropServices; public static class PackageWindow { public delegate bool EnumProc(IntPtr h,IntPtr d); [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p,IntPtr d); [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,System.Text.StringBuilder b,int count); [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p); [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h,uint m,IntPtr w,IntPtr l,uint f,uint timeout,out IntPtr result); [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l); }'
$process=[Diagnostics.Process]::new()
$process.StartInfo=[Diagnostics.ProcessStartInfo]::new($exe)
$process.StartInfo.WorkingDirectory=$folder
$process.StartInfo.UseShellExecute=$false
$process.StartInfo.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
foreach($key in @('FILETRANSFER_GUI_SETTINGS','FILETRANSFER_LAN_PORT')){$process.StartInfo.EnvironmentVariables.Remove($key)}
$started=$false
try {
 $started=$process.Start()
 $script:packageWindow=[IntPtr]::Zero
 $watch=[Diagnostics.Stopwatch]::StartNew()
 while($script:packageWindow -eq [IntPtr]::Zero -and $watch.ElapsedMilliseconds -lt 7000){
  if($process.HasExited){throw "Extracted GUI exited early: $($process.ExitCode)"}
  [PackageWindow]::EnumWindows({param($h,$d) [uint32]$pidValue=0;[PackageWindow]::GetWindowThreadProcessId($h,[ref]$pidValue)|Out-Null;if($pidValue -eq $process.Id){$class=[Text.StringBuilder]::new(128);[PackageWindow]::GetClassName($h,$class,128)|Out-Null;if($class.ToString() -eq 'FileTransferAppGui'){$script:packageWindow=$h}};return $true},[IntPtr]::Zero)|Out-Null
  Start-Sleep -Milliseconds 50
 }
 if($script:packageWindow -eq [IntPtr]::Zero){throw 'Extracted GUI window not found'}
 $result=[IntPtr]::Zero
 if([PackageWindow]::SendMessageTimeout($script:packageWindow,0,[IntPtr]::Zero,[IntPtr]::Zero,2,1000,[ref]$result) -eq [IntPtr]::Zero){throw 'Extracted GUI unresponsive'}
 [PackageWindow]::PostMessage($script:packageWindow,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
 if(!$process.WaitForExit(7000) -or $process.ExitCode -ne 0){throw 'Extracted GUI shutdown failed'}
 Write-Output 'PASS: ZIP exact allowlist, checksum, extraction, version and EXE identity'
 Write-Output 'PASS: extracted GUI startup/responsiveness/shutdown with release environment overrides removed'
 Write-Output "Extraction retained: $extract"
}finally{if($started -and !$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
