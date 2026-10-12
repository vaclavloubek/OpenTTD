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

### Doplněno 12. 10. 2026 (větev `claude/cab-view-stage1-ricdkr`)

- Celá hra se sestaví bez chyb a varování a okno je **otestované ve hře** (rozhraní 1× i 2×): sledování vlaku,
  výhled vpřed, otočení v koncové stanici, nakládání, zoom tlačítky i kolečkem, přepínač „Výhled vpřed“.
- Palubní deska má **ručičkový tachometr** (kreslený čarami, takže je ostrý na Retině) vedle digitální rychlosti;
  sloupce se počítají podle obsahu, takže se rychlost na 1× už neusekává.
- Tlačítko v okně vozidla má **vlastní ikonu** (čelo lokomotivy), kreslenou kódem, místo kopie ikony „vycentrovat“.
- Vlak se **dívá dopředu podél koleje** (`GetTrackLookAheadTarget`): v zatáčce kamera nahlíží za roh, na konci trati
  se nedívá do prázdna, na výhybce jde po rezervované cestě, jinak se zastaví. Ostatní vozidla dál podle směru jízdy.

- Vyzkoušeno i u **autobusu, lodi a letadla** (výhled vpřed podle směru jízdy funguje u všech) a **zavření okna
  při prodeji vozidla** (okno kabiny i okno vozidla se zavřou, hra běží dál).
- Tachometr má maximální rychlost vozidla na 85 % stupnice a za ní červené pole, takže vlak nebo letadlo
  v plné rychlosti nemá ručičku trvale v červeném.
- **macOS arm64** (Release i Debug) se v GitHub CI sestaví a projde testy. Windows CI padalo na staré verzi
  knihovny breakpad z vcpkg; baseline vcpkg je převzatá z upstreamu (commit 935674800).

## Další kroky

1. Vyzkoušet u sebe na MacBooku (Retina, rozhraní 2×).
2. Případně: výhled podél silnice i pro silniční vozidla (`CFollowTrackRoad`), další údaje na palubní desce
   (návěst, sklon), klávesová zkratka pro otevření kabiny.
3. Etapa 2: jednoduchý 3D pohled.

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
  depo, koupí lokomotivu + 3 osobní vozy a pustí vlak. Pak postaví silnici do L s autobusem, kanál s bójkami a lodí
  a dvě letiště s letadlem; autobus po 150 dnech prodá (test zavření okna). Do logu (`-d script=4`) píše dlaždice
  stanic, čísla vozidel (`TRAIN`, `BUS`, `SHIP`, `PLANE`) a polohu vlaku.
- Spuštění: `Xvfb :99 -screen 0 1600x1000x24 &` a pak
  `DISPLAY=:99 ./openttd -c test.cfg -g -s null -m null -d script=4`.
  `test.cfg` = plochá mapa 256×256, rok 1995, čeština, bez poruch, vysoký úvěr.
- Okno Kabina u vozidla AI: v nástrojové liště „Vlaky“ (nebo silniční vozidla, lodě, letadla) → v rozbalovací nabídce
  druhá společnost („Cab Test Rail“) → klik na vozidlo v seznamu → tlačítko s lokomotivou v okně vozidla. (Klik přímo na vlak v mapě v Xvfb nefungoval.)
- `test.cfg` nastavuje `language` v `[misc]`; velikost rozhraní se mění tamtéž (`gui_scale = 100` / `200`).
  Pozor: `kill` hru neukončí, je potřeba `pkill -9 openttd`, jinak běží dvě hry přes sebe.
- Ovládání: `python3 x.py click X Y`, `key F1`, `wheel X Y N`…; snímek obrazovky `import -display :99 -window root shot.png`
  nebo v konzoli hry příkaz `screenshot`. Konzole: `scrollto <tile>`, `pause`.
