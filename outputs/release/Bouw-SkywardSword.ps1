<#
.SYNOPSIS
    Bouwt Skyward Sword PC vanaf je EIGEN schijf (The Legend of Zelda: Skyward Sword, VS 1.00 = SOUE01 revisie 0).

.DESCRIPTION
    Een stap voor alles (PKG10, roadmap M7.1/M7.2):
      1. controleert de hulpprogramma's (Visual Studio 2022 C++, CMake, Ninja, Python, DolphinTool);
      2. controleert de schijf (spelcode SOUE01, revisie 0) en na het uitpakken de SHA-1 van sys/main.dol,
         files/rels.arc en alle 636 REL-modules;
      3. pakt de schijf uit met DolphinTool (alleen de DATA-partitie);
      4. bouwt de vertaler (nWiiRecomp) uit de broncode in deze map;
      5. vertaalt main.dol en de REL-modules naar C++;
      6. bouwt SkywardSwordPC.exe met MSVC (CMake + Ninja);
      7. zet alles in een zelfstandige map SkywardSwordPC\ met Speel.cmd, instellingen.toml en LEESMIJ.txt.
    Opnieuw starten gaat sneller: een geldige uitgepakte schijf wordt hergebruikt en de C++-build is incrementeel.
    Er wordt niets gedownload, behalve wanneer CMake de bibliotheken SDL2 en toml++ niet lokaal vindt
    (zie -Bronnen); dat meldt het script vooraf.
    De gebouwde map bevat code en gegevens uit jouw schijf: alleen voor eigen gebruik, niet delen.

.PARAMETER Iso
    Je schijfbestand (.iso, .wbfs, .rvz, .gcz, .wia, .ciso) of een al uitgepakte DATA-partitie (map met sys\ en files\).

.PARAMETER Uitvoer
    Doelmap. Standaard: SkywardSwordPC naast dit script.

.PARAMETER Werkmap
    Map voor tussenbestanden (gegenereerde C++ en de build, ca. 3 GB). Standaard: bouw naast dit script.

.PARAMETER SpelMap
    Waar de uitgepakte spelbestanden (ca. 4 GB) komen. Standaard: <Uitvoer>\game. Bij -Iso <map> wordt die map
    zelf gebruikt (er wordt dan niets gekopieerd).

.PARAMETER Draagbaar
    Opslag (NAND) in de map zelf (gebruikersdata\nand) in plaats van in %APPDATA%\SkywardSwordPC\nand.

.PARAMETER Bronnen
    Map met de broncode van de bibliotheken: tomlplusplus-src en sdl2-src (zoals CMake ze in _deps zet).
    Standaard wordt work\nwii-build\_deps van dit project gebruikt als die bestaat; anders haalt CMake ze op
    van GitHub (git en internet nodig).

.PARAMETER Taken
    Aantal parallelle compileertaken (standaard: aantal processorkernen min 2).

.PARAMETER AlleenControle
    Alleen de hulpprogramma's en de schijf controleren; niets uitpakken of bouwen.

.PARAMETER Opruimen
    Na een geslaagde bouw de werkmap verwijderen (scheelt ca. 3 GB, maar de volgende bouw duurt dan langer).

.PARAMETER VoortgangJson
    INSTALL19 (SkyBreak-Setup): schrijf per stap JSON-regels (start/voortgang/klaar/fout/einde) naar dit bestand.

.PARAMETER ExeNaam
    INSTALL19: naam van de gekopieerde exe in <Uitvoer>\bin (zonder .exe). Standaard SkywardSwordPC.

.PARAMETER NandRoot
    INSTALL19: [nand] root voor instellingen.toml (gaat voor -Draagbaar).

.PARAMETER Product
    INSTALL19: productnaam op de eerste regel van VERSIE.txt (bijvoorbeeld "SkyBreak 0.19.0").

.PARAMETER SluitWacht
    REL22: draait SkyBreak uit de uitvoermap nog (SkyBreak.exe of een exe in bin\) op het moment dat het script de exe of de
    spelmap moet vervangen, dan wacht het script zo veel seconden tot jij het sluit (standaard 3600). Het script sluit nooit
    zelf iets af.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Bouw-SkywardSword.ps1 -Iso "D:\Spellen\Skyward Sword.iso"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Iso,
    [string]$Uitvoer = '',
    [string]$Werkmap = '',
    [string]$SpelMap = '',
    [string]$DolphinTool = '',
    [string]$Python = '',
    [string]$CMake = '',
    [string]$Ninja = '',
    [string]$Bronnen = '',
    [int]$Taken = 0,
    [switch]$Draagbaar,
    [switch]$AlleenControle,
    [switch]$Opruimen,
    # INSTALL19: koppeling met SkyBreak-Setup (standaardwaarden = oud gedrag)
    [string]$VoortgangJson = '',
    [string]$ExeNaam = 'SkywardSwordPC',
    [string]$NandRoot = '',
    [string]$Product = '',
    [int]$SluitWacht = 3600  # REL22
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

# ------------------------------------------------------------------------------------------ vaste gegevens
$GameId = 'SOUE01'
$Revision = 0
$MainDolSha1 = '450a6806f46d59dcf8278db08e06f94865a4b18a'

$releaseDir = $PSScriptRoot
$outputsDir = Split-Path -Parent $releaseDir
$projectRoot = Split-Path -Parent $outputsDir
$nwiiSrc = Join-Path $outputsDir 'nwiirecomp'
$runtimeSrc = Join-Path $nwiiSrc 'nWiiRuntime'
$sigdb = Join-Path $nwiiSrc 'third_party\dolphin-sigdb\totaldb.dsy'
$profileToml = Join-Path $outputsDir 'pc-profile\recomp_config.toml'
$symbolsCsv = Join-Path $outputsDir 'pc-profile\symbols.csv'
$decompCfgDir = Join-Path $outputsDir 'skyward-sword\config\SOUE01'
$helper = Join-Path $releaseDir 'tools\pkg10_release.py'
$templates = Join-Path $releaseDir 'sjablonen'
$freezeSymbols = Join-Path $outputsDir 'pc-tests\freeze8\freeze_symbols.txt'

$stappen = New-Object System.Collections.ArrayList
$totaalKlok = [Diagnostics.Stopwatch]::StartNew()

# ------------------------------------------------------------------------------------------ hulpfuncties
function Fout([string]$Melding) { throw [System.Exception]::new($Melding) }

# INSTALL19: voortgang als JSON-regels voor SkyBreak-Setup (alleen met -VoortgangJson; anders doet dit niets).
$script:HuidigeStap = 0
$script:HuidigeNaam = ''
$script:LaatsteJson = ''
function Voortgang([string]$Status, [int]$A = -1, [int]$B = -1, [string]$Soort = '', [string]$Melding = '') {
    if (-not $VoortgangJson) { return }
    $o = [ordered]@{ t = [Math]::Round($totaalKlok.Elapsed.TotalSeconds, 1); stap = $script:HuidigeStap; naam = $script:HuidigeNaam; status = $Status }
    if ($A -ge 0) { $o['a'] = $A; $o['b'] = $B }
    if ($Soort) { $o['soort'] = $Soort }
    if ($Melding) { $o['msg'] = $Melding }
    try { [IO.File]::AppendAllText($VoortgangJson, (($o | ConvertTo-Json -Compress) + "`n"), (New-Object System.Text.UTF8Encoding($false))) } catch { }
}
# INSTALL19: soort fout voor de foutpagina van Setup (schijf/hulpmiddel/ruimte/bouw/pakket).
function Fout-Soort($Uitzondering) {
    $m = "$($Uitzondering.Message)"
    if ($m -match '^SkyBreak draait nog') { return 'draait' }  # REL22
    $hr = 0; try { $hr = $Uitzondering.HResult } catch { }
    if ($hr -eq -2147024784 -or $hr -eq -2147024857 -or $m -match 'not enough space|disk is full|onvoldoende (schijf)?ruimte') { return 'ruimte' }
    switch ($script:HuidigeStap) {
        0 { if ($m -match '^Onderdeel van het project') { return 'pakket' } return 'schijf' }
        1 { return 'hulpmiddel' }
        2 { return 'schijf' }
        3 { return 'schijf' }
        4 { return 'schijf' }
        default { return 'bouw' }
    }
}

function Volledig-Pad([string]$Pad) {
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Pad).TrimEnd('\')
}

function Quote-Arg([string]$A, [switch]$Cmd) {
    if ($A -eq '') { return '""' }
    # REVINSTALL19: -Cmd (Start-Gelogd, which runs through cmd.exe /c) also quotes & | < > ^, so a path such as C:\Games&Apps\x
    # no longer splits the command. Without -Cmd unchanged: Start-Direct passes operators like '2>&1' and '&&' on purpose.
    if ($A -notmatch $(if ($Cmd) { '[\s"&|<>^]' } else { '[\s"]' })) { return $A }
    $s = [regex]::Replace($A, '(\\*)"', { param($m) $m.Groups[1].Value + $m.Groups[1].Value + '\"' })
    $s = [regex]::Replace($s, '(\\+)$', { param($m) $m.Groups[1].Value + $m.Groups[1].Value })
    return '"' + $s + '"'
}

# Start een programma direct (zonder cmd.exe) en vangt de uitvoer op. Geeft @{Code; Uit; Fout} terug.
function Start-Direct([string]$Exe, [string[]]$Argumenten, [string]$Map = '') {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = (@($Argumenten) | ForEach-Object { Quote-Arg $_ }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    if ($Map) { $psi.WorkingDirectory = $Map }
    $p = [System.Diagnostics.Process]::Start($psi)
    $uit = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit()
    return @{ Code = $p.ExitCode; Uit = $uit.Result; Fout = $err.Result }
}

# Laatste regels van een (mogelijk nog open) logbestand.
function Log-Staart([string]$Log, [int]$Regels = 25) {
    if (-not (Test-Path -LiteralPath $Log)) { return @() }
    $fs = [System.IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
    try {
        $len = $fs.Length
        $start = [Math]::Max(0, $len - 16384)
        [void]$fs.Seek($start, 'Begin')
        $buf = New-Object byte[] ($len - $start)
        [void]$fs.Read($buf, 0, $buf.Length)
    } finally { $fs.Close() }
    $tekst = [System.Text.Encoding]::UTF8.GetString($buf)
    return @($tekst -split "`r?`n" | Where-Object { $_ -ne '' } | Select-Object -Last $Regels)
}

# Start een langlopend programma via cmd.exe met alle uitvoer naar $Log; toont voortgang ("[12/345]").
# Het proces draait met lagere prioriteit, zodat de pc bruikbaar blijft.
function Start-Gelogd([string]$Exe, [string[]]$Argumenten, [string]$Log, [string]$Titel, [string]$Map = '') {
    $regel = (Quote-Arg $Exe -Cmd) + ' ' + ((@($Argumenten) | ForEach-Object { Quote-Arg $_ -Cmd }) -join ' ')  # REVINSTALL19: -Cmd
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    # REVINSTALL19 (continuation): console code page UTF-8 first. cl writes /showIncludes in the console code page and ninja reads
    # it as UTF-8: in a folder with an accented letter (a user name with a trema) every later build rebuilt everything; outside the
    # ANSI code page the second build failed ("ninja: error: FindFirstFileExA(...?...)"). Only this hidden console changes.
    # Test: install19/test_bouw_voortgang.py B6.
    $psi.Arguments = '/d /s /c "chcp 65001 >nul & ' + $regel + ' >> "' + $Log + '" 2>&1"'
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    if ($Map) { $psi.WorkingDirectory = $Map }
    Add-Content -LiteralPath $Log -Value ("`r`n> " + $regel) -Encoding UTF8
    $p = [System.Diagnostics.Process]::Start($psi)
    try { $p.PriorityClass = 'BelowNormal' } catch { }
    $laatste = ''
    while (-not $p.WaitForExit(2000)) {
        $staart = Log-Staart $Log 5
        $voortgang = @($staart | Where-Object { $_ -match '^\[(\d+)/(\d+)\]' } | Select-Object -Last 1)
        if ($voortgang.Count -gt 0 -and $voortgang[0] -match '^\[(\d+)/(\d+)\]') {
            $a = [int]$Matches[1]; $b = [int]$Matches[2]
            $pct = [int](100 * $a / [Math]::Max(1, $b))
            Write-Progress -Activity $Titel -Status "$a van $b" -PercentComplete $pct
            $nu = "$a/$b"
            if ($nu -ne $script:LaatsteJson) { Voortgang 'voortgang' $a $b; $script:LaatsteJson = $nu }  # INSTALL19
            if ($nu -ne $laatste -and ($a % 25 -eq 0 -or $a -eq $b)) { Write-Host "    $Titel`: $a van $b"; $laatste = $nu }
        }
    }
    Write-Progress -Activity $Titel -Completed
    $p.WaitForExit()
    return $p.ExitCode
}

function Stap([string]$Naam, [scriptblock]$Werk) {
    Write-Host ''
    Write-Host ("== " + $Naam) -ForegroundColor Cyan
    if ($Naam -match '^(\d+)\.') { $script:HuidigeStap = [int]$Matches[1] }  # INSTALL19
    $script:HuidigeNaam = $Naam; $script:LaatsteJson = ''; Voortgang 'start'  # INSTALL19
    $klok = [Diagnostics.Stopwatch]::StartNew()
    $opmerking = & $Werk
    $klok.Stop()
    Voortgang 'klaar' -Melding "$opmerking"  # INSTALL19
    [void]$stappen.Add([pscustomobject]@{ Stap = $Naam; Seconden = [Math]::Round($klok.Elapsed.TotalSeconds, 1); Opmerking = "$opmerking" })
    Write-Host ("   klaar in {0:F0} s {1}" -f $klok.Elapsed.TotalSeconds, $(if ($opmerking) { "($opmerking)" } else { '' }))
}

# REL22: draait SkyBreak uit DEZE map? <Map>\SkyBreak.exe of een exe in <Map>\bin, vergeleken op het volledige pad (een
# SkyBreak elders, bijvoorbeeld een andere installatie, telt niet). Alleen opvragen: het script sluit nooit een programma af.
function Draaiende-SkyBreak([string]$Map) {
    $start = Join-Path $Map 'SkyBreak.exe'
    $bin = Join-Path $Map 'bin'
    try {
        $procs = @(Get-CimInstance Win32_Process -ErrorAction Stop | Where-Object { $_.ExecutablePath } |
            ForEach-Object { [pscustomobject]@{ Id = [int]$_.ProcessId; Pad = [string]$_.ExecutablePath } })
    } catch {
        $procs = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path } | ForEach-Object { [pscustomobject]@{ Id = $_.Id; Pad = [string]$_.Path } })
    }
    $lijst = New-Object System.Collections.ArrayList
    foreach ($p in $procs) {
        if ($p.Id -eq $PID) { continue }
        $ouder = Split-Path -Parent $p.Pad
        if ([string]::Equals($p.Pad, $start, [StringComparison]::OrdinalIgnoreCase) -or [string]::Equals($ouder, $bin, [StringComparison]::OrdinalIgnoreCase)) {
            [void]$lijst.Add($p)
        }
    }
    return ,$lijst
}

# REL22: wacht (hoogstens -SluitWacht seconden) tot SkyBreak uit $Map gesloten is; meldt het aan Setup ('wacht') en in de console.
function Wacht-Op-Gesloten([string]$Map) {
    $klok = [Diagnostics.Stopwatch]::StartNew()
    $gemeld = ''
    while ($true) {
        $draait = Draaiende-SkyBreak $Map
        if ($draait.Count -eq 0) {
            if ($gemeld) { Write-Host '   SkyBreak is gesloten; het bouwen gaat verder.' -ForegroundColor Green; Voortgang 'verder' }
            return
        }
        $tekst = (@($draait) | ForEach-Object { "$($_.Pad) (pid $($_.Id))" }) -join '; '
        if ($tekst -ne $gemeld) {
            Write-Host "   SkyBreak draait nog: $tekst" -ForegroundColor Yellow
            Write-Host '   Sluit SkyBreak (het spel en de launcher); het bouwen gaat daarna vanzelf verder. Dit script sluit niets af.' -ForegroundColor Yellow
            Voortgang 'wacht' -Melding $tekst
            $gemeld = $tekst
        }
        if ($klok.Elapsed.TotalSeconds -ge $SluitWacht) {
            Fout "SkyBreak draait nog ($tekst). Sluit SkyBreak en start het bouwen opnieuw: het gaat verder waar het stopte."
        }
        Start-Sleep -Seconds 2
    }
}

function Map-Grootte([string]$Pad) {
    if (-not (Test-Path -LiteralPath $Pad)) { return 0 }
    $som = (Get-ChildItem -LiteralPath $Pad -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
    if ($null -eq $som) { return 0 }
    return [int64]$som
}

function Heeft-Eigenschap($Object, [string]$Naam) { return ($null -ne $Object) -and ($null -ne $Object.PSObject.Properties[$Naam]) }

function CMake-Pad([string]$Pad) { return $Pad.Replace('\', '/') }

function GB([int64]$Bytes) { return ('{0:N2} GB' -f ($Bytes / 1GB)) }

function Python-Helper([string[]]$Argumenten) {
    $r = Start-Direct $script:PythonExe (@($helper) + $Argumenten)
    $json = $null
    $laatste = @($r.Uit -split "`r?`n" | Where-Object { $_.Trim().StartsWith('{') } | Select-Object -Last 1)
    if ($laatste.Count -gt 0) { try { $json = $laatste[0] | ConvertFrom-Json } catch { $json = $null } }
    if ($r.Code -ne 0) {
        if ($json -and (Heeft-Eigenschap $json 'melding')) { Fout $json.melding }
        Fout ("Hulpscript mislukt ($($Argumenten[0])): " + ($r.Uit + $r.Fout).Trim())
    }
    return $json
}

function Zoek-Exe([string]$Opgegeven, [string[]]$Namen, [string[]]$Kandidaten) {
    if ($Opgegeven) {
        if (Test-Path -LiteralPath $Opgegeven -PathType Leaf) { return (Volledig-Pad $Opgegeven) }
        Fout "Opgegeven programma bestaat niet: $Opgegeven"
    }
    foreach ($k in $Kandidaten) { if ($k -and (Test-Path -LiteralPath $k -PathType Leaf)) { return $k } }
    foreach ($n in $Namen) {
        $c = Get-Command $n -CommandType Application -ErrorAction SilentlyContinue | Where-Object { $_.Source -notmatch '\\WindowsApps\\' } | Select-Object -First 1
        if ($c) { return $c.Source }
    }
    return $null
}

function Zoek-SpelRoot([string]$Map) {
    foreach ($kandidaat in @($Map, (Join-Path $Map 'DATA'), (Join-Path $Map 'P-SOUE'))) {
        if (Test-Path -LiteralPath (Join-Path $kandidaat 'sys\main.dol')) { return $kandidaat }
    }
    $dol = Get-ChildItem -LiteralPath $Map -Recurse -Filter main.dol -File -Depth 3 -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dol) { return (Split-Path -Parent (Split-Path -Parent $dol.FullName)) }
    return $null
}

# ------------------------------------------------------------------------------------------ hoofdprogramma
try {
    Write-Host 'Skyward Sword PC - bouwen vanaf je eigen schijf' -ForegroundColor Green
    Write-Host 'De gebouwde map bevat code en gegevens uit JOUW schijf. Alleen voor eigen gebruik: niet delen of uploaden.'

    if (-not $Uitvoer) { $Uitvoer = Join-Path $releaseDir 'SkywardSwordPC' }
    if (-not $Werkmap) { $Werkmap = Join-Path $releaseDir 'bouw' }
    $Uitvoer = Volledig-Pad $Uitvoer
    $Werkmap = Volledig-Pad $Werkmap
    $IsoPad = Volledig-Pad $Iso
    if (-not (Test-Path -LiteralPath $IsoPad)) { Fout "Schijfbestand of map niet gevonden: $IsoPad" }
    $isoIsMap = Test-Path -LiteralPath $IsoPad -PathType Container
    foreach ($nodig in @($runtimeSrc, $profileToml, $symbolsCsv, $sigdb, $helper, $templates, (Join-Path $decompCfgDir 'config.yml'), (Join-Path $nwiiSrc 'CMakeLists.txt'))) {
        if (-not (Test-Path -LiteralPath $nodig)) { Fout "Onderdeel van het project ontbreekt: $nodig`n       Pak de HELE zip uit en laat de mappen bij elkaar: start Bouw-SkywardSword.cmd in de hoofdmap SkywardSwordPC-bouwpakket (niet een losse kopie van de map release)." }
    }
    if ($Taken -le 0) { $Taken = [Math]::Max(1, [Environment]::ProcessorCount - 2) }
    New-Item -ItemType Directory -Force -Path $Werkmap | Out-Null
    $logMap = Join-Path $Werkmap 'logs'
    New-Item -ItemType Directory -Force -Path $logMap | Out-Null
    $stempel = Get-Date -Format 'yyyyMMdd-HHmmss'
    Write-Host "Uitvoer:  $Uitvoer"
    Write-Host "Werkmap:  $Werkmap  (logs in $logMap)"

    # ---------------------------------------------------------------------------- 1. voorwaarden
    Stap '1. Hulpprogramma''s controleren' {
        # Python 3.8+
        $script:PythonExe = Zoek-Exe $Python @('python.exe', 'python3.exe') @((Join-Path $projectRoot 'work\venv\Scripts\python.exe'))
        if (-not $script:PythonExe) {
            $py = Get-Command py.exe -ErrorAction SilentlyContinue
            if ($py) { $r = Start-Direct $py.Source @('-3', '-c', 'import sys;print(sys.executable)'); if ($r.Code -eq 0) { $script:PythonExe = $r.Uit.Trim() } }
        }
        if (-not $script:PythonExe) { Fout 'Python 3 niet gevonden. Installeer Python 3.10 of nieuwer (python.org) of geef -Python <pad\python.exe>.' }
        $r = Start-Direct $script:PythonExe @('-c', 'import sys;print("%d.%d" % sys.version_info[:2]);sys.exit(0 if sys.version_info >= (3,8) else 1)')
        if ($r.Code -ne 0) { Fout "Python $($r.Uit.Trim()) is te oud ($script:PythonExe). Nodig: 3.8 of nieuwer." }
        $pyVersie = $r.Uit.Trim()

        # Visual Studio 2022 (of Build Tools) met C++: omgeving van VsDevCmd overnemen
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { Fout 'Visual Studio 2022 (of de Build Tools) is niet gevonden. Installeer "Desktopontwikkeling met C++" (zie README.md).' }
        $r = Start-Direct $vswhere @('-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
        $vsPad = ($r.Uit -split "`r?`n" | Where-Object { $_ } | Select-Object -First 1)
        if (-not $vsPad) { Fout 'Visual Studio is gevonden, maar zonder de C++-compiler (MSVC x64). Voeg "Desktopontwikkeling met C++" toe via de Visual Studio Installer.' }
        $vsDev = Join-Path $vsPad 'Common7\Tools\VsDevCmd.bat'
        $r = Start-Direct $env:ComSpec @('/d', '/c', 'call', $vsDev, '-arch=amd64', '-host_arch=amd64', '-no_logo', '>nul', '2>&1', '&&', 'set')
        if ($r.Code -ne 0) { Fout "VsDevCmd.bat mislukt ($vsDev)." }
        foreach ($l in ($r.Uit -split "`r?`n")) {
            $i = $l.IndexOf('=')
            if ($i -gt 0) { [Environment]::SetEnvironmentVariable($l.Substring(0, $i), $l.Substring($i + 1), 'Process') }
        }
        if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { Fout 'De MSVC-compiler (cl.exe) is na VsDevCmd niet beschikbaar.' }

        # CMake 3.20+ en Ninja (uit Visual Studio, PATH, of een Python-omgeving: py -m pip install cmake ninja)
        $pyScripts = Split-Path -Parent $script:PythonExe
        $vsCMake = Join-Path $vsPad 'Common7\IDE\CommonExtensions\Microsoft\CMake'
        $script:CMakeExe = Zoek-Exe $CMake @('cmake.exe') @((Join-Path $vsCMake 'CMake\bin\cmake.exe'), (Join-Path $projectRoot 'work\venv\Scripts\cmake.exe'), (Join-Path $pyScripts 'cmake.exe'), (Join-Path $pyScripts 'Scripts\cmake.exe'), (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'))
        if (-not $script:CMakeExe) { Fout 'CMake niet gevonden. Installeer het "C++ CMake tools"-onderdeel van Visual Studio, of: py -m pip install cmake ninja. Of geef -CMake <pad>.' }
        $script:NinjaExe = Zoek-Exe $Ninja @('ninja.exe') @((Join-Path $vsCMake 'Ninja\ninja.exe'), (Join-Path $projectRoot 'work\venv\Scripts\ninja.exe'), (Join-Path $pyScripts 'ninja.exe'), (Join-Path $pyScripts 'Scripts\ninja.exe'))
        if (-not $script:NinjaExe) { Fout 'Ninja niet gevonden. Installeer het "C++ CMake tools"-onderdeel van Visual Studio, of: py -m pip install ninja. Of geef -Ninja <pad>.' }
        $r = Start-Direct $script:CMakeExe @('--version')
        if ($r.Uit -notmatch 'cmake version (\d+)\.(\d+)') { Fout "CMake-versie onbekend ($script:CMakeExe)." }
        if ([int]$Matches[1] -lt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -lt 20)) { Fout "CMake $($Matches[1]).$($Matches[2]) is te oud; nodig: 3.20 of nieuwer." }
        $cmakeVersie = "$($Matches[1]).$($Matches[2])"

        # DolphinTool (alleen nodig voor een schijfbestand)
        $script:DolphinExe = $null
        if (-not $isoIsMap) {
            $kandidaten = @()
            if ($env:DOLPHINTOOL) { $kandidaten += $env:DOLPHINTOOL }
            foreach ($basis in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA)) {
                if (-not $basis -or -not (Test-Path -LiteralPath $basis)) { continue }
                foreach ($d in @(Get-ChildItem -LiteralPath $basis -Directory -Filter 'Dolphin*' -ErrorAction SilentlyContinue)) {
                    $kandidaten += @(Get-ChildItem -LiteralPath $d.FullName -Filter DolphinTool.exe -File -Recurse -Depth 2 -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
                }
            }
            $script:DolphinExe = Zoek-Exe $DolphinTool @('DolphinTool.exe', 'dolphin-tool.exe') $kandidaten
            if (-not $script:DolphinExe) {
                $dl = Join-Path $env:USERPROFILE 'Downloads'
                if (Test-Path -LiteralPath $dl) {
                    $gevonden = Get-ChildItem -LiteralPath $dl -Filter DolphinTool.exe -Recurse -Depth 3 -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
                    if ($gevonden) { $script:DolphinExe = $gevonden.FullName }
                }
            }
            if (-not $script:DolphinExe) { Fout 'DolphinTool.exe niet gevonden. Het hoort bij de Dolphin-emulator (dolphin-emu.org, map Dolphin-x64). Geef het pad met -DolphinTool <pad\DolphinTool.exe>, of pak de schijf zelf uit met Dolphin (rechtsklik > Eigenschappen > Bestandssysteem > Hele schijf uitpakken) en geef die map als -Iso.' }
        }

        # Bibliotheekbronnen (offline) of CMake laat ze ophalen
        $script:BronMap = $null
        foreach ($k in @($Bronnen, (Join-Path $projectRoot 'work\nwii-build\_deps'))) {
            if ($k -and (Test-Path -LiteralPath (Join-Path $k 'tomlplusplus-src\include')) -and (Test-Path -LiteralPath (Join-Path $k 'sdl2-src\CMakeLists.txt')) -and (Test-Path -LiteralPath (Join-Path $k 'imgui-src\imgui.cpp'))) { $script:BronMap = Volledig-Pad $k; break }
        }
        if ($Bronnen -and -not $script:BronMap) { Fout "In -Bronnen ($Bronnen) staan geen tomlplusplus-src, sdl2-src en imgui-src." }
        if (-not $script:BronMap) {
            if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { Fout 'SDL2, toml++ en Dear ImGui zijn niet lokaal aanwezig en git ontbreekt, dus CMake kan ze niet ophalen. Installeer Git for Windows of geef -Bronnen <map>.' }
            Write-Host '   LET OP: CMake haalt SDL2 (release-2.28.5), toml++ (v3.4.0) en Dear ImGui (v1.91.9b) op van github.com.' -ForegroundColor Yellow
        }

        Write-Host "   Python $pyVersie   : $script:PythonExe"
        Write-Host "   Visual Studio    : $vsPad"
        Write-Host "   CMake $cmakeVersie      : $script:CMakeExe"
        Write-Host "   Ninja            : $script:NinjaExe"
        if ($script:DolphinExe) { Write-Host "   DolphinTool      : $script:DolphinExe" }
        Write-Host "   Bibliotheken     : $(if ($script:BronMap) { $script:BronMap } else { 'via internet (CMake FetchContent)' })"
        Write-Host "   Compileertaken   : $Taken"
        return "Python $pyVersie, CMake $cmakeVersie"
    }

    # ---------------------------------------------------------------------------- 2. schijf controleren
    Stap '2. Schijf controleren' {
        if ($isoIsMap) {
            $root = Zoek-SpelRoot $IsoPad
            if (-not $root) { Fout "In $IsoPad staat geen uitgepakte Wii-schijf (sys\main.dol ontbreekt)." }
            $v = Python-Helper @('verify-game', $root)
            $script:SpelBron = $root
            return "uitgepakte map, $($v.game_id) rev $($v.revision), main.dol SHA-1 klopt"
        }
        $r = Start-Direct $script:DolphinExe @('header', '-i', $IsoPad, '-j')
        $kop = $null
        try { $kop = ($r.Uit.Trim() | ConvertFrom-Json) } catch { }
        if ($r.Code -ne 0 -or -not $kop -or -not (Heeft-Eigenschap $kop 'game_id')) {
            Fout "Dit bestand is geen leesbare Wii-schijf (DolphinTool herkent het niet): $IsoPad"
        }
        if (-not (Heeft-Eigenschap $kop 'internal_name')) { $kop | Add-Member -NotePropertyName internal_name -NotePropertyValue '' }
        if (-not (Heeft-Eigenschap $kop 'revision')) { $kop | Add-Member -NotePropertyName revision -NotePropertyValue -1 }
        $id = "$($kop.game_id)"; $rev = [int]$kop.revision
        if ($id -ne $GameId) {
            $regio = @{ 'P' = 'Europese (PAL)'; 'J' = 'Japanse'; 'K' = 'Koreaanse' }
            if ($id.Length -ge 4 -and $id.StartsWith('SOU') -and $regio.ContainsKey($id.Substring(3, 1))) {
                Fout "Dit is de $($regio[$id.Substring(3, 1)]) versie van Skyward Sword ($id). Alleen de Amerikaanse versie 1.00 (SOUE01, revisie 0) wordt ondersteund."
            }
            Fout "Dit is geen Skyward Sword-schijf voor dit project (spelcode $id, '$($kop.internal_name)'). Nodig: SOUE01, revisie 0."
        }
        if ($rev -ne $Revision) { Fout "Dit is SOUE01 revisie $rev. Alleen revisie 0 (versie 1.00) wordt ondersteund." }
        $script:SpelBron = $null
        return "$id rev $rev, '$($kop.internal_name)'"
    }
    if ($AlleenControle) {
        Write-Host ''
        Write-Host 'Controle geslaagd (-AlleenControle): er is niets uitgepakt of gebouwd.' -ForegroundColor Green
        Voortgang 'einde' -Melding 'controle'  # INSTALL19
        exit 0
    }

    # ---------------------------------------------------------------------------- 3. uitpakken
    if ($script:SpelBron -and -not $SpelMap) { $spelDoel = $script:SpelBron }
    elseif ($SpelMap) { $spelDoel = Volledig-Pad $SpelMap }
    else { $spelDoel = Join-Path $Uitvoer 'game' }
    Stap '3. Spelbestanden uitpakken' {
        if ($script:SpelBron -and $spelDoel -eq $script:SpelBron) { return 'bestaande map wordt gebruikt, niets gekopieerd' }
        # Alleen de eigen standaardmap <Uitvoer>\game mag het script leegmaken; een opgegeven -SpelMap nooit.
        $eigenMap = ($spelDoel -eq (Join-Path $Uitvoer 'game'))
        if (Test-Path -LiteralPath (Join-Path $spelDoel 'sys\main.dol')) {
            $r = Start-Direct $script:PythonExe @($helper, 'verify-game', $spelDoel)
            if ($r.Code -eq 0) { return "al uitgepakt in $spelDoel (gecontroleerd), overgeslagen" }
            if (-not $eigenMap) { Fout "In -SpelMap $spelDoel staat al een schijf die niet klopt. Kies een lege of nieuwe map." }
            Write-Host '   De bestaande uitgepakte map klopt niet; opnieuw uitpakken.' -ForegroundColor Yellow
            Wacht-Op-Gesloten $Uitvoer  # REL22: niet half leegmaken terwijl het spel eruit leest
            Remove-Item -LiteralPath $spelDoel -Recurse -Force
        } elseif (Test-Path -LiteralPath $spelDoel) {
            if (@(Get-ChildItem -LiteralPath $spelDoel -Force).Count -gt 0) {
                if (-not $eigenMap) { Fout "-SpelMap $spelDoel is niet leeg. Kies een lege of nieuwe map." }
                Wacht-Op-Gesloten $Uitvoer  # REL22
                Remove-Item -LiteralPath $spelDoel -Recurse -Force
            } else {
                Remove-Item -LiteralPath $spelDoel -Force
            }
        }
        $tijdelijk = $spelDoel + '.uitpakken'
        if (Test-Path -LiteralPath $tijdelijk) { Remove-Item -LiteralPath $tijdelijk -Recurse -Force }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $spelDoel) | Out-Null
        if ($script:SpelBron) {
            Write-Host "   Kopieren van $script:SpelBron ..."
            Copy-Item -LiteralPath $script:SpelBron -Destination $tijdelijk -Recurse
            $root = $tijdelijk
        } else {
            Write-Host '   DolphinTool pakt de DATA-partitie uit (ca. 4 GB, enkele minuten) ...'
            $log = Join-Path $logMap "uitpakken-$stempel.log"
            $code = Start-Gelogd $script:DolphinExe @('extract', '-i', $IsoPad, '-o', $tijdelijk, '-g', '-q') $log 'Uitpakken'
            if ($code -ne 0) { Write-Host ((Log-Staart $log 15) -join "`n"); Fout "Uitpakken mislukt (code $code). Zie $log" }
            $root = Zoek-SpelRoot $tijdelijk
            if (-not $root) { Fout "Na het uitpakken staat er geen sys\main.dol in $tijdelijk." }
        }
        if ($root -ne $tijdelijk) {
            Move-Item -LiteralPath $root -Destination $spelDoel
            Remove-Item -LiteralPath $tijdelijk -Recurse -Force
        } else {
            Move-Item -LiteralPath $tijdelijk -Destination $spelDoel
        }
        [void](Python-Helper @('verify-game', $spelDoel))
        return ('{0} uitgepakt, main.dol en rels.arc gecontroleerd' -f (GB (Map-Grootte $spelDoel)))
    }

    # ---------------------------------------------------------------------------- 4. REL-modules
    $relMap = Join-Path $Werkmap 'rels-bron'
    Stap '4. REL-modules uitpakken en controleren' {
        $v = Python-Helper @('extract-rels', $spelDoel, $relMap, '--decomp-config', (Join-Path $decompCfgDir 'config.yml'))
        return "$($v.modules) modules, $($v.hash_ok) SHA-1 correct"
    }

    # ---------------------------------------------------------------------------- 5. vertaler bouwen
    $genBuild = Join-Path $Werkmap 'nwiirecomp-build'
    $genExe = Join-Path $genBuild 'nWiiRecomp\nwiirecomp.exe'
    $tempMap = Join-Path $Werkmap 'tmp'
    New-Item -ItemType Directory -Force -Path $tempMap | Out-Null
    $env:TEMP = $tempMap; $env:TMP = $tempMap
    $env:NINJA_STATUS = '[%f/%t] '  # INSTALL19: vaste voortgangsnotatie (ninja-standaard, expliciet)
    $bronArgs = @()
    if ($script:BronMap) {
        # UI13: Dear ImGui (launcher, instellingen, menu in het spel) uit dezelfde map, geen download.
        $bronArgs = @("-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$(CMake-Pad (Join-Path $script:BronMap 'tomlplusplus-src'))", "-DFETCHCONTENT_SOURCE_DIR_SDL2=$(CMake-Pad (Join-Path $script:BronMap 'sdl2-src'))", "-DFETCHCONTENT_SOURCE_DIR_IMGUI=$(CMake-Pad (Join-Path $script:BronMap 'imgui-src'))")
        # INSTALL19: met -Bronnen nooit iets ophalen (een ontbrekende bibliotheek faalt dan meteen).
        if ($Bronnen) { $bronArgs += '-DFETCHCONTENT_FULLY_DISCONNECTED=ON' }
    }
    Stap '5. Vertaler (nWiiRecomp) bouwen' {
        $log = Join-Path $logMap "vertaler-$stempel.log"
        if (-not (Test-Path -LiteralPath (Join-Path $genBuild 'build.ninja'))) {
            $code = Start-Gelogd $script:CMakeExe (@('-S', $nwiiSrc, '-B', $genBuild, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DNWII_BUILD_STUDIO=OFF', "-DCMAKE_MAKE_PROGRAM=$(CMake-Pad $script:NinjaExe)") + $bronArgs) $log 'Vertaler configureren'
            if ($code -ne 0) { Write-Host ((Log-Staart $log 20) -join "`n"); Fout "CMake-configuratie van de vertaler mislukt. Zie $log" }
        }
        $code = Start-Gelogd $script:CMakeExe @('--build', $genBuild, '--target', 'nwiirecomp', '-j', "$Taken") $log 'Vertaler bouwen'
        if ($code -ne 0 -or -not (Test-Path -LiteralPath $genExe)) { Write-Host ((Log-Staart $log 20) -join "`n"); Fout "Bouwen van de vertaler mislukt. Zie $log" }
        return 'nwiirecomp.exe gebouwd'
    }

    # ---------------------------------------------------------------------------- 6. code genereren
    $exportNieuw = Join-Path $Werkmap 'export-nieuw'
    $export = Join-Path $Werkmap 'export'
    $genConfig = Join-Path $Werkmap 'recomp_config.toml'
    Stap '6. main.dol en REL-modules vertalen naar C++' {
        [void](Python-Helper @('make-recomp-config', '--profile', $profileToml, '--out', $genConfig, '--game', $spelDoel,
                '--export', $exportNieuw, '--runtime', $runtimeSrc, '--sigdb', $sigdb, '--symbols-csv', $symbolsCsv,
                '--rel-dir', $relMap, '--rel-symbols-dir', (Join-Path $decompCfgDir 'rels')))
        if (Test-Path -LiteralPath $exportNieuw) { Remove-Item -LiteralPath $exportNieuw -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $exportNieuw | Out-Null
        $log = Join-Path $logMap "genereren-$stempel.log"
        $r = Start-Direct $genExe @($genConfig) $Werkmap
        Set-Content -LiteralPath $log -Value ($r.Uit + $r.Fout) -Encoding UTF8
        if ($r.Code -ne 0 -or $r.Uit -notmatch 'Successfully exported' -or -not (Test-Path -LiteralPath (Join-Path $exportNieuw 'functions.h'))) {
            Write-Host ((Log-Staart $log 20) -join "`n"); Fout "Vertalen van main.dol mislukt. Zie $log"
        }
        $r = Start-Direct $genExe @($genConfig, '--rels') $Werkmap
        Add-Content -LiteralPath $log -Value ($r.Uit + $r.Fout) -Encoding UTF8
        if ($r.Code -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $exportNieuw 'rels\rel_aot_table.cpp'))) {
            Write-Host ((Log-Staart $log 20) -join "`n"); Fout "Vertalen van de REL-modules mislukt. Zie $log"
        }
        $relRegel = ($r.Uit -split "`r?`n" | Where-Object { $_ -match '\[RELAOT\] generated' } | Select-Object -First 1)
        $s = Python-Helper @('sync-tree', $exportNieuw, $export)
        $n = @(Get-ChildItem -LiteralPath $export -Filter 'output_*.cpp' -File).Count
        return ("{0} DOL-bestanden; {1}; {2} gewijzigd, {3} ongewijzigd" -f $n, ($relRegel -replace '^\[RELAOT\] generated ', '' -replace ',.*$', ''), $s.copied, $s.unchanged)
    }

    # ---------------------------------------------------------------------------- 7. spel bouwen
    $gameBuild = Join-Path $Werkmap 'build'
    $gameExe = Join-Path $gameBuild 'bin\SkywardSwordPC.exe'
    Stap '7. SkywardSwordPC.exe bouwen (MSVC, kan 15-30 min duren)' {
        $log = Join-Path $logMap "bouwen-$stempel.log"
        $cfgArgs = @('-S', $export, '-B', $gameBuild, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_MAKE_PROGRAM=$(CMake-Pad $script:NinjaExe)",
            "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$(CMake-Pad (Join-Path $gameBuild 'bin'))",
            '-DCMAKE_CXX_FLAGS_RELEASE=/O1 /Ob1 /DNDEBUG /bigobj /DSDL_MAIN_HANDLED /D_CRT_SECURE_NO_WARNINGS',
            '-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF') + $bronArgs
        # REL22: the export copies the runtime to <Werkmap>\export\nWiiRuntime, so the runtime's own default (<runtime>\..\..\branding)
        # misses outputs\branding: pass it explicitly (SkyBreak icon in the exe + the wing in the launcher). Missing = old behaviour.
        $brandingMap = Join-Path $outputsDir 'branding'
        if ((Test-Path -LiteralPath (Join-Path $brandingMap 'skybreak.ico')) -and (Test-Path -LiteralPath (Join-Path $brandingMap 'skybreak-logo-512.png'))) {
            $cfgArgs += "-DSKYBREAK_BRANDING_DIR=$(CMake-Pad $brandingMap)"
        }
        $code = Start-Gelogd $script:CMakeExe $cfgArgs $log 'Spel configureren'
        if ($code -ne 0) { Write-Host ((Log-Staart $log 20) -join "`n"); Fout "CMake-configuratie van het spel mislukt. Zie $log" }
        $code = Start-Gelogd $script:CMakeExe @('--build', $gameBuild, '--target', 'SkywardSwordPC', '-j', "$Taken") $log 'Spel bouwen'
        if ($code -ne 0 -or -not (Test-Path -LiteralPath $gameExe)) {
            Write-Host ((Log-Staart $log 25) -join "`n"); Fout "Bouwen van het spel mislukt. Zie $log"
        }
        return ('exe {0:N0} MB' -f ((Get-Item -LiteralPath $gameExe).Length / 1MB))
    }

    # ---------------------------------------------------------------------------- 8. map samenstellen
    Stap '8. Map SkywardSwordPC samenstellen' {
        $bin = Join-Path $Uitvoer 'bin'
        $hulp = Join-Path $Uitvoer 'hulp'
        New-Item -ItemType Directory -Force -Path $bin, $hulp | Out-Null
        # REL22: een draaiende SkyBreak uit deze map houdt de exe vast (vroeger: fout in stap 8). Eerst vragen/wachten; nooit afsluiten.
        Wacht-Op-Gesloten $Uitvoer
        $pdb = [IO.Path]::ChangeExtension($gameExe, '.pdb')
        try {
            Copy-Item -LiteralPath $gameExe -Destination (Join-Path $bin "$ExeNaam.exe") -Force  # INSTALL19: -ExeNaam
            if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination $bin -Force }
        } catch {
            Wacht-Op-Gesloten $Uitvoer  # REL22: net tussendoor gestart -> nog een keer vragen, dan opnieuw
            Copy-Item -LiteralPath $gameExe -Destination (Join-Path $bin "$ExeNaam.exe") -Force
            if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination $bin -Force }
        }
        if (Test-Path -LiteralPath $freezeSymbols) { Copy-Item -LiteralPath $freezeSymbols -Destination $bin -Force }
        Copy-Item -LiteralPath (Join-Path $templates 'Speel.cmd') -Destination $Uitvoer -Force
        Copy-Item -LiteralPath (Join-Path $templates 'LEESMIJ.txt') -Destination $Uitvoer -Force
        Copy-Item -LiteralPath (Join-Path $templates 'Speel.ps1') -Destination $hulp -Force
        # INSTALL19: Speel.ps1 leest een afwijkende exe-naam uit hulp\exenaam.txt
        $naamBestand = Join-Path $hulp 'exenaam.txt'
        if ($ExeNaam -ne 'SkywardSwordPC') { Set-Content -LiteralPath $naamBestand -Value $ExeNaam -Encoding ASCII }
        elseif (Test-Path -LiteralPath $naamBestand) { Remove-Item -LiteralPath $naamBestand -Force }

        $schermMap = 'screenshots'
        if ($NandRoot) { $nandRoot = $NandRoot }  # INSTALL19
        elseif ($Draagbaar) { $nandRoot = 'gebruikersdata/nand' } else { $nandRoot = '%APPDATA%/SkywardSwordPC/nand' }
        $standaard = Join-Path $Uitvoer 'instellingen.standaard.toml'
        [void](Python-Helper @('make-settings', '--profile', $profileToml, '--out', $standaard, '--nand-root', $nandRoot, '--screenshot-dir', $schermMap))
        $instellingen = Join-Path $Uitvoer 'instellingen.toml'
        $nieuw = -not (Test-Path -LiteralPath $instellingen)
        if ($nieuw) { Copy-Item -LiteralPath $standaard -Destination $instellingen }

        # wijzer naar de spelbestanden als die niet in <Uitvoer>\game staan
        $wijzer = Join-Path $hulp 'spelmap.txt'
        if ($spelDoel -ne (Join-Path $Uitvoer 'game')) { Set-Content -LiteralPath $wijzer -Value $spelDoel -Encoding UTF8 }
        elseif (Test-Path -LiteralPath $wijzer) { Remove-Item -LiteralPath $wijzer -Force }

        # COMMUNITY30: authoring tools ship with the installer; model assets and mod selection stay local.
        $modToolsBron = Join-Path $outputsDir 'pc-tools\mods'
        if (Test-Path -LiteralPath $modToolsBron) {
            $modMap = Join-Path $Uitvoer 'mods'
            $modToolsDoel = Join-Path $modMap 'tools'
            [void](New-Item -ItemType Directory -Path $modToolsDoel -Force)
            foreach ($naam in @('make-link-mod.py', 'convert-sshd-link.py', 'codec.py', 'lz11_encode.py')) {
                Copy-Item -LiteralPath (Join-Path $modToolsBron $naam) -Destination (Join-Path $modToolsDoel $naam) -Force
            }
            Copy-Item -LiteralPath (Join-Path $outputsDir 'pc-tools\hdtex\gx_texture.py') -Destination (Join-Path $modToolsDoel 'gx_texture.py') -Force
            foreach ($naam in @('README.md', 'SSHD-conversion.md')) {
                Copy-Item -LiteralPath (Join-Path $modToolsBron $naam) -Destination (Join-Path $modMap $naam) -Force
            }
            # Never create/reset mods/state.toml or overwrite user model/texture folders.
        }

        # REL22: knopiconen (Kenney "Input Prompts", CC0) passend gemaakt op de knopplaatjes van JOUW schijf, naar
        # <Uitvoer>\textures\glyphs\SOUE01 (daar zoekt de exe: <exe>\..\textures\glyphs). Alleen opnieuw als het recept of het
        # script veranderd is (stempel); mislukt het, dan bouwt de rest gewoon door en toont het spel de Wii-knoppen.
        $glyphNoot = ''
        $glyphScript = Join-Path $outputsDir 'pc-tools\glyphs\glyph_local.py'
        if (Test-Path -LiteralPath $glyphScript) {
            $r = Start-Direct $script:PythonExe @('-I', $glyphScript, '--game', $spelDoel, '--out', (Join-Path $Uitvoer 'textures\glyphs\SOUE01'))
            $regel = @($r.Uit -split "`r?`n" | Where-Object { $_.Trim().StartsWith('{') } | Select-Object -Last 1)
            $g = $null
            if ($regel.Count -gt 0) { try { $g = $regel[0] | ConvertFrom-Json } catch { $g = $null } }
            if ($r.Code -eq 0 -and $g -and (Heeft-Eigenschap $g 'status')) {
                if ($g.status -eq 'actueel') { $glyphNoot = '; knopiconen actueel' } else { $glyphNoot = "; knopiconen gemaakt ($($g.pictures))" }
            } else {
                $glyphNoot = '; knopiconen NIET gemaakt (het spel toont de Wii-knoppen)'
                Write-Host ('   Waarschuwing: knopiconen niet gemaakt: ' + ($r.Uit + $r.Fout).Trim()) -ForegroundColor Yellow
            }
        }

        # COMMUNITY30: Windows PowerShell may inherit a PowerShell 7-only module
        # path from its parent. Use .NET directly; no delayed Utility-module import.
        $hashStream = [System.IO.File]::OpenRead((Join-Path $bin "$ExeNaam.exe"))
        $hashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
        try {
            $exeHash = [System.BitConverter]::ToString($hashAlgorithm.ComputeHash($hashStream)).Replace('-', '').ToLowerInvariant()
        } finally {
            $hashAlgorithm.Dispose()
            $hashStream.Dispose()
        }
        $productNaam = if ($Product) { $Product } else { 'Skyward Sword PC' }  # INSTALL19: -Product
        $versie = @(
            "$productNaam - lokaal gebouwd op $(Get-Date -Format 'yyyy-MM-dd HH:mm') door Bouw-SkywardSword.ps1",
            "Schijf: $GameId revisie $Revision, main.dol SHA-1 $MainDolSha1",
            "Bron: $(Split-Path -Leaf $IsoPad)",
            "Spelbestanden: $(if ($spelDoel.StartsWith($Uitvoer + [IO.Path]::DirectorySeparatorChar)) { $spelDoel.Substring($Uitvoer.Length + 1) } else { $spelDoel })",
            "$ExeNaam.exe SHA-256: $exeHash",
            "Opslag (NAND): $nandRoot",
            '',
            'Deze map bevat code en gegevens die uit jouw eigen schijf zijn gemaakt. Alleen voor eigen gebruik; niet delen.'
        )
        Set-Content -LiteralPath (Join-Path $Uitvoer 'VERSIE.txt') -Value $versie -Encoding UTF8
        if ($nieuw) { return ('instellingen.toml aangemaakt' + $glyphNoot) }
        return ('bestaande instellingen.toml behouden (fabrieksinstellingen: instellingen.standaard.toml)' + $glyphNoot)
    }

    if ($Opruimen) {
        Stap '9. Werkmap opruimen' { Remove-Item -LiteralPath $Werkmap -Recurse -Force; return $Werkmap }
    }

    # ---------------------------------------------------------------------------- samenvatting
    $totaalKlok.Stop()
    $grootteUitvoer = Map-Grootte $Uitvoer
    $grootteSpel = if ($spelDoel.StartsWith($Uitvoer + '\')) { 0 } else { Map-Grootte $spelDoel }
    $grootteWerk = Map-Grootte $Werkmap
    Write-Host ''
    Write-Host 'Klaar!' -ForegroundColor Green
    $stappen | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    Write-Host ("Totale tijd: {0:N1} min" -f $totaalKlok.Elapsed.TotalMinutes)
    Write-Host ("Map {0}: {1}" -f $Uitvoer, (GB $grootteUitvoer))
    if ($grootteSpel -gt 0) { Write-Host ("Spelbestanden {0}: {1}" -f $spelDoel, (GB $grootteSpel)) }
    if (Test-Path -LiteralPath $Werkmap) { Write-Host ("Werkmap {0}: {1} (mag weg; dan duurt de volgende bouw langer)" -f $Werkmap, (GB $grootteWerk)) }
    Write-Host ''
    Write-Host "Spelen: dubbelklik $(Join-Path $Uitvoer 'Speel.cmd')  (uitleg in LEESMIJ.txt)" -ForegroundColor Green
    $samenvatting = [pscustomobject]@{ uitvoer = $Uitvoer; spelbestanden = $spelDoel; werkmap = $Werkmap; minuten = [Math]::Round($totaalKlok.Elapsed.TotalMinutes, 2)
        bytes_uitvoer = $grootteUitvoer; bytes_spel_buiten_uitvoer = $grootteSpel; bytes_werkmap = $grootteWerk; stappen = $stappen }
    $samenvatting | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $logMap "samenvatting-$stempel.json") -Encoding UTF8
    Voortgang 'einde' -Melding ('{0:N1} min' -f $totaalKlok.Elapsed.TotalMinutes)  # INSTALL19
    exit 0
}
catch {
    Write-Progress -Activity 'Bouwen' -Completed
    Write-Host ''
    Write-Host ('FOUT: ' + $_.Exception.Message) -ForegroundColor Red
    Voortgang 'fout' -Soort (Fout-Soort $_.Exception) -Melding $_.Exception.Message  # INSTALL19
    if ($_.Exception.GetType().FullName -ne 'System.Exception' -and $_.InvocationInfo) { Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray }
    if ($stappen.Count -gt 0) { Write-Host ('Gelukt tot en met: ' + $stappen[$stappen.Count - 1].Stap) }
    exit 1
}
