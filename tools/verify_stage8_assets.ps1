[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$SqlitePath = 'E:\MSYS2\mingw64\bin\sqlite3.exe'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}
$projectRootFull = [IO.Path]::GetFullPath($ProjectRoot)
$checks = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
    $script:checks++
}

function Resolve-ProjectFile([string]$RelativePath) {
    $path = [IO.Path]::GetFullPath(
        (Join-Path $projectRootFull $RelativePath.Replace('/', '\')))
    $prefix = $projectRootFull.TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    Assert-True ($path.StartsWith(
            $prefix,
            [StringComparison]::OrdinalIgnoreCase)) (
        "Manifest path escapes the project: $RelativePath")
    return $path
}

function Invoke-Sqlite([string]$DatabasePath, [string]$Sql) {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $SqlitePath
    $startInfo.Arguments = '-batch -bail "' +
        $DatabasePath.Replace('"', '""') + '"'
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $null = $process.Start()
    $process.StandardInput.Write($Sql)
    $process.StandardInput.Close()
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "sqlite3 failed ($($process.ExitCode)): $stderr"
    }
    return $stdout.Trim()
}

function Test-HashManifest([string]$ManifestRelativePath) {
    $manifestPath = Resolve-ProjectFile $ManifestRelativePath
    Assert-True (Test-Path -LiteralPath $manifestPath -PathType Leaf) (
        "Hash manifest is missing: $ManifestRelativePath")
    foreach ($line in [IO.File]::ReadAllLines($manifestPath)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        Assert-True ($line -match '^([0-9a-f]{64})  (.+)$') (
            "Malformed hash line in ${ManifestRelativePath}: $line")
        $expected = $Matches[1]
        $relative = $Matches[2]
        $path = Resolve-ProjectFile $relative
        Assert-True (Test-Path -LiteralPath $path -PathType Leaf) (
            "Hashed file is missing: $relative")
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).
            Hash.ToLowerInvariant()
        Assert-True ($actual -eq $expected) (
            "SHA-256 mismatch for ${relative}: expected $expected, got $actual")
    }
}

Assert-True (Test-Path -LiteralPath $SqlitePath -PathType Leaf) (
    "sqlite3 was not found: $SqlitePath")

$configPath = Resolve-ProjectFile 'config/default.json'
$config = Get-Content -Raw -Encoding UTF8 -LiteralPath $configPath |
    ConvertFrom-Json
Assert-True ($config.format_version -eq 1) 'Unexpected config format version.'
Assert-True (
    $config.status -eq 'frozen-reference-not-runtime-loaded') (
    'config/default.json must remain explicit about its non-runtime status.')
Assert-True (
    $config.application.project_database -eq 'data/signallab.sqlite3') (
    'The documented project database path is inconsistent.')

$csvReferencePath = Resolve-ProjectFile (
    'data/samples/expected/example_signal_reference.json')
$csvReference = Get-Content -Raw -Encoding UTF8 -LiteralPath $csvReferencePath |
    ConvertFrom-Json
Assert-True ($csvReference.import_result.sample_count -eq 21) (
    'CSV reference sample count is not 21.')
Assert-True ($csvReference.import_result.sample_rate_hz -eq 1000.0) (
    'CSV reference sample rate is not 1000 Hz.')
Assert-True ($csvReference.default_analysis.expected.fft_size -eq 32) (
    'CSV reference FFT size is not 32.')

$wavPath = Resolve-ProjectFile 'data/samples/example_signal_pcm16.wav'
$wav = [IO.File]::ReadAllBytes($wavPath)
Assert-True ($wav.Length -eq 86) 'Frozen WAV size must be 86 bytes.'
Assert-True (
    [Text.Encoding]::ASCII.GetString($wav, 0, 4) -eq 'RIFF') (
    'Frozen WAV has no RIFF header.')
Assert-True (
    [Text.Encoding]::ASCII.GetString($wav, 8, 4) -eq 'WAVE') (
    'Frozen WAV has no WAVE signature.')
Assert-True ([BitConverter]::ToUInt16($wav, 20) -eq 1) (
    'Frozen WAV is not PCM.')
Assert-True ([BitConverter]::ToUInt16($wav, 22) -eq 1) (
    'Frozen WAV is not mono.')
Assert-True ([BitConverter]::ToUInt32($wav, 24) -eq 1000) (
    'Frozen WAV sample rate is not 1000 Hz.')
Assert-True ([BitConverter]::ToUInt16($wav, 34) -eq 16) (
    'Frozen WAV is not 16-bit.')
Assert-True ([BitConverter]::ToUInt32($wav, 40) -eq 42) (
    'Frozen WAV does not contain 21 PCM16 samples.')

$databasePath = Resolve-ProjectFile 'data/templates/blank.sqlite3'
Assert-True (
    (Invoke-Sqlite $databasePath 'PRAGMA integrity_check;') -eq 'ok') (
    'Blank database integrity_check failed.')
Assert-True (
    (Invoke-Sqlite $databasePath 'PRAGMA foreign_key_check;').Length -eq 0) (
    'Blank database foreign_key_check returned rows.')

$expectedVersions = @(
    '001_init.sql',
    '002_task_scheduler.sql',
    '003_signal_script.sql') -join "`n"
$actualVersions = (Invoke-Sqlite $databasePath (
        'SELECT version FROM schema_migration ORDER BY version;')) -replace "`r", ''
Assert-True ($actualVersions -eq $expectedVersions) (
    "Blank database migration set is wrong: $actualVersions")

foreach ($table in @(
        'file_version',
        'signal_source',
        'signal_record',
        'analysis_task',
        'feature_result',
        'script_history',
        'operation_log')) {
    $count = Invoke-Sqlite $databasePath "SELECT count(*) FROM $table;"
    Assert-True ($count -eq '0') "Blank database table $table is not empty."
}

foreach ($required in @(
        'LICENSE',
        'THIRD_PARTY_NOTICES.md',
        'licenses/README.md',
        'data/samples/demo_analysis.sig',
        'docs/demo-script.md',
        'docs/user-guide.md',
        'docs/test-plan.md')) {
    Assert-True (
        (Test-Path -LiteralPath (Resolve-ProjectFile $required) -PathType Leaf)) (
        "Required Stage 8 document or asset is missing: $required")
}

Test-HashManifest 'data/samples/SHA256SUMS.txt'
Test-HashManifest 'licenses/SHA256SUMS.txt'

Write-Host "Stage 8 asset verification passed ($checks checks)."
