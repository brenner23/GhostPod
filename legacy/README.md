# Legacy – die Basis / the original sketches

Die Arduino-Sketches, aus denen der GhostPod entstanden ist. Unverändert übernommen.
*The Arduino sketches the GhostPod grew out of, kept unchanged.*

| Ordner | Version | Was sie macht |
|---|---|---|
| [`antennenpod_touch/`](antennenpod_touch/antennenpod_touch.ino) | Urversion | 3 s Touch-Antenne lesen, dann 60 s lang die Messwerte + Zähler mit mbedTLS hashen (beide Kerne). LEDs nach führenden Hex-Nullen: 4 / 5 / 6 / 7. |
| [`GhostPod_2.6/`](GhostPod_2.6/GhostPod_2.6.ino) | Ghost Pod 2.6 | dazu: Hash-Kette aus Touch, Temperatur, Uptime und GhostZahl (333/666/999), geistgesteuerte Nonce-Sprünge, Piepser, EEPROM-Historie mit Betriebszeit, Sicherungsbrücke an Pin 21 (gezogen = Historie löschen). Lief 61,7 Tage, 465 Vollgeister. |

**Bauen:** Arduino IDE, Board „ESP32 Dev Module“, esp32-Core 3.x (2.6 wurde mit 3.3.8 gebaut).

**Hinweis zu den Pins:** Im Code heißen GPIO 26 „RotRechts“ und GPIO 27 „RotOben“. Am echten Deckel ist es umgekehrt: 26 sitzt oben (12 Uhr), 27 rechts (3 Uhr). Die Reihenfolge LED 1 → 2 → 3 lief also schon immer im Uhrzeigersinn.

*Pin note: the code calls GPIO 26 "right" and GPIO 27 "top"; on the real lid 26 is at 12 o'clock and 27 at 3 o'clock.*

Diese Sketches sind keine echten Miner: Sie hashen einen selbst gebauten Text, nicht einen Bitcoin-Blockheader, und reden mit keinem Pool. Genau das hat GhostPod 3.0 geändert.
*These sketches are not real miners – they hash a home-made string, not a Bitcoin block header, and never talk to a pool. That's what GhostPod 3.0 changed.*
