**First person view for OpenTTD.** Ride along in the cab of your trains, road vehicles, ships and aircraft, on a dashboard map or in a simple 3D view drawn from the game's own graphics.

This is an **alpha**: an early test build based on OpenTTD 15.3. Expect rough edges, and please report what looks wrong.

*Česky: [níže](#česky).*

## 1. Download

Download **one file** for your computer:

| Computer | File |
|---|---|
| **Mac** (Apple Silicon and Intel, macOS 10.13 or newer) | [openttd-{VERSION}-macos.zip](https://github.com/{REPO}/releases/download/{VERSION}/openttd-{VERSION}-macos.zip) |
| **Windows** 10 or 11, 64-bit | [openttd-{VERSION}-windows-64bit.zip](https://github.com/{REPO}/releases/download/{VERSION}/openttd-{VERSION}-windows-64bit.zip) |
| **Linux**, 64-bit | [openttd-{VERSION}-linux-64bit.tar.xz](https://github.com/{REPO}/releases/download/{VERSION}/openttd-{VERSION}-linux-64bit.tar.xz) |

The same files are listed under **Assets** at the bottom of this page. You do not need the *Source code* files or `SHA256SUMS.txt`.

You do not need OpenTTD installed: graphics (OpenGFX), sounds (OpenSFX) and music (OpenMSX) are included.

## 2. Unpack and start

- **Mac:** double-click the zip, then double-click `OpenTTD.app` (you can move it to Applications first).
  The build is not signed by Apple, so the first time macOS says it cannot open it. Click *Done*, open *System Settings > Privacy & Security*, scroll down and click *Open Anyway* next to OpenTTD, then confirm. After that it opens normally.
- **Windows:** right-click the zip, choose *Extract All*, open the extracted folder and run `openttd.exe`.
  If a blue *Windows protected your PC* window appears, click *More info* and *Run anyway*.
- **Linux:** unpack it (`tar xf openttd-{VERSION}-linux-64bit.tar.xz`) and run `./openttd` in the unpacked folder.

It does not replace an OpenTTD you already have. Settings and **savegames are shared** with it, so your own games are right there under *Load Game*.

## 3. Open the cab view

1. Start or load a game and click any train, road vehicle, ship or aircraft to open its vehicle window.
2. Press the new button with the front of a locomotive, under "Details" on the right side.
3. The **Cab view** window opens: the map follows the vehicle and looks ahead along the track or road, with a dashboard below (speedometer, maximum speed, next stop, load).
4. Press **3D** to switch to the view from the cab window. Press it again to go back to the map.

The 3D view shows terrain, water, track, roads, stations, houses, industries, trees, signals, bridges, tunnels, fences and other vehicles. Aircraft see the airport, take off and fly; ships sail canals, locks and the sea.

## Good to know

- **Savegames** are fully compatible with OpenTTD 15.3 in both directions: the cab view changes nothing in the game itself. Games saved by a newer OpenTTD (like 16.0) cannot be opened, as in any 15.3.
- **Multiplayer** works only with players running the same alpha build.
- The 3D view is drawn by the processor, without the graphics card. On a recent computer it runs smoothly; if it stutters, make the Cab view window smaller.
- Only the default graphics are tested. NewGRF vehicles and buildings are shown, but may look odd.
- Not yet: catenary, smoke and night lights.

## Coming next: HD graphics

The next step is support for high resolution base graphics (like OpenGFX2 High Def) in the 3D view, for much sharper ground, buildings and vehicles on large and Retina screens. It will be optional: the default graphics keep working.

Graphics: OpenGFX (GPL v2). Sounds: OpenSFX (CC BY-SA 3.0, GPL v2 or CDDL 1.1). Music: OpenMSX (GPL v2). OpenTTD is licensed under the GPL v2; the source code of this build is in the `cab-view` branch of this repository.

---

## Česky

**Pohled z kabiny pro OpenTTD.** Jeďte v kabině svých vlaků, aut a autobusů, lodí i letadel, buď na mapě s palubní deskou, nebo v jednoduchém 3D pohledu z grafiky hry.

Je to **alfa verze**, první zkušební sestavení založené na OpenTTD 15.3. Ne všechno je dotažené; co vypadá špatně, dejte prosím vědět.

**1. Stáhněte jeden soubor** podle svého počítače:

- **Mac** (Apple Silicon i Intel): [openttd-{VERSION}-macos.zip](https://github.com/{REPO}/releases/download/{VERSION}/openttd-{VERSION}-macos.zip)
- **Windows** 10 nebo 11, 64bit: [openttd-{VERSION}-windows-64bit.zip](https://github.com/{REPO}/releases/download/{VERSION}/openttd-{VERSION}-windows-64bit.zip)
- **Linux**, 64bit: [openttd-{VERSION}-linux-64bit.tar.xz](https://github.com/{REPO}/releases/download/{VERSION}/openttd-{VERSION}-linux-64bit.tar.xz)

Soubory *Source code* a `SHA256SUMS.txt` dole na stránce nepotřebujete. OpenTTD nemusíte mít nainstalované, grafika, zvuky i hudba jsou přibalené.

**2. Rozbalte a spusťte.**
- **Mac:** dvakrát klikněte na zip a pak na `OpenTTD.app`. Sestavení není podepsané od Applu, takže ho macOS napoprvé neotevře: klikněte na *Hotovo*, otevřete *Nastavení systému > Soukromí a zabezpečení*, sjeďte dolů, klikněte u OpenTTD na *Přesto otevřít* a potvrďte.
- **Windows:** pravým tlačítkem na zip, *Extrahovat vše*, v rozbalené složce spusťte `openttd.exe`. Když se objeví modré okno *Systém Windows ochránil váš počítač*, klikněte na *Další informace* a *Přesto spustit*.
- **Linux:** rozbalte (`tar xf ...`) a spusťte `./openttd`.

Nainstalované OpenTTD to nepřepíše. Nastavení a **uložené hry jsou společné**, svoje hry najdete rovnou v *Nahrát hru*.

**3. Kabina:** klikněte na vozidlo, v okně vozidla stiskněte nové tlačítko s čelem lokomotivy (vpravo pod „Detaily“), otevře se okno **Kabina**. Tlačítko **3D** přepne na pohled z okna kabiny.

**Další krok: HD grafika.** 3D pohled dostane podporu grafiky ve vysokém rozlišení (třeba OpenGFX2 High Def), takže země, budovy i vozidla budou na velkých a Retina displejích mnohem ostřejší. Bude volitelná, výchozí grafika zůstane.

Uložené hry jsou plně kompatibilní s OpenTTD 15.3. Hra po síti funguje jen s hráči, kteří mají stejnou alfa verzi.
