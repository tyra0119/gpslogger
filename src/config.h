#pragma once

// =============================================================
// GPS Unit v1.1 接続設定
// =============================================================
// GPS Unit v1.1 を M5Stack Tab5 の HY2.0-4P Port A に接続してください。
//
// 配線 (HY2.0-4P 4ピン):
//   Pin 1 (Red)    → 3.3V
//   Pin 2 (White)  → G54 (Tab5 RX ← GPS TX)
//   Pin 3 (Yellow) → G53 (Tab5 TX → GPS RX)
//   Pin 4 (Black)  → GND
#define GPS_RX_PIN  54      // G54 (White)  ← GPS Unit TX
#define GPS_TX_PIN  53      // G53 (Yellow) → GPS Unit RX
#define GPS_BAUD    115200

// =============================================================
// SD カード設定 (M5Stack Tab5 SDIO 4ビットモード)
// =============================================================
#define SD_CLK_PIN  43
#define SD_CMD_PIN  44   // SPI MOSI
#define SD_D0_PIN   39   // SPI MISO
#define SD_D1_PIN   40
#define SD_D2_PIN   41
#define SD_D3_PIN   42
#define SD_CS_PIN   SD_D3_PIN  // SPI CS (D3 を CS として使用)

// =============================================================
// 住所データベース設定
// =============================================================
// tools/generate_addr_db.py で生成した CSV を SD カードに配置してください。
//   SD カード内パス: /gpsdb/addr.csv
//
// CSV 形式: lat,lon,prefecture,city (緯度昇順でソート済み)
// データ数: 約 1,700 レコード (日本全市区町村の代表点)
//
// データは OpenStreetMap (© OpenStreetMap contributors, ODbL) を使用。
#define ADDR_DB_PATH    "/gpsdb/addr.csv"

// 検索範囲: 現在地から ±この緯度内のレコードのみ評価 (度)
// 1.0° ≈ 111km。日本国内であれば必ず市区町村が見つかる範囲です。
#define ADDR_SEARCH_RANGE_DEG   1.0f

// 検索結果として採用する最大距離 (度の二乗)
// 1.0f ≈ 半径 111km 以内。海上など陸地外の場合に "範囲外" と判定するため。
#define ADDR_MAX_DIST_SQ        1.0f

// =============================================================
// データ記録設定
// =============================================================
#define LOG_INTERVAL_SEC    (5 * 60)    // 記録間隔: 5分
#define LOG_DIRECTORY       "/gpslog"   // ログディレクトリ

// 起動後、最初の GPS フィックスを待つ最大時間 (秒)
#define INITIAL_FIX_WAIT_SEC  120

// =============================================================
// ディスプレイ設定
// =============================================================
#define DISPLAY_TIMEOUT_MS  30000   // 自動消灯: 30秒
#define DISPLAY_BRIGHTNESS  200     // 輝度 (0=消灯, 255=最大)

// =============================================================
// 省電力設定
// =============================================================
// 240MHz → 80MHz で ESP32-S3 の CPU 電力を約 35% 削減。
// GPS UART 読み取りには 80MHz で十分です。
#define CPU_FREQ_MHZ    80
