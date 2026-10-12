<#
  INSTALL19 test double for Bouw-SkywardSword.ps1 (used by test_setup_flow.py via SkyBreak-Setup --build-script).
  Same parameters and the same -VoortgangJson stream, but nothing is extracted or compiled: step 7 counts 40 fake
  objects (resumable: <Werkmap>\fake-done.txt), step 8 lays out the install folder like the real script
  (bin\<ExeNaam>.exe = a copy of $env:FAKE_GAME_EXE, hulp\exenaam.txt, hulp\spelmap.txt, instellingen.toml with
  [nand] root = -NandRoot when new, VERSIE.txt, LEESMIJ.txt, Speel.cmd).
#>
param(
    [string]$Iso, [string]$Uitvoer, [string]$Werkmap, [string]$SpelMap = '', [string]$DolphinTool = '', [string]$Python = '',
    [string]$CMake = '', [string]$Ninja = '', [string]$Bronnen = '', [int]$Taken = 0, [switch]$Opruimen,
    [string]$VoortgangJson = '', [string]$ExeNaam = 'SkywardSwordPC', [string]$NandRoot = '', [string]$Product = ''
)
$ErrorActionPreference = 'Stop'
$klok = [Diagnostics.Stopwatch]::StartNew()
$utf8 = New-Object System.Text.UTF8Encoding($false)
function V([int]$Stap, [string]$Status, [int]$A = -1, [int]$B = -1) {
    $o = [ordered]@{ t = [Math]::Round($klok.Elapsed.TotalSeconds, 2); stap = $Stap; naam = "$Stap. nep"; status = $Status }
    if ($A -ge 0) { $o['a'] = $A; $o['b'] = $B }
    [IO.File]::AppendAllText($VoortgangJson, (($o | ConvertTo-Json -Compress) + "`n"), $utf8)
}
Write-Host "fake-bouw: Iso=$Iso Uitvoer=$Uitvoer NandRoot=$NandRoot ExeNaam=$ExeNaam Product=$Product"
New-Item -ItemType Directory -Force -Path $Werkmap | Out-Null
foreach ($s in 1..6) { V $s 'start'; Start-Sleep -Milliseconds 30; V $s 'klaar' }
V 7 'start'
$doneFile = Join-Path $Werkmap 'fake-done.txt'
$start = 0
if (Test-Path -LiteralPath $doneFile) { $start = [int](Get-Content -LiteralPath $doneFile -TotalCount 1) ; Write-Host "fake-bouw: hervat vanaf $start" }
for ($i = $start + 1; $i -le 40; $i++) {
    Start-Sleep -Milliseconds 120
    Write-Host "[$i/40] Building CXX object CMakeFiles/SkywardSwordPC.dir/output_$i.cpp.obj"
    V 7 'voortgang' $i 40
    Set-Content -LiteralPath $doneFile -Value $i -Encoding ASCII
}
V 7 'klaar'
V 8 'start'
$bin = Join-Path $Uitvoer 'bin'; $hulp = Join-Path $Uitvoer 'hulp'
New-Item -ItemType Directory -Force -Path $bin, $hulp | Out-Null
Copy-Item -LiteralPath $env:FAKE_GAME_EXE -Destination (Join-Path $bin "$ExeNaam.exe") -Force
Set-Content -LiteralPath (Join-Path $hulp 'exenaam.txt') -Value $ExeNaam -Encoding ASCII
if (-not $SpelMap) { Set-Content -LiteralPath (Join-Path $hulp 'spelmap.txt') -Value $Iso -Encoding UTF8 }
$toml = Join-Path $Uitvoer 'instellingen.toml'
if (-not (Test-Path -LiteralPath $toml)) {
    [IO.File]::WriteAllText($toml, "# fake`n[nand]`nroot = `"$($NandRoot.Replace('\','/'))`"`n", $utf8)
}
Set-Content -LiteralPath (Join-Path $Uitvoer 'VERSIE.txt') -Value "$Product - lokaal gebouwd (nep)" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $Uitvoer 'LEESMIJ.txt') -Value 'nep' -Encoding UTF8
Set-Content -LiteralPath (Join-Path $Uitvoer 'Speel.cmd') -Value '@echo nep' -Encoding ASCII
V 8 'klaar'
V 0 'einde'
exit 0
