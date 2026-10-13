# Skyward Sword PC - starter (wordt door Speel.cmd aangeroepen).
# Opties voor een keer: -Schaal 1..6 (interne resolutie), -Volledig (schermvullend), -Venster, -Diagnose.
param(
    [ValidateRange(0, 6)][int]$Schaal = 0,
    [switch]$Volledig,
    [switch]$Venster,
    [switch]$Diagnose
)
$ErrorActionPreference = 'Stop'
$map = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $map 'bin\SkywardSwordPC.exe'
# INSTALL19: een andere exe-naam (bijvoorbeeld SkyBreak-game) staat in hulp\exenaam.txt
$naamBestand = Join-Path $PSScriptRoot 'exenaam.txt'
if (Test-Path -LiteralPath $naamBestand) {
    $n = "$(Get-Content -LiteralPath $naamBestand -TotalCount 1)".Trim()
    if ($n -match '^[A-Za-z0-9._-]+$') { $exe = Join-Path (Join-Path $map 'bin') ($n + '.exe') }
}
$instellingen = Join-Path $map 'instellingen.toml'
$spel = Join-Path $map 'game'
$wijzer = Join-Path $PSScriptRoot 'spelmap.txt'
if (Test-Path -LiteralPath $wijzer) {
    $p = "$(Get-Content -LiteralPath $wijzer -TotalCount 1)".Trim()
    if ($p) {
        if (-not [IO.Path]::IsPathRooted($p)) { $p = Join-Path $map $p }
        $spel = $p
    }
}
function Stop-Met([string]$Melding) { Write-Host $Melding -ForegroundColor Red; exit 1 }
if (-not (Test-Path -LiteralPath $exe)) { Stop-Met "$(Split-Path -Leaf $exe) is missing in $map\bin. Run SkyBreak-Setup and choose Repair." }
if (-not (Test-Path -LiteralPath (Join-Path $spel 'sys\main.dol'))) { Stop-Met "Game files are missing ($spel). Run SkyBreak-Setup and choose Repair." }
if (-not (Test-Path -LiteralPath $instellingen)) {
    $standaard = Join-Path $map 'instellingen.standaard.toml'
    if (Test-Path -LiteralPath $standaard) { Copy-Item -LiteralPath $standaard -Destination $instellingen }
}
$versie = Join-Path $map 'VERSIE.txt'
if (Test-Path -LiteralPath $versie) { Write-Host (Get-Content -LiteralPath $versie -TotalCount 1) }
Write-Host 'F11 or Alt+Enter toggles fullscreen. F12 takes a screenshot. Close the window to exit.'

# Logboek van deze sessie (de nieuwste 20 blijven bewaard).
$logMap = Join-Path $map 'logs'
New-Item -ItemType Directory -Force -Path $logMap | Out-Null
Get-ChildItem -LiteralPath $logMap -Filter 'speel-*.log' -File | Sort-Object Name -Descending | Select-Object -Skip 19 | Remove-Item -Force -ErrorAction SilentlyContinue
$log = Join-Path $logMap ("speel-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Write-Host "Log: $log"

$namen = @('NWII_HEADLESS', 'NWII_RUN_SECONDS', 'NWII_SS_INPUT_REPLAY', 'NWII_SAMPLE', 'NWII_INTERNAL_SCALE', 'NWII_FULLSCREEN')
$oud = @{}
foreach ($n in $namen) { $oud[$n] = [Environment]::GetEnvironmentVariable($n, 'Process') }
try {
    foreach ($n in $namen) { [Environment]::SetEnvironmentVariable($n, $null, 'Process') }
    if ($Diagnose) { $env:NWII_SAMPLE = '1' }
    if ($Schaal -gt 0) { $env:NWII_INTERNAL_SCALE = "$Schaal" }
    if ($Volledig) { $env:NWII_FULLSCREEN = '1' } elseif ($Venster) { $env:NWII_FULLSCREEN = '0' }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $psi.Arguments = '/d /s /c ""' + $exe + '" "' + $spel + '" "' + $instellingen + '" > "' + $log + '" 2>&1"'
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $map
    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.WaitForExit()
    $code = $proc.ExitCode
} finally {
    foreach ($n in $namen) { [Environment]::SetEnvironmentVariable($n, $oud[$n], 'Process') }
}

# De vastloopdetector schrijft "[FREEZE8] =====" in het logboek als er 8 s geen nieuw beeld kwam.
try {
    $vast = @(Select-String -LiteralPath $log -SimpleMatch -Pattern '[FREEZE8] =====' -ErrorAction SilentlyContinue)
    if ($vast.Count -gt 0) {
        Write-Host ''
        Write-Host "Freeze detector: $($vast.Count) report(s) this session. Full report:" -ForegroundColor Yellow
        Write-Host "  $log" -ForegroundColor Yellow
    }
} catch { }
if ($code -ne 0) {
    Write-Host ''
    Write-Host ("The game stopped with exit code {0} (0x{0:X8})." -f $code) -ForegroundColor Red
    $staart = @(Get-Content -LiteralPath $log -Tail 4 -ErrorAction SilentlyContinue)
    if ($staart.Count -gt 0) { Write-Host 'Last log lines:'; $staart | ForEach-Object { Write-Host "  $_" } }
    if (Select-String -LiteralPath $log -SimpleMatch -Pattern '[GL] fatal' -Quiet -ErrorAction SilentlyContinue) {
        Write-Host 'OpenGL failed: check your graphics driver (or restart your PC).' -ForegroundColor Yellow
    }
    Write-Host "Full log: $log"
    exit 1
}
