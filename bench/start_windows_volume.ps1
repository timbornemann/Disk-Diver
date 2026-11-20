# Stage a benchmark in the protected installation created by tests/windows_volume.ps1.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$Cpp,
    [Parameter(Mandatory)][string]$Rust,
    [Parameter(Mandatory)][string]$Gdu,
    [Parameter(Mandatory)][string]$Hardware,
    [Parameter(Mandatory)][string]$Storage,
    [switch]$ReuseFixture
)
$ErrorActionPreference = 'Stop'
$stagePath = (Resolve-Path -LiteralPath $Stage).Path
$programFiles = [Environment]::GetFolderPath('ProgramFiles')
if ((Split-Path $stagePath -Parent) -ne $programFiles -or (Split-Path $stagePath -Leaf) -notmatch '^Disk-Diver-Test-[0-9a-f-]{36}$') {
    throw 'Expected a protected Disk-Diver test installation'
}
$target = 'bench-' + [Guid]::NewGuid().ToString('N')
$files = [ordered]@{}
foreach ($name in 'windows_volume.py','run.py','fixture.py','measure.py') { $files[$name] = Join-Path $PSScriptRoot $name }
foreach ($name in 'worker.py','dd_privilege.py','dd_journal.py') { $files[$name] = Join-Path $stagePath $name }
$files['disk-diver-cpp.exe'] = (Resolve-Path -LiteralPath $Cpp).Path
$files['disk-diver-rust.exe'] = (Resolve-Path -LiteralPath $Rust).Path
$files['gdu.exe'] = (Resolve-Path -LiteralPath $Gdu).Path
$rows = @($files.GetEnumerator() | ForEach-Object { @{name=$_.Key; src=$_.Value; sha=(Get-FileHash -Algorithm SHA256 -LiteralPath $_.Value).Hash} })
$config = @{stage=$stagePath; folder=$target; files=$rows; hardware=$Hardware; storage=$Storage; reuse=$ReuseFixture.IsPresent}
$encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($config | ConvertTo-Json -Depth 5 -Compress)))
$bootstrap = @'
$ErrorActionPreference = 'Stop'
$cfg = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('__CONFIG__')) | ConvertFrom-Json
$stage = $cfg.stage
$bench = Join-Path $stage $cfg.folder
$null = New-Item -ItemType Directory -Path $bench
Set-Location -LiteralPath $bench
try {
    $root = (Get-Content -Raw -LiteralPath (Join-Path $stage 'ready.json') | ConvertFrom-Json).root
    $drive = $root.Substring(0,1)
    $disk = Get-DiskImage -ImagePath (Join-Path $stage 'test.vhdx') | Get-Disk
    $part = Get-Partition -DriveLetter $drive
    if ($disk.Number -ne $part.DiskNumber -or $disk.IsBoot -or $disk.IsSystem) { throw 'Unexpected mounted volume' }
    foreach ($row in $cfg.files) {
        $dest = Join-Path $bench $row.name
        Copy-Item -LiteralPath $row.src -Destination $dest
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $dest).Hash -ne $row.sha) { throw "Input hash changed: $($row.name)" }
    }
    $env:DD_BENCH_HARDWARE = $cfg.hardware
    $env:DD_BENCH_STORAGE = $cfg.storage
    $env:DD_BENCH_REUSE_FIXTURE = if ($cfg.reuse) { '1' } else { '0' }
    $env:PATH = (Join-Path $env:SystemRoot 'System32') + ';' + $env:SystemRoot
    $proc = Start-Process -WindowStyle Hidden -PassThru -Wait -FilePath (Join-Path $stage 'python\python.exe') -WorkingDirectory $bench -ArgumentList '-u','windows_volume.py' -RedirectStandardOutput (Join-Path $bench 'progress.log') -RedirectStandardError (Join-Path $bench 'errors.log')
    if ($proc.ExitCode -ne 0) { throw "Benchmark exited with $($proc.ExitCode)" }
    'ok' | Set-Content -LiteralPath (Join-Path $bench 'finished.txt')
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $bench 'launcher-error.txt')
    exit 1
}
'@
$command = $bootstrap.Replace('__CONFIG__', $encoded)
$command64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
Write-Output (Join-Path $stagePath $target)
$null = Start-Process -Verb RunAs -WindowStyle Hidden -FilePath (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -ArgumentList '-NoProfile','-NonInteractive','-EncodedCommand',$command64
