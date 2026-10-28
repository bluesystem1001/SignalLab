[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDirectory,

    [string]$Objdump = "E:\MSYS2\mingw64\bin\objdump.exe",

    [string]$Sqlite = "E:\MSYS2\mingw64\bin\sqlite3.exe",

    [ValidateRange(1, 60)]
    [int]$StartupTimeoutSeconds = 15,

    [switch]$SkipLaunchCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-AbsolutePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

function Assert-FileExists {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $candidate = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "Required release file is missing: $RelativePath"
    }
}

function Restore-EnvironmentVariable {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowNull()][string]$Value
    )

    if ($null -eq $Value) {
        Remove-Item -LiteralPath ("Env:" + $Name) -ErrorAction SilentlyContinue
    } else {
        Set-Item -LiteralPath ("Env:" + $Name) -Value $Value
    }
}

function Test-HashManifest {
    param([Parameter(Mandatory = $true)][string]$Root)

    $manifestPath = Join-Path $Root "SHA256SUMS.txt"
    Assert-FileExists -Root $Root -RelativePath "SHA256SUMS.txt"

    $listedFiles = @{}
    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath $manifestPath -Encoding UTF8) {
        ++$lineNumber
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -notmatch '^([0-9A-Fa-f]{64})  (.+)$') {
            throw "Invalid SHA256SUMS.txt entry at line ${lineNumber}: $line"
        }

        $expectedHash = $Matches[1].ToLowerInvariant()
        $relativePath = $Matches[2].Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        if ([System.IO.Path]::IsPathRooted($relativePath) -or $relativePath -match '(^|[\\/])\.\.([\\/]|$)') {
            throw "Unsafe path in SHA256SUMS.txt: $relativePath"
        }

        $filePath = Join-Path $Root $relativePath
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            throw "Manifest references a missing file: $relativePath"
        }
        $actualHash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "SHA-256 mismatch for $relativePath"
        }
        $listedFiles[$relativePath.ToLowerInvariant()] = $true
    }

    $actualFiles = Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object {
        $_.FullName -ne $manifestPath
    }
    foreach ($file in $actualFiles) {
        $relativePath = $file.FullName.Substring($Root.Length).TrimStart('\', '/')
        if (-not $listedFiles.ContainsKey($relativePath.ToLowerInvariant())) {
            throw "Release file is not covered by SHA256SUMS.txt: $relativePath"
        }
    }

    if ($listedFiles.Count -ne $actualFiles.Count) {
        throw "SHA256SUMS.txt contains duplicate or inconsistent entries."
    }
}

function Test-NativeDependencies {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$ObjdumpPath
    )

    if (-not (Test-Path -LiteralPath $ObjdumpPath -PathType Leaf)) {
        throw "objdump was not found: $ObjdumpPath"
    }

    $availableNames = @{}
    $binaryFiles = Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object {
        $_.Extension -ieq ".exe" -or $_.Extension -ieq ".dll"
    }
    foreach ($file in $binaryFiles) {
        $availableNames[$file.Name.ToLowerInvariant()] = $true
    }

    $systemDirectories = @(
        (Join-Path $env:SystemRoot "System32"),
        (Join-Path $env:SystemRoot "SysWOW64")
    )
    $missing = New-Object System.Collections.Generic.List[string]
    $usesMariaDbClient = $false
    foreach ($binary in $binaryFiles) {
        $fileHeader = & $ObjdumpPath -f $binary.FullName 2>&1
        if ($LASTEXITCODE -ne 0 -or ($fileHeader -join "`n") -notmatch 'file format pei-x86-64') {
            $relativeBinary = $binary.FullName.Substring($Root.Length).TrimStart('\', '/')
            throw "Release binary is not a valid Windows x64 PE file: $relativeBinary"
        }
        $output = & $ObjdumpPath -p $binary.FullName 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "objdump failed for $($binary.FullName): $($output -join [Environment]::NewLine)"
        }

        foreach ($line in $output) {
            if ($line -notmatch 'DLL Name:\s*(\S+)') {
                continue
            }
            $dependency = $Matches[1]
            $dependencyKey = $dependency.ToLowerInvariant()
            if ($binary.Name -ieq "SignalLab.exe" -and $dependencyKey -eq "libmariadb.dll") {
                $usesMariaDbClient = $true
            }
            if ($availableNames.ContainsKey($dependencyKey)) {
                continue
            }
            if ($dependencyKey -match '^(api-ms-win-|ext-ms-win-)') {
                continue
            }

            $isSystemLibrary = $false
            foreach ($systemDirectory in $systemDirectories) {
                if (Test-Path -LiteralPath (Join-Path $systemDirectory $dependency) -PathType Leaf) {
                    $isSystemLibrary = $true
                    break
                }
            }
            if (-not $isSystemLibrary) {
                $relativeBinary = $binary.FullName.Substring($Root.Length).TrimStart('\', '/')
                $missing.Add("$relativeBinary -> $dependency")
            }
        }
    }

    if ($missing.Count -gt 0) {
        throw "Unresolved non-system DLL imports:`n$($missing -join "`n")"
    }
    if (-not $usesMariaDbClient) {
        throw "SignalLab.exe must link directly to MariaDB Connector/C (libmariadb.dll)."
    }
}

function Test-FrozenAssets {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$SqlitePath,
        [Parameter(Mandatory = $true)][int]$ExpectedMigrationCount
    )

    if (-not (Test-Path -LiteralPath $SqlitePath -PathType Leaf)) {
        throw "sqlite3 was not found: $SqlitePath"
    }

    $configPath = Join-Path $Root "config\default.json"
    try {
        $config = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        throw "config/default.json is not valid JSON: $($_.Exception.Message)"
    }
    if ($config.application.project_database -ne "data/signallab.sqlite3" `
        -or $config.application.migrations_directory -ne "migrations") {
        throw "config/default.json does not describe the packaged runtime layout."
    }

    $templatePath = Join-Path $Root "data\templates\blank.sqlite3"
    $templateIntegrity = (& $SqlitePath -batch $templatePath "PRAGMA integrity_check;").Trim()
    if ($LASTEXITCODE -ne 0 -or $templateIntegrity -ne "ok") {
        throw "Blank database template integrity_check failed: $templateIntegrity"
    }
    $templateForeignKeyRows = & $SqlitePath -batch $templatePath "PRAGMA foreign_key_check;"
    if ($LASTEXITCODE -ne 0 -or @($templateForeignKeyRows).Count -ne 0) {
        throw "Blank database template foreign_key_check returned rows: $($templateForeignKeyRows -join '; ')"
    }
    $templateMigrationCount = (& $SqlitePath -batch $templatePath "SELECT COUNT(*) FROM schema_migration;").Trim()
    if ($LASTEXITCODE -ne 0 -or [int]$templateMigrationCount -ne $ExpectedMigrationCount) {
        throw "Blank database template has $templateMigrationCount migrations; expected $ExpectedMigrationCount."
    }
    $businessRowCount = (& $SqlitePath -batch $templatePath @"
SELECT
    (SELECT COUNT(*) FROM file_version)
  + (SELECT COUNT(*) FROM signal_source)
  + (SELECT COUNT(*) FROM signal_record)
  + (SELECT COUNT(*) FROM analysis_task)
  + (SELECT COUNT(*) FROM feature_result)
  + (SELECT COUNT(*) FROM script_history)
  + (SELECT COUNT(*) FROM operation_log);
"@).Trim()
    if ($LASTEXITCODE -ne 0 -or [int]$businessRowCount -ne 0) {
        throw "Blank database template contains $businessRowCount business rows; expected 0."
    }

    $licenseRoot = Join-Path $Root "licenses"
    $licenseManifest = Join-Path $licenseRoot "SHA256SUMS.txt"
    $licenseEntryCount = 0
    $verifiedLicenses = @{}
    foreach ($line in Get-Content -LiteralPath $licenseManifest -Encoding UTF8) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -notmatch '^([0-9A-Fa-f]{64})  (.+)$') {
            throw "Invalid entry in licenses/SHA256SUMS.txt: $line"
        }
        $expectedHash = $Matches[1].ToLowerInvariant()
        $relativePath = $Matches[2].Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        if ([System.IO.Path]::IsPathRooted($relativePath) -or $relativePath -match '(^|[\\/])\.\.([\\/]|$)') {
            throw "Unsafe path in licenses/SHA256SUMS.txt: $relativePath"
        }
        if ($relativePath -notmatch '^licenses[\\/]third-party[\\/]') {
            throw "License manifest path is outside licenses/third-party: $relativePath"
        }
        # The frozen asset manifest intentionally uses project/package-root
        # relative paths so the same file verifies both the source tree and
        # the portable release layout.
        $licensePath = Join-Path $Root $relativePath
        if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf)) {
            throw "License manifest references a missing file: $relativePath"
        }
        $actualHash = (Get-FileHash -LiteralPath $licensePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "License SHA-256 mismatch: $relativePath"
        }
        $verifiedLicenses[$relativePath.Replace('\', '/')] = $true
        ++$licenseEntryCount
    }
    if ($licenseEntryCount -lt 1) {
        throw "licenses/SHA256SUMS.txt contains no third-party license entries."
    }
    foreach ($requiredLicense in @(
            "curl/LICENSE",
            "libidn2/NOTICE",
            "libmariadbclient/COPYING.LIB",
            "libpsl/COPYING",
            "libssh2/LICENSE",
            "libunistring/LICENSE",
            "libunistring/LICENSE.LIB",
            "nghttp2/COPYING",
            "nghttp3/COPYING",
            "ngtcp2/COPYING",
            "openssl/LICENSE")) {
        if (-not $verifiedLicenses.ContainsKey("licenses/third-party/$requiredLicense")) {
            throw "MariaDB runtime license is missing from the verified license manifest: $requiredLicense"
        }
    }
}

function Test-IsolatedStartup {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$SqlitePath,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    if (-not (Test-Path -LiteralPath $SqlitePath -PathType Leaf)) {
        throw "sqlite3 was not found: $SqlitePath"
    }

    $temporaryBase = [System.IO.Path]::GetTempPath()
    $smokeRoot = Join-Path $temporaryBase ("SignalLab-package-smoke-" + [guid]::NewGuid().ToString("N"))
    $smokePackage = Join-Path $smokeRoot "package"
    New-Item -ItemType Directory -Path $smokePackage | Out-Null

    $process = $null
    $oldPath = $env:PATH
    $oldQpaPlatform = $env:QT_QPA_PLATFORM
    $oldPluginPath = $env:QT_PLUGIN_PATH
    $oldQpaPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
    try {
        Get-ChildItem -LiteralPath $Root -Force | Copy-Item -Destination $smokePackage -Recurse -Force

        $smokeExecutable = Join-Path $smokePackage "SignalLab.exe"
        $env:PATH = (Join-Path $env:SystemRoot "System32") + ";" + $env:SystemRoot
        $env:QT_QPA_PLATFORM = "offscreen"
        $env:QT_PLUGIN_PATH = $smokePackage
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $smokePackage "platforms"

        $process = Start-Process `
            -FilePath $smokeExecutable `
            -ArgumentList "--smoke-test" `
            -WorkingDirectory $smokePackage `
            -WindowStyle Hidden `
            -PassThru

        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            throw "Packaged SignalLab did not finish --smoke-test within $TimeoutSeconds seconds."
        }
        # On Windows PowerShell, the timed overload can leave ExitCode
        # unpopulated until the parameterless wait refreshes the process.
        $process.WaitForExit()
        $process.Refresh()
        if ($process.ExitCode -ne 0) {
            throw "Packaged SignalLab --smoke-test exited with code $($process.ExitCode)."
        }

        $databasePath = Join-Path $smokePackage "data\signallab.sqlite3"
        if (-not (Test-Path -LiteralPath $databasePath -PathType Leaf)) {
            throw "The isolated smoke test did not create data/signallab.sqlite3."
        }
        if ((Get-Item -LiteralPath $databasePath).Length -le 0) {
            throw "The isolated smoke-test database is empty."
        }

        $integrity = (& $SqlitePath -batch $databasePath "PRAGMA integrity_check;").Trim()
        if ($LASTEXITCODE -ne 0 -or $integrity -ne "ok") {
            throw "Smoke-test database integrity_check failed: $integrity"
        }
        $foreignKeyRows = & $SqlitePath -batch $databasePath "PRAGMA foreign_key_check;"
        if ($LASTEXITCODE -ne 0 -or @($foreignKeyRows).Count -ne 0) {
            throw "Smoke-test database foreign_key_check returned rows: $($foreignKeyRows -join '; ')"
        }
        $migrationCount = (& $SqlitePath -batch $databasePath "SELECT COUNT(*) FROM schema_migration;").Trim()
        if ($LASTEXITCODE -ne 0 -or [int]$migrationCount -lt 1) {
            throw "Smoke-test database has no applied migrations."
        }
        $signalCount = (& $SqlitePath -batch $databasePath "SELECT COUNT(*) FROM signal_record;").Trim()
        if ($LASTEXITCODE -ne 0 -or [int]$signalCount -lt 1) {
            throw "Clean-start smoke test did not persist the built-in demonstration signal."
        }
    } finally {
        if ($null -ne $process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        Restore-EnvironmentVariable -Name "PATH" -Value $oldPath
        Restore-EnvironmentVariable -Name "QT_QPA_PLATFORM" -Value $oldQpaPlatform
        Restore-EnvironmentVariable -Name "QT_PLUGIN_PATH" -Value $oldPluginPath
        Restore-EnvironmentVariable -Name "QT_QPA_PLATFORM_PLUGIN_PATH" -Value $oldQpaPluginPath

        $resolvedTemporaryBase = [System.IO.Path]::GetFullPath($temporaryBase).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
        $resolvedSmokeRoot = [System.IO.Path]::GetFullPath($smokeRoot)
        if ($resolvedSmokeRoot.StartsWith($resolvedTemporaryBase, [System.StringComparison]::OrdinalIgnoreCase) `
            -and (Split-Path -Leaf $resolvedSmokeRoot).StartsWith("SignalLab-package-smoke-")) {
            Remove-Item -LiteralPath $resolvedSmokeRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

$packageRoot = Get-AbsolutePath $PackageDirectory
if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
    throw "Package directory was not found: $packageRoot"
}

$requiredFiles = @(
    "SignalLab.exe",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "Qt6Charts.dll",
    "Qt6Sql.dll",
    "libgcc_s_seh-1.dll",
    "libstdc++-6.dll",
    "libwinpthread-1.dll",
    "libmariadb.dll",
    "platforms\qwindows.dll",
    "platforms\qoffscreen.dll",
    "sqldrivers\qsqlite.dll",
    "imageformats\qgif.dll",
    "imageformats\qico.dll",
    "imageformats\qjpeg.dll",
    "README.md",
    "THIRD_PARTY_NOTICES.md",
    "docs\user-guide.md",
    "docs\release-assets.md",
    "docs\demo-script.md",
    "docs\test-plan.md",
    "docs\stage8-validation.md",
    "docs\algorithms.md",
    "docs\signalscript.md",
    "docs\msys2-package-snapshot.txt",
    "docs\adr\0005-clion-bundled-gdb.md",
    "LICENSE",
    "config\default.json",
    "data\templates\blank.sqlite3",
    "data\templates\README.md",
    "licenses\SHA256SUMS.txt",
    "SHA256SUMS.txt"
)
foreach ($relativePath in $requiredFiles) {
    Assert-FileExists -Root $packageRoot -RelativePath $relativePath
}

$mysqlPlugin = Get-ChildItem -LiteralPath $packageRoot -Recurse -File -Filter "qsqlmysql*.dll" |
    Select-Object -First 1
if ($null -ne $mysqlPlugin) {
    throw "QMYSQL must not be bundled; MariaDB backup uses Connector/C directly: $($mysqlPlugin.FullName)"
}

$migrationFiles = Get-ChildItem -LiteralPath (Join-Path $packageRoot "migrations") -Filter "*.sql" -File
if ($migrationFiles.Count -lt 1) {
    throw "The release package contains no SQL migrations."
}

$sqliteExecutable = Get-AbsolutePath $Sqlite
Test-FrozenAssets `
    -Root $packageRoot `
    -SqlitePath $sqliteExecutable `
    -ExpectedMigrationCount $migrationFiles.Count

$activeDataFiles = @(
    "data\signallab.sqlite3",
    "data\signallab.sqlite3-shm",
    "data\signallab.sqlite3-wal"
)
foreach ($relativePath in $activeDataFiles) {
    if (Test-Path -LiteralPath (Join-Path $packageRoot $relativePath)) {
        throw "Mutable project state must not be shipped: $relativePath"
    }
}
foreach ($relativePath in @("data\imports", "data\script_outputs")) {
    if (Test-Path -LiteralPath (Join-Path $packageRoot $relativePath)) {
        throw "Mutable project directory must not be shipped: $relativePath"
    }
}

$allowedDocuments = @{
    "docs\user-guide.md" = $true
    "docs\release-assets.md" = $true
    "docs\demo-script.md" = $true
    "docs\test-plan.md" = $true
    "docs\stage8-validation.md" = $true
    "docs\algorithms.md" = $true
    "docs\signalscript.md" = $true
    "docs\msys2-package-snapshot.txt" = $true
    "docs\adr\0005-clion-bundled-gdb.md" = $true
}
foreach ($document in Get-ChildItem -LiteralPath (Join-Path $packageRoot "docs") -Recurse -File) {
    $relativeDocument = $document.FullName.Substring($packageRoot.Length).TrimStart('\', '/')
    if (-not $allowedDocuments.ContainsKey($relativeDocument)) {
        throw "Unexpected document in release package: $relativeDocument"
    }
}

Test-HashManifest -Root $packageRoot
Test-NativeDependencies -Root $packageRoot -ObjdumpPath (Get-AbsolutePath $Objdump)
if (-not $SkipLaunchCheck) {
    Test-IsolatedStartup `
        -Root $packageRoot `
        -SqlitePath $sqliteExecutable `
        -TimeoutSeconds $StartupTimeoutSeconds
}

Write-Host "Windows package verification passed: $packageRoot"
