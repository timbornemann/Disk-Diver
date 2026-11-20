# Re-check only the disposable mount created by windows_volume.ps1.
param([Parameter(Mandatory)][guid]$RunId, [Parameter(Mandatory)][string]$CallerSid,
      [string]$BenchBinDir)
$ErrorActionPreference = 'Stop'
$stage = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) "Disk-Diver-Test-$RunId"
$project = Split-Path $PSScriptRoot -Parent
$image = Join-Path $stage 'test.vhdx'
$root = (Get-Content (Join-Path $stage 'ready.json') -Raw | ConvertFrom-Json).root
$drive = $root.Substring(0,1)
$disk = Get-DiskImage -ImagePath $image | Get-Disk
$partition = Get-Partition -DriveLetter $drive
if ($disk.Number -ne $partition.DiskNumber -or $disk.IsSystem -or $disk.IsBoot -or
    (Get-Volume -DriveLetter $drive).FileSystemLabel -ne 'DiskDiverTest' -or
    $CallerSid -notmatch '^S-1-5-21-[0-9-]+$') { throw "Wrong test mount" }
try {
    foreach ($module in 'worker.py','dd_journal.py','dd_privilege.py') {
        Copy-Item -LiteralPath (Join-Path $project $module) -Destination $stage -Force
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check_ntfs.py') -Destination $stage -Force
    $python = Join-Path $stage 'python/python.exe'
    if ($BenchBinDir) {
        foreach ($binary in 'disk-diver-cpp.exe','disk-diver-rust.exe') {
            Copy-Item -LiteralPath (Join-Path $BenchBinDir $binary) -Destination $stage -Force
        }
        'rust' | Set-Content -LiteralPath (Join-Path $stage 'disk-diver-engine.txt') -Encoding Ascii
        Copy-Item -LiteralPath (Join-Path $project 'cpp/bench_ntfs_layout.py') -Destination $stage -Force
        Copy-Item -LiteralPath (Join-Path $project 'bench/measure.py') -Destination $stage -Force
        $bench = Start-Process $python -ArgumentList @('-I','-S','-B','bench_ntfs_layout.py','--root',$root,'--rounds','6') -WorkingDirectory $stage -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $stage 'layout-benchmark.json') -RedirectStandardError (Join-Path $stage 'layout-benchmark.err')
        if ($bench.ExitCode -ne 0) { throw 'Matched-record volume layout benchmark failed' }
    }
    & $python -I -S -B (Join-Path $stage 'check_ntfs.py') $stage $root > (Join-Path $stage 'journal.json') 2> (Join-Path $stage 'journal.err')
    if ($LASTEXITCODE -ne 0) { throw 'NTFS journal acceptance failed' }
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $stage 'check-error.txt')
} finally {
    # Allow identical *ordinary user* measurements of all files on this disposable
    # test mount, after the protected-directory test has completed.
    $systemInfo = Join-Path $root 'System Volume Information'
    if (Test-Path -LiteralPath $systemInfo) {
        & "$env:SystemRoot\System32\takeown.exe" /F $systemInfo /A | Out-Null
        & "$env:SystemRoot\System32\icacls.exe" $systemInfo /grant ('*{0}:(OI)(CI)(RX)' -f $CallerSid) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Cannot prepare the disposable volume for matched-rights tests" }
    }
    'ready' | Set-Content -LiteralPath (Join-Path $stage 'checks-finished.txt')
}
