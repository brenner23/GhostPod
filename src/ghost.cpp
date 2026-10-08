#include "ghost.h"
#include "config.h"
#include "mining_job.h"
#include "sha256_sw.h"
#include "stats.h"
#include "utils.h"
#include "web_server.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <math.h>
#include <xtensa/core-macros.h>
#include <soc/rtc_io_periph.h>
#include <soc/soc.h>

#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();   // interner Temperatursensor (ESP32 klassisch, grob)
#ifdef __cplusplus
}
#endif

namespace ghost {

// ---------------------------------------------------------------------------
//  Historie (NVS-Namespace "ghosthist")
// ---------------------------------------------------------------------------
struct Hist {
    uint32_t rounds;
    uint32_t led[4];      // Runden, in denen die Stufe erreicht wurde
    uint32_t uptimeS;     // Betriebszeit, ueberlebt Neustarts
    uint32_t blocks;
    uint32_t sniffs;
    uint32_t shares;      // vom Pool akzeptierte Shares, alle Zeiten
    double   best;        // beste Difficulty aller Zeiten
};

// Ghost Pod 2.6 ("Aera 2.6"): Zaehler nach fuehrenden Hex-Nullen, eingefroren
struct Era26 {
    uint32_t valid;
    uint32_t scans;
    uint32_t led[4];
    uint32_t uptimeS;
};

static const char* HIST_NS = "ghosthist";
static Hist  s_hist  = {};
static Era26 s_era26 = {};
static bool  s_dirty = false;

static void hist_save()
{
    Preferences p;
    if (!p.begin(HIST_NS, false)) return;
    p.putBytes("h", &s_hist, sizeof(s_hist));
    p.end();
    s_dirty = false;
}

// Ghost Pod 2.6 hat seine Historie per Arduino-EEPROM (= NVS "eeprom"/"eeprom") abgelegt
static bool read_legacy_26(Era26& e)
{
    Preferences p;
    if (!p.begin("eeprom", true)) return false;
    uint8_t b[28] = {};
    bool ok = p.getBytesLength("eeprom") >= sizeof(b) && p.getBytes("eeprom", b, sizeof(b)) == sizeof(b);
    p.end();
    if (!ok) return false;
    uint32_t v[7];
    memcpy(v, b, sizeof(v));
    if (v[5] != 0xDEADBEEF) return false;
    e.valid  = 1;
    e.scans  = v[0];
    e.led[0] = v[1]; e.led[1] = v[2]; e.led[2] = v[3]; e.led[3] = v[4];
    e.uptimeS = v[6];
    return true;
}

static void hist_load()
{
    Preferences p;
    if (p.begin(HIST_NS, true)) {
        if (p.getBytesLength("h") == sizeof(s_hist)) p.getBytes("h", &s_hist, sizeof(s_hist));
        if (p.getBytesLength("e26") == sizeof(s_era26)) p.getBytes("e26", &s_era26, sizeof(s_era26));
        p.end();
    }
    if (!s_era26.valid) {
        Era26 e = {};
        if (read_legacy_26(e)) {
            s_era26 = e;
            if (p.begin(HIST_NS, false)) {
                p.putBytes("e26", &s_era26, sizeof(s_era26));
                p.end();
            }
            Serial.printf("[GHOST] Historie von Ghost Pod 2.6 uebernommen: %lu Scans, %lu Vollgeister\n",
                          (unsigned long)e.scans, (unsigned long)e.led[3]);
        }
    }
}

// ---------------------------------------------------------------------------
//  Laufzeitzustand
// ---------------------------------------------------------------------------
static const int LED_PINS[4] = { PIN_LED1, PIN_LED2, PIN_LED3, PIN_LED4 };
static const char* LEVEL_NAMES[4] = { "kleine Seele", "Seele (Share!)", "starker Geist (neuer Highscore!)", "V O L L G E I S T (BLOCK!)" };
static const char* LEVEL_ICONS[4] = { "\xF0\x9F\x91\xBB", "\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB",
                                      "\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB",
                                      "\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB" };
static const int BEEP_ON[4] = { GHOST_BEEP_LED1, GHOST_BEEP_LED2, GHOST_BEEP_LED3, GHOST_BEEP_LED4 };

static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile double s_roundBest = 0.0;
static volatile bool   s_poltergeist = false;
static volatile bool   s_sniffing = false;
static volatile bool   s_roundSniff = false;   // naechstes Riechen = langer Rundenstart-Scan
static volatile uint32_t s_sniffDone = 0;     // zaehlt abgeschlossene Riech-Vorgaenge

enum Phase : uint8_t { PH_WAIT, PH_SCAN, PH_SEARCH, PH_SHOW };
static volatile Phase  s_phase = PH_WAIT;
static volatile int    s_level = 0;          // in dieser Runde erreichte Stufe (0..4)
static uint32_t        s_phaseStart = 0;
static uint32_t        s_scanSniffNo = 0;
static double          s_thr[4] = {};
static uint8_t         s_litMask = 0;        // in dieser Runde angegangene Stufen (Bit k = LED k+1)
static double          s_recordRef = 0.0;    // Highscore, der in dieser Runde zu knacken ist
static uint32_t        s_blockLedUntil = 0;  // Herz-LED nach Block an bis (millis), 0 = aus
static double          s_sessionBest = 0.0;  // Bestwert seit dem Start (fuer gezogene Bruecke)
#if GHOST_HIGHSCORE_HOURS > 0
static double          s_hourBest[GHOST_HIGHSCORE_HOURS] = {};   // Bestwert je Stunde (Ring)
static uint32_t        s_hourIdx = 0;
#endif
static double          s_shownBest = 0.0;    // fuer die Webseite: beste Diff der letzten/aktuellen Runde

// Antenne (aus dem letzten Riechen)
struct Antenna {
    float    meanA, meanB;
    uint32_t minA, maxA, minB, maxB;
    float    baseA, baseB;       // gleitender Grundwert
    uint32_t adc;
    uint32_t samples;
    float    activity;           // Abweichung vom Grundwert + Zappeln
    int      ghostNum, ghostQs;
    float    tempC;
    char     seed[17];           // die ersten 8 Byte des letzten Ergebnisses (Hex)
};
static Antenna s_ant = {};

// Stellung der Stabantenne aus dem Touch-Wert (naechstgelegener Kalibrierwert)
static const float ANT_CAL[] = GHOST_ANT_CAL;
static const int   ANT_STEPS = sizeof(ANT_CAL) / sizeof(ANT_CAL[0]);

static int antenna_segments(float v)
{
    int best = 0;
    for (int i = 1; i < ANT_STEPS; i++)
        if (fabsf(v - ANT_CAL[i]) < fabsf(v - ANT_CAL[best])) best = i;
    return best;
}
static uint8_t s_pool[32] = {};  // Entropie-Pool, wird ueber alle Riech-Vorgaenge weitergemischt

// ---------------------------------------------------------------------------
//  Piepser und LEDs
// ---------------------------------------------------------------------------
static void beep(int ms)
{
    digitalWrite(PIN_BEEPER, HIGH);
    delay(ms);
    digitalWrite(PIN_BEEPER, LOW);
    delay(40);
}

// Drahtbruecke gesteckt = Pin auf GND. Nur einmal beim Start gelesen (wie Ghost Pod 2.6).
static bool s_bridge = true;
static bool bridge_set() { return s_bridge; }

static bool block_led_active()
{
    return s_blockLedUntil && (int32_t)(s_blockLedUntil - millis()) > 0;
}

static void red_leds(bool on)
{
    for (int p : LED_PINS) digitalWrite(p, on ? HIGH : LOW);
    if (!on && block_led_active()) digitalWrite(PIN_LED4, HIGH);   // Block-Erinnerung
}

// Highscore, gegen den die naechste Runde antritt
static double highscore()
{
#if GHOST_HIGHSCORE_HOURS > 0
    double m = 0;
    for (double v : s_hourBest) if (v > m) m = v;
    return m;
#else
    return bridge_set() ? s_hist.best : s_sessionBest;
#endif
}

// Alle sechs LEDs in Deckel-Reihenfolge (fuer LED-Test und Diagnose)
static const int   ALL_LEDS[6]      = { PIN_GREEN_STEADY, PIN_GREEN_BLINK, PIN_LED1, PIN_LED2, PIN_LED3, PIN_LED4 };
static const char* ALL_LED_NAMES[6] = { "gruen Dauerlicht", "gruen Blinker", "LED 1 Lebenszeichen",
                                        "LED 2 Share", "LED 3 Highscore", "LED 4 Block (Herz)" };
static volatile int s_testStep = -1;   // LED-Test: Index in ALL_LEDS, -1 = aus
static uint32_t     s_testStart = 0;
static uint32_t     s_reclaims = 0;    // wie oft ein LED-Pin vom Touch-Treiber zurueckgeholt wurde

// true = der Pin haengt am RTC-Signalweg (z.B. vom Touch-Treiber uebernommen) und
// gibt dann kein normales digitalWrite mehr aus
static bool pin_rtc_muxed(int pin)
{
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return false;
    int rn = rtc_io_num_map[pin];
    if (rn < 0) return false;
    return REG_GET_BIT(rtc_io_desc[rn].reg, rtc_io_desc[rn].mux) != 0;
}

// Holt LED-Pins zurueck, die der Touch-Treiber an sich gezogen hat (GPIO 27/14/13/12 sind Touch-Pads)
static void reclaim_led_pins()
{
    for (int p : ALL_LEDS) {
        if (!pin_rtc_muxed(p)) continue;
        int level = (REG_READ(GPIO_OUT_REG) >> p) & 1;
        pinMode(p, OUTPUT);
        digitalWrite(p, level);
        s_reclaims++;
    }
}

void start_led_test()
{
    s_testStart = millis();
    s_testStep = 0;
}

static void set_greens(bool steady, bool blink)
{
    digitalWrite(PIN_GREEN_STEADY, steady ? HIGH : LOW);
    digitalWrite(PIN_GREEN_BLINK, blink ? HIGH : LOW);
}

void begin()
{
    pinMode(PIN_BEEPER, OUTPUT);
    digitalWrite(PIN_BEEPER, LOW);
    const int all[] = { PIN_GREEN_STEADY, PIN_GREEN_BLINK, PIN_LED1, PIN_LED2, PIN_LED3, PIN_LED4 };
    for (int p : all) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }

    beep(80);
    for (int p : all) { digitalWrite(p, HIGH); delay(80); }
    delay(300);
    for (int p : all) digitalWrite(p, LOW);

    analogReadResolution(12);
    pinMode(PIN_BRIDGE, INPUT_PULLUP);
    delay(10);
    s_bridge = digitalRead(PIN_BRIDGE) == LOW;
    Serial.printf("[BRUECKE] Pin %d %s\n", PIN_BRIDGE,
                  s_bridge ? "gesteckt -> Highscore dauerhaft" : "gezogen -> Highscore vergessen, startet bei 0");
    hist_load();

    Serial.println();
    Serial.println("==============================");
    Serial.println("   G H O S T   P O D   3.0");
    Serial.println("   + echter Stratum-Miner");
    Serial.println("   + Antennen-Entropie");
    Serial.println("   + LEDs nach echter Diff");
    Serial.println("==============================");
    Serial.printf("[HISTORY] Runden=%lu | LED1=%lu | LED2=%lu | LED3=%lu | LED4=%lu | Bloecke=%lu\n",
                  (unsigned long)s_hist.rounds, (unsigned long)s_hist.led[0], (unsigned long)s_hist.led[1],
                  (unsigned long)s_hist.led[2], (unsigned long)s_hist.led[3], (unsigned long)s_hist.blocks);
}

// ---------------------------------------------------------------------------
//  In die Luft riechen
// ---------------------------------------------------------------------------
static int quersumme(int n)
{
    int s = 0;
    for (n = abs(n); n > 0; n /= 10) s += n % 10;
    return s;
}

// pool = SHA256(pool || data)
static void mix(const uint8_t* data, size_t len)
{
    uint8_t buf[32 + 256];
    while (len) {
        size_t n = len > 256 ? 256 : len;
        memcpy(buf, s_pool, 32);
        memcpy(buf + 32, data, n);
        sha256(buf, 32 + n, s_pool);
        data += n;
        len -= n;
    }
}

bool round_sniff_pending() { return s_roundSniff; }

void sniff(const char* salt, uint8_t* out, size_t outLen)
{
    const uint32_t sniffMs = s_roundSniff ? GHOST_SCAN_MS : GHOST_SNIFF_MS;
    s_roundSniff = false;
    s_sniffing = true;

    uint8_t  buf[240];
    size_t   n = 0;
    uint32_t cnt = 0, minA = UINT32_MAX, maxA = 0, minB = UINT32_MAX, maxB = 0, adc = 0;
    uint64_t sumA = 0, sumB = 0;
    const uint32_t t0 = millis();

    while (millis() - t0 < sniffMs) {
        uint32_t a  = touchRead(PIN_TOUCH_A);
        uint32_t b  = touchRead(PIN_TOUCH_B);
        adc         = analogRead(PIN_NOISE_ADC);
        uint32_t cc = XTHAL_GET_CCOUNT();   // Takt-Jitter zwischen den Messungen

        uint32_t rec[4] = { a, b, adc, cc };
        if (n + sizeof(rec) > sizeof(buf)) { mix(buf, n); n = 0; }
        memcpy(buf + n, rec, sizeof(rec));
        n += sizeof(rec);

        sumA += a; sumB += b; cnt++;
        if (a < minA) minA = a;
        if (a > maxA) maxA = a;
        if (b < minB) minB = b;
        if (b > maxB) maxB = b;
    }

    // Zum Schluss: HW-Zufall (Funkrauschen), WLAN-Pegel, Temperatur, Geisterzahl, Salz
    static const int GHOST_NUMS[3] = { 333, 666, 999 };
    int     gnum  = GHOST_NUMS[esp_random() % 3];
    uint8_t tempF = temprature_sens_read();
    struct {
        uint32_t rnd[2];
        int32_t  rssi;
        uint32_t temp, gnum, sniffNo, ms;
    } extra = { { esp_random(), esp_random() }, WiFi.RSSI(), tempF, (uint32_t)gnum, s_hist.sniffs, millis() };
    if (n) mix(buf, n);
    mix((const uint8_t*)&extra, sizeof(extra));
    if (salt) mix((const uint8_t*)salt, strlen(salt));

    // Ausgabe: SHA256(pool || "out" || Zaehler), damit der Pool selbst nie herausgegeben wird
    for (size_t off = 0, blk = 0; off < outLen; blk++) {
        uint8_t in[32 + 8], h[32];
        memcpy(in, s_pool, 32);
        memcpy(in + 32, "out", 3);
        memcpy(in + 35, &blk, 4);
        sha256(in, 39, h);
        size_t take = outLen - off < 32 ? outLen - off : 32;
        memcpy(out + off, h, take);
        off += take;
    }

    // Fuer die Webseite
    Antenna& A = s_ant;
    A.samples = cnt;
    if (cnt) {
        A.meanA = (float)sumA / cnt;
        A.meanB = (float)sumB / cnt;
        A.minA = minA; A.maxA = maxA; A.minB = minB; A.maxB = maxB;
        if (A.baseA == 0) { A.baseA = A.meanA; A.baseB = A.meanB; }
        A.activity = fabsf(A.meanA - A.baseA) + fabsf(A.meanB - A.baseB) + (maxA - minA) + (maxB - minB);
        A.baseA += (A.meanA - A.baseA) * 0.1f;
        A.baseB += (A.meanB - A.baseB) * 0.1f;
    }
    A.adc     = adc;
    A.ghostNum = gnum;
    A.ghostQs  = quersumme(gnum);
    A.tempC    = (tempF - 32) / 1.8f;
    bytes_to_hex(out, outLen < 8 ? outLen : 8, A.seed);

    s_hist.sniffs++;
    Serial.printf("[SNIFF] %lu Messungen | T%d=%.1f (%lu..%lu) T%d=%.1f (%lu..%lu) | ADC=%lu | Aktivitaet=%.1f | Ghost=%d (QS %d) | Seed %s\n",
                  (unsigned long)cnt, PIN_TOUCH_A, A.meanA, (unsigned long)minA, (unsigned long)maxA,
                  PIN_TOUCH_B, A.meanB, (unsigned long)minB, (unsigned long)maxB, (unsigned long)adc,
                  A.activity, gnum, A.ghostQs, A.seed);
    s_sniffDone++;
    s_sniffing = false;
}

// ---------------------------------------------------------------------------
//  Meldungen vom Miner
// ---------------------------------------------------------------------------
void on_hash(double diff)
{
    portENTER_CRITICAL(&s_mux);
    if (diff > s_roundBest) s_roundBest = diff;
    portEXIT_CRITICAL(&s_mux);
}

void on_block() { s_poltergeist = true; }

// ---------------------------------------------------------------------------
//  Ghost-Task
// ---------------------------------------------------------------------------
static void update_thresholds()
{
    double pool = jobs::difficulty();
    double net  = g_stats.networkDiff > 0 ? g_stats.networkDiff : INFINITY;
    double t1   = pool * GHOST_LED1_FACTOR;
    if (t1 < 2e-5) t1 = 2e-5;   // feiner meldet die Hardware nicht (16 Nullbits)
    double t3 = s_recordRef;          // Highscore knacken ...
    if (t3 < pool * 2) t3 = pool * 2; // ... aber immer deutlich ueber einem normalen Share
    s_thr[0] = t1; s_thr[1] = pool; s_thr[2] = t3; s_thr[3] = net;
}

static void poltergeist()
{
    Serial.println("\n[POLTERGEIST] \xF0\x9F\x91\xBB\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB BLOCK GEFUNDEN!!! \xF0\x9F\x91\xBB\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB\xF0\x9F\x91\xBB\n");
    s_hist.blocks++;
    hist_save();
    s_blockLedUntil = millis() + GHOST_BLOCK_LED_HOURS * 3600000UL;
    if (!s_blockLedUntil) s_blockLedUntil = 1;
    const int all[] = { PIN_GREEN_STEADY, PIN_GREEN_BLINK, PIN_LED1, PIN_LED2, PIN_LED3, PIN_LED4 };
    const uint32_t t0 = millis();
    while (millis() - t0 < 10000) {
        for (int p : all) digitalWrite(p, esp_random() & 1);
        if ((millis() - t0) % 1000 < 60) beep(120);
        else delay(40);
    }
    red_leds(true);   // danach bleiben alle 4 an, bis die Runde endet
}

static void light_level(int k, double diff)
{
    char d[16], t[16];
    format_difficulty(diff, d, sizeof(d));
    format_difficulty(s_thr[k], t, sizeof(t));
    digitalWrite(LED_PINS[k], HIGH);
    Serial.printf("[LED%d] %s %s  (Diff %s >= %s)\n", k + 1, LEVEL_ICONS[k], LEVEL_NAMES[k], d, t);
    s_hist.led[k]++;
    s_dirty = true;
    if (BEEP_ON[k]) {
        int reps = k == 3 ? 4 : 1;
        for (int i = 0; i < reps; i++) beep(80 + 40 * k);
    }
    if (k >= 2) hist_save();   // seltene Ereignisse sofort sichern
}

static void ghost_task(void*)
{
    uint32_t lastUptime = millis();
    uint32_t lastAccepted = 0;
    uint32_t lastSave   = millis();

    for (;;) {
        const uint32_t now = millis();
        const bool mining  = jobs::valid() && g_stats.poolAuthorized;

        // Betriebszeit
        if (now - lastUptime >= 1000) {
            uint32_t s = (now - lastUptime) / 1000;
            s_hist.uptimeS += s;
            lastUptime += s * 1000;
            s_dirty = true;
        }
        uint32_t acc = g_stats.accepted;
        if (acc != lastAccepted) { s_hist.shares += acc - lastAccepted; lastAccepted = acc; s_dirty = true; }
        if (g_stats.bestDiff > s_hist.best) { s_hist.best = g_stats.bestDiff; s_dirty = true; }
        if (g_stats.bestDiff > s_sessionBest) s_sessionBest = g_stats.bestDiff;
#if GHOST_HIGHSCORE_HOURS > 0
        uint32_t hour = now / 3600000UL;
        if (hour != s_hourIdx) { s_hourIdx = hour; s_hourBest[hour % GHOST_HIGHSCORE_HOURS] = 0; }
        if (s_phase == PH_SEARCH && s_roundBest > s_hourBest[hour % GHOST_HIGHSCORE_HOURS])
            s_hourBest[hour % GHOST_HIGHSCORE_HOURS] = s_roundBest;
#endif
        if (s_blockLedUntil && !block_led_active()) { s_blockLedUntil = 0; digitalWrite(PIN_LED4, LOW); }
        if (s_dirty && now - lastSave >= GHOST_SAVE_INTERVAL_S * 1000UL) {
            hist_save();
            lastSave = now;
        }

        reclaim_led_pins();

        // LED-Test: jede LED 2 s allein, danach wieder normaler Betrieb
        if (s_testStep >= 0) {
            int step = (now - s_testStart) / 2000;
            if (step >= 6) {
                s_testStep = -1;
                // Zustand der Runde wiederherstellen (gruene setzt die Schleife gleich selbst)
                for (int p : ALL_LEDS) digitalWrite(p, LOW);
                for (int k = 0; k < 4; k++) digitalWrite(LED_PINS[k], (s_litMask >> k) & 1);
                if (block_led_active()) digitalWrite(PIN_LED4, HIGH);
            } else {
                if (step != s_testStep || now - s_testStart < 30) {
                    for (int p : ALL_LEDS) digitalWrite(p, LOW);
                    digitalWrite(ALL_LEDS[step], HIGH);
                    Serial.printf("[LED-TEST] GPIO %d = %s\n", ALL_LEDS[step], ALL_LED_NAMES[step]);
                }
                s_testStep = step;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }

        if (s_poltergeist) {
            s_poltergeist = false;
            poltergeist();
        }

        update_thresholds();

        // Gruene LEDs wie Ghost Pod 2.6: Scan/Riechen = beide schnell, Ergebnis = beide aus,
        // Suche = an + Sekundentakt. Dazu: Portal = beide langsam, kein Pool = kurzes Zucken
        if (s_sniffing || s_phase == PH_SCAN) set_greens((now / 80) % 2, (now / 80) % 2);
        else if (s_phase == PH_SHOW) set_greens(false, false);
        else if (web_portal_active() && !mining) set_greens((now / 400) % 2, (now / 400) % 2);
        else if (mining)             set_greens(true, (now / 1000) % 2);
        else                         set_greens(false, (now / 2000) % 2 && (now % 2000) < 100);

        // Runden
        switch (s_phase) {
        case PH_WAIT:
            if (mining) {
                // Rundenstart: Antenne lesen -> neuer Geister-Startpunkt (Stratum-Task riecht)
                s_roundSniff = true;
                s_scanSniffNo = s_sniffDone;
                jobs::requestNewWork();
                red_leds(false);
                s_phase = PH_SCAN;
                s_phaseStart = now;
                Serial.println("\n[SCAN] Neue Messung - Touch-Antenne wird gelesen ...");
            }
            break;

        case PH_SCAN:
            // fertig, sobald der Stratum-Task gerochen hat (Notbremse nach 10 s)
            if (s_sniffDone != s_scanSniffNo || now - s_phaseStart > 10000) {
                s_roundSniff = false;
                portENTER_CRITICAL(&s_mux);
                s_roundBest = 0;
                portEXIT_CRITICAL(&s_mux);
                s_level = 0;
                s_litMask = 0;
                s_shownBest = 0;
                s_recordRef = highscore();
                Serial.printf("[HIGHSCORE] zu knacken: %.6f (%s)\n", s_recordRef,
                              GHOST_HIGHSCORE_HOURS ? "Zeitfenster" : bridge_set() ? "Bruecke gesteckt: dauerhaft" : "Bruecke gezogen: seit Start");
                s_phase = PH_SEARCH;
                s_phaseStart = now;
                Serial.println("[RUNDE] Geistersuche laeuft ...");
            }
            break;

        case PH_SEARCH: {
            double best = s_roundBest;
            s_shownBest = best;
            // Jede Stufe einzeln: der Highscore kann ueber der Block-Diff liegen
            for (int k = 0; k < 4; k++) {
                if (s_litMask & (1 << k)) continue;
                // Highscore muss echt ueberschritten werden, die anderen Stufen nur erreicht
                if (k == 2 ? best <= s_thr[k] : best < s_thr[k]) continue;
                light_level(k, best);
                s_litMask |= 1 << k;
                if (k + 1 > s_level) s_level = k + 1;
            }
            if (now - s_phaseStart >= GHOST_ROUND_S * 1000UL) {
                s_hist.rounds++;
                s_dirty = true;
                char d[16];
                format_difficulty(best, d, sizeof(d));
                switch (s_level) {
                case 4:  Serial.printf("[ERGEBNIS] %s VOLLGEIST DETECTED!!!  (beste Diff %s)\n", LEVEL_ICONS[3], d); break;
                case 3:  Serial.printf("[ERGEBNIS] %s Starker Geist  (beste Diff %s)\n", LEVEL_ICONS[2], d); break;
                case 2:  Serial.printf("[ERGEBNIS] %s Seele  (beste Diff %s)\n", LEVEL_ICONS[1], d); break;
                case 1:  Serial.printf("[ERGEBNIS] %s Kleine Seele  (beste Diff %s)\n", LEVEL_ICONS[0], d); break;
                default: Serial.printf("[ERGEBNIS] Nichts gefunden. Kein Geist.  (beste Diff %s)\n", d); break;
                }
                s_phase = PH_SHOW;
                s_phaseStart = now;
            }
            break;
        }

        case PH_SHOW:
            if (now - s_phaseStart >= GHOST_SHOW_S * 1000UL) s_phase = PH_WAIT;
            break;
        }

        if (!mining && (s_phase == PH_SEARCH || s_phase == PH_SCAN)) {
            // Pool weg: Runde abbrechen, LEDs bleiben bis zur naechsten Runde stehen
            s_phase = PH_WAIT;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void start_task()
{
    xTaskCreatePinnedToCore(ghost_task, "ghost", 4096, nullptr, 1, nullptr, 0);
}

// ---------------------------------------------------------------------------
//  Webseite
// ---------------------------------------------------------------------------
void to_json(JsonObject o)
{
    static const char* PH[] = { "wartet", "Scan", "sucht", "Ergebnis" };
    o["phase"]     = PH[s_phase];
    o["level"]     = s_level;
    o["roundBest"] = s_shownBest;
    uint32_t el    = (millis() - s_phaseStart) / 1000;
    o["roundLeft"] = s_phase == PH_SEARCH ? (int)GHOST_ROUND_S - (int)el : 0;
    o["roundLen"]  = GHOST_ROUND_S;
    o["sniffing"]  = (bool)s_sniffing;
    o["lit"]       = s_litMask;
    o["blockLed"]  = block_led_active();
    o["hsHours"]   = GHOST_HIGHSCORE_HOURS;
    o["bridge"]    = bridge_set();
    o["hsRef"]     = s_recordRef;
    o["test"]      = s_testStep >= 0 ? ALL_LEDS[s_testStep] : -1;
    o["testName"]  = s_testStep >= 0 ? ALL_LED_NAMES[s_testStep] : "";
    o["reclaims"]  = s_reclaims;
    JsonArray dg = o["pins"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        JsonObject d = dg.add<JsonObject>();
        d["pin"]  = ALL_LEDS[i];
        d["name"] = ALL_LED_NAMES[i];
        d["out"]  = (REG_READ(GPIO_OUT_REG) >> ALL_LEDS[i]) & 1;
        d["rtc"]  = pin_rtc_muxed(ALL_LEDS[i]);
    }
    JsonArray t = o["thr"].to<JsonArray>();
    for (double v : s_thr) t.add(isinf(v) ? 0.0 : v);

    JsonObject a = o["ant"].to<JsonObject>();
    a["pinA"] = PIN_TOUCH_A;  a["pinB"] = PIN_TOUCH_B;
    a["a"] = roundf(s_ant.meanA * 10) / 10;  a["b"] = roundf(s_ant.meanB * 10) / 10;
    a["baseA"] = roundf(s_ant.baseA * 10) / 10; a["baseB"] = roundf(s_ant.baseB * 10) / 10;
    a["jA"] = s_ant.maxA - s_ant.minA;  a["jB"] = s_ant.maxB - s_ant.minB;
    a["adc"] = s_ant.adc;  a["n"] = s_ant.samples;
    a["act"] = roundf(s_ant.activity * 10) / 10;
    a["ghost"] = s_ant.ghostNum;  a["qs"] = s_ant.ghostQs;
    a["temp"] = roundf(s_ant.tempC * 10) / 10;
    a["seed"] = s_ant.seed;
    int seg = antenna_segments(s_ant.meanB);
    int cm  = GHOST_ANT_BASE_CM + seg * GHOST_ANT_SEG_CM;
    a["seg"]   = seg;
    a["segs"]  = ANT_STEPS - 1;
    a["cm"]    = cm;
    a["mhz"]   = 7500 / cm;   // Viertelwelle: f = c / (4 * L)

    JsonObject h = o["hist"].to<JsonObject>();
    h["rounds"] = s_hist.rounds;
    JsonArray hl = h["led"].to<JsonArray>();
    for (uint32_t v : s_hist.led) hl.add(v);
    h["uptime"] = s_hist.uptimeS;
    h["best"]   = s_hist.best;
    h["blocks"] = s_hist.blocks;
    h["sniffs"] = s_hist.sniffs;
    h["shares"] = s_hist.shares;

    if (s_era26.valid) {
        JsonObject e = o["era26"].to<JsonObject>();
        e["scans"] = s_era26.scans;
        JsonArray el2 = e["led"].to<JsonArray>();
        for (uint32_t v : s_era26.led) el2.add(v);
        e["uptime"] = s_era26.uptimeS;
    }
}

}  // namespace ghost
