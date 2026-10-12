<#
  Installeert alles wat nodig is om SkyBreak te bouwen:
    - Visual Studio 2022 Build Tools met C++ (MSVC x64), CMake en Ninja
    - Python 3
  Gebruikt winget (zit standaard in Windows 10/11 via "App-installatieprogramma").
  Wat al aanwezig is, wordt overgeslagen. Windows vraagt om toestemming (UAC) voor de installatie.
  Dolphin installeer je zelf (zie LEESMIJ.txt): alleen DolphinTool.exe is nodig.
#>
$ErrorActionPreference = 'Stop'
function Stap($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }
function OK($t)   { Write-Host "   $t" -ForegroundColor Green }

Write-Host "SkyBreak - benodigdheden installeren" -ForegroundColor Yellow

Stap "winget controleren"
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host "   winget ontbreekt. Installeer 'App-installatieprogramma' uit de Microsoft Store en start dit script opnieuw." -ForegroundColor Red
    Read-Host "Druk op Enter om te sluiten"; exit 1
}
OK "winget gevonden"

$vsComponenten = @(
    'Microsoft.VisualStudio.Workload.VCTools',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    'Microsoft.VisualStudio.Component.Windows11SDK.22621',
    'Microsoft.VisualStudio.Component.VC.CMake.Project'
)

Stap "Visual Studio 2022 Build Tools (C++, CMake, Ninja)"
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPad = $null
if (Test-Path $vswhere) {
    $vsPad = & $vswhere -latest -products * -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
}
if ($vsPad) {
    OK "al aanwezig: $vsPad"
} else {
    $bestaand = $null
    if (Test-Path $vswhere) { $bestaand = & $vswhere -latest -products * -version '[17.0,18.0)' -property installationPath }
    $adds = ($vsComponenten | ForEach-Object { "--add $_" }) -join ' '
    if ($bestaand) {
        # Visual Studio 2022 bestaat al, maar zonder C++/CMake: onderdelen toevoegen.
        Write-Host "   Visual Studio 2022 gevonden zonder C++/CMake; onderdelen worden toegevoegd (een paar GB)..."
        $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
        $p = Start-Process -FilePath $installer -ArgumentList "modify --installPath `"$bestaand`" $adds --includeRecommended --passive --norestart" -Wait -PassThru -Verb RunAs
        if ($p.ExitCode -notin 0, 3010) { throw "Visual Studio Installer gaf foutcode $($p.ExitCode)" }
    } else {
        Write-Host "   Downloaden en installeren (een paar GB, dit kan even duren)..."
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget --accept-package-agreements --accept-source-agreements `
            --override "--wait --passive --norestart $adds --includeRecommended"
        if ($LASTEXITCODE -notin 0, 3010, -1978335189) { throw "winget gaf foutcode $LASTEXITCODE bij Visual Studio Build Tools" }
    }
    OK "Visual Studio Build Tools geinstalleerd"
}

Stap "Python 3"
$py = Get-Command py -ErrorAction SilentlyContinue
$pyOk = $false
if ($py) { & py -3 --version 2>$null; if ($LASTEXITCODE -eq 0) { $pyOk = $true } }
if (-not $pyOk) {
    $p2 = Get-Command python -ErrorAction SilentlyContinue
    if ($p2 -and $p2.Source -notlike '*WindowsApps*') { $pyOk = $true }
}
if ($pyOk) {
    OK "al aanwezig"
} else {
    winget install --id Python.Python.3.12 -e --source winget --accept-package-agreements --accept-source-agreements --scope user
    if ($LASTEXITCODE -notin 0, -1978335189) { throw "winget gaf foutcode $LASTEXITCODE bij Python" }
    OK "Python 3.12 geinstalleerd"
}

Stap "Klaar"
Write-Host "   Alles staat klaar. Nog nodig: Dolphin (voor DolphinTool.exe), zie LEESMIJ.txt."
Write-Host "   Start daarna SkyBreak-Setup.exe opnieuw (of sleep je schijfbestand op geavanceerd\Bouw-SkyBreak.cmd)."
Write-Host "   (Als Windows om een herstart vraagt: eerst herstarten.)"
Read-Host "Druk op Enter om te sluiten"
