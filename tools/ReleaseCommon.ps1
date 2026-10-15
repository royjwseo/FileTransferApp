$RepositoryRoot = Split-Path $PSScriptRoot -Parent
function Get-ReleaseVersion {
    $header = Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot 'FileTransferApp/include/AppVersion.h')
    $parts = foreach ($name in @('MAJOR','MINOR','PATCH')) {
        $match = [regex]::Match($header, "(?m)^#define FTA_VERSION_$name ([0-9]+)\s*$")
        if (!$match.Success) { throw "Missing version component: $name" }
        $match.Groups[1].Value
    }
    return ($parts -join '.')
}
function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere not found. Install VS 2026 C++ tools.' }
    $paths = @(& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe')
    if (!$paths.Count) { throw 'MSBuild not found.' }
    return $paths[0]
}
