# Jednoduchý 3D pohled z kabiny – návrh (etapa 2)

Tohle je návrh k připomínkám, ne hotový kód. Obrázky jsou vyrenderované prototypem
`3d_mockup/mockup.py`, který kreslí **stejným způsobem, jaký by použila hra** (plné mnohoúhelníky
po dlaždicích od nejvzdálenější k nejbližší, barvy jen z 256barevné palety OpenTTD). Takže obrázky
nejsou malba „jak by to mohlo být“, ale zhruba to, co hra opravdu dokáže nakreslit.

## Jak to bude vypadat

Okno Kabina zůstane stejné (palubní deska, tlačítka). V liště tlačítek přibude přepínač **3D**:
vypnutý = dnešní pohled shora, zapnutý = pohled strojvedoucího dopředu po trati.

Krok 3 (první verze: terén a trať):

![Krok 3](3d_mockup/krok3_teren_a_trat_kabina.png)

Krok 4 (doplněný svět: domy, nádraží, návěstidla, stromy, jiné vlaky):

![Krok 4](3d_mockup/krok4_cely_svet_kabina.png)

Obrázky jsou ve velikosti rozhraní 2× (jako na Retině). Pohled se kreslí po pixelech obrazovky,
takže na Retině je ostrý, ne rozmazaný.

### Co je na obrázcích

| Věc | Jak se kreslí | Odkud se vezme |
|---|---|---|
| Terén | každá dlaždice jako čtyřúhelník (nebo 2 trojúhelníky na svahu), stínovaný podle sklonu ke slunci | výšky rohů dlaždic (`TileHeight`, `GetTileSlope`) |
| Tráva u vlaku | dlaždice rozdělená na 2×2 plošky lehce jiné zelené – bez toho není na rovině vidět rychlost | jen kosmetika |
| Voda, pláž, pole | barva podle typu dlaždice, pole s brázdami | `GetTileType`, typ terénu (`MP_WATER`, `MP_CLEAR` + pole) |
| Koleje | hnědé štěrkové lože, pražce (jen v blízkosti), dvě kolejnice jako čáry | `GetTrackBits` – trať jde středem dlaždice, výhybky a oblouky podle bitů |
| Silnice | šedý pás | `MP_ROAD`, `GetRoadBits` |
| Domy | kvádr, malé domy se sedlovou střechou, výška a barva podle typu domu | `MP_HOUSE`, `GetHouseType` (výška z velikosti budovy, barva z tabulky) |
| Nádraží | nástupiště jako nízký kvádr, přístřešky | `MP_STATION`, typ stanice |
| Návěstidla | sloup + hlava se světlem (zelená / červená) | `HasSignals`, stav návěstidla |
| Stromy | jednoduché „lízátko“ natočené vždy k nám (2 osmiúhelníky + kmen) | `MP_TREES`, počet stromů na dlaždici |
| Jiná vozidla | kvádr na každý vagon, barva společnosti, okna pruhem, čelo jinou barvou | vozidla na dlaždici (`VehiclesOnTile`), jejich `x_pos`, `y_pos`, `z_pos`, směr |
| Obloha | 5 pruhů modré z palety | – |
| Opar v dálce | barvy postupně přechází do barvy oblohy, konec mapy tak není vidět | vzdálenost |

Průmysl a objekty (vysílače, majáky…) budou v kroku 4 také kvádry s barvou podle typu.

## Jak to bude fungovat v kódu

- **Kamera** sedí na čele vlaku (`x_pos`, `y_pos`, `z_pos` prvního vozidla) ve výšce střechy (8 jednotek),
  dívá se ve směru jízdy, 6° dolů, zorný úhel 64°. Směr kamery se plynule dotáčí stejně jako dnešní
  výhled vpřed (časová konstanta 650 ms) a v obloucích se může natáčet podle `GetTrackLookAheadTarget`,
  který už v etapě 1 existuje.
- **Měřítko** je převzaté ze hry: dlaždice 16 jednotek, výškový stupeň 8, vagon 8 jednotek dlouhý.
  Výšky domů a vlaků jsou ale „herní“, ne skutečné (dům ~10, věžák 20–35, vlak 6), jinak by ve 3D
  všechno vedle velkých dlaždic vypadalo jako mravenci.
- **Kreslení**: projdou se dlaždice v zorném poli do vzdálenosti 56 dlaždic, seřadí se od nejvzdálenější
  a každá se nakreslí celá (zem → koleje/silnice → objekty). Bližší věci tak přirozeně překryjí vzdálenější
  (malířův algoritmus), žádný z-buffer není potřeba.
- **Nástroje hry**: jen `GfxFillPolygon`, `GfxFillRect` a `GfxDrawLine`. Ty fungují se všemi blittery
  (8bpp i 32bpp) i s OpenGL ovladačem, protože kreslí do stejné obrazovky jako zbytek rozhraní.
  Žádné OpenGL ani nové knihovny – funguje to všude, kde běží OpenTTD, včetně Macu.
- **Barvy**: každá barva se při startu přepočítá na nejbližší barvu palety a pro opar se předpočítá
  8 stupňů k barvě oblohy. Za běhu je to jen vyhledání v tabulce.
- **Okno**: místo viewportu bude v okně `NWID_SELECTION`, který přepíná mezi dnešním viewportem a novým
  panelem, kam se 3D kreslí v `DrawWidget`. Překreslování při každém pohybu vlaku, nejvýš ~30× za sekundu.
- **Kolik je to práce pro počítač**: prototyp kreslí na scénu z obrázku ~4 200 (krok 3) a ~4 600 (krok 4)
  mnohoúhelníků. To je pro software málo; na Retině je ale okno 4× víc pixelů, takže v kroku 3
  změřím čas jednoho snímku na Linuxu a na Macu. Rezervy, pokud by to bylo pomalé: kratší dohled,
  vzdálené dlaždice slučovat po 2×2, kreslit 3D jen při změně polohy.

## Co nebude (záměrně)

- Žádné textury ani původní grafika (sprity) – NewGRF vzhled vlaků a domů se nepřenese, jen barvy.
- Tunely: uvnitř tunelu jen tma s obrysem portálu na konci; mosty jako deska na pilířích (krok 4).
- Stíny, odrazy, noc – ne.
- Pohled zezadu nebo z boku vlaku – možná později, kamera je na to připravená.

## Rozdělení na další kroky

**Krok 3 – první verze**: přepínač 2D/3D v okně Kabina, kamera, terén s výškami, voda, koleje
(rovné, oblouky, výhybky), opar a obloha. Jen vlaky. Test na Linuxu (1× a 2×), pak Václav na Macu.

**Krok 4 – doplnit svět**: silnice, domy, nádraží, návěstidla se stavem, stromy, ostatní vozidla, průmysl,
mosty a tunely; doladit plynulost na Retině.

## Otázky k rozhodnutí

1. **Výška pohledu**: navrhuji střechu vlaku (jako na obrázku). Nižší (oči strojvedoucího) je realističtější,
   ale vidí se mnohem méně z trati před vlakem.
2. **Obloha v pruzích** (jako na obrázku, pasuje k retro paletě), nebo jedna barva.
3. **Kde přepínač**: tlačítko „3D“ vedle „Výhled vpřed“ (jako na obrázku).

## Prototyp

```
python3 cab_view_dev/3d_mockup/mockup.py src/table/palettes.h <snímek okna Kabina ve 2×> <výstupní složka>
```

Potřebuje Python 3 s `numpy` a `Pillow`. Svět je smyšlený (dvoukolejka s obloukem, městečko,
nádraží, kopce), z mapy hry se nic nečte.
