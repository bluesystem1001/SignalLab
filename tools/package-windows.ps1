[CmdletBinding()]
param(
    [string]$SourceRoot,

    [string]$BuildDirectory,

    [string]$DistRoot,

    [string]$CMake = "E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe",

    [string]$Ninja = "E:\CLion\CLion 2026.1.4\bin\ninja\win\x64\ninja.exe",

    [string]$MingwRoot = "E:\MSYS2\mingw64",

    [string]$WindeployQt = "E:\MSYS2\mingw64\bin\windeployqt6.exe",

    [string]$Objdump = "E:\MSYS2\mingw64\bin\objdump.exe",

    [string]$Sqlite = "E:\MSYS2\mingw64\bin\sqlite3.exe",

    [switch]$SkipBuild,

    [switch]$SkipLaunchCheck,

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-AbsolutePath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$BasePath
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Assert-ToolExists {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Name was not found: $Path"
    }
}

function Invoke-NativeTool {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Description,
        [switch]$AllowStderr
    )

    Write-Host "> $Description"
    if ($AllowStderr) {
        $previousPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = "Continue"
            & $FilePath @Arguments
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
        }
    } else {
        & $FilePath @Arguments
        $exitCode = $LASTEXITCODE
    }
    if ($exitCode -ne 0) {
        throw "$Description failed with exit code $exitCode."
    }
}

function Assert-SafeDistTarget {
    param(
        [Parameter(Mandatory = $true)][string]$Target,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $resolvedRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $resolvedTarget = [System.IO.Path]::GetFullPath($Target)
    if (-not $resolvedTarget.StartsWith($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the distribution directory: $resolvedTarget"
    }
    if ((Split-Path -Leaf $resolvedTarget) -notmatch '^SignalLab-[0-9A-Za-z.+-]+-windows-x64(?:\.zip(?:\.sha256)?)?$') {
        throw "Refusing to modify an unexpected distribution target: $resolvedTarget"
    }
    if (Test-Path -LiteralPath $resolvedTarget) {
        $targetItem = Get-Item -LiteralPath $resolvedTarget -Force
        if (($targetItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to replace a distribution target that is a reparse point: $resolvedTarget"
        }
    }
}

function Copy-OptionalItem {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (Test-Path -LiteralPath $Source) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
    }
}

function Copy-MingwDependencyClosure {
    param(
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$MingwBin,
        [Parameter(Mandatory = $true)][string]$ObjdumpPath
    )

    $systemDirectories = @(
        (Join-Path $env:SystemRoot "System32"),
        (Join-Path $env:SystemRoot "SysWOW64")
    )
    for ($pass = 0; $pass -lt 32; ++$pass) {
        $availableNames = @{}
        $binaryFiles = Get-ChildItem -LiteralPath $PackageRoot -Recurse -File | Where-Object {
            $_.Extension -ieq ".exe" -or $_.Extension -ieq ".dll"
        }
        foreach ($file in $binaryFiles) {
            $availableNames[$file.Name.ToLowerInvariant()] = $true
        }

        $copiedThisPass = $false
        foreach ($binary in $binaryFiles) {
            $output = & $ObjdumpPath -p $binary.FullName 2>&1
            if ($LASTEXITCODE -ne 0) {
                throw "objdump failed while resolving $($binary.FullName): $($output -join [Environment]::NewLine)"
            }
            foreach ($line in $output) {
                if ($line -notmatch 'DLL Name:\s*(\S+)') {
                    continue
                }
                $dependency = $Matches[1]
                $dependencyKey = $dependency.ToLowerInvariant()
                if ($availableNames.ContainsKey($dependencyKey) `
                    -or $dependencyKey -match '^(api-ms-win-|ext-ms-win-)') {
                    continue
                }

                $isSystemLibrary = $false
                foreach ($systemDirectory in $systemDirectories) {
                    if (Test-Path -LiteralPath (Join-Path $systemDirectory $dependency) -PathType Leaf) {
                        $isSystemLibrary = $true
                        break
                    }
                }
                if ($isSystemLibrary) {
                    continue
                }

                $mingwSource = Join-Path $MingwBin $dependency
                if (-not (Test-Path -LiteralPath $mingwSource -PathType Leaf)) {
                    $relativeBinary = $binary.FullName.Substring($PackageRoot.Length).TrimStart('\', '/')
                    throw "Cannot resolve non-system dependency $dependency required by $relativeBinary."
                }
                Copy-Item -LiteralPath $mingwSource -Destination (Join-Path $PackageRoot $dependency)
                $availableNames[$dependencyKey] = $true
                $copiedThisPass = $true
                Write-Host "Bundled MinGW dependency: $dependency"
            }
        }

        if (-not $copiedThisPass) {
            return
        }
    }

    throw "MinGW dependency closure did not converge after 32 passes."
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = Split-Path -Parent $PSScriptRoot
}
$sourceDirectory = Get-AbsolutePath -Path $SourceRoot -BasePath (Get-Location)
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $sourceDirectory "cmake-build-package-release"
}
if ([string]::IsNullOrWhiteSpace($DistRoot)) {
    $DistRoot = Join-Path $sourceDirectory "dist"
}
$buildRoot = Get-AbsolutePath -Path $BuildDirectory -BasePath $sourceDirectory
$distributionRoot = Get-AbsolutePath -Path $DistRoot -BasePath $sourceDirectory
$cmakePath = Get-AbsolutePath -Path $CMake -BasePath $sourceDirectory
$ninjaPath = Get-AbsolutePath -Path $Ninja -BasePath $sourceDirectory
$mingwDirectory = Get-AbsolutePath -Path $MingwRoot -BasePath $sourceDirectory
$windeployQtPath = Get-AbsolutePath -Path $WindeployQt -BasePath $sourceDirectory
$objdumpPath = Get-AbsolutePath -Path $Objdump -BasePath $sourceDirectory
$sqlitePath = Get-AbsolutePath -Path $Sqlite -BasePath $sourceDirectory

Assert-ToolExists -Name "CMake" -Path $cmakePath
Assert-ToolExists -Name "Ninja" -Path $ninjaPath
Assert-ToolExists -Name "MinGW compiler" -Path (Join-Path $mingwDirectory "bin\g++.exe")
Assert-ToolExists -Name "windeployqt6" -Path $windeployQtPath
Assert-ToolExists -Name "objdump" -Path $objdumpPath
Assert-ToolExists -Name "sqlite3" -Path $sqlitePath
Assert-ToolExists -Name "MariaDB backup client runtime (install mingw-w64-x86_64-libmariadbclient)" -Path (Join-Path $mingwDirectory "bin\libmariadb.dll")

$oldPath = $env:PATH
try {
    $env:PATH = (Join-Path $mingwDirectory "bin") + ";" + $env:PATH
    if (-not $SkipBuild) {
        $configureArguments = @(
            "-S", $sourceDirectory,
            "-B", $buildRoot,
            "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_TESTING=OFF",
            "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF",
            ("-DCMAKE_CXX_COMPILER=" + (Join-Path $mingwDirectory "bin\g++.exe")),
            ("-DCMAKE_MAKE_PROGRAM=" + $ninjaPath),
            ("-DCMAKE_PREFIX_PATH=" + $mingwDirectory)
        )
        Invoke-NativeTool `
            -FilePath $cmakePath `
            -Arguments $configureArguments `
            -Description "Configure the dedicated Release package build"
        Invoke-NativeTool `
            -FilePath $cmakePath `
            -Arguments @("--build", $buildRoot, "--target", "SignalLab", "--parallel") `
            -Description "Build SignalLab Release"
    }

    $cachePath = Join-Path $buildRoot "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        throw "CMake cache was not found: $cachePath"
    }
    $releaseVersionMatch = Select-String `
        -LiteralPath (Join-Path $sourceDirectory "CMakeLists.txt") `
        -Pattern '^\s*set\(SIGNALLAB_RELEASE_VERSION\s+"?([^"\)]+)"?\)' |
        Select-Object -First 1
    if ($null -ne $releaseVersionMatch) {
        $releaseVersion = $releaseVersionMatch.Matches[0].Groups[1].Value.Trim()
    } else {
        $projectVersionMatch = Select-String `
            -LiteralPath $cachePath `
            -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$' |
            Select-Object -First 1
        if ($null -eq $projectVersionMatch) {
            throw "Cannot determine the SignalLab release version."
        }
        $releaseVersion = $projectVersionMatch.Matches[0].Groups[1].Value.Trim()
    }
    if ($releaseVersion -notmatch '^[0-9A-Za-z][0-9A-Za-z.+-]*$') {
        throw "Unsafe release version for a package name: $releaseVersion"
    }

    $packageName = "SignalLab-$releaseVersion-windows-x64"
    $packageDirectory = Join-Path $distributionRoot $packageName
    $zipPath = Join-Path $distributionRoot ($packageName + ".zip")
    $zipHashPath = $zipPath + ".sha256"
    Assert-SafeDistTarget -Target $packageDirectory -Root $distributionRoot
    Assert-SafeDistTarget -Target $zipPath -Root $distributionRoot
    Assert-SafeDistTarget -Target $zipHashPath -Root $distributionRoot

    New-Item -ItemType Directory -Path $distributionRoot -Force | Out-Null
    foreach ($existingTarget in @($packageDirectory, $zipPath, $zipHashPath)) {
        if ((Test-Path -LiteralPath $existingTarget) -and -not $Force) {
            throw "Distribution target already exists; rerun with -Force to replace it: $existingTarget"
        }
    }

    $stagingRoot = Join-Path $distributionRoot (".staging-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $stagingRoot | Out-Null
    try {
        $releaseExecutable = Join-Path $buildRoot "bin\SignalLab.exe"
        if (-not (Test-Path -LiteralPath $releaseExecutable -PathType Leaf)) {
            throw "Release executable was not found: $releaseExecutable"
        }
        Copy-Item -LiteralPath $releaseExecutable -Destination (Join-Path $stagingRoot "SignalLab.exe")

        Copy-Item -LiteralPath (Join-Path $sourceDirectory "migrations") -Destination $stagingRoot -Recurse
        $stagingData = Join-Path $stagingRoot "data"
        New-Item -ItemType Directory -Path $stagingData | Out-Null
        Copy-Item -LiteralPath (Join-Path $sourceDirectory "data\samples") -Destination $stagingData -Recurse
        Copy-OptionalItem -Source (Join-Path $sourceDirectory "data\templates") -Destination $stagingData
        Copy-Item -LiteralPath (Join-Path $sourceDirectory "README.md") -Destination $stagingRoot
        Copy-Item -LiteralPath (Join-Path $sourceDirectory "THIRD_PARTY_NOTICES.md") -Destination $stagingRoot
        $stagingDocs = Join-Path $stagingRoot "docs"
        New-Item -ItemType Directory -Path $stagingDocs | Out-Null
        foreach ($documentName in @(
            "user-guide.md",
            "release-assets.md",
            "demo-script.md",
            "test-plan.md",
            "stage8-validation.md",
            "algorithms.md",
            "signalscript.md",
            "msys2-package-snapshot.txt"
        )) {
            Copy-Item `
                -LiteralPath (Join-Path $sourceDirectory "docs\$documentName") `
                -Destination $stagingDocs
        }
        $stagingAdr = Join-Path $stagingDocs "adr"
        New-Item -ItemType Directory -Path $stagingAdr | Out-Null
        Copy-Item `
            -LiteralPath (Join-Path $sourceDirectory "docs\adr\0005-clion-bundled-gdb.md") `
            -Destination $stagingAdr
        Copy-OptionalItem -Source (Join-Path $sourceDirectory "LICENSE") -Destination $stagingRoot
        Copy-OptionalItem -Source (Join-Path $sourceDirectory "licenses") -Destination $stagingRoot
        Copy-OptionalItem -Source (Join-Path $sourceDirectory "config") -Destination $stagingRoot

        $deployArguments = @(
            "--release",
            "--compiler-runtime",
            "--no-translations",
            "--dir", $stagingRoot,
            "--add-plugin-types", "imageformats",
            "--include-plugins", "qwindows,qoffscreen,qsqlite,qgif,qico,qjpeg",
            "--exclude-plugins", "qdirect2d,qminimal,qsqlmysql,qsqlibase,qsqlodbc,qsqlpsql,qcertonlybackend,qopensslbackend,qschannelbackend,qglib,qnetworklistmanager,qtuiotouchplugin,qmodernwindowsstyle",
            (Join-Path $stagingRoot "SignalLab.exe")
        )
        Invoke-NativeTool `
            -FilePath $windeployQtPath `
            -Arguments $deployArguments `
            -Description "Deploy Qt, plugins, and compiler runtime" `
            -AllowStderr

        $mysqlPlugin = Get-ChildItem -LiteralPath $stagingRoot -Recurse -File -Filter "qsqlmysql*.dll" |
            Select-Object -First 1
        if ($null -ne $mysqlPlugin) {
            throw "Qt deployment included $($mysqlPlugin.Name); MariaDB backup uses Connector/C directly and must not bundle QMYSQL."
        }

        foreach ($runtimeName in @("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll", "libmariadb.dll")) {
            $runtimeTarget = Join-Path $stagingRoot $runtimeName
            if (-not (Test-Path -LiteralPath $runtimeTarget -PathType Leaf)) {
                Copy-Item `
                    -LiteralPath (Join-Path $mingwDirectory "bin\$runtimeName") `
                    -Destination $runtimeTarget
            }
        }
        Copy-MingwDependencyClosure `
            -PackageRoot $stagingRoot `
            -MingwBin (Join-Path $mingwDirectory "bin") `
            -ObjdumpPath $objdumpPath

        foreach ($mutablePath in @(
            "data\signallab.sqlite3",
            "data\signallab.sqlite3-shm",
            "data\signallab.sqlite3-wal",
            "data\imports",
            "data\script_outputs"
        )) {
            if (Test-Path -LiteralPath (Join-Path $stagingRoot $mutablePath)) {
                throw "Mutable runtime state entered the staging package: $mutablePath"
            }
        }

        $manifestLines = New-Object System.Collections.Generic.List[string]
        $stagedFiles = Get-ChildItem -LiteralPath $stagingRoot -Recurse -File | Sort-Object FullName
        foreach ($file in $stagedFiles) {
            $relativePath = $file.FullName.Substring($stagingRoot.Length).TrimStart('\', '/').Replace('\', '/')
            $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $manifestLines.Add("$hash  $relativePath")
        }
        $manifestPath = Join-Path $stagingRoot "SHA256SUMS.txt"
        [System.IO.File]::WriteAllLines(
            $manifestPath,
            $manifestLines,
            (New-Object System.Text.UTF8Encoding($false)))

        $verifyArguments = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $PSScriptRoot "verify-windows-package.ps1"),
            "-PackageDirectory", $stagingRoot,
            "-Objdump", $objdumpPath,
            "-Sqlite", $sqlitePath
        )
        if ($SkipLaunchCheck) {
            $verifyArguments += "-SkipLaunchCheck"
        }
        Invoke-NativeTool `
            -FilePath "powershell.exe" `
            -Arguments $verifyArguments `
            -Description "Verify the staged Windows package"

        # Keep a previously verified package intact until the replacement has
        # itself passed every check above.
        if (Test-Path -LiteralPath $packageDirectory) {
            Assert-SafeDistTarget -Target $packageDirectory -Root $distributionRoot
            Remove-Item -LiteralPath $packageDirectory -Recurse -Force
        }
        Move-Item -LiteralPath $stagingRoot -Destination $packageDirectory
        if (Test-Path -LiteralPath $zipPath) {
            Assert-SafeDistTarget -Target $zipPath -Root $distributionRoot
            Remove-Item -LiteralPath $zipPath -Force
        }
        if (Test-Path -LiteralPath $zipHashPath) {
            Assert-SafeDistTarget -Target $zipHashPath -Root $distributionRoot
            Remove-Item -LiteralPath $zipHashPath -Force
        }
        Compress-Archive -LiteralPath $packageDirectory -DestinationPath $zipPath -CompressionLevel Optimal

        $zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        [System.IO.File]::WriteAllText(
            $zipHashPath,
            $zipHash + "  " + (Split-Path -Leaf $zipPath) + [Environment]::NewLine,
            (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "Windows release package created."
        Write-Host "Directory: $packageDirectory"
        Write-Host "Archive:   $zipPath"
        Write-Host "Checksum:  $zipHashPath"
        Write-Host "SHA-256:   $zipHash"
    } finally {
        if (Test-Path -LiteralPath $stagingRoot) {
            $resolvedDist = [System.IO.Path]::GetFullPath($distributionRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
            $resolvedStaging = [System.IO.Path]::GetFullPath($stagingRoot)
            if ($resolvedStaging.StartsWith($resolvedDist, [System.StringComparison]::OrdinalIgnoreCase) `
                -and (Split-Path -Leaf $resolvedStaging).StartsWith(".staging-")) {
                Remove-Item -LiteralPath $resolvedStaging -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }
} finally {
    $env:PATH = $oldPath
}
