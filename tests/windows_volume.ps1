# Elevated acceptance fixture. All writes target a new VHD or a per-run
# Program Files directory. The caller releases the fixture by creating done.flag.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][guid]$RunId,
    [Parameter(Mandatory)][string]$Scanner,
    [Parameter(Mandatory)][string]$PythonZip,
    [Parameter(Mandatory)][string]$SignalDir,
    [Parameter(Mandatory)][string]$CallerSid
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$programFiles = [Environment]::GetFolderPath('ProgramFiles')
$stage = Join-Path $programFiles "Disk-Diver-Test-$RunId"
$image = Join-Path $stage 'test.vhdx'
$project = Split-Path $PSScriptRoot -Parent
if (Test-Path -LiteralPath $stage) { throw "Temporary install already exists" }
if ($CallerSid -notmatch '^S-1-5-21-[0-9-]+$') { throw "Invalid test account SID" }
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $PythonZip).Hash -ne
    '3c01d7cfc666f746a6f064f60fb2156ab0ec464d3d738ab796371a46a9bb24a2') { throw "Unexpected runtime archive" }
$stage = (New-Item -ItemType Directory -Path $stage).FullName
$mounted = $false
try {
    Copy-Item -LiteralPath $Scanner -Destination (Join-Path $stage 'disk-diver-cpp.exe')
    foreach ($module in 'worker.py','dd_journal.py','dd_privilege.py') {
        Copy-Item -LiteralPath (Join-Path $project $module) -Destination $stage
    }
    Expand-Archive -LiteralPath $PythonZip -DestinationPath (Join-Path $stage 'python')
    # DiskPart only creates the new file. Partitioning below takes a verified disk object.
    $commands = Join-Path $stage 'create.txt'
    "create vdisk file=""$image"" maximum=8192 type=expandable" | Set-Content -LiteralPath $commands -Encoding Ascii
    & "$env:SystemRoot\System32\diskpart.exe" /s $commands | Out-File (Join-Path $stage 'create.log')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $image -PathType Leaf)) { throw "VHD creation failed" }
    $attachment = Mount-DiskImage -ImagePath $image -PassThru
    $mounted = $true
    $disk = $attachment | Get-Disk
    if ($attachment.ImagePath -ne $image -or $disk.PartitionStyle -ne 'RAW' -or
        $disk.Size -ne 8GB -or $disk.IsBoot -or $disk.IsSystem -or
        $disk.BusType -notin 'File Backed Virtual','Virtual') { throw "Unexpected scratch disk identity" }
    $initialized = $disk | Initialize-Disk -PartitionStyle GPT -PassThru
    $partition = $initialized | New-Partition -UseMaximumSize -AssignDriveLetter
    $volume = $partition | Format-Volume -FileSystem NTFS -NewFileSystemLabel 'DiskDiverTest' -Confirm:$false
    if ($partition.DiskNumber -ne $disk.Number -or $volume.FileSystemLabel -ne 'DiskDiverTest' -or
        -not $partition.DriveLetter) { throw "Unexpected formatted test volume" }
    $drive = "$($partition.DriveLetter):"
    $root = "$drive\"
    $grant = '*{0}:(OI)(CI)(M)' -f $CallerSid
    & "$env:SystemRoot\System32\icacls.exe" $root /grant $grant | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Cannot grant access to the test volume" }
    & "$env:SystemRoot\System32\fsutil.exe" usn createjournal m=0x4000000 a=0x800000 $drive | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Cannot initialize the scratch journal" }
    $sample = New-Item -ItemType Directory -Path (Join-Path $root 'acceptance')
    [IO.File]::WriteAllBytes((Join-Path $sample.FullName 'resident'), [byte[]](1,2,3))
    [IO.File]::WriteAllBytes((Join-Path $sample.FullName 'regular'), (New-Object 'byte[]' 32768))
    $out = Join-Path $stage 'layout.ddv'
    $err = Join-Path $stage 'layout.err'
    $bin = Join-Path $stage 'disk-diver-cpp.exe'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $layout = Start-Process $bin -ArgumentList @('layout',$root) -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $elapsed = $timer.ElapsedMilliseconds
    $row = [ordered]@{root=$root; layout_exit=$layout.ExitCode; layout_ms=$elapsed}
    if ($layout.ExitCode -eq 0) { $row['layout'] = (& $bin replay $out | ConvertFrom-Json) }
    $row['traversal'] = (& $bin summary $root | ConvertFrom-Json)
    $row | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'ready.json')
    $deadline = [DateTime]::UtcNow.AddHours(2)
    while (-not (Test-Path -LiteralPath (Join-Path $SignalDir 'done.flag')) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Seconds 2
    }
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $stage 'failure.txt')
    Start-Sleep -Seconds 30
    throw
} finally {
    if ($mounted) { Dismount-DiskImage -ImagePath $image -ErrorAction Stop }
    $actual = (Resolve-Path -LiteralPath $stage).Path
    if ([IO.Path]::GetDirectoryName($actual) -ne $programFiles -or
        [IO.Path]::GetFileName($actual) -ne "Disk-Diver-Test-$RunId") { throw "Unexpected cleanup target" }
    Remove-Item -LiteralPath $actual -Force -Recurse
}
