/*
 * GPS Logger for M5Stack Tab5 + GPS Unit v1.1
 *
 * 機能:
 *   - GPS データを 5 分おきに SD カードへ CSV 形式で記録
 *   - ボタン押下時にディスプレイ点灯、現在地（都道府県・市）を表示
 *   - 住所はオフライン (SD カード上の addr.csv) から検索
 *   - 30 秒後にディスプレイ自動消灯
 *   - 省電力設計（目標バッテリー持続: 24 時間以上）
 *
 * 省電力のポイント:
 *   - CPU 周波数 240MHz → 80MHz に削減
 *   - WiFi / Bluetooth は完全オフ (使用しない)
 *   - ディスプレイはボタン押下時のみ点灯
 *
 * 住所検索:
 *   SD カードの /gpsdb/addr.csv を緯度でスキャンして最近傍の市区町村を返す。
 *   事前に tools/generate_addr_db.py を実行して CSV を生成してください。
 *
 * ログファイル:
 *   /gpslog/gps_YYYYMMDD.csv (日付ごとに 1 ファイル)
 *   ヘッダー: timestamp,latitude,longitude,altitude_m,speed_kmh,satellites,hdop
 */

#include <M5Unified.h>
#include <TinyGPSPlus.h>
#include <SD_MMC.h>
#include <math.h>
#include "config.h"

// =============================================================
// 型定義
// =============================================================

struct GpsSnapshot {
    bool    valid      = false;
    double  lat        = 0.0;
    double  lon        = 0.0;
    float   altitude   = 0.0f;
    float   speed      = 0.0f;
    uint8_t satellites = 0;
    float   hdop       = 99.9f;
    int     year       = 0;
    uint8_t month      = 0;
    uint8_t day        = 0;
    uint8_t hour       = 0;
    uint8_t minute     = 0;
    uint8_t second     = 0;
};

struct AddressInfo {
    char  prefecture[32] = "";
    char  city[48]       = "";
    bool  fetched        = false;
    double cachedLat     = 0.0;
    double cachedLon     = 0.0;
};

// =============================================================
// グローバル状態
// =============================================================

TinyGPSPlus    gps;
HardwareSerial gpsSerial(2);
char           gpsRawPreview[24] = "---";  // 受信生データ先頭プレビュー
uint8_t        gpsPreviewLen     = 0;

GpsSnapshot current;
AddressInfo address;

bool     sdAvailable       = false;
bool     displayOn         = false;
uint32_t displayOnTime     = 0;
uint32_t lastDisplayUpdate = 0;
uint32_t lastLogTime       = 0;
bool     waitingForFix     = true;

// =============================================================
// 関数プロトタイプ
// =============================================================
void initSD();
void readGPS();
void logToSD();
String buildLogPath();
void turnOnDisplay();
void turnOffDisplay();
void drawDisplay();
void onButtonPressed();
bool lookupAddress(double lat, double lon);
bool locationMoved(double newLat, double newLon);

// =============================================================
// setup
// =============================================================
void setup() {
    delay(3000);           // USB CDC 再接続待ち
    Serial.begin(115200);
    Serial.println("[BOOT] Serial OK");

    auto cfg = M5.config();
    Serial.println("[BOOT] M5.begin...");
    M5.begin(cfg);
    Serial.println("[BOOT] M5.begin OK");

    Serial.println("[GPS Logger] Booting...");

    // 省電力: CPU 周波数を下げる
    // ESP32-P4 での動作確認後に有効化してください
    // setCpuFrequencyMhz(CPU_FREQ_MHZ);
    // Serial.printf("[Power] CPU: %d MHz\n", getCpuFrequencyMhz());

    // ディスプレイ: 消灯状態で初期化
    M5.Display.setRotation(1);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setBrightness(0);

    // GPS UART 初期化
    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[GPS] UART2 RX=%d TX=%d BAUD=%d\n", GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    // SD カード初期化
    initSD();

    lastLogTime = millis();
    Serial.println("[GPS Logger] Ready. Waiting for GPS fix...");
}

// =============================================================
// loop
// =============================================================
void loop() {
    M5.update();

    // GPS UART を読み込む
    readGPS();

    // ボタン A / B でディスプレイ点灯
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) {
        onButtonPressed();
    }

    // タッチのタップでもディスプレイ点灯
    auto touch = M5.Touch.getDetail();
    if (touch.wasClicked()) {
        onButtonPressed();
    }

    uint32_t now = millis();

    // GPS 受信デバッグ: 5 秒ごとに処理文字数を表示
    static uint32_t lastGpsDebug  = 0;
    static uint32_t lastCharCount = 0;
    if (now - lastGpsDebug >= 5000) {
        uint32_t chars = gps.charsProcessed();
        Serial.printf("[GPS DBG] 処理文字数=%u (+%u)  衛星=%u  フィックス=%s\n",
                      chars, chars - lastCharCount,
                      gps.satellites.value(),
                      current.valid ? "あり" : "なし");
        lastCharCount = chars;
        lastGpsDebug  = now;
    }

    // GPS フィックス待ち (起動直後)
    if (waitingForFix) {
        if (current.valid) {
            waitingForFix = false;
            Serial.println("[GPS] Fix acquired!");
            logToSD();
            lastLogTime = now;
        } else if (now - lastLogTime > (uint32_t)(INITIAL_FIX_WAIT_SEC * 1000UL)) {
            waitingForFix = false;
            lastLogTime   = now;
        }
    } else {
        // 5 分ごとにログ記録
        if (now - lastLogTime >= (uint32_t)(LOG_INTERVAL_SEC * 1000UL)) {
            logToSD();
            lastLogTime = now;
        }
    }

    // ディスプレイ自動消灯
    if (displayOn && now - displayOnTime >= DISPLAY_TIMEOUT_MS) {
        turnOffDisplay();
    }

    // ディスプレイ描画: 1 秒ごと (カウントダウン更新のため)
    if (displayOn && now - lastDisplayUpdate >= 1000UL) {
        drawDisplay();
        lastDisplayUpdate = now;
    }

    delay(50);
}

// =============================================================
// SD カード初期化
// =============================================================
void initSD() {
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
    if (SD_MMC.begin("/sdcard", true, false, 400000)) {  // 1-bit mode, 400kHz
        sdAvailable = true;
        if (!SD_MMC.exists(LOG_DIRECTORY)) {
            if (SD_MMC.mkdir(LOG_DIRECTORY)) {
                Serial.printf("[SD] Created %s\n", LOG_DIRECTORY);
            } else {
                Serial.printf("[SD] ERROR: mkdir %s failed!\n", LOG_DIRECTORY);
            }
        }
        if (!SD_MMC.exists("/gpsdb")) {
            Serial.println("[SD] WARNING: /gpsdb が存在しません。"
                           "tools/generate_addr_db.py を実行して addr.csv を配置してください。");
        }
        Serial.printf("[SD] OK. 容量: %llu MB\n", SD_MMC.totalBytes() / (1024 * 1024));
    } else {
        sdAvailable = false;
        Serial.println("[SD] Failed! SD カードを確認してください。");
    }
}

// =============================================================
// GPS UART を読み込んで TinyGPSPlus に渡す
// =============================================================
void readGPS() {
    while (gpsSerial.available() > 0) {
        char c = gpsSerial.read();
        // '$' で始まる NMEA 文の先頭 23 文字を常に最新に更新
        if (c == '$') gpsPreviewLen = 0;
        if (gpsPreviewLen < sizeof(gpsRawPreview) - 1) {
            gpsRawPreview[gpsPreviewLen++] = (c >= 0x20 && c < 0x7f) ? c : '.';
            gpsRawPreview[gpsPreviewLen]   = '\0';
        }
        if (gps.encode(c) && gps.location.isValid()) {
            bool moved = locationMoved(gps.location.lat(), gps.location.lng());

            current.valid      = true;
            current.lat        = gps.location.lat();
            current.lon        = gps.location.lng();
            current.altitude   = gps.altitude.isValid()   ? gps.altitude.meters()  : 0.0f;
            current.speed      = gps.speed.isValid()      ? gps.speed.kmph()       : 0.0f;
            current.satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
            current.hdop       = gps.hdop.isValid()       ? gps.hdop.hdop()        : 99.9f;

            if (gps.date.isValid()) {
                current.year  = gps.date.year();
                current.month = gps.date.month();
                current.day   = gps.date.day();
            }
            if (gps.time.isValid()) {
                current.hour   = gps.time.hour();
                current.minute = gps.time.minute();
                current.second = gps.time.second();
            }

            // 約 100m 以上移動したら住所キャッシュをリセット
            if (moved) {
                address.fetched = false;
            }
        }
    }
}

// 前回住所を取得した位置から 0.001 度 (≈ 100m) 以上移動したか判定
bool locationMoved(double newLat, double newLon) {
    return fabs(newLat - address.cachedLat) > 0.001 ||
           fabs(newLon - address.cachedLon) > 0.001;
}

// =============================================================
// SD カードへ GPS データを書き込む
// =============================================================
void logToSD() {
    if (!sdAvailable) {
        Serial.println("[LOG] Skip: SD not available");
        return;
    }
    if (!current.valid) {
        Serial.println("[LOG] Skip: No GPS fix");
        return;
    }

    String path  = buildLogPath();
    bool   isNew = !SD_MMC.exists(path);
    File   f     = SD_MMC.open(path, FILE_APPEND);

    if (!f) {
        Serial.printf("[LOG] Cannot open: %s\n", path.c_str());
        return;
    }

    if (isNew) {
        f.println("timestamp,latitude,longitude,altitude_m,speed_kmh,satellites,hdop");
    }

    char line[128];
    snprintf(line, sizeof(line),
             "%04d-%02d-%02dT%02d:%02d:%02dZ,%.6f,%.6f,%.1f,%.1f,%d,%.2f",
             current.year, current.month, current.day,
             current.hour, current.minute, current.second,
             current.lat, current.lon,
             current.altitude, current.speed,
             current.satellites, current.hdop);

    f.println(line);
    f.close();

    Serial.printf("[LOG] %s\n", line);
}

// =============================================================
// ログファイルパスを生成 (日付別)
// =============================================================
String buildLogPath() {
    if (current.year > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s/gps_%04d%02d%02d.csv",
                 LOG_DIRECTORY, current.year, current.month, current.day);
        return String(buf);
    }
    return String(LOG_DIRECTORY) + "/gps_nodate.csv";
}

// =============================================================
// ディスプレイ点灯
// =============================================================
void turnOnDisplay() {
    displayOn         = true;
    displayOnTime     = millis();
    lastDisplayUpdate = 0;
    M5.Display.setBrightness(DISPLAY_BRIGHTNESS);
    drawDisplay();
    Serial.println("[Display] ON");
}

// =============================================================
// ディスプレイ消灯
// =============================================================
void turnOffDisplay() {
    displayOn = false;
    M5.Display.setBrightness(0);
    M5.Display.fillScreen(TFT_BLACK);
    Serial.println("[Display] OFF");
}

// =============================================================
// ディスプレイ描画
// =============================================================
void drawDisplay() {
    int w = M5.Display.width();
    int h = M5.Display.height();

    M5.Display.setFont(&lgfx::fonts::lgfxJapanGothic_16);
    M5.Display.fillScreen(TFT_BLACK);

    // --- ヘッダーバー ---
    M5.Display.fillRect(0, 0, w, 52, TFT_NAVY);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(12, 14);
    M5.Display.print("GPS Logger");

    // バッテリー残量
    char batBuf[16];
    snprintf(batBuf, sizeof(batBuf), "BAT %d%%", M5.Power.getBatteryLevel());
    M5.Display.setCursor(w - 110, 14);
    M5.Display.print(batBuf);

    // 衛星数
    M5.Display.setTextColor(current.valid ? TFT_GREEN : TFT_YELLOW);
    char satBuf[12];
    snprintf(satBuf, sizeof(satBuf), "SAT %d", current.satellites);
    M5.Display.setCursor(w - 230, 14);
    M5.Display.print(satBuf);

    // --- 本文エリア ---
    int y = 70;

    if (!current.valid) {
        M5.Display.setTextColor(TFT_YELLOW);
        M5.Display.setTextSize(3);
        M5.Display.setCursor(12, y);
        M5.Display.print("GPS 測位中...");
        y += 55;
        M5.Display.setTextColor(TFT_LIGHTGREY);
        M5.Display.setTextSize(2);
        M5.Display.setCursor(12, y);
        M5.Display.print("衛星が見える場所へ移動してください");
        y += 36;

        // GPS デバッグ情報
        uint32_t chars = gps.charsProcessed();
        M5.Display.setTextColor(chars > 0 ? TFT_GREEN : TFT_RED);
        M5.Display.setCursor(12, y);
        M5.Display.printf("UART: %s  chars=%u",
                          chars > 0 ? "受信中" : "データなし", chars);
        y += 30;
        M5.Display.setTextColor(TFT_LIGHTGREY);
        M5.Display.setCursor(12, y);
        M5.Display.printf("衛星: %u機  HDOP: %.1f",
                          gps.satellites.isValid() ? gps.satellites.value() : 0,
                          gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f);
        y += 30;
        // チェックサムエラーが多い場合はボーレートが違う
        uint32_t failed = gps.failedChecksum();
        M5.Display.setTextColor(failed > 10 ? TFT_RED : TFT_DARKGREY);
        M5.Display.setCursor(12, y);
        M5.Display.printf("文: ok=%u  fix=%u  err=%u",
                          gps.passedChecksum(), gps.sentencesWithFix(), failed);
        y += 30;
        // 正常な NMEA なら "$GPRMC,..." または "$GNRMC,..." で始まるはず
        M5.Display.setTextColor(TFT_CYAN);
        M5.Display.setCursor(12, y);
        M5.Display.printf("RAW: %s", gpsRawPreview);
    } else {
        // 住所表示
        if (address.fetched) {
            if (address.prefecture[0] != '\0') {
                M5.Display.setTextColor(TFT_CYAN);
                M5.Display.setTextSize(4);
                M5.Display.setCursor(12, y);
                M5.Display.print(address.prefecture);
                y += 65;

                M5.Display.setTextColor(TFT_WHITE);
                M5.Display.setTextSize(3);
                M5.Display.setCursor(12, y);
                M5.Display.print(address.city);
                y += 52;
            } else {
                M5.Display.setTextColor(TFT_YELLOW);
                M5.Display.setTextSize(2);
                M5.Display.setCursor(12, y);
                M5.Display.print("住所: 範囲外 (海上?)");
                y += 38;
            }
        } else {
            M5.Display.setTextColor(TFT_YELLOW);
            M5.Display.setTextSize(2);
            M5.Display.setCursor(12, y);
            M5.Display.print("住所検索中...");
            y += 38;
        }

        // GPS 座標
        M5.Display.setTextColor(TFT_GREEN);
        M5.Display.setTextSize(2);
        M5.Display.setCursor(12, y);
        M5.Display.printf("%.5f, %.5f", current.lat, current.lon);
        y += 35;

        // 詳細情報
        M5.Display.setTextColor(TFT_DARKGREY);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(12, y);
        M5.Display.printf("高度:%.0fm  速度:%.1fkm/h  HDOP:%.1f",
                          current.altitude, current.speed, current.hdop);
        y += 20;

        // UTC 時刻
        if (current.year > 0) {
            M5.Display.setCursor(12, y);
            M5.Display.printf("UTC: %04d-%02d-%02d %02d:%02d:%02d",
                              current.year, current.month, current.day,
                              current.hour, current.minute, current.second);
        }
    }

    // --- フッター: 消灯カウントダウン ---
    uint32_t elapsed   = millis() - displayOnTime;
    uint32_t remaining = (DISPLAY_TIMEOUT_MS > elapsed)
                         ? (DISPLAY_TIMEOUT_MS - elapsed) / 1000 : 0;

    M5.Display.fillRect(0, h - 42, w, 42, 0x2104);
    M5.Display.setTextColor(TFT_LIGHTGREY);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(12, h - 28);
    M5.Display.printf("消灯まで %2ds   [ボタン/タップで延長]", remaining);
}

// =============================================================
// ボタン / タップ処理
// =============================================================
void onButtonPressed() {
    if (!displayOn) {
        turnOnDisplay();

        // GPS フィックスがあり住所未取得なら SD から検索
        if (current.valid && !address.fetched) {
            bool ok = lookupAddress(current.lat, current.lon);
            if (!ok) {
                strncpy(address.prefecture, "住所 DB なし", sizeof(address.prefecture) - 1);
                strncpy(address.city, ADDR_DB_PATH " を確認", sizeof(address.city) - 1);
                address.fetched = true;
            }
            displayOnTime = millis();  // 検索時間分タイムアウトをリセット
            drawDisplay();
        }
    } else {
        // 既に点灯中: タイムアウトをリセット
        displayOnTime     = millis();
        lastDisplayUpdate = 0;
    }
}

// =============================================================
// SD カードの addr.csv から最近傍の市区町村を検索する
// =============================================================
bool lookupAddress(double targetLat, double targetLon) {
    if (!sdAvailable) {
        Serial.println("[Addr] SD not available");
        return false;
    }

    File f = SD_MMC.open(ADDR_DB_PATH);
    if (!f) {
        Serial.printf("[Addr] Cannot open %s\n", ADDR_DB_PATH);
        return false;
    }

    // ヘッダー行をスキップ
    f.readStringUntil('\n');

    float bestDistSq  = ADDR_MAX_DIST_SQ;
    char  bestPref[32] = "";
    char  bestCity[48] = "";

    float cosLat = cosf((float)targetLat * (float)M_PI / 180.0f);

    char  line[128];
    int   lineLen;

    while (f.available()) {
        lineLen = 0;
        while (f.available() && lineLen < (int)sizeof(line) - 1) {
            char c = (char)f.read();
            if (c == '\n') break;
            if (c != '\r') line[lineLen++] = c;
        }
        line[lineLen] = '\0';
        if (lineLen == 0) continue;

        char* fields[4];
        int   fi  = 0;
        char* ptr = line;
        fields[fi++] = ptr;
        while (*ptr != '\0' && fi < 4) {
            if (*ptr == ',') {
                *ptr = '\0';
                fields[fi++] = ptr + 1;
            }
            ptr++;
        }
        if (fi < 4) continue;

        float recLat = atof(fields[0]);
        float recLon = atof(fields[1]);

        float dLat = recLat - (float)targetLat;

        if (dLat < -ADDR_SEARCH_RANGE_DEG) continue;
        if (dLat >  ADDR_SEARCH_RANGE_DEG) break;

        float dLon   = (recLon - (float)targetLon) * cosLat;
        float distSq = dLat * dLat + dLon * dLon;

        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            strncpy(bestPref, fields[2], sizeof(bestPref) - 1);
            bestPref[sizeof(bestPref) - 1] = '\0';
            strncpy(bestCity, fields[3], sizeof(bestCity) - 1);
            bestCity[sizeof(bestCity) - 1] = '\0';
        }
    }

    f.close();

    if (bestDistSq < ADDR_MAX_DIST_SQ) {
        strncpy(address.prefecture, bestPref, sizeof(address.prefecture) - 1);
        strncpy(address.city,       bestCity, sizeof(address.city) - 1);
        address.fetched    = true;
        address.cachedLat  = targetLat;
        address.cachedLon  = targetLon;

        Serial.printf("[Addr] %s %s (dist²=%.4f)\n",
                      address.prefecture, address.city, bestDistSq);
        return true;
    }

    Serial.println("[Addr] No match within range");
    return false;
}
