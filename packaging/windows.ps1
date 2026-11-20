[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$BuildDir,
    [Parameter(Mandatory)] [string]$QtBin,
    [Parameter(Mandatory)] [string]$PythonZip,
    [string]$OutputDir = 'dist',
    [string]$PythonSha256 = '3c01d7cfc666f746a6f064f60fb2156ab0ec464d3d738ab796371a46a9bb24a2',
    [string]$InnoCompiler = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Run([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with $LASTEXITCODE" }
}
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$qt = (Resolve-Path -LiteralPath $QtBin).Path
$zip = (Resolve-Path -LiteralPath $PythonZip).Path
if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $PythonSha256) {
    throw 'The Python embeddable archive does not match the expected SHA-256 digest.'
}
$inno = (Get-Command $InnoCompiler -ErrorAction Stop).Source
$deploy = (Get-Command (Join-Path $qt 'windeployqt.exe') -ErrorAction Stop).Source
$output = [IO.Directory]::CreateDirectory($OutputDir).FullName
$stage = [IO.Directory]::CreateDirectory((Join-Path ([IO.Path]::GetTempPath()) ('disk-diver-package-' + [guid]::NewGuid()))).FullName
$prefix = Join-Path $stage 'install'
$app = Join-Path $prefix 'bin'
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
$versionLine = $cache | Where-Object { $_ -match '^CMAKE_PROJECT_VERSION:STATIC=[0-9]+(\.[0-9]+){2}$' } | Select-Object -First 1
if (-not $versionLine) { throw 'Configure the root CMake project before packaging.' }
$version = $versionLine.Split('=')[1]

try {
    Run 'cmake' @('--build', $build, '--config', 'Release', '--parallel')
    Run 'cmake' @('--install', $build, '--config', 'Release', '--prefix', $prefix)
    foreach ($file in 'disk-diver.exe', 'disk-diver-cpp.exe', 'worker.py', 'dd_journal.py', 'dd_privilege.py') {
        if (-not (Test-Path -LiteralPath (Join-Path $app $file) -PathType Leaf)) { throw "Missing $file in install stage" }
    }
    Run $deploy @('--release', '--no-translations', '--no-compiler-runtime', '--no-opengl-sw', (Join-Path $app 'disk-diver.exe'))
    if (-not (Test-Path -LiteralPath (Join-Path $app 'platforms/qwindows.dll'))) { throw 'Qt Windows platform plugin missing' }

    # App-local MSVC DLLs also cover machines without the shared redistributable installed.
    $locator = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory |
        Sort-Object { [version]$_.Parent.Parent.Name } -Descending | Select-Object -First 1
    if (-not $crt) { throw 'Install the x64 Visual C++ redistributable files with Visual Studio.' }
    Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' | Copy-Item -Destination $app

    $python = Join-Path $app 'python'
    Expand-Archive -LiteralPath $zip -DestinationPath $python
    Run (Join-Path $python 'python.exe') @('-I', '-S', '-B', '-c', 'import ctypes, multiprocessing.connection, sqlite3')
    Run (Join-Path $python 'python.exe') @('-I', '-S', '-B', (Join-Path $PSScriptRoot 'smoke.py'), $app)
    Copy-Item -LiteralPath (Join-Path $prefix 'share/disk-diver') -Destination (Join-Path $app 'licenses') -Recurse
    $sbom = Join-Path (Split-Path $qt) 'sbom'
    if (Test-Path -LiteralPath $sbom) { Copy-Item -LiteralPath $sbom -Destination (Join-Path $app 'licenses/qt-sbom') -Recurse }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third-party.txt') -Destination (Join-Path $app 'licenses')
    Run $inno @("/DPayload=$app", "/DAppVersion=$version", "/O$output", (Join-Path $PSScriptRoot 'windows.iss'))
} finally {
    # The stage is created by this invocation, immediately below the OS temp directory.
    $actual = (Resolve-Path -LiteralPath $stage).Path
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName($actual) -eq $temp -and [IO.Path]::GetFileName($actual) -match '^disk-diver-package-[a-f0-9-]{36}$') {
        Remove-Item -LiteralPath $actual -Force -Recurse
    }
}
