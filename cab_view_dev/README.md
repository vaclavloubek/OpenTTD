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
- Zem rovné koleje (osa X nebo Y) se vyhladí podél koleje (o 2 jednotky, `SmoothAlong`): v izometrických spritech
  jsou okraje štěrku schody z pixelů (2:1), které by ve 3D tvořily zuby ±0,25 jednotky do stran, a při jízdě se
  okraje kolejového lože třásly ze strany na stranu (na Macu ve zvětšeném okně, ~4–6 px za snímek v okně 1268 px).
- **Nádraží** (doplněno po zkoušce na Macu, kde ve stanici chyběla nástupiště): nástupiště, budovy a střechy
  železničních stanic jsou kvádry, podle kterých je hra řadí v mapě (`CollectTileGroundSprites` je teď vrací
  s jejich sprity), a na stěny kvádru se promítne sprite tak, jak ho kreslí mapa. Stěny, které izometrický pohled
  neukazuje, dostanou obrázek protější stěny; kde je sprite průhledný, je průhledný i kvádr (sloupy, okraje).
  Trolejové vedení se vynechává (zatím jen tenké čáry). Funguje i pro stanice z NewGRF. Budova nádraží je zatím
  jen kvádr se střechou namalovanou na stěnách – skutečné tvary budov jsou úkol kroku 4.
- Opar od 14 dlaždic, dohled 56 dlaždic, obloha s přechodem do oparu.
- **Kamera** na čele vlaku ve výšce oken kabiny (4 jednotky nad kolejí, asi 2,5 m), 6° dolů, zorný úhel 64°. Dotáčí se podle ujeté dráhy
  (ne podle času), takže v zatáčce stíhá i ve 250 km/h. V depu je obraz černý.
  Na svahu a nájezdu mostu se pohled naklání se sklonem koleje kolem čela vlaku, ale jen napůl
  (`CAB_VIEW_3D_TILT`): bez naklonění vypadal strmý nájezd z oken jako zeď, s plným nakloněním kamera na
  vrcholu nájezdu koukala do nebe.
- **Plynulost**: vlak se ve hře posouvá po celých krocích (1/16 dlaždice), v 90 km/h jen asi každý třetí herní
  tik, a herní tiky (27 ms) přicházejí jinak než snímky obrazovky, často pozdě nebo po dvou (když kreslení
  trvá dlouho). Kamera proto bere i rozjetý krok (`progress`), pamatuje si posledních 8 tiků a obraz ukazuje
  okamžik asi 1,5 tiku pozadu. Ten běží podle hodin a podle tiků se mění jen jeho rychlost (nejvýš o 10 %),
  takže se pohyb netřese (dřív se okamžik dorovnával skokem k tikům a blízké pražce se chvěly).
  Výška oka se bere z koleje pod čelem vlaku (`GetTrackHeight`; hra dává výšku středu vlaku, která na svahu
  a nájezdu mostu zaostává) a vyhlazuje se podle ujeté dráhy na 1,5 jednotky.
- Překresluje se při pohybu kamery na každý snímek obrazovky (60×), když obrázek zabere nejvýš 65 % času mezi
  snímky. Jinak se obrázek zmenší (až na 50 % v každém směru) a na obrazovku se hladce zvětší; když nestačí ani
  to, kreslí se na každý druhý (třetí…) snímek, rovnoměrně. Pomalejší kreslení každý druhý snímek na Macu
  cukalo. Když vlak stojí, překresluje se jednou za sekundu.
- Při 3D pohledu okno Kabina nemá viewport mapy (zruší se a při návratu na mapu se vytvoří znovu se stejným
  přiblížením). Skrytý viewport za 3D pohledem dál sledoval vlak, a když se viewport posune, hra posune i jeho
  pixely na obrazovce: hotový 3D obrázek se tak na 2 z 5 snímků ukázal posunutý o 8–12 px šikmo (2:1), což byl
  hlavní zdroj třesení „ze strany na stranu“ na Macu. Interní snímky rendereru to neukázaly; našel to až záznam
  obrazovky (na Linuxu `ffmpeg -f x11grab`, posun vzdálené části obrazu mezi snímky).
- **Měření**: hru spustit s `-d misc=3`, do logu se pak u každého snímku píše čas přípravy, vyplnění a výstupu
  a změny tempa a velikosti obrázku; s `-d misc=4` i čas a poloha kamery každého snímku (na kontrolu plynulosti).
  Linux VM se 4 jádry, okno 1268×620 px (rozhraní 2×): příprava ~3 ms, vyplnění ~12–15 ms, výstup ~2 ms, tedy
  30 snímků/s; v menším okně 60 snímků/s s obrázkem na 60 %. MacBook Air (Retina), výchozí velikost okna:
  ~7 ms na obrázek (okno „Počet snímků za sekundu“, řádek „Vykreslování grafiky“), tedy 60 snímků/s.
- Zatím chybí (krok 4): budovy, návěstidla, trolejové vedení, stromy, vozidla, mosty a portály tunelů (vlak zatím vjede
  „do kopce“), silnice jen jako textura země. Pravoúhlé zatáčky jsou ve hře velmi ostré, takže v nich kolej na
  okamžik zmizí ze záběru; svahy jsou strmé (50 %) stejně jako ve hře.

### 3D pohled – krok 4

- **Domy, průmysl, objekty a budovy nádraží** jsou skutečné 3D tvary. Box, podle kterého hra řadí sprite, bývá
  mnohem větší než budova, proto se tvar hledá ve spritu (`GetBuildingShape`, výsledek se pamatuje pro každý sprite):
  1. Kvádr (u nádraží i se střechou skloněnou na jednu stranu) se přiloží k obrysu spritu tak, aby v něm bylo co
     nejvíc neprůhledných pixelů a co nejméně průhledných – mění se vždy jedno číslo (rohy půdorysu, výška), dokud
     to pomáhá, z několika začátků (nejnižší bod spritu jako jižní roh, nejnižší přímky spodních hran, celý box).
  2. Výška stěn se pak čte ze svislých hran u západního a východního rohu, střecha (valbová, sedlová podél osy x
     nebo y, nebo plochá) je ta, která nejlépe sedí na vršek spritu – jen když sedí znatelně lépe než plochá
     (jinak jde o atiku nebo něco na střeše) a není strmější než 2,5 : 1 na polovinu rozpětí (dřív vznikaly špice).
  3. Výřez otočený ke kameře je jen les a jiné rostliny, štíhlé věci (sila, komíny: výška > 7× užší strana pod 6)
     a sprity, kterým žádný tvar nesedí a nad tvarem je velký kus obrázku (statek pod velkým stromem). Výřez
     stojí na zemi uprostřed boxu, takže zadní část nevisí ve vzduchu.
  Každý bod stěny i střechy dostane pixel, který tam kreslí hra, takže namalovaná střecha leží na střeše. Výšky
  jsou 0,6× výšky ve spritu (izometrie budovy převyšuje), jako v návrhu C, i u budov nádraží. Nejnižší pás stěny
  bere obrázek těsně nad ním, aby spodní hrana nebyla zubatá a aby zakryl, co pod budovou kreslí sprite země.
  Na Linuxu trvá rozpoznání jednoho spritu kolem 1 ms (nejvýš 2 ms).
  Budova výchozího nádraží má tvar daný ručně (stěny do 2/3 boxu, sedlová střecha podél nástupiště až
  k vršku boxu, pod ní nízké nástupiště), protože stojí na nástupišti a prokládání ji dělalo „šikmou deskou“.
  U ostatních budov nádraží střecha skloněná na jednu stranu začíná aspoň v polovině výšky.
- **Návěstidla** jsou skutečná 3D: šedý sloupek (6 jednotek) s černou hlavou otočenou k vlakům, které zastavuje,
  na místě a straně trati, kde je kreslí hra (`DrawSignals`, nastavení strany návěstidel). Červené a zelené
  světlo svítí podle stavu návěstidla ve hře, zhasnuté je tmavé. Mechanická návěstidla mají červené rameno
  od trati – vodorovné = stůj, zvednuté šikmo = volno – a pod ním světlo v černém pouzdře. Hlava i pouzdro
  zakrývají světla zezadu.
- **Stromy** (a jiné tenké věci jako sloupy trolejí) jsou výřezy otočené ke kameře (0,28 jednotky na pixel spritu),
  bez rozšiřování neprůhledné oblasti, aby zůstaly štíhlé.
- **Ostatní vlaky a silniční vozidla** jsou kvádry podél směru jízdy, potažené spritem vozidla jedoucího na
  jihozápad (délka z hry, šířka a výška ze spritu). Vozidlo, na kterém je kamera, se vynechává.
- Budovy, stromy, návěstidla a vozidla se kreslí do 32 dlaždic.
- **Ploty podél kolejí** (hra je staví u kolejí, kde soused není dráha té společnosti) jsou sloupky po 4 jednotkách
  s horním a dolním břevnem, 2 jednotky vysoké (polovina výšky oka), v průměrné barvě spritu plotu (tedy v barvě společnosti), kousek
  od okraje dlaždice; úhlopříčné ploty u kusů kolejí v rohu vedou přes úhlopříčku. Sprite plotu je v OpenGFX
  pletivo, které ve 3D dávalo zubatou zeď, proto geometrie (`AddRailFence`, `AddPostFence`). Kreslí se do
  12 dlaždic, dál by sloupky byly tenčí než pixel.
- **Ploty polí** (živé ploty, kvetoucí keře, kamenné zídky, dřevěné ohrady) jsou svislé výřezy ze spritu podél
  strany pole, 0,6 jednotky uvnitř dlaždice. Kde plot stojí a jak je vysoký, se změří ze spodku spritu
  (`FitFence`: ze sloupců vyjde stejná přímka i na svahu; bere se nejnižší desetina, protože mezi sloupky je
  spodek břevno nad zemí). Obrys se průměruje podél plotu jako u boků mostů, barvy listí a kamenů ale zůstávají
  ostré (`GetSideTexture` s `detail`); plot o kousek kratší než dlaždice se natáhne, aby na sebe navazovaly.
- Testovací AI staví dvě dvojitá návěstidla na trati a vedle ní druhou rovnou trať s vlakem (na zkoušku
  návěstidel spustit novou hru s AI; vlak #1 v `kabina3d.sav` kolem žádných neprojíždí).
- **Mosty**: mostovka je deska tlustá 2 jednotky, přes dlaždici široká jako ve hře (2 až 14 napříč), nahoře
  obrázek podlahy mostu se sprity hry (bližší polovina dostane zrcadlově obrázek vzdálenější, protože ve hře ji
  zakrývá bok mostu), na ní 3D kolej. Části mostu nad dlaždicí pozná `CollectTileGroundSprites` podle prázdného
  plochého boxu, kterým hra kreslení mostu začíná (`BoxSpriteToDraw::bridge`). Boky (zábradlí, příhradové
  nosníky, oblouky) jsou svislé výřezy ze spritu bližšího boku; kde stojí, se změří ze spodního okraje spritu
  (`FindBridgeSide`, medián přes sloupce), vzdálenější bok je zrcadlo bližšího. Šikmé čáry podél mostu jsou ve
  spritu schody pixelů, které by ve 3D byly zuby; průhlednost se proto průměruje podél mostu přes celý schod
  a o čtvrtinu zesílí, aby tenké čáry zůstaly (`GetSideTexture`). Výšky boků jsou 0,6× jako u budov.
  Pod mostovkou jsou dva pilíře s příčníkem v barvě pilířů hry, jen nad volnou zemí a stromy (ne nad kolejí,
  silnicí nebo vodou, kde by stály v cestě). Nájezd je šikmá deska stejné šíře na pilířích u mostu (ve hře je to
  taky jen deska), kolej na něm stoupá. Silniční mosty jsou stejné, jen bez koleje.
- **Akvadukty** jsou z kamene (světle šedé): mostovka s vodou ze spritu, podél ní zídky 2 jednotky vysoké,
  pilíře i nájezdy v barvě kamene (pilíře ze spritu hry jsou tmavé, akvadukt pak vypadal jako černý trám).
- **Pod mostem**: hra pouští vozidla pod most o úroveň výš (spodek mostovky je 6 jednotek nad kolejí). Oko
  bylo dřív na střeše vlaku (8 jednotek) a pod mostem se snižovalo, takže to na Macu vypadalo, že kamera
  naráží do mostu, akvaduktu i horní hrany tunelu. Teď je oko trvale ve výšce oken (4 jednotky) a pod mosty
  projede bez snižování. Nástupiště jsou proto poloviční výšky a ploty nižší, jinak z oken vypadaly jako zdi.
- **Tunely**: vjezd je kamenný portál s obloukem, světlým věncem kolem otvoru, tmou uvnitř a opěrnými zdmi
  podél koleje; vrchol oblouku je 7,5 jednotky nad kolejí, pod hranou kopce (8) (dřív byl nad ní a v otvoru byl
  vidět pruh trávy); kopec nad vjezdem se ve 3D neklesá, kolej vede rovně do portálu (`SetTunnelGround`,
  `AddTunnelPortal`). Když je kamera v ose tunelu (v něm nebo na koleji před ním), místo tmy v otvoru je
  vidět dovnitř: tmavá klenba po 4 jednotkách střídavě tmavší (aby bylo vidět, že ubíhá), kolej a pražce,
  u obou konců světlejší od denního světla, a protože tunely jsou ve hře rovné, na konci světlý výjezd
  s krajinou za ním (`AddTunnelInside`). Vlak v tunelu má obraz normální, ne černý.
- **Přejezdy**: u silnice před a za kolejí stojí čtyři sloupky s bílým křížem a dvěma světly směrem k silnici;
  když hra přejezd zavře, svítí červeně (`AddCrossing`). Silnice jinak zůstávají texturou země.
- Testovací AI teď navíc staví kopec s tunelem, dva železniční mosty přes silnice, přejezd, silniční most přes
  trať (kde je místo) a akvadukt přes trať mezi dvěma uměle navršenými svahy.
- Na Macu (13. 10.) byly vidět les jako zelený kvádr, sila
  jako šedé „šipky“ a budova nádraží jako vysoký sloup; opraveno novým rozpoznáním tvaru výše. Pak budova
  nádraží jako šikmá deska a návěstidla jako tři zelené obrázky nad sebou; opraveno tvarem nádraží a 3D návěstidly.

### 3D pohled z letadla

- Tlačítko **3D** funguje i u letadel a vrtulníků. Kamera je v kabině ve **skutečné herní výšce** letadla
  (`z_pos` + 3 jednotky, na dráze tedy stejně vysoko jako u vlaku): rozjezd po dráze, stoupání, let, klesání a přistání.
  Ve hře letadla stoupají i klesají strmě (o jednotku výšky na jednotku cesty, stoupání ze země do letové výšky
  trvá asi 2 s), 3D pohled to ukazuje tak, jak to je.
- Čím výš letadlo letí, tím víc se pohled dívá dolů (míří na zem asi 20 dlaždic před sebou, nejvýš o 0,5 rad
  navíc k obvyklým 6°). Stoupání a klesání pohled naklání jen trochu (nejvýš 0,2 rad), jinak by se při vzletu
  díval jen do nebe. Výška země pod letadlem se vyhlazuje na 2 dlaždice, aby pohled nekýval nad kopci.
- Opar a dohled se posouvají o výšku nad zemí (nejvýš o 15 dlaždic), protože i zem přímo pod letadlem je daleko
  od oka; do strany se berou i dlaždice, které z výšky vidět jsou (dřív v dolních rozích chyběly).
- Na letištích se světla dráhy (sprity v prázdném boxu) vynechávají, postavená jako výřezy vypadala jako hromádky.
  Hangáry hra kreslí jako přední a zadní stěnu v tenkých boxech; tvar budovy se proto hledá v celé dlaždici.
  Zblízka (letadlo stojí u terminálu nebo hangáru) jsou budovy letiště rozmazané, jejich sprity jsou malé.
- **Ostatní letadla** se ve 3D (z vlaku i z letadla) kreslí ve své herní výšce jako výřez otočený ke kameře,
  se spritem letadla z té strany, ze které ho kamera vidí (1 z 8 směrů). Délka letadla letícího od mapy je ve
  spritu nahoru, proto se výřez zespodu svisle zmenšuje (jinak by letadlo letící pryč stálo jako sloup).
  Stíny a rotory vrtulníků se nekreslí.
- Uložená hra na zkoušku: `kabina3d_letadlo.sav` (kopcovitá mapa, letadlo #1 druhé společnosti „Cab Test Rail“).

### 3D pohled ze silničních vozidel

- Tlačítko **3D** funguje i u autobusů, nákladních aut a tramvají. Oko je 3 jednotky nad silnicí (o kus níž než ve
  vlaku) a jede přesně po dráze vozidla, ne před ním: silniční vozidla zatáčejí na křižovatkách ostře a čelo by
  se přehouplo přes chodník do domů. Pohled se dotáčí na kratší dráze než u vlaku (3 jednotky místo 6).
  Uprostřed odbočení (vozidlo míří na sever, východ, jih nebo západ) se kamera dívá rovnou do ulice, do které
  vozidlo odbočuje, jako řidič, a ne šikmo do rohového domu.
- **Silnice jsou 3D** (`AddRoad`, `GetRoadLook`), pro typy silnic ze základní grafiky (sprity jsou známé):
  - **Chodníky** městských silnic (okraj silnice dlážděný, s lampami nebo stromy) jsou o 0,3 jednotky vyvýšené,
    s obrubníkem. Jsou to kusy země dlaždice zvednuté nahoru, takže si nesou svůj obrázek ze hry. Šířka 3 jednotky
    od kraje dlaždice, jak je kreslí grafika. Rovné kusy a křižovatky se dělí na 3 × 3 pole (vozovka uprostřed a ke
    stranám se silnicí), zatáčky jsou oblouky kolem rohu dlaždice (malý chodník v rohu, velký kolem).
  - **Dělicí čára** uprostřed je ostrá geometrie (čárky 1,6 dlouhé po 4 jednotkách, v zatáčce tři čárky po oblouku).
    Rozmazanou čáru ze spritu (barva 15 palety) textura vynechá a doplní z okolí, na křižovatkách se nekreslí žádná.
  - Zem rovné silnice se vyhlazuje podél ní jako u kolejí.
  - **Tramvajové koleje** jsou kolejnice v silnici (jedna kolej uprostřed, v zatáčce po oblouku); tam není dělicí čára.
    Ve hře bez NewGRF tramvaje nejsou, takže jsou vyzkoušené jen naoko (koleje na všech silnicích).
  - **Zastávky** (průjezdné, ze základní grafiky) mají chodníky a čáru jako silnice a místo spritů na chodnících
    stojí přístřešky: autobusové se skleněnou střechou a zadní stěnou, sloupky a lavičkou, pro nákladní auta malá
    cihlová budka. U každého stojí sloupek s cedulí v barvě společnosti (`AddRoadStop`).
  - **Silniční tunely** mají uvnitř silnici s čárou místo kolejí a pražců.
- Ostatní silniční vozidla jsou plné kvádry (bez vyřezávání průhledných míst): zezadu se v nich jinak dělaly díry
  mezi střechou a zádí. Jsou 1,6× vyšší a širší než podle spritu (nejvýš 4 jednotky široká), aby střecha autobusu
  byla nad okem.
- Testovací AI `CabRoadAI/`: dva autobusy (různým směrem) mezi zastávkou u středu největšího města, zastávkou na
  druhém konci města a zastávkou na nové silnici ven z města; ta vede tunelem kopcem (AI ho navrší) a přes železniční
  přejezd. Spustit jako `CabTestAI` (zkopírovat do `ai/`, v `game_start.scr` `start_ai CabRoadAI`), nejlépe s
  `larger_towns = 1` a `initial_city_size = 4` v `[economy]`, aby bylo velké město. Do logu píše `BUS <číslo>`.

### 3D pohled z lodi

- Tlačítko **3D** funguje i u lodí. Oko je na přídi můstku (3 jednotky před středem lodi), 4 jednotky nad vodou.
  Směr pohledu se řídí tím, jak je loď otočená (`rotation`): loď se ve hře otáčí na místě po 45° každých 8 tiků
  a pohled se s ní dotáčí, i když stojí.
- **Voda leží o jednotku níž než země** (`CAB_VIEW_3D_WATER_DROP`): kanály, řeky (i na svahu), moře, depa, konce
  plavebních komor, bójky, vodní část přístavů a ropné plošiny. Kde voda sousedí se zemí, je svislý břeh
  (`AddBanks`); hrany kanálů a řek namalované ve spritu vody se vynechávají, ve 3D ležely jako proužek na hladině.
  Pláže (pobřežní svahy) se rohy u vody svažují pod hladinu, takže moře nemá před pláží schod. Věci na vodě
  (bójky, depa) jsou posunuté s ní. Akvadukty mají vodu ve výšce mostovky jako dřív.
- **Plavební komory** (`AddLock`): prostřední díl je komora s kamennými zdmi po stranách (od dna až nad horní
  břeh), dole kamenný práh a nahoře vrata (dvě dřevěná křídla s lávkou), dokud je voda v komoře níž než nad ní,
  a dolní vrata, když je výš než pod ní. Hladina v komoře je ve výšce lodi, která v ní je (loď ve hře stoupá
  o jednotku každých 8 tiků; u vlastní lodi jde hladina s plynule stoupajícím okem, i když loď teprve vplouvá
  z dolního nebo horního dílu), jinak dole. Sprity zdí komory hra kreslí v tenkých boxech, ty se nepoužijí.
- **Ostatní lodě** jsou výřezy otočené ke kameře se spritem z té strany, ze které je kamera vidí (jako letadla),
  0,4 jednotky na pixel. Hra kreslí sprity lodí daleko od jejich polohy (u lodi na jihozápad o ~15 jednotek
  vpřed), proto se výřez staví podle neprůhledné části spritu: střed trupu na polohu lodi, bližší půlka trupu
  pod hladinu. Délka lodi plující od kamery nebo ke kameře je ve spritu nahoru, proto se výřez svisle zmenšuje
  až na 0,4×. Jako kvádr potažený spritem (jako vlaky) vypadala loď jako plochá deska.
- Testovací AI teď staví kanál 2 dlaždice široký a 28 dlouhý: uprostřed stoupá o úroveň dvěma plavebními komorami
  vedle sebe, přes dolní část vede silniční most, přes horní železniční, u komor je přístav, po březích stromy.
  Dvě lodě (trajekty, ne vznášedla, ta jsou nízká) plují proti sobě z dep na obou koncích mezi dvěma bójkami.
  Na mapě s mořem (`terrain_type = 2`, `quantity_sea_lakes = 3`) postaví ještě dva přístavy na pobřeží
  a vznášedlo mezi nimi (`SEASHIP` v logu).
- Uložené hry na zkoušku: `kabina3d_lod.sav` (kanál s komorami, lodě #1 a #2 společnosti „Cab Test Rail“)
  a `kabina3d_more.sav` (moře, loď #1).

### Doladění vzhledu (13. 10.)

- **Základy** (boky dlaždic zvednutých na svahu, hlavně pod domy v kopcovitých městech) jsou kamenné zdi
  (`AddFoundationSide`, `MakeMasonryTexture`): řady kamenů jednotku vysoké a 2–3 dlouhé na vazbu, s maltou mezi nimi.
  Textura je široká jako dlaždice a řady začínají ve výškách po 16 jednotkách, takže kameny sousedních dlaždic
  navazují. Dřív byly boky jednolitě hnědé a z ulice vypadaly jako hliněné stěny.

- **Stromy podél městských silnic** (sprite 0x1212 v kraji silnice) jsou výřezy 0,65× menší než ostatní stromy
  (`ROADSIDE_TREE_SCALE`). Stojí hned u vozovky, a ve velikosti lesních stromů zakrývaly z autobusu půl obrazu.

## Další kroky

1. Mac: hotovou aplikaci sestavuje u každého PR workflow `cab-view-macos-app.yml` (artefakt
   `openttd-cab-view-macos-arm64`, stáhnout ze stránky běhu na GitHubu, rozbalit, spustit). Na MacBooku Air
   3D pohled běží v plném rozlišení Retiny.
2. Krok 4: doplnit 3D svět – budovy, návěstidla, stromy, ostatní vozidla, mosty, portály tunelů, přejezdy,
   ploty, silniční mosty a akvadukty jsou hotové; kamera je níž (okna kabiny); zbývá vyzkoušet na Macu.
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
