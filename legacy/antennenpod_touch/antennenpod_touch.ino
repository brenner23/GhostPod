#include <mbedtls/md.h>
#include "esp_task_wdt.h"

// --- EINSTELLUNGEN ---
const int Reset_LEDS = 1;           // 1 = LEDs gehen bei Mining-Start aus | 0 = bleiben bis zum ersten Fund
const int ShowFinalLED_Time = 5;    // Sekunden zum Bewundern der LEDs nach dem Mining
const int MaxTimeHashPerBlock = 60; 
const int ShowHash = 10;
const int DIFF_LED1 = 4; 
const int DIFF_LED2 = 5; 
const int DIFF_LED3 = 6; 
const int DIFF_LED4 = 7; 

// --- SYSTEM VARIABLEN ---
volatile int val33 = 0, val32 = 0, totalVal = 0;
int blockVal33, blockVal32, blockTotal; 

volatile uint32_t hashesTotal = 0;
uint32_t lastStatsMillis = 0;
uint32_t lastBlockChangeMillis = 0;
volatile bool isCollecting = true;
volatile bool firstFindInBlock = false; 
volatile int highestLevelInBlock = 0;

SemaphoreHandle_t serialMutex;

// Pins
const int pinRotLinks = 25, pinRotRechts = 26, pinRotOben = 27, pinHerzschlag = 14;
const int pinGruenStabil = 13;
const int pinGruenBlinker = 12;

// --- HILFSFUNKTIONEN ---

int countLeadingZeros(byte* hash) {
    int zeros = 0;
    for (int i = 0; i < 32; i++) {
        if (hash[i] == 0) { zeros += 2; } 
        else {
            if ((hash[i] >> 4) == 0) zeros += 1;
            break;
        }
    }
    return zeros;
}

// Betriebszeit seit Einschalten als lesbarer String
void formatUptime(char* buf, size_t bufLen) {
    uint32_t totalSec = millis() / 1000;
    uint32_t s  = totalSec % 60;
    uint32_t m  = (totalSec / 60) % 60;
    uint32_t h  = (totalSec / 3600) % 24;
    uint32_t d  = (totalSec / 86400) % 7;
    uint32_t w  = (totalSec / 604800) % 4;
    uint32_t mo = (totalSec / 2419200) % 12;  // ~28 Tage pro Monat
    uint32_t y  = (totalSec / 29030400);       // ~336 Tage pro Jahr
    snprintf(buf, bufLen, "%luY %luM %luW %luD %luH %luM %lus",
             y, mo, w, d, h, m, s);
}

void setVisualLevel(int zeros) {
    if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
        
        // RESET-LOGIK: Beim ersten Fund im neuen Block alles Alte löschen
        if (!firstFindInBlock) {
            digitalWrite(pinRotLinks, LOW);
            digitalWrite(pinRotRechts, LOW);
            digitalWrite(pinRotOben, LOW);
            digitalWrite(pinHerzschlag, LOW);
            
            firstFindInBlock = true; 
            highestLevelInBlock = 0; 
        }

        // --- LED 1 ---
        if (zeros >= DIFF_LED1 && highestLevelInBlock < DIFF_LED1) {
            digitalWrite(pinRotLinks, HIGH);
            Serial.printf("[LED1] Found DIFF_LED1 -> %d (", DIFF_LED1);
            for(int i=0; i<DIFF_LED1; i++) Serial.print("0");
            Serial.println(")");
            highestLevelInBlock = DIFF_LED1;
        }

        // --- LED 2 ---
        if (zeros >= DIFF_LED2 && highestLevelInBlock < DIFF_LED2) {
            digitalWrite(pinRotRechts, HIGH);
            Serial.printf("[LED2] Found DIFF_LED2 -> %d (", DIFF_LED2);
            for(int i=0; i<DIFF_LED2; i++) Serial.print("0");
            Serial.println(")");
            highestLevelInBlock = DIFF_LED2;
        }

        // --- LED 3 ---
        if (zeros >= DIFF_LED3 && highestLevelInBlock < DIFF_LED3) {
            digitalWrite(pinRotOben, HIGH);
            Serial.printf("[LED3] Found DIFF_LED3 -> %d (", DIFF_LED3);
            for(int i=0; i<DIFF_LED3; i++) Serial.print("0");
            Serial.println(")");
            highestLevelInBlock = DIFF_LED3;
        }

        // --- LED 4 (JACKPOT) ---
        if (zeros >= DIFF_LED4 && highestLevelInBlock < DIFF_LED4) {
            digitalWrite(pinHerzschlag, HIGH);
            Serial.printf("[LED4] Found DIFF_LED4 -> %d (", DIFF_LED4);
            for(int i=0; i<DIFF_LED4; i++) Serial.print("0");
            Serial.println(") !!! JACKPOT !!!");
            highestLevelInBlock = DIFF_LED4;
        }

        xSemaphoreGive(serialMutex);
    }
}

void miningTaskCore0(void * pvParameters) {
    byte shaResult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    uint32_t core0Nonce = 1000000;
    uint32_t yieldCounter = 0;

    while(true) {
        if (!isCollecting) {
            core0Nonce++;
            hashesTotal++;
            yieldCounter++;
            String payload = String(blockVal33) + String(blockVal32) + String(blockTotal) + "C0" + String(core0Nonce);
            mbedtls_md_starts(&ctx);
            mbedtls_md_update(&ctx, (const unsigned char*) payload.c_str(), payload.length());
            mbedtls_md_finish(&ctx, shaResult);
            int zeros = countLeadingZeros(shaResult);
            if (zeros >= DIFF_LED1) setVisualLevel(zeros);
            if (yieldCounter >= 1000) { vTaskDelay(1); yieldCounter = 0; }
        } else {
            vTaskDelay(10 / portTICK_PERIOD_MS);
            yieldCounter = 0;
        }
    }
}

void passiveScanTask(void * pvParameters) {
    while(true) {
        val33 = touchRead(33); val32 = touchRead(32);
        totalVal = val33 + val32;
        vTaskDelay(10 / portTICK_PERIOD_MS); 
    }
}

void setup() {
    setCpuFrequencyMhz(240);
    Serial.begin(115200);
    serialMutex = xSemaphoreCreateMutex();
    int pins[] = {pinGruenStabil, pinGruenBlinker, pinRotLinks, pinRotRechts, pinRotOben, pinHerzschlag};
    for (int p : pins) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
    xTaskCreatePinnedToCore(passiveScanTask, "Scan", 2048, NULL, 1, NULL, 0);
    TaskHandle_t miningTask0Handle;
    xTaskCreatePinnedToCore(miningTaskCore0, "Mining0", 4096, NULL, 1, &miningTask0Handle, 0);
    esp_task_wdt_delete(miningTask0Handle); 
    lastBlockChangeMillis = millis();
}

void loop() {
    uint32_t currentMillis = millis();
    uint32_t elapsed = (currentMillis - lastBlockChangeMillis) / 1000;

    // --- PHASE 1: SCAN ---
    if (elapsed < 3) {
        if (!isCollecting) {
            Serial.println("\n[!] PHASE: SCAN...");
            isCollecting = true;
        }
        digitalWrite(pinGruenStabil, (currentMillis / 80) % 2);
        digitalWrite(pinGruenBlinker, (currentMillis / 80) % 2);
        blockVal33 = val33; blockVal32 = val32; blockTotal = totalVal;
        return; 
    }

    // --- PHASE 2: MINING START ---
    if (isCollecting) {
        isCollecting = false;
        firstFindInBlock = false; 
        highestLevelInBlock = 0;
        digitalWrite(pinGruenStabil, HIGH);
        if (Reset_LEDS == 1) {
            digitalWrite(pinRotLinks, LOW); digitalWrite(pinRotRechts, LOW);
            digitalWrite(pinRotOben, LOW); digitalWrite(pinHerzschlag, LOW);
            firstFindInBlock = true; 
        }
    }

    digitalWrite(pinGruenBlinker, (currentMillis / 1000) % 2);

    // --- BLOCK-ENDE CHECK (Mining Zeit abgelaufen) ---
    if (elapsed >= MaxTimeHashPerBlock + 3) {
        digitalWrite(pinGruenStabil, LOW);
        digitalWrite(pinGruenBlinker, LOW);
        isCollecting = true; // Stoppt Core 0 Mining sofort

        if (ShowFinalLED_Time > 0) {
            Serial.printf("[IDLE] Displaying results for %ds...\n", ShowFinalLED_Time);
            delay(ShowFinalLED_Time * 1000);
        }

        if (!firstFindInBlock || highestLevelInBlock == 0) {
            digitalWrite(pinRotLinks, LOW); digitalWrite(pinRotRechts, LOW);
            digitalWrite(pinRotOben, LOW); digitalWrite(pinHerzschlag, LOW);
        }
        lastBlockChangeMillis = millis(); 
        return;
    }

    // --- CORE 1 MINING ---
    static uint32_t core1Nonce = 5000000;
    core1Nonce++;
    hashesTotal++;

    String payload = String(blockVal33) + String(blockVal32) + String(blockTotal) + "C1" + String(core1Nonce);
    byte shaResult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char*) payload.c_str(), payload.length());
    mbedtls_md_finish(&ctx, shaResult);
    mbedtls_md_free(&ctx);

    int foundZeros = countLeadingZeros(shaResult);
    if (foundZeros >= DIFF_LED1) setVisualLevel(foundZeros);

    // Statistik
    if (currentMillis - lastStatsMillis >= (ShowHash * 1000)) {
        float realHz = (float)hashesTotal / ShowHash; 
        String unit = " kH/s"; realHz /= 1000.0;
        Serial.printf("DUAL-CORE Mining... %ds left | Speed: ", (MaxTimeHashPerBlock + 3 - elapsed));
        Serial.print(realHz, 2); Serial.println(unit);
        hashesTotal = 0;
        lastStatsMillis = currentMillis;
    }
}