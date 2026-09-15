# /*<<----- VEYA_COOKER: reproducible Windows build entry, never updates the pinned source. */
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SConsPython,
    [ValidateRange(1, 64)][int]$Jobs = 12,
    [ValidateSet('none', 'full', 'auto')][string]$Lto = 'none',
    [string]$LogPath,
    [string]$LuauSource,
    [switch]$KeepGoing,
    [switch]$CompilationDatabase
)

$ErrorActionPreference = 'Stop'
$cookerRoot = Split-Path -Parent $PSScriptRoot
$pythonExecutable = (Resolve-Path -LiteralPath $SConsPython).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere.exe is required.' }
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Install Visual Studio C++ build tools and Windows SDK first.' }
& (Join-Path $vsInstall 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation

# Windows platform defaults override Python profile values for these two options.
$sconsArguments = @('-m', 'SCons', 'platform=windows', 'profile=cooker/profile.py', 'arch=x86_64', 'd3d12=no', "lto=$Lto", "-j$Jobs")
if ($LuauSource) { $sconsArguments += "veya_luau_source=$((Resolve-Path -LiteralPath $LuauSource).Path)" }
if ($KeepGoing) { $sconsArguments += '-k' }
if ($CompilationDatabase) { $sconsArguments += 'compiledb=yes' }
if ($LogPath) { $LogPath = [System.IO.Path]::GetFullPath($LogPath) }
Push-Location -LiteralPath $cookerRoot
try {
    if ($LogPath) {
        & $pythonExecutable @sconsArguments 2>&1 | Tee-Object -FilePath $LogPath
    } else {
        & $pythonExecutable @sconsArguments
    }
    if ($LASTEXITCODE -ne 0) { throw "Cooker build failed with exit code $LASTEXITCODE." }
    Write-Output (Join-Path $cookerRoot 'bin/veya_cooke.exe')
} finally {
    Pop-Location
}
# /*>>----- VEYA_COOKER */
