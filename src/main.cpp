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
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <math.h>
#include <time.h>
#include <sys/stat.h>
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

GpsSnapshot current;
AddressInfo address;

static sdmmc_card_t* s_card = nullptr;

bool     sdAvailable       = false;
bool     displayOn         = false;
uint32_t displayOnTime     = 0;
uint32_t lastDisplayUpdate = 0;
uint32_t lastLogTime       = 0;
bool     waitingForFix     = true;
int      logCount          = 0;
bool     lastWriteOk       = true;
char     sdErrMsg[32]      = "";

// =============================================================
// 関数プロトタイプ
// =============================================================
struct tm utcToJst(const GpsSnapshot& s);
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
    Serial.begin(115200);

    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.println("[GPS Logger] Booting...");

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
    // GPIO 45 = Tab5 SD カード電源制御
    pinMode(45, OUTPUT);
    digitalWrite(45, HIGH);
    delay(300);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files              = 8;
    mount_config.allocation_unit_size   = 16 * 1024;

    // IOMUX モード: 全ピンを SDMMC_SLOT_NO_PIN のままにすることで
    // GPIO マトリックスを経由せずハードウェア本来の接続を使う。
    // SD_MMC.setPins() は GPIO 番号を明示するため常に GPIO マトリックス経由になり、
    // ESP32-P4 HP_SDMMC では書き込み時に CRC エラー(0x109)が発生していた。
    sdmmc_host_t host    = SDMMC_HOST_DEFAULT();
    host.max_freq_khz    = 400;  // 400kHz (安定確認後に上げる)

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;  // 1ビットモード

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        sdAvailable = false;
        snprintf(sdErrMsg, sizeof(sdErrMsg), "mnt失敗(%x)", (unsigned)ret);
        Serial.printf("[SD] esp_vfs_fat_sdmmc_mount: 0x%x\n", ret);
        return;
    }
    Serial.println("[SD] Mounted (IOMUX 1-bit 400kHz).");

    // ルートへの書き込みテスト
    FILE* tf = fopen("/sdcard/writetest.tmp", "w");
    if (!tf) {
        sdAvailable = false;
        strncpy(sdErrMsg, "root書込不可", sizeof(sdErrMsg) - 1);
        Serial.println("[SD] fopen /sdcard/writetest.tmp failed");
        return;
    }
    fputs("x", tf);
    fflush(tf);
    fclose(tf);

    struct stat st;
    bool writeOk = (stat("/sdcard/writetest.tmp", &st) == 0 && st.st_size > 0);
    remove("/sdcard/writetest.tmp");
    if (!writeOk) {
        sdAvailable = false;
        strncpy(sdErrMsg, "root書込失敗", sizeof(sdErrMsg) - 1);
        Serial.println("[SD] Write test: size=0 after write");
        return;
    }
    Serial.println("[SD] Write test OK.");

    mkdir("/sdcard" LOG_DIRECTORY, 0777);
    if (stat("/sdcard" LOG_DIRECTORY, &st) != 0) {
        sdAvailable = false;
        strncpy(sdErrMsg, "mkdir失敗", sizeof(sdErrMsg) - 1);
        Serial.printf("[SD] mkdir %s failed\n", LOG_DIRECTORY);
        return;
    }

    sdAvailable = true;
    Serial.println("[SD] Ready.");
}

// =============================================================
// GPS UART を読み込んで TinyGPSPlus に渡す
// =============================================================
void readGPS() {
    while (gpsSerial.available() > 0) {
        char c = gpsSerial.read();
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
        Serial.println("[LOG] Skip: no GPS fix");
        return;
    }

    String path = buildLogPath();  // VFS パス (/sdcard/gpslog/...)

    struct stat st;
    bool isNew = (stat(path.c_str(), &st) != 0 || st.st_size == 0);

    FILE* fp = fopen(path.c_str(), "a");
    if (!fp) {
        Serial.printf("[LOG] Cannot open: %s\n", path.c_str());
        strncpy(sdErrMsg, "open失敗", sizeof(sdErrMsg) - 1);
        lastWriteOk = false;
        return;
    }

    if (isNew) {
        fputs("timestamp,latitude,longitude,altitude_m,speed_kmh,satellites,hdop\n", fp);
    }

    struct tm jst = utcToJst(current);
    char line[128];
    snprintf(line, sizeof(line),
             "%04d-%02d-%02dT%02d:%02d:%02d+09:00,%.6f,%.6f,%.1f,%.1f,%d,%.2f\n",
             jst.tm_year + 1900, jst.tm_mon + 1, jst.tm_mday,
             jst.tm_hour, jst.tm_min, jst.tm_sec,
             current.lat, current.lon,
             current.altitude, current.speed,
             current.satellites, current.hdop);

    fputs(line, fp);
    fflush(fp);
    fclose(fp);

    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
        logCount++;
        lastWriteOk = true;
        sdErrMsg[0] = '\0';
        Serial.printf("[LOG] #%d %s", logCount, line);
    } else {
        lastWriteOk = false;
        strncpy(sdErrMsg, "書込後size=0", sizeof(sdErrMsg) - 1);
        Serial.printf("[LOG] WRITE FAILED: %s", line);
    }
}

// =============================================================
// UTC → JST 変換 (+9時間、月末・年末は mktime が正規化)
// =============================================================
struct tm utcToJst(const GpsSnapshot& s) {
    struct tm t = {};
    t.tm_year  = s.year - 1900;
    t.tm_mon   = s.month - 1;
    t.tm_mday  = s.day;
    t.tm_hour  = s.hour + 9;  // JST = UTC+9
    t.tm_min   = s.minute;
    t.tm_sec   = s.second;
    t.tm_isdst = -1;
    mktime(&t);  // hour>=24 の繰り上げなどを正規化
    return t;
}

// =============================================================
// ログファイルパスを生成 (JST 日付別)
// =============================================================
String buildLogPath() {
    if (current.year > 0) {
        struct tm jst = utcToJst(current);
        char buf[80];
        snprintf(buf, sizeof(buf), "/sdcard%s/gps_%04d%02d%02d.csv",
                 LOG_DIRECTORY, jst.tm_year + 1900, jst.tm_mon + 1, jst.tm_mday);
        return String(buf);
    }
    return "/sdcard" + String(LOG_DIRECTORY) + "/gps_nodate.csv";
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
}

// =============================================================
// ディスプレイ消灯
// =============================================================
void turnOffDisplay() {
    displayOn = false;
    M5.Display.setBrightness(0);
    M5.Display.fillScreen(TFT_BLACK);
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

        // JST 時刻
        if (current.year > 0) {
            struct tm jst = utcToJst(current);
            M5.Display.setCursor(12, y);
            M5.Display.printf("JST: %04d-%02d-%02d %02d:%02d:%02d",
                              jst.tm_year + 1900, jst.tm_mon + 1, jst.tm_mday,
                              jst.tm_hour, jst.tm_min, jst.tm_sec);
        }
    }

    // --- フッター: 記録件数 + 消灯カウントダウン ---
    uint32_t elapsed   = millis() - displayOnTime;
    uint32_t remaining = (DISPLAY_TIMEOUT_MS > elapsed)
                         ? (DISPLAY_TIMEOUT_MS - elapsed) / 1000 : 0;

    M5.Display.fillRect(0, h - 62, w, 62, 0x2104);
    M5.Display.setTextSize(2);

    if (!lastWriteOk || (!sdAvailable && sdErrMsg[0] != '\0')) {
        M5.Display.setTextColor(TFT_RED);
        M5.Display.setCursor(12, h - 58);
        M5.Display.printf("SDエラー:%s", sdErrMsg);
    } else if (!sdAvailable) {
        M5.Display.setTextColor(TFT_RED);
        M5.Display.setCursor(12, h - 58);
        M5.Display.print("SD未検出!");
    } else {
        M5.Display.setTextColor(TFT_GREEN);
        M5.Display.setCursor(12, h - 58);
        M5.Display.printf("記録済: %d件", logCount);
    }

    M5.Display.setTextColor(TFT_LIGHTGREY);
    M5.Display.setCursor(12, h - 30);
    M5.Display.printf("消灯まで %2ds   [タップで延長]", remaining);
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
            displayOnTime = millis();
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
//
// アルゴリズム:
//   CSV は緯度昇順でソートされている。
//   対象緯度 ±ADDR_SEARCH_RANGE_DEG のレコードのみ評価し、
//   近似距離 (dLat² + (dLon·cosLat)²) が最小のレコードを返す。
// =============================================================
bool lookupAddress(double targetLat, double targetLon) {
    if (!sdAvailable) return false;

    FILE* fp = fopen("/sdcard" ADDR_DB_PATH, "r");
    if (!fp) {
        Serial.printf("[Addr] Cannot open /sdcard%s\n", ADDR_DB_PATH);
        return false;
    }

    char line[128];
    fgets(line, sizeof(line), fp);  // ヘッダー行をスキップ

    float bestDistSq   = ADDR_MAX_DIST_SQ;
    char  bestPref[32] = "";
    char  bestCity[48] = "";
    float cosLat       = cosf((float)targetLat * (float)M_PI / 180.0f);

    while (fgets(line, sizeof(line), fp) != nullptr) {
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        char* fields[4];
        int   fi  = 0;
        char* ptr = line;
        fields[fi++] = ptr;
        while (*ptr != '\0' && fi < 4) {
            if (*ptr == ',') { *ptr = '\0'; fields[fi++] = ptr + 1; }
            ptr++;
        }
        if (fi < 4) continue;

        float recLat = atof(fields[0]);
        float dLat   = recLat - (float)targetLat;

        if (dLat < -ADDR_SEARCH_RANGE_DEG) continue;
        if (dLat >  ADDR_SEARCH_RANGE_DEG) break;

        float dLon   = (atof(fields[1]) - (float)targetLon) * cosLat;
        float distSq = dLat * dLat + dLon * dLon;

        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            strncpy(bestPref, fields[2], sizeof(bestPref) - 1);
            bestPref[sizeof(bestPref) - 1] = '\0';
            strncpy(bestCity, fields[3], sizeof(bestCity) - 1);
            bestCity[sizeof(bestCity) - 1] = '\0';
        }
    }

    fclose(fp);

    if (bestDistSq < ADDR_MAX_DIST_SQ) {
        strncpy(address.prefecture, bestPref, sizeof(address.prefecture) - 1);
        strncpy(address.city,       bestCity, sizeof(address.city) - 1);
        address.fetched   = true;
        address.cachedLat = targetLat;
        address.cachedLon = targetLon;
        Serial.printf("[Addr] %s %s\n", address.prefecture, address.city);
        return true;
    }

    Serial.println("[Addr] No match within range");
    return false;
}
