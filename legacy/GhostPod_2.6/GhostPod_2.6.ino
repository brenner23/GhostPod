#include <mbedtls/md.h>
#include "esp_task_wdt.h"
#include <EEPROM.h>

#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif

// ============================================================
//   G H O S T   P O D   2 . 6
//   Geisterdetector – 4 Stufen vom Seelchen bis zum Vollgeist
//
//   Basis: 2.5
//   NEU in 2.6:
//   + Betriebszeit-Zähler (akkumuliert über Neustarts per EEPROM)
//   + formatUptime() → Y M W D H Min S
//   + [HISTORY] zeigt jetzt WORKTIME=
// ============================================================

// --- EINSTELLUNGEN ---
const int Reset_LEDS          = 1;   // 1 = LEDs gehen bei Scan-Start aus
const int ShowFinalLED_Time   = 5;   // Sekunden Ergebnis bewundern
const int MaxTimeHashPerBlock = 60;  // Sekunden pro Scan-Block
const int ShowHash            = 10;  // Statistik alle N Sekunden
const int ShowPayload           = 1;      // 1 = Payload-Chain im Serial anzeigen
const int PayloadLength         = 20;     // Länge des finalen Mining-Prefix (max 64)
const int NonceRandom           = 1;      // 1 = Geist-gesteuerte Nonce-Sprünge
const int NonceJumpAfterHashes  = 50000;  // alle N Hashes springt der Geist

// --- GEISTER-STUFEN (führende Hex-Nullen im SHA256) ---
const int DIFF_LED1 = 4;  // 👻  kleine Seele    (1 : 65k)
const int DIFF_LED2 = 5;  // 👻👻 Seele           (1 : 1M)
const int DIFF_LED3 = 6;  // 👻👻👻 starker Geist (1 : 16M)
const int DIFF_LED4 = 7;  // 👻👻👻👻 VOLLGEIST!  (1 : 268M)

// --- BEEPER AN/AUS pro Stufe ---
const int BeepON_LED1 = 0;
const int BeepON_LED2 = 0;
const int BeepON_LED3 = 0;
const int BeepON_LED4 = 1;

// --- PINS ---
const int pinRotLinks     = 25;
const int pinRotRechts    = 26;
const int pinRotOben      = 27;
const int pinHerzschlag   = 14;
const int pinGruenStabil  = 13;
const int pinGruenBlinker = 12;
const int pinSicherung    = 21;   // Sicherungs-Pin (INPUT_PULLUP, GND = normal)
const int BEEPER_PIN      = 23;

// ============================================================
//   EEPROM LAYOUT
//   Adresse 0..3   : uint32_t scanCount
//   Adresse 4..7   : uint32_t countLED1
//   Adresse 8..11  : uint32_t countLED2
//   Adresse 12..15 : uint32_t countLED3
//   Adresse 16..19 : uint32_t countLED4
//   Adresse 20..23 : uint32_t magic (0xDEADBEEF = gültig)
//   Adresse 24..27 : uint32_t Betriebszeit in Sekunden (akkumuliert)
// ============================================================
#define EEPROM_SIZE       28
#define ADDR_SCAN         0
#define ADDR_LED1         4
#define ADDR_LED2         8
#define ADDR_LED3         12
#define ADDR_LED4         16
#define ADDR_MAGIC        20
#define ADDR_UPTIME       24
#define EEPROM_MAGIC      0xDEADBEEF

uint32_t histScan      = 0;
uint32_t histLED1      = 0;
uint32_t histLED2      = 0;
uint32_t histLED3      = 0;
uint32_t histLED4      = 0;
uint32_t histUptimeSec    = 0;   // Betriebszeit in Sekunden, überlebt Neustarts
uint32_t lastUptimeSaveMs = 0;   // millis() beim letzten Uptime-Save (für Delta-Berechnung)

// EEPROM lesen
void eepromLoad() {
    EEPROM.get(ADDR_SCAN,   histScan);
    EEPROM.get(ADDR_LED1,   histLED1);
    EEPROM.get(ADDR_LED2,   histLED2);
    EEPROM.get(ADDR_LED3,   histLED3);
    EEPROM.get(ADDR_LED4,   histLED4);
    EEPROM.get(ADDR_UPTIME, histUptimeSec);
}

// EEPROM schreiben
void eepromSave() {
    EEPROM.put(ADDR_SCAN,   histScan);
    EEPROM.put(ADDR_LED1,   histLED1);
    EEPROM.put(ADDR_LED2,   histLED2);
    EEPROM.put(ADDR_LED3,   histLED3);
    EEPROM.put(ADDR_LED4,   histLED4);
    EEPROM.put(ADDR_UPTIME, histUptimeSec);
    EEPROM.put(ADDR_MAGIC,  (uint32_t)EEPROM_MAGIC);
    EEPROM.commit();
}

// EEPROM auf 0 zurücksetzen
void eepromReset() {
    histScan = histLED1 = histLED2 = histLED3 = histLED4 = histUptimeSec = 0;
    eepromSave();
}

// Sekunden in lesbaren String umrechnen
void formatUptime(char* buf, size_t bufLen) {
    uint32_t t  = histUptimeSec + (millis() - lastUptimeSaveMs) / 1000;
    uint32_t s  = t % 60;
    uint32_t mi = (t / 60) % 60;
    uint32_t h  = (t / 3600) % 24;
    uint32_t d  = (t / 86400) % 7;
    uint32_t w  = (t / 604800) % 4;
    uint32_t mo = (t / 2419200) % 12;
    uint32_t y  =  t / 29030400;
    snprintf(buf, bufLen, "%luY %luM %luW %luD %luH %luMin %luS", y, mo, w, d, h, mi, s);
}

// History im Serial ausgeben
void printHistory() {
    char upStr[56];
    formatUptime(upStr, sizeof(upStr));
    Serial.println("------------------------------");
    Serial.printf("[HISTORY] WORKTIME= %s | Scans=%lu | LED1=%lu | LED2=%lu | LED3=%lu | LED4=%lu\n",
                  upStr,
                  (unsigned long)histScan,
                  (unsigned long)histLED1,
                  (unsigned long)histLED2,
                  (unsigned long)histLED3,
                  (unsigned long)histLED4);
    Serial.println("------------------------------");
}

// --- SYSTEM VARIABLEN ---
volatile int val33 = 0, val32 = 0, totalVal = 0;
int blockVal33, blockVal32, blockTotal;

int      blockTempK    = 0;
uint32_t blockUptimeMs = 0;
int      blockGhostNum = 0;
int      blockGhostQS  = 0;

char blockMiningPrefix[68];

volatile uint32_t hashesTotal           = 0;
uint32_t          lastStatsMillis       = 0;
uint32_t          lastBlockChangeMillis = 0;

volatile bool isCollecting        = true;
volatile bool firstFindInBlock    = false;
volatile int  highestLevelInBlock = 0;

SemaphoreHandle_t serialMutex;

// ============================================================
//   HILFSFUNKTIONEN
// ============================================================

void Beep(int ms) {
    digitalWrite(BEEPER_PIN, HIGH);
    delay(ms);
    digitalWrite(BEEPER_PIN, LOW);
    delay(40);  // kurze Pause zwischen mehreren Beeps
}

int getCpuTempK() {
    float celsius = (temprature_sens_read() - 32) / 1.8f;
    return (int)(celsius + 273.15f);
}

int quersumme(int n) {
    int sum = 0;
    if (n < 0) n = -n;
    while (n > 0) { sum += n % 10; n /= 10; }
    return sum;
}

void sha256hex(const char* input, char* outHex) {
    byte raw[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char*)input, strlen(input));
    mbedtls_md_finish(&ctx, raw);
    mbedtls_md_free(&ctx);
    for (int i = 0; i < 32; i++) sprintf(outHex + i * 2, "%02x", raw[i]);
    outHex[64] = '\0';
}

void buildMiningPrefix() {
    char basis[128];
    snprintf(basis, sizeof(basis), "%d%d%d%d%lu%d%d",
             blockVal33, blockVal32, blockTotal,
             blockTempK, (unsigned long)blockUptimeMs,
             blockGhostNum, blockGhostQS);

    char hash1[65];
    sha256hex(basis, hash1);

    char mid[21];
    strncpy(mid, hash1, 20);
    mid[20] = '\0';

    char hash2[65];
    sha256hex(mid, hash2);

    int len = PayloadLength;
    if (len > 64) len = 64;
    strncpy(blockMiningPrefix, hash2, len);
    blockMiningPrefix[len] = '\0';

    if (ShowPayload == 1) {
        Serial.printf("[CHAIN] Basis  : %s\n", basis);
        Serial.printf("[CHAIN] Hash1  : %s\n", hash1);
        Serial.printf("[CHAIN] Mid    : %s\n", mid);
        Serial.printf("[CHAIN] Hash2  : %s\n", hash2);
        Serial.printf("[CHAIN] PREFIX : %s  (len=%d)\n", blockMiningPrefix, len);
        Serial.printf("[CHAIN] Mining : %s00001 ...\n", blockMiningPrefix);
    }
}

int countLeadingZeros(byte* hash) {
    int zeros = 0;
    for (int i = 0; i < 32; i++) {
        if (hash[i] == 0) {
            zeros += 2;
        } else {
            if ((hash[i] >> 4) == 0) zeros += 1;
            break;
        }
    }
    return zeros;
}

void setVisualLevel(int zeros) {
    if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
        if (!firstFindInBlock) {
            digitalWrite(pinRotLinks,   LOW);
            digitalWrite(pinRotRechts,  LOW);
            digitalWrite(pinRotOben,    LOW);
            digitalWrite(pinHerzschlag, LOW);
            firstFindInBlock    = true;
            highestLevelInBlock = 0;
        }
        if (zeros >= DIFF_LED1 && highestLevelInBlock < DIFF_LED1) {
            digitalWrite(pinRotLinks, HIGH);
            Serial.println("[LED1] 👻  kleine Seele  (0000)");
            histLED1++;
            eepromSave();
            if (BeepON_LED1 == 1) Beep(80);
            highestLevelInBlock = DIFF_LED1;
        }
        if (zeros >= DIFF_LED2 && highestLevelInBlock < DIFF_LED2) {
            digitalWrite(pinRotRechts, HIGH);
            Serial.println("[LED2] 👻👻 Seele         (00000)");
            histLED2++;
            eepromSave();
            if (BeepON_LED2 == 1) Beep(120);
            highestLevelInBlock = DIFF_LED2;
        }
        if (zeros >= DIFF_LED3 && highestLevelInBlock < DIFF_LED3) {
            digitalWrite(pinRotOben, HIGH);
            Serial.println("[LED3] 👻👻👻 starker Geist (000000)");
            histLED3++;
            eepromSave();
            if (BeepON_LED3 == 1) Beep(160);
            highestLevelInBlock = DIFF_LED3;
        }
        if (zeros >= DIFF_LED4 && highestLevelInBlock < DIFF_LED4) {
            digitalWrite(pinHerzschlag, HIGH);
            Serial.println("[LED4] 👻👻👻👻 !!!  V O L L G E I S T  !!! (0000000)");
            histLED4++;
            eepromSave();
            if (BeepON_LED4 == 1) {
                Beep(80); Beep(80); Beep(80); Beep(80);
            }
            highestLevelInBlock = DIFF_LED4;
        }
        xSemaphoreGive(serialMutex);
    }
}

// ============================================================
//   CORE 0 – Mining Task
// ============================================================
void miningTaskCore0(void* pvParameters) {
    byte shaResult[32];
    char payload[96];

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);

    uint32_t nonce    = (NonceRandom == 1) ? esp_random() : 1000000;
    uint32_t yieldCnt = 0;
    uint32_t jumpCnt  = 0;

    while (true) {
        if (!isCollecting) {
            nonce++;
            hashesTotal++;
            yieldCnt++;
            jumpCnt++;

            if (NonceRandom == 1 && jumpCnt >= (uint32_t)NonceJumpAfterHashes) {
                int32_t geistSchubs = (int32_t)esp_random();
                nonce   += (uint32_t)geistSchubs;
                jumpCnt  = 0;
            }

            snprintf(payload, sizeof(payload), "%sC0%lu",
                     blockMiningPrefix, (unsigned long)nonce);

            mbedtls_md_starts(&ctx);
            mbedtls_md_update(&ctx, (const unsigned char*)payload, strlen(payload));
            mbedtls_md_finish(&ctx, shaResult);

            int zeros = countLeadingZeros(shaResult);
            if (zeros >= DIFF_LED1) setVisualLevel(zeros);

            if (yieldCnt >= 1000) { vTaskDelay(1); yieldCnt = 0; }
        } else {
            nonce   = (NonceRandom == 1) ? esp_random() : 1000000;
            jumpCnt = 0;
            vTaskDelay(10 / portTICK_PERIOD_MS);
            yieldCnt = 0;
        }
    }
}

// ============================================================
//   CORE 0 – Passiver Touch-Scan Task
// ============================================================
void passiveScanTask(void* pvParameters) {
    while (true) {
        val33    = touchRead(33);
        val32    = touchRead(32);
        totalVal = val33 + val32;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ============================================================
//   SETUP
// ============================================================
void setup() {
    pinMode(BEEPER_PIN, OUTPUT);
    Beep(80);

    setCpuFrequencyMhz(240);
    Serial.begin(115200);
    delay(3000);  // etwas länger warten damit Serial Monitor Zeit hat

    Serial.println("\n==============================");
    Serial.println("   G H O S T   P O D   2.6   ");
    Serial.println("   + EEPROM Historie          ");
    Serial.println("   + Betriebszeit WORKTIME     ");
    Serial.println("   + Pin Sicherung             ");
    Serial.println("==============================\n");

    // EEPROM initialisieren
    EEPROM.begin(EEPROM_SIZE);
    uint32_t magic = 0;
    EEPROM.get(ADDR_MAGIC, magic);

    if (magic != (uint32_t)EEPROM_MAGIC) {
        // Erster Start oder korrupte Daten → sauber initialisieren
        Serial.println("[EEPROM] Kein gueltiger Speicher gefunden – initialisiere...");
        eepromReset();
    } else {
        eepromLoad();
    }

    // ── PIN 21 SICHERUNG ──────────────────────────────────────
    // Pin 21 wird als Input mit PullUp gelesen.
    // Normal: Draht auf GND gezogen → LOW → alles OK
    // Sicherung gezogen (kein GND): HIGH → History-Reset!
    pinMode(pinSicherung, INPUT_PULLUP);
    delay(10);  // kurz stabilisieren
    int sicherung = digitalRead(pinSicherung);

    if (sicherung == HIGH) {
        Serial.println("[SICHERUNG] Pin 21 NICHT auf GND! --> History wird GELOESCHT!");
        Beep(300);
        delay(100);
        Beep(300);
        eepromReset();
        Serial.println("[SICHERUNG] History auf 0 gesetzt.");
    } else {
        Serial.println("[SICHERUNG] Pin 21 auf GND – OK, History bleibt erhalten.");
    }
    // ─────────────────────────────────────────────────────────

    // History ausgeben
    printHistory();

    delay(3000);
    Beep(80);

    serialMutex = xSemaphoreCreateMutex();

    int pins[] = { pinGruenStabil, pinGruenBlinker,
                   pinRotLinks, pinRotRechts, pinRotOben, pinHerzschlag };
    for (int p : pins) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }

    for (int p : pins) { digitalWrite(p, HIGH); delay(80); }
    delay(300);
    for (int p : pins) { digitalWrite(p, LOW); }

    xTaskCreatePinnedToCore(passiveScanTask, "Scan",    2048, NULL, 1, NULL, 0);
    TaskHandle_t miningTask0Handle;
    xTaskCreatePinnedToCore(miningTaskCore0, "Mining0", 4096, NULL, 1, &miningTask0Handle, 0);
    esp_task_wdt_delete(miningTask0Handle);

    lastBlockChangeMillis = millis();
}

// ============================================================
//   LOOP – Core 1 Mining + Phasen-Steuerung
// ============================================================
void loop() {
    static mbedtls_md_context_t ctx1;
    static bool ctx1Ready = false;
    if (!ctx1Ready) {
        mbedtls_md_init(&ctx1);
        mbedtls_md_setup(&ctx1, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
        ctx1Ready = true;
    }

    uint32_t currentMillis = millis();
    uint32_t elapsed       = (currentMillis - lastBlockChangeMillis) / 1000;

    // --- PHASE 1: SCAN ---
    if (elapsed < 3) {
        if (!isCollecting) {
            Serial.println("\n[SCAN] Neue Messung – Touch-Antenne wird gelesen...");
            isCollecting = true;
        }
        digitalWrite(pinGruenStabil,  (currentMillis / 80) % 2);
        digitalWrite(pinGruenBlinker, (currentMillis / 80) % 2);
        blockVal33 = val33;
        blockVal32 = val32;
        blockTotal = totalVal;
        return;
    }

    // --- PHASE 2: MINING START ---
    if (isCollecting) {
        isCollecting        = false;
        firstFindInBlock    = false;
        highestLevelInBlock = 0;

        blockTempK    = getCpuTempK();
        blockUptimeMs = millis();

        int ghostChoice = esp_random() % 3;
        blockGhostNum = (ghostChoice == 0) ? 333 : (ghostChoice == 1) ? 666 : 999;
        blockGhostQS  = quersumme(blockGhostNum);

        // Scan-Counter erhöhen
        histScan++;
        eepromSave();

        digitalWrite(pinGruenStabil, HIGH);

        Serial.printf("[MINING] Start! Touch: T33=%d T32=%d Total=%d | Temp=%dK | Uptime=%lums\n",
                      blockVal33, blockVal32, blockTotal,
                      blockTempK, (unsigned long)blockUptimeMs);
        Serial.printf("[GHOST]  GhostZahl=%d | Quersumme=%d\n",
                      blockGhostNum, blockGhostQS);

        buildMiningPrefix();

        if (Reset_LEDS == 1) {
            digitalWrite(pinRotLinks,   LOW);
            digitalWrite(pinRotRechts,  LOW);
            digitalWrite(pinRotOben,    LOW);
            digitalWrite(pinHerzschlag, LOW);
            firstFindInBlock = true;
        }
    }

    digitalWrite(pinGruenBlinker, (currentMillis / 1000) % 2);

    // --- BLOCK-ENDE ---
    if (elapsed >= MaxTimeHashPerBlock + 3) {
        digitalWrite(pinGruenStabil,  LOW);
        digitalWrite(pinGruenBlinker, LOW);
        isCollecting = true;

        int ghostLevel = highestLevelInBlock;
        if      (ghostLevel >= DIFF_LED4) Serial.println("[ERGEBNIS] 👻👻👻👻 VOLLGEIST DETECTED!!!");
        else if (ghostLevel >= DIFF_LED3) Serial.println("[ERGEBNIS] 👻👻👻 Starker Geist");
        else if (ghostLevel >= DIFF_LED2) Serial.println("[ERGEBNIS] 👻👻 Seele");
        else if (ghostLevel >= DIFF_LED1) Serial.println("[ERGEBNIS] 👻  Kleine Seele");
        else                              Serial.println("[ERGEBNIS] Nichts gefunden. Kein Geist.");

        // Betriebszeit akkumulieren: nur das Delta seit letztem Save addieren
        uint32_t nowMs = millis();
        histUptimeSec += (nowMs - lastUptimeSaveMs) / 1000;
        lastUptimeSaveMs = nowMs;
        // History nach Block ausgeben
        printHistory();

        if (ShowFinalLED_Time > 0) {
            Serial.printf("[IDLE] Zeige Ergebnis fuer %ds...\n", ShowFinalLED_Time);
            delay(ShowFinalLED_Time * 1000);
        }

        if (!firstFindInBlock || highestLevelInBlock == 0) {
            digitalWrite(pinRotLinks,   LOW);
            digitalWrite(pinRotRechts,  LOW);
            digitalWrite(pinRotOben,    LOW);
            digitalWrite(pinHerzschlag, LOW);
        }

        lastBlockChangeMillis = millis();
        return;
    }

    // --- CORE 1 MINING ---
    static uint32_t core1Nonce   = 0;
    static uint32_t core1JumpCnt = 0;
    static bool     core1Started = false;

    if (!core1Started || isCollecting) {
        core1Nonce    = (NonceRandom == 1) ? esp_random() : 5000000;
        core1JumpCnt  = 0;
        core1Started  = true;
    }

    core1Nonce++;
    core1JumpCnt++;

    if (NonceRandom == 1 && core1JumpCnt >= (uint32_t)NonceJumpAfterHashes) {
        int32_t geistSchubs = (int32_t)esp_random();
        core1Nonce   += (uint32_t)geistSchubs;
        core1JumpCnt  = 0;
    }
    hashesTotal++;

    char payload[96];
    snprintf(payload, sizeof(payload), "%sC1%lu",
             blockMiningPrefix, (unsigned long)core1Nonce);

    byte shaResult[32];
    mbedtls_md_starts(&ctx1);
    mbedtls_md_update(&ctx1, (const unsigned char*)payload, strlen(payload));
    mbedtls_md_finish(&ctx1, shaResult);

    int foundZeros = countLeadingZeros(shaResult);
    if (foundZeros >= DIFF_LED1) setVisualLevel(foundZeros);

    // --- STATISTIK ---
    if (currentMillis - lastStatsMillis >= (uint32_t)(ShowHash * 1000)) {
        float khs = (float)hashesTotal / ShowHash / 1000.0f;
        Serial.printf("[GHOST POD] %ds left | %.2f kH/s | T33=%d T32=%d | %dK | Ghost=%d\n",
                      (MaxTimeHashPerBlock + 3 - (int)elapsed),
                      khs, blockVal33, blockVal32,
                      blockTempK, blockGhostNum);
        hashesTotal     = 0;
        lastStatsMillis = currentMillis;
    }
}