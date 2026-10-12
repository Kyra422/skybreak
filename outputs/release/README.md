# Skyward Sword PC bouwen vanaf je eigen schijf

`Bouw-SkywardSword.ps1` maakt van **jouw eigen** schijf van *The Legend of Zelda: Skyward Sword* een
zelfstandige Windows-map `SkywardSwordPC\` met `SkywardSwordPC.exe`, een instellingenbestand en een
Nederlandse `LEESMIJ.txt`. Het script controleert de schijf, pakt hem uit, vertaalt de spelcode naar C++ en
compileert die lokaal (roadmap M7.1/M7.2).

> **Juridisch.** Deze map (`outputs/release`) bevat alleen hulpmiddelen. Alles wat uit de schijf komt —
> de uitgepakte bestanden, de gegenereerde C++ en de exe — wordt **alleen lokaal** gemaakt, in `bouw\` en
> `SkywardSwordPC\`. Die twee mappen staan in `.gitignore`. Deel ze niet, upload ze niet en publiceer ze niet.

## Benodigd

| Wat | Waarvoor | Hoe |
|---|---|---|
| Je schijf: **SOUE01, revisie 0** (Amerikaanse versie 1.00) | spelcode en -bestanden | `.iso`, `.wbfs`, `.rvz`, `.gcz`, `.wia` of een al uitgepakte DATA-partitie |
| **DolphinTool.exe** | schijf lezen en uitpakken | zit bij de Dolphin-emulator (map `Dolphin-x64`). Het script zoekt in Program Files, `%LOCALAPPDATA%`, `Downloads` en PATH, anders `-DolphinTool <pad>`. Niet nodig als je een uitgepakte map opgeeft. |
| **Visual Studio 2022** of de **Build Tools 2022** met *Desktopontwikkeling met C++* (MSVC x64) | compileren | gevonden via `vswhere` |
| **CMake 3.20+** en **Ninja** | buildsysteem | het onderdeel *C++ CMake tools for Windows* van Visual Studio, of `py -m pip install cmake ninja`; ook `work\venv` van dit project wordt gebruikt |
| **Python 3.8+** | controles, REL-modules, configuratie | python.org, of `work\venv` van dit project |
| Broncode van **SDL2 2.28.5** en **toml++ 3.4.0** | runtimebibliotheken | uit `work\nwii-build\_deps` van dit project, of `-Bronnen <map>` (met `sdl2-src` en `tomlplusplus-src`). Ontbreken ze, dan haalt CMake ze van GitHub (git nodig); het script meldt dat vooraf. |

Schijfruimte: ca. 4 GB spelbestanden + 3,5 GB werkmap + 0,2 GB exe.

## Gebruik

```powershell
powershell -ExecutionPolicy Bypass -File outputs\release\Bouw-SkywardSword.ps1 -Iso "D:\Spellen\Skyward Sword.iso"
```

of sleep het schijfbestand op `Bouw-SkywardSword.cmd`. Daarna: dubbelklik `outputs\release\SkywardSwordPC\Speel.cmd`.

| Optie | Betekenis |
|---|---|
| `-Uitvoer <map>` | doelmap (standaard `outputs\release\SkywardSwordPC`) |
| `-Werkmap <map>` | tussenbestanden (standaard `outputs\release\bouw`) |
| `-SpelMap <map>` | waar de uitgepakte spelbestanden komen (standaard `<Uitvoer>\game`); bij `-Iso <uitgepakte map>` wordt die map zelf gebruikt |
| `-Draagbaar` | opslag (NAND) in `<Uitvoer>\gebruikersdata\nand` in plaats van `%APPDATA%\SkywardSwordPC\nand` |
| `-Taken <n>` | parallelle compileertaken (standaard: kernen − 2) |
| `-AlleenControle` | alleen hulpprogramma's en schijf controleren |
| `-Opruimen` | werkmap na afloop verwijderen |
| `-DolphinTool`, `-Python`, `-CMake`, `-Ninja`, `-Bronnen` | paden opgeven als het zoeken niet lukt |

### Wat het script doet

1. **Hulpprogramma's**: zoekt alles hierboven en neemt de MSVC-omgeving over (`VsDevCmd.bat`).
2. **Schijf**: `DolphinTool header` moet `SOUE01` en revisie `0` geven. Een PAL-, Japanse of Koreaanse schijf of
   een andere revisie wordt met een duidelijke melding geweigerd.
3. **Uitpakken**: `DolphinTool extract -g` (alleen de DATA-partitie) naar `<SpelMap>`. Daarna wordt de SHA-1
   gecontroleerd van `sys/main.dol` (`450a6806f46d59dcf8278db08e06f94865a4b18a`) en `files/rels.arc`.
   Een geldige uitgepakte map wordt bij een volgende bouw hergebruikt.
4. **REL-modules**: `rels.arc` uitpakken. Alle 636 modules moeten de SHA-1 uit de decompilatie
   (`outputs/skyward-sword/config/SOUE01/config.yml`) hebben.
5. **Vertaler**: bouwt `nwiirecomp.exe` uit `outputs/nwiirecomp`.
6. **Vertalen**: maakt `bouw\recomp_config.toml` uit het profiel `outputs/pc-profile/recomp_config.toml`
   (zelfde hooks, REL-lijst en instellingen; paden relatief aan het configbestand). Vertaalt `main.dol` en de
   REL-modules naar `bouw\export-nieuw`. Kopieert alleen gewijzigde bestanden naar `bouw\export`, zodat een
   nieuwe bouw incrementeel blijft.
7. **Compileren**: CMake + Ninja met dezelfde vlaggen als de ontwikkelbuild (`work\build-pc.cmd`).
8. **Map samenstellen**: `bin\SkywardSwordPC.exe` (+ `.pdb` en `freeze_symbols.txt`), `Speel.cmd`,
   `hulp\Speel.ps1`, `instellingen.toml` (wordt nooit overschreven), `instellingen.standaard.toml`,
   `LEESMIJ.txt` en `VERSIE.txt`.

Logs per stap staan in `bouw\logs\`, met een samenvatting (tijden en groottes) in `samenvatting-*.json`.

## Resultaat

```
SkywardSwordPC\
  Speel.cmd                      starten (opties: -Schaal 1..6, -Volledig, -Venster, -Diagnose)
  instellingen.toml              instellingen van de speler ([graphics], [input], [nand], ...)
  instellingen.standaard.toml    fabrieksinstellingen
  LEESMIJ.txt                    bediening en instellingen
  VERSIE.txt                     bouwdatum, schijf-hash, exe-hash
  bin\SkywardSwordPC.exe         (+ .pdb, freeze_symbols.txt)
  game\                          uitgepakte schijf (of hulp\spelmap.txt wijst ernaar)
  hulp\Speel.ps1
  logs\, screenshots\            ontstaan bij het spelen
%APPDATA%\SkywardSwordPC\nand\   opgeslagen spel (blijft bij een nieuwe bouw bewaard)
```

`SkywardSwordPC.exe <spelmap> <instellingen.toml>` leest `[nand] root` en `[graphics] screenshot_dir`
relatief aan de map van het instellingenbestand, met `%NAAM%`-variabelen (`Config::resolve_path` in
`nWiiRuntime/src/core/config.cpp`). De map kan dus verplaatst worden.

## Relatieve paden in het ontwikkelprofiel (M7.1)

`outputs/pc-profile/recomp_config.toml` heeft geen absolute paden meer. Paden zijn relatief aan de projectmap
(`work/pc-game`, `outputs/pc-prototype`, ...). `nwiirecomp` zoekt een relatief pad eerst t.o.v. de map van het
configbestand, dan t.o.v. de bovenliggende mappen, en anders t.o.v. de huidige map
(`nWiiRecomp/src/main.cpp`, `resolve_config_path`). Zo werken het profiel zelf, kopieën ervan elders in het
project (regen_check, reldiff, ...) en oudere generatorbuilds (die met cwd = projectmap draaien).

## Tests

`outputs/pc-tests/run-all.cmd --only pkg10` draait:
- `pkg10/build-config-paths-test.cmd`: runtimepaden, plus een negatieve controle;
- `pkg10/test_generator_paths.py`: generatorpaden, met een ontbrekend pad als negatieve controle;
- `pkg10/test_release_tools.py`: hulpscript en `-AlleenControle`, met PAL-schijf, revisie 2 en gewijzigde
  main.dol als negatieve controles.
