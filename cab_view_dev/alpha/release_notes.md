**First person view for OpenTTD.** Ride along in the cab of your trains, road vehicles, ships and aircraft, on a dashboard map or in a simple 3D view drawn from the game's own graphics.

This is an **alpha**: an early test build based on OpenTTD 15.3. Expect rough edges, and please report what looks wrong.

## How to use it

1. Click any train, road vehicle, ship or aircraft to open its vehicle window.
2. Press the new button with the front of a locomotive, under "Details" on the right side.
3. The **Cab view** window opens: the map follows the vehicle and looks ahead along the track or road, with a dashboard below (speedometer, maximum speed, next stop, load).
4. Press **3D** to switch to the view from the cab window. Press it again to go back to the map.

The 3D view shows terrain, water, track, roads, stations, houses, industries, trees, signals, bridges, tunnels, fences and other vehicles. Aircraft see the airport, take off and fly; ships sail canals, locks and the sea.

## Downloads

| System | File |
|---|---|
| macOS 10.13 or newer (Apple Silicon and Intel) | `openttd-...-macos-universal.zip` |
| Windows 10/11, 64-bit | `openttd-...-windows-win64.zip` |
| Linux, 64-bit (glibc 2.28 or newer) | `openttd-...-linux-generic-amd64.tar.xz` |

Every package already contains the free base sets OpenGFX (graphics), OpenSFX (sounds) and OpenMSX (music), so the game runs right after unpacking. If you own the original Transport Tycoon Deluxe data, you can add it as usual.

**Installation:** unpack the archive and start `OpenTTD.app` (macOS), `openttd.exe` (Windows) or `openttd` (Linux). It does not replace an installed OpenTTD; settings and savegames are shared with it.

**The builds are not signed**, so the system warns the first time:
- **macOS:** if it says the app cannot be opened, open *System Settings > Privacy & Security*, scroll down and click *Open Anyway* next to OpenTTD. (Or in Terminal: `xattr -dr com.apple.quarantine /path/to/OpenTTD.app`.)
- **Windows:** in the blue SmartScreen window, click *More info* and *Run anyway*.

## Good to know

- **Savegames** are fully compatible with OpenTTD 15.3 in both directions: the cab view changes nothing in the game itself.
- **Multiplayer** works only with players running the same alpha build.
- The 3D view is drawn by the processor, without the graphics card. On a recent computer it runs smoothly; if it stutters, make the Cab view window smaller.
- Only the default graphics are tested. NewGRF vehicles and buildings are shown, but may look odd.
- Not yet: catenary, smoke and night lights.

## Coming next: HD graphics

The next step is support for high resolution base graphics (like OpenGFX2 High Def) in the 3D view, for much sharper ground, buildings and vehicles on large and Retina screens. It will be optional: the default graphics keep working.

SHA-256 checksums of all files are in `SHA256SUMS.txt`.

Graphics: OpenGFX (GPL v2). Sounds: OpenSFX (CC BY-SA 3.0, GPL v2 or CDDL 1.1). Music: OpenMSX (GPL v2). OpenTTD is licensed under the GPL v2; the source code of this build is in the `cab-view` branch of this repository.

---

## Česky

**Pohled z kabiny pro OpenTTD.** Jeďte v kabině svých vlaků, aut a autobusů, lodí i letadel, buď na mapě s palubní deskou, nebo v jednoduchém 3D pohledu z grafiky hry.

Je to **alfa verze**, první zkušební sestavení založené na OpenTTD 15.3. Ne všechno je dotažené; co vypadá špatně, dejte prosím vědět.

**Jak na to:** klikněte na vozidlo, v okně vozidla stiskněte nové tlačítko s čelem lokomotivy (vpravo pod „Detaily“), otevře se okno **Kabina**. Tlačítko **3D** přepne na pohled z okna kabiny.

**Instalace:** stáhněte soubor pro svůj systém, rozbalte ho a spusťte. Grafika, zvuky i hudba jsou přibalené. Nainstalované OpenTTD to nepřepíše, nastavení a uložené hry jsou společné.

**Sestavení nejsou podepsaná.** Na Macu při prvním spuštění otevřete *Nastavení systému > Soukromí a zabezpečení* a klikněte na *Přesto otevřít*. Ve Windows klikněte na *Další informace* a *Přesto spustit*.

**Další krok: HD grafika.** 3D pohled dostane podporu grafiky ve vysokém rozlišení (třeba OpenGFX2 High Def), takže země, budovy i vozidla budou na velkých a Retina displejích mnohem ostřejší. Bude volitelná, výchozí grafika zůstane.

Uložené hry jsou plně kompatibilní s OpenTTD 15.3. Hra po síti funguje jen s hráči, kteří mají stejnou alfa verzi.
