# Pohled z kabiny – poznámky k vývoji

Tahle složka není součást hry, jsou to jen pomůcky pro vývoj a testování větve `cab-view`.

## Stav (12. 10. 2026)

Větev `cab-view` vychází z oficiálního tagu **15.3**. Jeden commit „Feature: Cab view window…“:

- `src/cab_view_gui.cpp`, `src/widgets/cab_view_widget.h` – nové okno „Kabina“ (třída `CabViewWindow`, `WC_VEHICLE_CAB_VIEW`).
  - Viewport sleduje vozidlo a dívá se dopředu ve směru jízdy (`v->direction` promítnutý na obrazovku), čím rychleji, tím dál (30–60 % poloviny výřezu), s plynulým dojezdem (časová konstanta 650 ms).
  - Palubní deska: rychlost velkým písmem + ukazatel, max. rychlost, další zastávka, stav (jede / stojí / nakládá / porucha…), naloženost.
  - Tlačítka: přiblížit, oddálit, „Výhled vpřed“ (přepínač), Rozjet/Zastavit.
- `ViewportData` dostala `follow_offset_x/y` (posun kamery od sledovaného vozidla), použito v `UpdateViewportPosition`.
- Okno vozidla má nové tlačítko pod „detaily“ (`WID_VV_CAB_VIEW`), zatím s ikonou `SPR_CENTRE_VIEW_VEHICLE` – ověřit, jak vypadá.
- Okno se zavírá při zániku vozidla a přečísluje při autoreplace.
- Texty v angličtině a češtině.

**Zkompilováno jen `cab_view_gui.cpp` samostatně, celá hra ještě nedoběhla a nic není otestované ve hře.**

## Další kroky

1. Dokončit build, spustit test (níže), vyfotit obrazovky, doladit vzhled palubní desky a ikonu tlačítka.
2. Vyzkoušet: zatáčka, otočení vlaku v koncové stanici, nakládání, zoom kolečkem, zavření při prodeji vozidla, silniční vozidlo/loď/letadlo.
3. Sestavit pro Mac (MacBook Air, Retina, rozhraní 2×).

## Testovací prostředí (cloud, Ubuntu 24.04)

Síť je omezená: apt (archive.ubuntu.com), PyPI i cdn.openttd.org jsou blokované, GitHub funguje.

- **SDL2**: runtime je v systému, chybí hlavičky → zkompilovat SDL 2.30.0 ze zdroje
  (`github.com/libsdl-org/SDL/releases/download/release-2.30.0/SDL2-2.30.0.tar.gz`), nainstalovat do vlastního prefixu
  a OpenTTD konfigurovat s `-DCMAKE_PREFIX_PATH=<prefix>`.
- **OpenGFX** (grafika): na GitHubu nejsou binárky → postavit z `github.com/OpenTTD/OpenGFX` (tag 8.0) příkazem `make bundle_tar`.
  Potřebuje `nmlc` (klon `github.com/OpenTTD/nml` + `github.com/dabeaz/ply`, spouštět přes PYTHONPATH; v klonu nml vytvořit tag, aby znal verzi)
  a `grfid` (z `github.com/OpenTTD/grfcodec`, cíl `grfid`). Výsledný `opengfx-8.0.tar` do `~/.local/share/openttd/baseset/`.
- **python-xlib** pro klikání do Xvfb: klon `github.com/python-xlib/python-xlib` (cesta je natvrdo v `x.py`).

## Test

- `CabTestAI/` zkopírovat do `~/.local/share/openttd/ai/`, `game_start.scr` do `~/.local/share/openttd/scripts/`
  (spustí AI při startu hry). AI najde rovné místo u města, postaví trať do L (stanice – rovinka – zatáčka – rovinka – stanice),
  depo, koupí lokomotivu + 3 osobní vozy a pustí vlak. Do logu (`-d script=4`) píše dlaždice stanic a polohu vlaku.
- Spuštění: `Xvfb :99 -screen 0 1600x1000x24 &` a pak
  `DISPLAY=:99 ./openttd -c test.cfg -g -s null -m null -d script=4`.
  `test.cfg` = plochá mapa 256×256, rok 1995, čeština, bez poruch, vysoký úvěr.
- Ovládání: `python3 x.py click X Y`, `key F1`, `wheel X Y N`…; snímek obrazovky `import -display :99 -window root shot.png`
  nebo v konzoli hry příkaz `screenshot`. Konzole: `scrollto <tile>`, `pause`.
