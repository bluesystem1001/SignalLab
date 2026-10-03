[CmdletBinding()]
param(
    [ValidateRange(0.01, 1440.0)]
    [double]$DurationMinutes = 30.0,

    [ValidateSet('Full', 'Critical', 'Mixed')]
    [string]$Mode = 'Mixed',

    [string]$BuildDirectory = '',

    [string]$OutputDirectory = '',

    [string]$CTestPath = '',

    [ValidateRange(1, 3600)]
    [int]$PerTestTimeoutSeconds = 180,

    [ValidateRange(1, 1000)]
    [int]$FullSuiteInterval = 5,

    [switch]$ContinueAfterFailure
)

$ErrorActionPreference = 'Stop'

function Resolve-ExecutablePath {
    param(
        [string]$RequestedPath,
        [string]$CommandName,
        [string[]]$FallbackPaths
    )

    if ($RequestedPath) {
        $resolvedRequestedPath = [System.IO.Path]::GetFullPath($RequestedPath)
        if (-not (Test-Path -LiteralPath $resolvedRequestedPath -PathType Leaf)) {
            throw "Executable not found: $resolvedRequestedPath"
        }
        return $resolvedRequestedPath
    }

    foreach ($fallbackPath in $FallbackPaths) {
        if (Test-Path -LiteralPath $fallbackPath -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($fallbackPath)
        }
    }

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    throw "Could not find $CommandName. Pass its absolute path explicitly."
}

function Get-CtestInventory {
    param(
        [string]$Executable,
        [string]$BuildPath,
        [string]$TestRegex = ''
    )

    $arguments = @('--test-dir', $BuildPath, '--show-only=json-v1')
    if ($TestRegex) {
        $arguments += @('-R', $TestRegex)
    }

    $jsonText = (& $Executable @arguments 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "CTest discovery failed with exit code $LASTEXITCODE.`n$jsonText"
    }

    try {
        return ($jsonText | ConvertFrom-Json)
    }
    catch {
        throw "CTest did not return valid JSON test metadata.`n$jsonText"
    }
}

function New-ExactTestRegex {
    param([string[]]$TestNames)

    $escapedNames = foreach ($testName in $TestNames) {
        [regex]::Escape($testName)
    }
    return '^(' + ($escapedNames -join '|') + ')$'
}

function Write-IterationRecord {
    param(
        [pscustomobject]$Record,
        [string]$CsvPath
    )

    if (Test-Path -LiteralPath $CsvPath) {
        $Record | Export-Csv -LiteralPath $CsvPath -NoTypeInformation -Append -Encoding UTF8
    }
    else {
        $Record | Export-Csv -LiteralPath $CsvPath -NoTypeInformation -Encoding UTF8
    }
}

function Invoke-CtestRun {
    param(
        [string]$Group,
        [string]$TestRegex,
        [int]$ExpectedTestCount,
        [int]$Sequence,
        [string]$Executable,
        [string]$BuildPath,
        [int]$TimeoutSeconds,
        [string]$LogPath,
        [string]$CsvPath
    )

    $arguments = @(
        '--test-dir', $BuildPath,
        '--output-on-failure',
        '--timeout', $TimeoutSeconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    )
    if ($TestRegex) {
        $arguments += @('-R', $TestRegex)
    }

    $startedUtc = [DateTimeOffset]::UtcNow
    $heading = "`n=== run $Sequence | $Group | $($startedUtc.ToString('o')) | expected tests: $ExpectedTestCount ==="
    $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
    $logWriter = [System.IO.StreamWriter]::new(
        $LogPath,
        $true,
        $utf8WithoutBom)
    $logWriter.WriteLine($heading)
    $logWriter.Flush()
    Write-Host $heading
    Write-Host "[$Sequence] Running $Group ($ExpectedTestCount tests)..."

    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    try {
        & $Executable @arguments 2>&1 | ForEach-Object {
            $line = $_.ToString()
            $logWriter.WriteLine($line)
            Write-Host $line
        }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $logWriter.Dispose()
    }
    $stopwatch.Stop()

    $record = [pscustomobject][ordered]@{
        sequence = $Sequence
        group = $Group
        expected_tests = $ExpectedTestCount
        started_utc = $startedUtc.ToString('o')
        duration_seconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
        exit_code = $exitCode
        passed = ($exitCode -eq 0)
    }
    Write-IterationRecord -Record $record -CsvPath $CsvPath
    return $record
}

$faultInjectionTests = @(
    'ThreadPoolTest.ContainsAnExceptionAndContinuesWithTheNextItem',
    'SchedulerTest.TenTasksCanCancelTwoRunningAndOnePending',
    'SchedulerTest.RunningTaskObservesCancellationWithinTwoHundredMilliseconds',
    'SchedulerTest.RetryCreatesANewIdAfterAnException',
    'SchedulerTest.ShutdownCancelsRunningAndPendingTasksAndIsIdempotent',
    'SchedulerTest.ConcurrentWorkerObserversCanRequestShutdown',
    'SchedulerTest.ConcurrentPendingObserversCanRequestShutdown',
    'SchedulerTest.ConcurrentRetryAndRemoveHaveALinearizedOutcome',
    'Radix2FftTest.NoticesCancellationDuringALargeTransform',
    'FirFilterTest.NoticesCancellationWhileFiltering',
    'ScriptFileGatewayTest.ConcurrentWritersPublishExactlyOneFile',
    'ScriptFileGatewayTest.CancellationLeavesNoOutputOrTemporaryFile',
    'FilesystemFileRepositoryTest.CancelsAnImportAndRemovesItsSnapshot',
    'ImportServiceTest.InvalidCsvDoesNotLeaveAStoredOrTemporaryFile',
    'ImportServiceTest.DatabaseFailureRemovesANewArchive',
    'ImportServiceTest.KeepsANewArchiveWhenReferencesCannotBeChecked',
    'ImportServiceTest.RejectsADamagedContentAddressedFile',
    'ImportServiceTest.ReportsArchiveCleanupFailureWithoutUndoingCommittedDeletion',
    'BackgroundAnalysisServiceTest.TenTasksCancelThreeWithoutFeaturesAndCompleteTheOtherSeven',
    'ScriptExecutionIntegrationTest.ConvertsUnexpectedRuntimeExceptionsToTerminalHistory',
    'ScriptExecutionIntegrationTest.RejectsUnsafeOutputPathsWithoutPublishingFilesAndRecordsFailures',
    'ScriptExecutionIntegrationTest.MapsARequestedStopToCancelledAndCompletesTheHistory',
    'SqliteTaskRepositoryTest.RollsBackPartialFeaturesAndMarksTaskFailed',
    'SqliteTaskRepositoryTest.RollsBackTaskDeletionWhenAuditLoggingFails',
    'SqliteSignalRepositoryTest.RollsBackEveryImportTableOnRegistrationFailure',
    'SqliteSignalRepositoryTest.RollsBackDeletionWhenAuditLoggingFails',
    'MigrationRunnerTest.RollsBackAFileWhenOneStatementFails',
    'SqliteScriptRepositoryTest.RollsBackFinishWhenLogCannotBeWritten',
    'DatabaseExecutorTest.CancelsAndDrainsALargeSignalLookupPromptly',
    'Stage5GuiRuntimeTest.AppShutdownOrderCancelsJoinsAndFlushesRunningWork'
)

$criticalSuitePatterns = @(
    'SchedulerTest\..+',
    'BackgroundFileServiceTest\..+',
    'BackgroundAnalysisServiceTest\..+',
    'BackgroundScriptServiceTest\..+',
    'ScriptExecutionIntegrationTest\..+',
    'Stage5GuiRuntimeTest\..+',
    'Stage7GuiRuntimeTest\..+'
)

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $repositoryRoot 'cmake-build-release-mingw64'
}
$buildPath = [System.IO.Path]::GetFullPath($BuildDirectory)
if (-not (Test-Path -LiteralPath $buildPath -PathType Container)) {
    throw "Build directory not found: $buildPath"
}
if (-not (Test-Path -LiteralPath (Join-Path $buildPath 'CTestTestfile.cmake') -PathType Leaf)) {
    throw "The build directory has no CTest metadata: $buildPath"
}

$ctestExecutable = Resolve-ExecutablePath `
    -RequestedPath $CTestPath `
    -CommandName 'ctest' `
    -FallbackPaths @(
        'E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\ctest.exe'
    )

if (-not $OutputDirectory) {
    $timestamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
    $OutputDirectory = Join-Path $buildPath "stage8-stability\$timestamp"
}
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

$logPath = Join-Path $outputPath 'ctest.log'
$iterationsPath = Join-Path $outputPath 'iterations.csv'
$summaryJsonPath = Join-Path $outputPath 'summary.json'
$summaryTextPath = Join-Path $outputPath 'summary.txt'

$originalPath = $env:Path
$originalQtPlatform = $env:QT_QPA_PLATFORM
$originalQtOpenGl = $env:QT_OPENGL
$mingwBin = 'E:\MSYS2\mingw64\bin'
if (Test-Path -LiteralPath $mingwBin -PathType Container) {
    $env:Path = $mingwBin + ';' + $env:Path
}
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_OPENGL = 'software'

$records = [Collections.Generic.List[object]]::new()
$infrastructureError = ''
$runSequence = 0
$requestedSeconds = $DurationMinutes * 60.0
$startedUtc = [DateTimeOffset]::UtcNow
$deadlineUtc = $startedUtc.AddSeconds($requestedSeconds)
$faultRegex = New-ExactTestRegex -TestNames $faultInjectionTests

try {
    $fullInventory = Get-CtestInventory `
        -Executable $ctestExecutable `
        -BuildPath $buildPath
    $fullTestCount = @($fullInventory.tests).Count
    if ($fullTestCount -lt 30) {
        throw "Only $fullTestCount tests were discovered; Stage 8 requires at least 30."
    }

    $faultInventory = Get-CtestInventory `
        -Executable $ctestExecutable `
        -BuildPath $buildPath `
        -TestRegex $faultRegex
    $discoveredFaultTests = @($faultInventory.tests | ForEach-Object { $_.name })
    $missingFaultTests = @($faultInjectionTests | Where-Object {
        $discoveredFaultTests -notcontains $_
    })
    if ($missingFaultTests.Count -gt 0) {
        throw "Fault-injection tests missing from CTest: $($missingFaultTests -join ', ')"
    }

    $criticalRegexParts = @($faultRegex.Substring(1, $faultRegex.Length - 2))
    $criticalRegexParts += $criticalSuitePatterns
    $criticalRegex = '^(' + ($criticalRegexParts -join '|') + ')$'
    $criticalInventory = Get-CtestInventory `
        -Executable $ctestExecutable `
        -BuildPath $buildPath `
        -TestRegex $criticalRegex
    $criticalTestCount = @($criticalInventory.tests).Count
    if ($criticalTestCount -lt $faultInjectionTests.Count) {
        throw 'Critical test discovery unexpectedly omitted fault-injection tests.'
    }

    $logHeader = @(
        "SignalLab Stage 8 stability run",
        "started_utc: $($startedUtc.ToString('o'))",
        "requested_duration_seconds: $requestedSeconds",
        "mode: $Mode",
        "build_directory: $buildPath",
        "ctest: $ctestExecutable",
        "all_tests: $fullTestCount",
        "fault_injection_tests: $($faultInjectionTests.Count)",
        "critical_tests: $criticalTestCount"
    )
    [System.IO.File]::WriteAllLines(
        $logPath,
        $logHeader,
        [System.Text.UTF8Encoding]::new($false))

    ++$runSequence
    $faultRecord = Invoke-CtestRun `
        -Group 'fault-injection-gate' `
        -TestRegex $faultRegex `
        -ExpectedTestCount $faultInjectionTests.Count `
        -Sequence $runSequence `
        -Executable $ctestExecutable `
        -BuildPath $buildPath `
        -TimeoutSeconds $PerTestTimeoutSeconds `
        -LogPath $logPath `
        -CsvPath $iterationsPath
    $records.Add($faultRecord)

    $failed = -not $faultRecord.passed
    $soakRun = 0
    while ((-not $failed -or $ContinueAfterFailure) -and
           ($soakRun -eq 0 -or [DateTimeOffset]::UtcNow -lt $deadlineUtc)) {
        ++$soakRun
        ++$runSequence

        $runGroup = 'critical'
        $runRegex = $criticalRegex
        $runTestCount = $criticalTestCount
        if ($Mode -eq 'Full' -or
            ($Mode -eq 'Mixed' -and
             ($soakRun -eq 1 -or (($soakRun - 1) % $FullSuiteInterval) -eq 0))) {
            $runGroup = 'full'
            $runRegex = ''
            $runTestCount = $fullTestCount
        }

        $record = Invoke-CtestRun `
            -Group $runGroup `
            -TestRegex $runRegex `
            -ExpectedTestCount $runTestCount `
            -Sequence $runSequence `
            -Executable $ctestExecutable `
            -BuildPath $buildPath `
            -TimeoutSeconds $PerTestTimeoutSeconds `
            -LogPath $logPath `
            -CsvPath $iterationsPath
        $records.Add($record)
        if (-not $record.passed) {
            $failed = $true
        }
    }
}
catch {
    $infrastructureError = $_.Exception.Message
    Add-Content -LiteralPath $logPath -Value "`nINFRASTRUCTURE ERROR: $infrastructureError" -Encoding UTF8
}
finally {
    $env:Path = $originalPath
    $env:QT_QPA_PLATFORM = $originalQtPlatform
    $env:QT_OPENGL = $originalQtOpenGl
}

$finishedUtc = [DateTimeOffset]::UtcNow
$failedRuns = @($records | Where-Object { -not $_.passed }).Count
$passed = (-not $infrastructureError) -and $failedRuns -eq 0
$summary = [pscustomobject][ordered]@{
    schema_version = 1
    result = $(if ($passed) { 'PASS' } else { 'FAIL' })
    mode = $Mode
    requested_duration_seconds = [Math]::Round($requestedSeconds, 3)
    actual_duration_seconds = [Math]::Round(($finishedUtc - $startedUtc).TotalSeconds, 3)
    started_utc = $startedUtc.ToString('o')
    finished_utc = $finishedUtc.ToString('o')
    build_directory = $buildPath
    ctest = $ctestExecutable
    total_runs = $records.Count
    full_runs = @($records | Where-Object { $_.group -eq 'full' }).Count
    critical_runs = @($records | Where-Object { $_.group -eq 'critical' }).Count
    fault_injection_runs = @($records | Where-Object { $_.group -eq 'fault-injection-gate' }).Count
    failed_runs = $failedRuns
    infrastructure_error = $infrastructureError
    fault_injection_tests = $faultInjectionTests
    artifacts = [ordered]@{
        ctest_log = $logPath
        iterations_csv = $iterationsPath
    }
}

$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $summaryJsonPath -Encoding UTF8
@(
    "result: $($summary.result)",
    "mode: $Mode",
    "requested_duration_seconds: $($summary.requested_duration_seconds)",
    "actual_duration_seconds: $($summary.actual_duration_seconds)",
    "total_runs: $($summary.total_runs)",
    "full_runs: $($summary.full_runs)",
    "critical_runs: $($summary.critical_runs)",
    "fault_injection_runs: $($summary.fault_injection_runs)",
    "failed_runs: $failedRuns",
    "infrastructure_error: $infrastructureError",
    "ctest_log: $logPath",
    "iterations_csv: $iterationsPath",
    "summary_json: $summaryJsonPath"
) | Set-Content -LiteralPath $summaryTextPath -Encoding UTF8

Write-Host "Stage 8 stability result: $($summary.result)"
Write-Host "Summary: $summaryTextPath"

if (-not $passed) {
    exit 1
}
