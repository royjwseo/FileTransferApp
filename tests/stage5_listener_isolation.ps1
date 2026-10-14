# Runs only an existing Windows PowerShell worker; never loads FileTransferApp.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/SocketIsolation.ps1"
foreach ($protected in @($false,$true)) {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $child = $null; $client = $null
    try {
        $listener.Start(); $port = $listener.LocalEndpoint.Port
        if ($protected) { Protect-TestListener $listener }
        $info = [Diagnostics.ProcessStartInfo]::new(
            "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe",
            '-NoProfile -Command Start-Sleep -Seconds 3')
        $info.UseShellExecute = $false; $info.CreateNoWindow = $true
        $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $child = [Diagnostics.Process]::Start($info)
        $listener.Stop()
        $client = [Net.Sockets.TcpClient]::new()
        $connected = $false
        try { $client.Connect('127.0.0.1',$port); $connected = $true }
        catch [Net.Sockets.SocketException] {
            if ($_.Exception.SocketErrorCode -ne [Net.Sockets.SocketError]::ConnectionRefused) { throw }
        }
        if ($protected -and $connected) { throw 'Listener survived parent Stop despite isolation' }
        if (!$protected -and !$connected) { throw 'Inherited-listener reproduction did not occur on this runtime' }
        Write-Output "PASS: infrastructure only; protected=$protected; connectAfterParentStop=$connected"
    } finally {
        if ($client) { $client.Dispose() }
        $listener.Stop()
        if ($child) { $child.WaitForExit(); $child.Dispose() }
    }
}
