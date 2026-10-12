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

### 3D pohled – krok 3

- Tlačítko **3D** vedle „Výhled vpřed“ přepíná u vlaků mezi mapou a 3D pohledem (platí pro všechna okna Kabina,
  stejně jako „Výhled vpřed“). U ostatních vozidel je zašedlé. Ve 3D jsou zašedlé zoom a „Výhled vpřed“.
- `src/cab_view_3d.cpp` – vlastní softwarový renderer, žádné OpenGL: trojúhelníky s perspektivně správnými
  texturami a z-bufferem, vyplňování po řádcích ve více vláknech (prokládané pruhy po 4 řádcích). Hotový obraz
  se zapíše přímo do obrazovky (32bpp blittery), u 8bpp přes sprite (`GfxBlitEncodedSprite` v `gfx.cpp`).
- **Terén**: výšky a svahy dlaždic, boky základů, voda. Textura země každé dlaždice je složená ze **spritů hry**
  (tráva, pole, štěrk pod kolejí, silnice, voda…) – sbírá je `CollectTileGroundSprites` ve `viewport.cpp`, který
  spustí kreslicí funkci dlaždice „naprázdno“. Sprity se berou v plném rozlišení a vzorkují hladce (bilineárně,
  4 úrovně mipmap), takže na Retině nejsou vidět kostičky. Stejně vypadající dlaždice sdílí jednu texturu.
- Zem pod domy, průmyslem a objekty má ve hře černé místo pro budovu – to se doplní z okolní země, dokud
  v kroku 4 nepřibudou 3D budovy.
- **Koleje**: pražce do 10 dlaždic a kolejnice do 24 dlaždic jako skutečná geometrie (zblízka ostré), dál už jen
  štěrk v textuře; vzdálené kolejnice postupně splývají se štěrkem, aby netvořily ostré čáry přes celý obraz.
  Rovné, oblouky, výhybky, koleje ve stanici a na přejezdu.
- **Nádraží** (doplněno po zkoušce na Macu, kde ve stanici chyběla nástupiště): nástupiště, budovy a střechy
  železničních stanic jsou kvádry, podle kterých je hra řadí v mapě (`CollectTileGroundSprites` je teď vrací
  s jejich sprity), a na stěny kvádru se promítne sprite tak, jak ho kreslí mapa. Stěny, které izometrický pohled
  neukazuje, dostanou obrázek protější stěny; kde je sprite průhledný, je průhledný i kvádr (sloupy, okraje).
  Trolejové vedení se vynechává (zatím jen tenké čáry). Funguje i pro stanice z NewGRF. Budova nádraží je zatím
  jen kvádr se střechou namalovanou na stěnách – skutečné tvary budov jsou úkol kroku 4.
- Opar od 14 dlaždic, dohled 56 dlaždic, obloha s přechodem do oparu.
- **Kamera** na čele vlaku ve výšce střechy (8 jednotek), 6° dolů, zorný úhel 64°. Dotáčí se podle ujeté dráhy
  (ne podle času), takže v zatáčce stíhá i ve 250 km/h. V depu a v tunelu je obraz černý.
- **Plynulost**: vlak se ve hře posouvá po celých krocích (1/16 dlaždice), v 90 km/h jen asi každý třetí herní
  tik, a herní tiky (27 ms) přicházejí jinak než snímky obrazovky, často pozdě nebo po dvou (když kreslení
  trvá dlouho). Kamera proto bere i rozjetý krok (`progress`), pamatuje si posledních 8 tiků a obraz ukazuje
  okamžik asi 1,5 tiku pozadu. Ten běží podle hodin a podle tiků se mění jen jeho rychlost (nejvýš o 10 %),
  takže se pohyb netřese (dřív se okamžik dorovnával skokem k tikům a blízké pražce se chvěly).
  Výška se na svazích vyhlazuje podle ujeté dráhy (ve hře stoupá po skocích).
- Překresluje se při pohybu kamery na každý snímek obrazovky (60×), když obrázek zabere nejvýš 65 % času mezi
  snímky. Jinak se obrázek zmenší (až na 50 % v každém směru) a na obrazovku se hladce zvětší; když nestačí ani
  to, kreslí se na každý druhý (třetí…) snímek, rovnoměrně. Pomalejší kreslení každý druhý snímek na Macu
  cukalo. Když vlak stojí, překresluje se jednou za sekundu.
- **Měření**: hru spustit s `-d misc=3`, do logu se pak u každého snímku píše čas přípravy, vyplnění a výstupu
  a změny tempa a velikosti obrázku; s `-d misc=4` i čas a poloha kamery každého snímku (na kontrolu plynulosti).
  Linux VM se 4 jádry, okno 1268×620 px (rozhraní 2×): příprava ~3 ms, vyplnění ~12–15 ms, výstup ~2 ms, tedy
  30 snímků/s; v menším okně 60 snímků/s s obrázkem na 60 %. MacBook Air (Retina), výchozí velikost okna:
  ~7 ms na obrázek (okno „Počet snímků za sekundu“, řádek „Vykreslování grafiky“), tedy 60 snímků/s.
- Zatím chybí (krok 4): budovy, návěstidla, trolejové vedení, stromy, vozidla, mosty a portály tunelů (vlak zatím vjede
  „do kopce“), silnice jen jako textura země. Pravoúhlé zatáčky jsou ve hře velmi ostré, takže v nich kolej na
  okamžik zmizí ze záběru; svahy jsou strmé (50 %) stejně jako ve hře.

## Další kroky

1. Mac: hotovou aplikaci sestavuje u každého PR workflow `cab-view-macos-app.yml` (artefakt
   `openttd-cab-view-macos-arm64`, stáhnout ze stránky běhu na GitHubu, rozbalit, spustit). Na MacBooku Air
   3D pohled běží v plném rozlišení Retiny.
2. Krok 4: doplnit 3D svět – budovy, návěstidla, stromy, ostatní vozidla, mosty a portály tunelů; doladit
   plynulost na Retině.
3. Případně: výhled podél silnice i pro silniční vozidla (`CFollowTrackRoad`), další údaje na palubní desce
   (návěst, sklon), klávesová zkratka pro otevření kabiny.

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
  Pro kopcovitou mapu (3D) stačí v `test.cfg` změnit `terrain_type = 3` (a třeba `generation_seed`).
  AI staví trať jinde při každém spuštění.
  Pozor: `kill` hru neukončí, je potřeba `pkill -9 openttd`, jinak běží dvě hry přes sebe.
- Ovládání: `python3 x.py click X Y`, `drag X1 Y1 X2 Y2`, `key F1`, `wheel X Y N`…; snímek obrazovky `import -display :99 -window root shot.png`
  nebo v konzoli hry příkaz `screenshot`. Konzole: `scrollto <tile>`, `pause`.
