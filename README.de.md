# 👻 GhostPod

**Der Geisterdetektor, der in die Luft riecht – und dabei Bitcoin schürft.**

*[English version](README.md)*

Ein ESP32 in einem Tiramisu-Eisbecher. Eine Teleskopantenne oben drauf, ein Kupferband ringsum, sechs LEDs im Deckel. Gebaut nach dem Vorbild der „REM-Pods“ aus Geisterjäger-Serien, also im Grunde ein halbes Theremin: Die Antenne baut ein Feld auf, und alles Leitfähige in der Nähe verstimmt es.

Der Unterschied zu den Geräten aus dem Fernsehen: Der GhostPod jagt keine Geister in verlassenen Häusern ohne Internet. Er jagt sie im **SHA-256-Raum** – als echter Stratum-Miner mit rund **786 kH/s**. Die Antenne liefert dabei echte physikalische Entropie (Kapazität, Netzbrummen, Rauschen), und die bestimmt, *wo* im Suchraum der Becher nach seinem Geist sucht.

> **Ehrlicher Hinweis 😉**
> Die Antenne macht den Becher nicht glücklicher. Jeder Nonce ist ein Los mit exakt derselben Chance – egal ob von 0 hochgezählt oder von einem Geist ausgewählt. Die Antenne entscheidet, *welche* Lose gezogen werden, nicht *wie viele*. Am echten Bitcoin-Netz findet der GhostPod im Schnitt alle **~22 Milliarden Jahre** einen Block. Die Chance pro Tag liegt bei etwa **0,000000000012 %**. Aber sie ist nicht null. 👻

---

## Was die LEDs bedeuten

Draufsicht auf den Deckel, Antenne in der Mitte:

```
                🔴 12 Uhr – Share
   🔴 9 Uhr                     🔴 3 Uhr
   Lebenszeichen    📡         Highscore

   🟢 Dauerlicht           🟢 Blinker
                🔴 6 Uhr – BLOCK (Herz)
```

| LED | Bedeutung | Grenze |
|---|---|---|
| 9 Uhr | Lebenszeichen – „da ist Aktivität“ | halbe Pool-Difficulty |
| 12 Uhr | **Share gefunden** – geht an den Pool | Pool-Difficulty |
| 3 Uhr | **Highscore geknackt** – höher als je zuvor | bisheriger Bestwert |
| 6 Uhr (Herz) | **BLOCK!** – Piepser, 10 s Poltergeist, danach 24 h an | Netz-Difficulty |

Alle Grenzen kommen **vom Pool selbst** (`mining.set_difficulty` und `nbits`). Der GhostPod passt sich damit automatisch an jeden Pool an – vom eigenen Spaß-Coin bis zum echten Bitcoin.

**Grüne LEDs:** schnelles Blinken = Antenne riecht, Dauerlicht + Sekundentakt = Suche läuft, beide aus = Ergebnis wird gezeigt.

### Eine Runde (60 s)

1. **Scan (3 s)** – die Antenne „hält die Nase in die Luft“. Aus den Messwerten entstehen ein neues `extranonce2` und ein neuer Start-Nonce.
2. **Suche (52 s)** – die roten LEDs bauen sich auf: Seelchen … Share … Highscore?
3. **Ergebnis (5 s)** – die LEDs bleiben stehen, dann beginnt die nächste Runde.

Gemint wird dabei **ununterbrochen**. Die Runde regelt nur die Anzeige.

---

## Die Antenne

| | Pin | Wozu |
|---|---|---|
| Teleskop-Stabantenne | GPIO 33 (Touch) | Kapazität, Netzbrummen, Annäherung |
| Kupferband ringsum | GPIO 32 (Touch) | Gegenpol des Dipols, Anfass-Sensor |
| offener ADC-Pin | GPIO 34 | Rauschen |

Bei jedem neuen Job vom Pool (0,2 s) und am Anfang jeder Runde (3 s) wird die Antenne tausendfach abgetastet. Die zappelnden unteren Bits, Takt-Jitter, Hardware-Zufall, WLAN-Pegel und die Temperatur fließen in einen SHA-256-Entropie-Pool.

**Kanal-Erkennung:** Jedes ausgezogene Segment ändert die Kapazität messbar. Der GhostPod erkennt so, wie weit die Antenne draußen ist, und zeigt den passenden Viertelwellen-Bereich an – z. B. *„5 von 5 Segmenten · ~94 cm · ~80 MHz“*. Die Frequenz ist berechnet, nicht gemessen (dafür bräuchte es einen Antennenanalysator), die Länge dagegen echt.

---

## Webseite

`http://ghostpod.local/` zeigt:

- den **Deckel live** – alle sechs LEDs mit dem echten Pin-Zustand
- die Geistersuche mit Rundenfortschritt und den aktuellen Grenzen
- Antennenwerte, Kanal, Geister-Aktivität, GhostZahl (333 / 666 / 999)
- die **Geister-Chronik**: Runden, Shares, Highscores, Blöcke, Betriebszeit – übersteht Neustarts
- Hashrate, Pool, Shares wie beim ESPressMiner32
- einen **LED-Test**-Knopf

---

## Hardware

- ESP32 klassisch (getestet: ESP32-D0WDQ6 **Revision 1.0**, DevKitC mit CP2102, Micro-USB)
- 4 rote LEDs: GPIO 25 (9 Uhr), 26 (12 Uhr), 27 (3 Uhr), 14 (Herz)
- 2 grüne LEDs: GPIO 13 (Dauerlicht), 12 (Blinker)
- Piepser: GPIO 23
- Drahtbrücke: GPIO 21 → GND (siehe unten)

Alte ESP32 der Revision 1 brauchen beim Hardware-SHA einen Workaround gegen einen Chipfehler (DPORT-Errata) – der wird automatisch erkannt und aktiviert.

### Drahtbrücke an GPIO 21

Wird **einmal beim Einschalten** gelesen:

- **gesteckt** → der Highscore ist dauerhaft und wird mit der Zeit immer schwerer zu knacken
- **gezogen** → der GhostPod vergisst den Highscore und beginnt bei null

---

## Bauen und flashen

[PlatformIO](https://platformio.org/):

```bash
pio run -t upload
```

Alte Micro-USB-Boards kommen nicht immer von selbst in den Download-Modus. Dann beim Flashen den **BOOT-Knopf gedrückt halten** (bewährt: ein Wattebausch unter dem Deckel 😄). Deshalb ist auch das Zurücksetzen per lang gedrückter BOOT-Taste abgeschaltet.

### Einrichten

Beim ersten Start öffnet der GhostPod ein eigenes WLAN **`GhostPod-XXXX`** (Passwort `ghostpod`). Dort WLAN, Pool-Adresse und Wallet eintragen – fertig.

### Einstellungen (`include/config.h`)

| Einstellung | Standard | Bedeutung |
|---|---|---|
| `GHOST_CYCLE_S` | 60 | Länge einer Runde (Scan + Suche + Ergebnis) |
| `GHOST_SCAN_MS` | 3000 | Riechen am Rundenanfang |
| `GHOST_SNIFF_MS` | 200 | Riechen bei jedem neuen Job |
| `GHOST_LED1_FACTOR` | 0.5 | LED 1 = Pool-Diff × Faktor |
| `GHOST_HIGHSCORE_HOURS` | 0 | 0 = Highscore ohne Zeitfenster, sonst die letzten N Stunden |
| `GHOST_BLOCK_LED_HOURS` | 24 | so lange bleibt das Herz nach einem Block an |
| `GHOST_BEEP_LED1..4` | 0/0/0/1 | Piepser je Stufe |
| `GHOST_ANT_CAL` | Messwerte | Antennen-Kalibrierung je Segment |

---

## Geschichte

- **Ghost Pod 1.x / 2.x** – Arduino-Sketch, hashte die Touch-Werte mit mbedTLS (20–60 kH/s), LEDs nach führenden Hex-Nullen. Version 2.6 lief **61,7 Tage** und fand **465 Vollgeister**. Diese „Ära 2.6“ wird beim Umstieg aus dem Flash übernommen und auf der Webseite als Erinnerung angezeigt.
- **GhostPod 3.0** – echter Stratum-Miner auf Basis von [ESPressMiner32](https://github.com/brenner23/ESPressMiner32), Hardware-SHA, LEDs nach echter Difficulty, Antenne bestimmt den Suchort.

---

## Danke

- [ESPressMiner32](https://github.com/brenner23/ESPressMiner32) – Miner-Kern, Stratum, Webseite, Einrichtungs-Portal
- Idee der gepufferten SHA-Register-Zugriffe: [SparkMiner](https://github.com/BitzyLabs/sparkminer) (MIT)
- Siehe [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)

## Lizenz

MIT – siehe [LICENSE](LICENSE).
