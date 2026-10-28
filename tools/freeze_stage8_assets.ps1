[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$SqlitePath = 'E:\MSYS2\mingw64\bin\sqlite3.exe',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}
$projectRootFull = [IO.Path]::GetFullPath($ProjectRoot)
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Assert-InProject([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $projectRootFull.TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith(
            $prefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the project: $full"
    }
    return $full
}

function Write-Utf8Lf([string]$Path, [string]$Content) {
    $target = Assert-InProject $Path
    $normalized = $Content -replace "`r`n", "`n" -replace "`r", "`n"
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) |
        Out-Null
    [IO.File]::WriteAllText($target, $normalized, $utf8NoBom)
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
    if (-not $process.Start()) {
        throw 'Could not start sqlite3.'
    }
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

function Write-Pcm16Sample {
    $csvPath = Join-Path $projectRootFull 'data\samples\example_signal.csv'
    $outputPath = Assert-InProject (
        Join-Path $projectRootFull 'data\samples\example_signal_pcm16.wav')
    if ((Test-Path -LiteralPath $outputPath) -and -not $Force) {
        return
    }

    $samples = @(
        Import-Csv -LiteralPath $csvPath |
            ForEach-Object {
                [double]::Parse(
                    $_.value,
                    [Globalization.CultureInfo]::InvariantCulture)
            })
    $pcm = @(
        $samples | ForEach-Object {
            $scaled = [Math]::Round(
                $_ * 32767.0,
                [MidpointRounding]::AwayFromZero)
            [int16][Math]::Max(-32768.0, [Math]::Min(32767.0, $scaled))
        })

    $temporary = "$outputPath.tmp.$([Guid]::NewGuid().ToString('N'))"
    try {
        $stream = [IO.File]::Open(
            $temporary,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        try {
            $writer = [IO.BinaryWriter]::new($stream, [Text.Encoding]::ASCII)
            try {
                $dataSize = $pcm.Count * 2
                $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
                $writer.Write([uint32](36 + $dataSize))
                $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
                $writer.Write([Text.Encoding]::ASCII.GetBytes('fmt '))
                $writer.Write([uint32]16)
                $writer.Write([uint16]1)
                $writer.Write([uint16]1)
                $writer.Write([uint32]1000)
                $writer.Write([uint32]2000)
                $writer.Write([uint16]2)
                $writer.Write([uint16]16)
                $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
                $writer.Write([uint32]$dataSize)
                foreach ($sample in $pcm) {
                    $writer.Write([int16]$sample)
                }
            }
            finally {
                $writer.Dispose()
            }
        }
        finally {
            $stream.Dispose()
        }
        Move-Item -LiteralPath $temporary -Destination $outputPath -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Write-BlankProjectDatabase {
    if (-not (Test-Path -LiteralPath $SqlitePath -PathType Leaf)) {
        throw "sqlite3 was not found: $SqlitePath"
    }

    $outputPath = Assert-InProject (
        Join-Path $projectRootFull 'data\templates\blank.sqlite3')
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath)) |
        Out-Null
    if ((Test-Path -LiteralPath $outputPath) -and -not $Force) {
        return
    }

    $temporary = "$outputPath.tmp.$([Guid]::NewGuid().ToString('N'))"
    try {
        $parts = [Collections.Generic.List[string]]::new()
        $parts.Add('PRAGMA page_size=4096;')
        $parts.Add('PRAGMA journal_mode=DELETE;')
        $parts.Add('PRAGMA foreign_keys=ON;')
        $parts.Add(@'
CREATE TABLE schema_migration (
    version TEXT PRIMARY KEY,
    applied_utc TEXT NOT NULL
);
'@)

        foreach ($migrationName in @(
                '001_init.sql',
                '002_task_scheduler.sql',
                '003_signal_script.sql')) {
            $migrationPath = Join-Path (
                Join-Path $projectRootFull 'migrations') $migrationName
            $migrationSql = [IO.File]::ReadAllText($migrationPath)
            $escapedName = $migrationName.Replace("'", "''")
            $parts.Add('BEGIN IMMEDIATE;')
            $parts.Add($migrationSql)
            $parts.Add(
                "INSERT INTO schema_migration(version, applied_utc) VALUES ('$escapedName', '2000-01-01T00:00:00.000Z');")
            $parts.Add('COMMIT;')
        }
        $parts.Add('VACUUM;')

        $null = Invoke-Sqlite $temporary ($parts -join "`n")

        $integrity = Invoke-Sqlite $temporary 'PRAGMA integrity_check;'
        if ($integrity -ne 'ok') {
            throw "Blank database integrity check failed: $integrity"
        }
        $foreignKeys = Invoke-Sqlite $temporary 'PRAGMA foreign_key_check;'
        if ($foreignKeys.Length -ne 0) {
            throw "Blank database foreign-key check failed: $foreignKeys"
        }

        $businessTables = @(
            'file_version',
            'signal_source',
            'signal_record',
            'analysis_task',
            'feature_result',
            'script_history',
            'operation_log')
        foreach ($table in $businessTables) {
            $count = Invoke-Sqlite $temporary "SELECT count(*) FROM $table;"
            if ($count -ne '0') {
                throw "Blank database table $table is not empty: $count"
            }
        }

        $versions = Invoke-Sqlite $temporary (
            'SELECT version FROM schema_migration ORDER BY version;')
        $expectedVersions = @(
            '001_init.sql',
            '002_task_scheduler.sql',
            '003_signal_script.sql') -join "`n"
        if (($versions -replace "`r", '') -ne $expectedVersions) {
            throw "Blank database migration set is wrong: $versions"
        }

        Move-Item -LiteralPath $temporary -Destination $outputPath -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Copy-ThirdPartyLicenses {
    $sourceRoot = 'E:\MSYS2\mingw64\share\licenses'
    $targetRoot = Assert-InProject (
        Join-Path $projectRootFull 'licenses\third-party')
    [IO.Directory]::CreateDirectory($targetRoot) | Out-Null

    $components = @(
        'brotli',
        'bzip2',
        'crt',
        'curl',
        'double-conversion',
        'freetype',
        'gcc-libs',
        'gettext-runtime',
        'glib2',
        'graphite2',
        'gtest',
        'harfbuzz',
        'headers',
        'libb2',
        'libiconv',
        'libjpeg-turbo',
        'libmariadbclient',
        'libpng',
        'libpsl',
        'libssh2',
        'libunistring',
        'libwinpthread',
        'md4c',
        'nghttp2',
        'nghttp3',
        'ngtcp2',
        'openssl',
        'pcre2',
        'qt6-base',
        'qt6-charts',
        'sqlite3',
        'winpthreads',
        'zlib',
        'zstd')

    foreach ($component in $components) {
        $source = Join-Path $sourceRoot $component
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            throw "Installed package license directory is missing: $source"
        }
        $target = Assert-InProject (Join-Path $targetRoot $component)
        if (Test-Path -LiteralPath $target) {
            if (-not $Force) {
                continue
            }
            Remove-Item -LiteralPath $target -Recurse -Force
        }
        Copy-Item -LiteralPath $source -Destination $target -Recurse
    }

    $icuSource = 'E:\MSYS2\mingw64\share\icu\78.3\LICENSE'
    if (-not (Test-Path -LiteralPath $icuSource -PathType Leaf)) {
        throw "Installed ICU license is missing: $icuSource"
    }
    $icuTargetDirectory = Assert-InProject (Join-Path $targetRoot 'icu')
    [IO.Directory]::CreateDirectory($icuTargetDirectory) | Out-Null
    $icuTarget = Join-Path $icuTargetDirectory 'LICENSE'
    if ($Force -or -not (Test-Path -LiteralPath $icuTarget)) {
        Copy-Item -LiteralPath $icuSource -Destination $icuTarget -Force
    }

    $idnHeader = 'E:\MSYS2\mingw64\include\idn2.h'
    if (-not (Test-Path -LiteralPath $idnHeader -PathType Leaf)) {
        throw "Installed libidn2 license notice source is missing: $idnHeader"
    }
    $idnSource = [IO.File]::ReadAllText($idnHeader)
    $idnNoticeEnd = $idnSource.IndexOf('*/')
    if (-not $idnSource.StartsWith('/*') -or $idnNoticeEnd -lt 0) {
        throw "Installed libidn2 header does not begin with the expected license notice."
    }
    $idnTarget = Assert-InProject (Join-Path $targetRoot 'libidn2\NOTICE')
    if ($Force -or -not (Test-Path -LiteralPath $idnTarget)) {
        Write-Utf8Lf $idnTarget ($idnSource.Substring(0, $idnNoticeEnd + 2) + "`n")
    }

    $licensesRoot = [IO.Path]::GetFullPath(
        (Join-Path $projectRootFull 'licenses'))
    $hashLines = Get-ChildItem -LiteralPath $targetRoot -Recurse -File |
        Sort-Object FullName |
        ForEach-Object {
            $relative = 'licenses/' + $_.FullName.
                Substring($licensesRoot.Length).
                TrimStart('\', '/').Replace('\', '/')
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).
                Hash.ToLowerInvariant()
            "$hash  $relative"
        }
    Write-Utf8Lf (
        Join-Path $projectRootFull 'licenses\SHA256SUMS.txt') (
        ($hashLines -join "`n") + "`n")
}

function Write-SampleHashes {
    $relativePaths = @(
        'data/templates/blank.sqlite3',
        'data/samples/demo_analysis.sig',
        'data/samples/example_analysis.sig',
        'data/samples/example_signal.csv',
        'data/samples/example_signal_pcm16.wav',
        'data/samples/expected/example_signal_reference.json',
        'data/samples/expected/example_signal_wav_reference.json')
    $lines = foreach ($relative in $relativePaths) {
        $path = Join-Path $projectRootFull ($relative.Replace('/', '\'))
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Frozen sample asset is missing: $relative"
        }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).
            Hash.ToLowerInvariant()
        "$hash  $relative"
    }
    Write-Utf8Lf (
        Join-Path $projectRootFull 'data\samples\SHA256SUMS.txt') (
        ($lines -join "`n") + "`n")
}

Write-Pcm16Sample
Write-BlankProjectDatabase
Copy-ThirdPartyLicenses
Write-SampleHashes

Write-Host 'Stage 8 assets were frozen successfully.'
