param([string]$Path=(Join-Path (Split-Path -Parent $PSScriptRoot) 'SignalLab_UML.vsdx'))
$ErrorActionPreference='Stop'
$Path=[IO.Path]::GetFullPath($Path)
$hashBefore=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
$app=$null;$document=$null
try {
    $app=New-Object -ComObject Visio.Application
    $app.Visible=$false;$app.AlertResponse=7
    $document=$app.Documents.Open($Path)
    if($document.Pages.Count -ne 15){throw 'Expected 15 pages'}
    $page=$document.Pages.Item('03领域与导入类')
    $node=$page.Shapes.ItemU('UML_import')
    $connector=$page.Shapes.ItemU('UML_Connector_2')
    $beginBefore=[double]$connector.CellsU('BeginX').ResultIU
    $diamond=$page.Shapes.ItemFromID($connector.ID+1)
    if($diamond.CellsU('PinX').FormulaU -notmatch 'BeginX\+COS') {throw 'Expected formula-linked composition diamond'}
    $diamondBefore=[double]$diamond.CellsU('PinX').ResultIU
    $node.CellsU('PinX').ResultIU=[double]$node.CellsU('PinX').ResultIU+0.25
    $beginAfter=[double]$connector.CellsU('BeginX').ResultIU
    $diamondAfter=[double]$diamond.CellsU('PinX').ResultIU
    if([math]::Abs(($beginAfter-$beginBefore)-0.25) -gt 0.001){throw 'Glued connector did not follow node'}
    # A single moved endpoint also changes connector angle, slightly rotating the diamond.
    if([math]::Abs(($diamondAfter-$diamondBefore)-0.25) -gt 0.005){throw 'Composition diamond did not follow connector'}
    $report=@{
        reopenedSuccessfully=$true;pages=$document.Pages.Count
        groupedNodeMovedInMemory=$true;moveInches=0.25
        gluedConnectorFollowed=$true;connectorMoveInches=($beginAfter-$beginBefore)
        compositionDiamondFollowed=$true;diamondMoveInches=($diamondAfter-$diamondBefore)
        testEditsDiscarded=$true
    }
    $document.Saved=$true
} finally {
    if($document){$document.Close() | Out-Null}
    if($app){$app.Quit() | Out-Null}
    [GC]::Collect();[GC]::WaitForPendingFinalizers()
}
$hashAfter=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
if($hashBefore -ne $hashAfter){throw 'Verification unexpectedly changed the saved document'}
$report.savedFileUnchanged=$true
$report.sha256=$hashAfter.ToLowerInvariant()
$report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path (Split-Path -Parent $Path) 'editability-verification.json') -Encoding utf8
$report | ConvertTo-Json -Compress
