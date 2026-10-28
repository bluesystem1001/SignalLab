param(
    [string]$OutputDirectory = (Split-Path -Parent $PSScriptRoot),
    [string]$OnlyPage = ''
)
$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture
$script:Scale = 0.01
$script:Height = 10.0
$script:Ink = 'RGB(39,51,69)'
$script:Muted = 'RGB(85,101,120)'
$script:Palette = @{
    blue='RGB(232,241,252)'; green='RGB(233,246,241)';
    purple='RGB(242,237,251)'; orange='RGB(255,244,226)';
    gray='RGB(242,245,248)'; white='RGB(255,255,255)'
}
function Cell($shape,[string]$name,[string]$formula) { $shape.CellsU($name).FormulaU = $formula }
function X([double]$n) { return $n * $script:Scale }
function Y([double]$n) { return $script:Height - $n * $script:Scale }
function Num([double]$n) { return $n.ToString('0.########',$culture) }
function Style($shape,[string]$fill='white',[bool]$border=$true,[double]$weight=1.0) {
    Cell $shape 'FillPattern' $(if($fill -eq 'none'){'0'}else{'1'})
    if($fill -ne 'none') { Cell $shape 'FillForegnd' $script:Palette[$fill] }
    Cell $shape 'LinePattern' $(if($border){'1'}else{'0'})
    Cell $shape 'LineColor' $script:Ink
    Cell $shape 'LineWeight' "$(Num $weight) pt"
    Cell $shape 'ShdwPattern' '0'
}
function Text($shape,[string]$value,[double]$size=11,[bool]$bold=$false,[int]$align=1,[int]$vertical=1) {
    $shape.Text = $value
    Cell $shape 'Char.Font' ([string]$script:FontId)
    Cell $shape 'Char.AsianFont' ([string]$script:FontId)
    Cell $shape 'Char.Size' "$(Num $size) pt"
    Cell $shape 'Char.Color' $script:Ink
    Cell $shape 'Char.Style' $(if($bold){'1'}else{'0'})
    Cell $shape 'Para.HorzAlign' ([string]$align)
    Cell $shape 'VerticalAlign' ([string]$vertical)
    foreach($margin in @('LeftMargin','RightMargin')) { Cell $shape $margin '5 pt' }
    foreach($margin in @('TopMargin','BottomMargin')) { Cell $shape $margin '3 pt' }
}
function Box([double]$x,[double]$y,[double]$w,[double]$h,[string]$value='', [string]$fill='white',[double]$size=11,[bool]$bold=$false,[bool]$border=$true,[int]$align=1) {
    $s = $script:Page.DrawRectangle((X $x),(Y ($y+$h)),(X ($x+$w)),(Y $y))
    Style $s $fill $border
    if($value) { Text $s $value $size $bold $align }
    return $s
}
function Label([double]$x,[double]$y,[double]$w,[double]$h,[string]$value,[double]$size=10.5,[bool]$bold=$false,[int]$align=1,[string]$fill='none') {
    return Box $x $y $w $h $value $fill $size $bold $false $align
}
function Oval([double]$x,[double]$y,[double]$w,[double]$h,[string]$value='', [string]$fill='white',[double]$size=11) {
    $s=$script:Page.DrawOval((X $x),(Y ($y+$h)),(X ($x+$w)),(Y $y))
    Style $s $fill
    if($value) { Text $s $value $size }
    return $s
}
function Line([double]$x1,[double]$y1,[double]$x2,[double]$y2,[bool]$dashed=$false,[double]$weight=1.0) {
    $s=$script:Page.DrawLine((X $x1),(Y $y1),(X $x2),(Y $y2))
    Cell $s 'LineColor' $script:Ink
    Cell $s 'LineWeight' "$(Num $weight) pt"
    Cell $s 'BeginArrow' '0'; Cell $s 'EndArrow' '0'
    if($dashed) {Cell $s 'LinePattern' '2'}
    return $s
}
function Poly($points,[bool]$closed=$false,[string]$fill='none',[bool]$oneD=$false) {
    $coords=New-Object 'System.Collections.Generic.List[double]'
    foreach($p in $points) { $coords.Add((X $p[0])); $coords.Add((Y $p[1])) }
    $flags=0; if($oneD) {$flags=8}
    $s=$script:Page.DrawPolyline($coords.ToArray(),$flags)
    Style $s $fill
    Cell $s 'BeginArrow' '0'; Cell $s 'EndArrow' '0'
    return $s
}
function Group-Shapes($items,[string]$name) {
    $selection=$script:Page.CreateSelection(0,0)
    foreach($s in $items) {$selection.Select($s,2)}
    $g=$selection.Group()
    $g.NameU=$name
    return $g
}
function Draw-Node($n) {
    $x=[double]$n.x; $y=[double]$n.y; $w=[double]$n.w; $h=[double]$n.h
    $fill='white'; if($n.fill){$fill=$n.fill}
    $parts=[Collections.Generic.List[object]]::new()
    switch($n.type) {
        'class' {
            $parts.Add((Box $x $y $w $h '' 'white'))
            $hh=52.0; if($n.text -match "`n"){$hh=64.0}
            $parts.Add((Box $x $y $w $hh $n.text $fill 11.5 $true))
            $sections=@($n.sections)
            if($sections.Count -gt 0) {
                $counts=@($sections | ForEach-Object { @($_ -split "`n").Count })
                $total=($counts | Measure-Object -Sum).Sum
                $cy=$y+$hh
                for($i=0;$i -lt $sections.Count;$i++) {
                    $sh=($h-$hh)*$counts[$i]/$total
                    if($i -gt 0){$parts.Add((Line $x $cy ($x+$w) $cy))}
                    $t=Label ($x+3) ($cy+2) ($w-6) ($sh-4) $sections[$i] 10.4 $false 0
                    Cell $t 'VerticalAlign' '0'
                    $parts.Add($t)
                    $cy+=$sh
                }
            }
            $shape=Group-Shapes $parts ('UML_Class_'+$n.id)
        }
        'actor' {
            $cx=$x+$w/2; $headY=$y+3; $k=($h-38)/100
            $parts.Add((Oval ($cx-12*$k) $headY (24*$k) (24*$k)))
            $parts.Add((Line $cx ($headY+24*$k) $cx ($headY+66*$k)))
            $parts.Add((Line ($cx-31*$k) ($headY+39*$k) ($cx+31*$k) ($headY+39*$k)))
            $parts.Add((Line $cx ($headY+66*$k) ($cx-27*$k) ($headY+96*$k)))
            $parts.Add((Line $cx ($headY+66*$k) ($cx+27*$k) ($headY+96*$k)))
            $parts.Add((Label $x ($y+$h-36) $w 38 $n.text 10.5 $false))
            $shape=Group-Shapes $parts ('UML_Actor_'+$n.id)
        }
        'diamond' {
            $shape=Poly @(@(($x+$w/2),$y),@(($x+$w),($y+$h/2)),@(($x+$w/2),($y+$h)),@($x,($y+$h/2)),@(($x+$w/2),$y)) $true $fill
            if($n.text){Text $shape $n.text 10.5}
        }
        'ellipse' {$shape=Oval $x $y $w $h $n.text $fill 11.5}
        'start' {$shape=Oval $x $y $w $h '' 'gray'; Cell $shape 'FillForegnd' $script:Ink}
        'end' {
            $parts.Add((Oval $x $y $w $h))
            $inner=Oval ($x+5) ($y+5) ($w-10) ($h-10) '' 'gray'
            Cell $inner 'FillForegnd' $script:Ink; $parts.Add($inner)
            $shape=Group-Shapes $parts ('UML_Final_'+$n.id)
        }
        'lifeline' {
            $parts.Add((Box $x $y $w $h $n.text $fill 9.5 $true))
            $parts.Add((Line ($x+$w/2) ($y+$h) ($x+$w/2) 900 $true 0.8))
            $shape=Group-Shapes $parts ('UML_Lifeline_'+$n.id)
        }
        'frame' {
            $parts.Add((Box $x $y $w $h '' 'none' 11 $false))
            $parts.Add((Label ($x+8) ($y+3) ($w-16) 30 $n.text 10.7 $true 0))
            $shape=Group-Shapes $parts ('UML_Frame_'+$n.id)
        }
        'note' {
            if($h -lt 50) {
                $shape=Label $x $y $w $h $n.text 9.3 $false 0 'orange'
                Cell $shape 'TopMargin' '1 pt'; Cell $shape 'BottomMargin' '1 pt'
                break
            }
            $shape=Poly @(@($x,$y),@(($x+$w-18),$y),@(($x+$w),($y+18)),@(($x+$w),($y+$h)),@($x,($y+$h)),@($x,$y)) $true 'orange'
            $parts.Add($shape)
            $parts.Add((Line ($x+$w-18) $y ($x+$w-18) ($y+18)))
            $parts.Add((Line ($x+$w-18) ($y+18) ($x+$w) ($y+18)))
            $parts.Add((Label ($x+5) ($y+9) ($w-12) ($h-15) $n.text 10.5 $false 0))
            $shape=Group-Shapes $parts ('UML_Note_'+$n.id)
        }
        'artifact' {
            $parts.Add((Poly @(@($x,$y),@(($x+$w-18),$y),@(($x+$w),($y+18)),@(($x+$w),($y+$h)),@($x,($y+$h)),@($x,$y)) $true $fill))
            $parts.Add((Line ($x+$w-18) $y ($x+$w-18) ($y+18)))
            $parts.Add((Line ($x+$w-18) ($y+18) ($x+$w) ($y+18)))
            $parts.Add((Label ($x+5) ($y+7) ($w-12) ($h-12) $n.text 11 $false))
            $shape=Group-Shapes $parts ('UML_Artifact_'+$n.id)
        }
        'component' {
            $parts.Add((Box $x $y $w $h $n.text $fill 11 $false))
            $parts.Add((Box ($x+$w-30) ($y+9) 20 23 '' 'white'))
            $parts.Add((Box ($x+$w-36) ($y+12) 12 5 '' 'white'))
            $parts.Add((Box ($x+$w-36) ($y+24) 12 5 '' 'white'))
            $shape=Group-Shapes $parts ('UML_Component_'+$n.id)
        }
        'device' {
            $parts.Add((Poly @(@($x,($y+14)),@(($x+20),$y),@(($x+$w),$y),@(($x+$w),($y+$h-14)),@(($x+$w-20),($y+$h)),@($x,($y+$h)),@($x,($y+14))) $true $fill))
            $parts.Add((Line $x ($y+14) ($x+$w-20) ($y+14)))
            $parts.Add((Line ($x+$w-20) ($y+14) ($x+$w) $y))
            $parts.Add((Line ($x+$w-20) ($y+14) ($x+$w-20) ($y+$h)))
            $parts.Add((Label ($x+5) ($y+20) ($w-30) ($h-26) $n.text 11 $false))
            $shape=Group-Shapes $parts ('UML_Device_'+$n.id)
        }
        'package' {
            $parts.Add((Box $x $y ($w*0.45) 25 '' $fill))
            $parts.Add((Box $x ($y+25) $w ($h-25) $n.text $fill 11 $true))
            $shape=Group-Shapes $parts ('UML_Package_'+$n.id)
        }
        default {$shape=Box $x $y $w $h $n.text $fill 11.5 $false; Cell $shape 'Rounding' '0.055 in'}
    }
    $shape.NameU='UML_'+$n.id
    return $shape
}
function Anchor($n,[string]$side) {
    switch($side) {
        'L' {return @([double]$n.x,([double]$n.y+[double]$n.h/2))}
        'R' {return @(([double]$n.x+[double]$n.w),([double]$n.y+[double]$n.h/2))}
        'T' {return @(([double]$n.x+[double]$n.w/2),[double]$n.y)}
        default {return @(([double]$n.x+[double]$n.w/2),([double]$n.y+[double]$n.h))}
    }
}
function Arrow($tip,$neighbor,[string]$type) {
    $dx=[double]$tip[0]-[double]$neighbor[0]; $dy=[double]$tip[1]-[double]$neighbor[1]
    $len=[math]::Sqrt($dx*$dx+$dy*$dy); if($len -lt 0.01){return}
    $ux=$dx/$len; $uy=$dy/$len; $size=11.0; $half=5.5
    if($type -eq 'realization'){$size=19;$half=9}
    if($type -eq 'composition'){
        $back=@(($tip[0]-$ux*22),($tip[1]-$uy*22))
        $left=@(($tip[0]-$ux*11-$uy*7),($tip[1]-$uy*11+$ux*7))
        $right=@(($tip[0]-$ux*11+$uy*7),($tip[1]-$uy*11-$ux*7))
        $s=Poly @($tip,$left,$back,$right,$tip) $true 'gray'; Cell $s 'FillForegnd' $script:Ink
        return $s
    } else {
        $a=@(($tip[0]-$ux*$size-$uy*$half),($tip[1]-$uy*$size+$ux*$half))
        $b=@(($tip[0]-$ux*$size+$uy*$half),($tip[1]-$uy*$size-$ux*$half))
        if($type -in @('realization','message','flow')) {
            $fill='white'; if($type -ne 'realization'){$fill='gray'}
            $s=Poly @($tip,$a,$b,$tip) $true $fill
            if($type -ne 'realization'){Cell $s 'FillForegnd' $script:Ink}
        } else {Line $a[0] $a[1] $tip[0] $tip[1] | Out-Null; Line $b[0] $b[1] $tip[0] $tip[1] | Out-Null}
    }
}
function Draw-Edge($e,$nodes,$shapes,[int]$index) {
    $points=@($e.points)
    if(!$e.points) {
        $fs='R'; $ts='L'; if($e.fromSide){$fs=$e.fromSide}; if($e.toSide){$ts=$e.toSide}
        $points=@((Anchor $nodes[$e.from] $fs),(Anchor $nodes[$e.to] $ts))
    }
    if($points.Count -lt 2){throw 'Connector requires at least two points'}
    $s=Poly $points $false 'none' $true
    $s.NameU='UML_Connector_'+$index
    if($e.style -in @('dependency','realization','return')){Cell $s 'LinePattern' '2'}
    if($e.from -and $e.to) {
        # Glue to proportional positions on the group, preserving explicit route vertices.
        $src=$nodes[$e.from]; $dst=$nodes[$e.to]
        $sx=([double]$points[0][0]-[double]$src.x)/[double]$src.w
        $sy=1-([double]$points[0][1]-[double]$src.y)/[double]$src.h
        $tx=([double]$points[-1][0]-[double]$dst.x)/[double]$dst.w
        $ty=1-([double]$points[-1][1]-[double]$dst.y)/[double]$dst.h
        $s.CellsU('BeginX').GlueToPos($shapes[$e.from],$sx,$sy)
        $s.CellsU('EndX').GlueToPos($shapes[$e.to],$tx,$ty)
    }
    Cell $s 'EndArrowSize' '2'
    switch($e.style) {
        'composition' {
            $diamond=Arrow $points[0] $points[1] 'composition'
            $baseAngle=[math]::Atan2(-($points[-1][1]-$points[0][1]),($points[-1][0]-$points[0][0]))
            $firstAngle=[math]::Atan2(-($points[1][1]-$points[0][1]),($points[1][0]-$points[0][0]))
            $angleDelta=$firstAngle-$baseAngle
            $ref='Sheet.'+$s.ID+'!'
            Cell $diamond 'PinX' ('='+$ref+'BeginX+COS('+$ref+'Angle+'+(Num $angleDelta)+')*0.11 in')
            Cell $diamond 'PinY' ('='+$ref+'BeginY+SIN('+$ref+'Angle+'+(Num $angleDelta)+')*0.11 in')
            Cell $diamond 'Angle' ('='+$ref+'Angle-'+(Num $baseAngle))
        }
        'realization' {Cell $s 'EndArrow' '14'; Cell $s 'EndArrowSize' '3'}
        'message' {Cell $s 'EndArrow' '4'}
        'flow' {Cell $s 'EndArrow' '4'}
        'dependency' {Cell $s 'EndArrow' '1'}
        'async' {Cell $s 'EndArrow' '1'}
        'return' {Cell $s 'EndArrow' '1'}
    }
    if($e.label) {
        $b=$e.labelAt
        if(!$b){$b=@((($points[0][0]+$points[-1][0])/2-100),(($points[0][1]+$points[-1][1])/2-28),200,26)}
        if($b[3] -lt 35 -and $e.label -notmatch "`n") {
            $units=0.0
            foreach($c in $e.label.ToCharArray()) {if([int]$c -gt 255){$units+=1.0}else{$units+=0.54}}
            $need=[math]::Min(580,($units*10.2+10)/0.72)
            if($need -gt $b[2]) {
                $b=@([math]::Max(50,[math]::Min(1550-$need,($b[0]+$b[2]/2-$need/2))),$b[1],$need,$b[3])
            }
        }
        $label=Label $b[0] $b[1] $b[2] $b[3] $e.label 10.2 $false 1 'white'
        Cell $label 'TopMargin' '1 pt'; Cell $label 'BottomMargin' '1 pt'
    }
    foreach($end in @('source','target')) {
        $val=$e.($end+'Multiplicity')
        if($val) {
            $p=$points[0]; $q=$points[1]
            if($end -eq 'target'){$p=$points[-1];$q=$points[-2]}
            $dx=[double]$q[0]-[double]$p[0];$dy=[double]$q[1]-[double]$p[1]
            $len=[math]::Sqrt($dx*$dx+$dy*$dy)
            $override=$e.($end+'MultiplicityAt')
            if($override) {
                $mult=Label $override[0] $override[1] $override[2] $override[3] $val 10 $false 1 'white'
            } elseif([math]::Abs($dx) -ge [math]::Abs($dy)) {
                $mult=Label ($p[0]+$dx/$len*27-22) ($p[1]-40) 44 24 $val 10 $false 1 'white'
            } else {
                $mult=Label ($p[0]+12) ($p[1]+$dy/$len*27-12) 56 24 $val 10 $false 1 'white'
            }
            Cell $mult 'TopMargin' '1 pt'; Cell $mult 'BottomMargin' '1 pt'
        }
    }
    $s.SendToBack() | Out-Null
}

$visio=$null; $doc=$null
$out=[IO.Path]::GetFullPath($OutputDirectory)
$target=Join-Path $out 'SignalLab_UML.vsdx'
$preview=Join-Path $out 'previews'
New-Item -ItemType Directory -Path $out,$preview -Force | Out-Null
if(Test-Path -LiteralPath $target) {
    $backupDir=Join-Path $out 'sources\backups'
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    $backup=Join-Path $backupDir ('SignalLab_UML.backup-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.vsdx')
    Copy-Item -LiteralPath $target -Destination $backup
}
try {
    $visio=New-Object -ComObject Visio.Application
    $visio.Visible=$false; $visio.AlertResponse=7
    $visio.ScreenUpdating=$false
    $doc=$visio.Documents.Add('')
    $script:FontId=$doc.Fonts.Item('Microsoft YaHei UI').ID
    $doc.Title='SignalLab 软件工程 UML 设计视图'
    $doc.Subject='基于当前工作区代码的 UML 结构与行为模型'
    $pages=@()
    foreach($f in @('guide_pages.json','structure_pages.json','class_pages.json','behavior_pages.json','service_pages.json','extension_pages.json')) {
        $inputFile=Join-Path $PSScriptRoot $f
        if(Test-Path -LiteralPath $inputFile) {$pages+=Get-Content -LiteralPath $inputFile -Raw -Encoding UTF8 | ConvertFrom-Json}
    }
    $pages=@($pages | Sort-Object name)
    if($OnlyPage){$pages=@($pages | Where-Object name -eq $OnlyPage)}
    $manifest=@();$i=0
    foreach($spec in $pages) {
        if($i -eq 0){$script:Page=$doc.Pages.Item(1)}else{$script:Page=$doc.Pages.Add()}
        $script:Page.Name=$spec.name
        Cell $script:Page.PageSheet 'PageWidth' '16 in'
        Cell $script:Page.PageSheet 'PageHeight' '10 in'
        Cell $script:Page.PageSheet 'PrintPageOrientation' '2'
        Label 48 24 1500 48 $spec.title 23 $true 0 | Out-Null
        Label 50 77 1500 29 $spec.subtitle 11 $false 0 | Out-Null
        Line 50 118 1550 118 $false 1.4 | Out-Null
        Line 50 932 1550 932 $false 0.7 | Out-Null
        Label 50 940 1350 45 $spec.footer 8.5 $false 0 | Out-Null
        Label 1400 947 145 30 ('SignalLab | '+('{0:D2}' -f $i)) 9 $true 2 | Out-Null
        $nodes=@{};$shapes=@{}
        foreach($n in $spec.nodes) {$nodes[$n.id]=$n; $shapes[$n.id]=Draw-Node $n}
        $edgeId=0
        foreach($e in $spec.edges) {Draw-Edge $e $nodes $shapes $edgeId; $edgeId++}
        if($i -eq 0){$doc.SaveAs($target) | Out-Null}else{$doc.Save() | Out-Null}
        $png=Join-Path $preview ($spec.name+'.png')
        $svg=Join-Path $preview ($spec.name+'.svg')
        $script:Page.Export($png);$script:Page.Export($svg)
        $manifest+=@{page=$spec.name;nodes=$spec.nodes.Count;edges=$spec.edges.Count;topLevelShapes=$script:Page.Shapes.Count;png=$png;svg=$svg}
        Write-Output ('PAGE '+$spec.name+' | nodes='+$spec.nodes.Count+' edges='+$spec.edges.Count+' shapes='+$script:Page.Shapes.Count)
        $i++
    }
    $doc.Save() | Out-Null
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'verification.json') -Encoding utf8
    Write-Output ('SAVED '+$target)
} finally {
    if($doc){try{$doc.Close()}catch{}}
    if($visio){try{$visio.Quit()}catch{}}
    if($doc){[Runtime.InteropServices.Marshal]::FinalReleaseComObject($doc)|Out-Null}
    if($visio){[Runtime.InteropServices.Marshal]::FinalReleaseComObject($visio)|Out-Null}
    [GC]::Collect();[GC]::WaitForPendingFinalizers()
}
