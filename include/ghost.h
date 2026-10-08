#pragma once
#include <stdint.h>
#include <stddef.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
//  Ghost Pod: Antenne, Entropie, LEDs, Piepser, Runden und Historie
//
//  - Bei jedem neuen Job "riecht" die Antenne GHOST_SNIFF_MS lang in die Luft
//    (Touch 32/33 + offener ADC-Pin + Takt-Jitter + HW-Zufall). Daraus werden
//    extranonce2 und der Start-Nonce des Jobs abgeleitet.
//  - Jeder vom Miner gepruefte Hash meldet seine Difficulty an ghost::on_hash().
//  - Der Ghost-Task (Core 0) laesst daraus pro Runde die 4 Geister-LEDs angehen.
// ---------------------------------------------------------------------------

namespace ghost {

// Frueh in setup(): Pins, Start-Piep, Lauflicht, Historie laden (inkl. Ghost Pod 2.6)
void begin();
// Ghost-Task starten (Core 0)
void start_task();

// Antenne abtasten und `outLen` Bytes Entropie liefern (blockiert ~GHOST_SNIFF_MS).
// `salt` fliesst mit ein (z.B. Job-ID), damit zwei Rieche nie gleich enden.
void sniff(const char* salt, uint8_t* out, size_t outLen);

// true = der Ghost-Task wartet auf den Rundenstart-Scan (Stratum-Task soll riechen)
bool round_sniff_pending();

// Vom Miner (Core 1) fuer jeden gueltig geprueften Hash. Muss billig bleiben.
void on_hash(double diff);
// Echter Block gefunden -> Poltergeist
void on_block();

// LED-Test: alle sechs LEDs nacheinander je 2 s (vom Webserver ausgeloest)
void start_led_test();

// Fuer /api/stats
void to_json(JsonObject o);

}  // namespace ghost
