---
name: Tab5 GPS Logger - 確定設定
description: M5Stack Tab5 + GPS Unit v1.1 で動作確認済みの設定値まとめ
type: project
originSessionId: 8fd8cdbd-0f8d-47fd-a9f8-908cd31db197
---
M5Stack Tab5 GPS ロガープロジェクトで動作確認済みの設定。

**Why:** 多くのピン・設定がデフォルトや推測値と異なっており、各所でハマった。

**How to apply:** 新しい設定変更時はこれを基準にする。

## platformio.ini 必須設定
- `board = esp32-p4-evboard`（Tab5 専用ボードはないが、M5Unified が自動認識する）
- `board_build.flash_mode = qio`（これがないとクラッシュループ）
- `BOARD_HAS_PSRAM`, `ARDUINO_USB_CDC_ON_BOOT=1`, `ARDUINO_USB_MODE=1` が必要
- Serial は USB CDC (COM3 のみ、リセット時に切断→再接続が必要)

## GPS Unit v1.1
- HY2.0-4P Port A に接続
- `GPS_RX_PIN = 54` (White G54)
- `GPS_TX_PIN = 53` (Yellow G53)
- `GPS_BAUD = 115200`（9600 では文字化け、115200 が正解）

## SD カード (SDIO)
- 4ビット SDIO ピン: CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42
- **1ビットモード + 400kHz** で動作安定: `SD_MMC.begin("/sdcard", true, false, 400000)`
- 60GB 以上の大容量 SD カードは write エラーが発生した（32GB が安定）
- 公式 SD Association フォーマッターで FAT32 フォーマット推奨
- SD カードは Windows から「安全に取り外す」必須

## SD カード書き込み (重要: 解決済み)
- **Arduino `SD_MMC.setPins()` は GPIO マトリックス経由になり、ESP32-P4 HP_SDMMC で書き込み CRC エラー (0x109) が発生する**
- **解決策: `esp_vfs_fat_sdmmc_mount()` + `SDMMC_SLOT_CONFIG_DEFAULT()` (IOMUX モード) を直接使う**
  - 全ピンを `SDMMC_SLOT_NO_PIN` のままにすることで GPIO マトリックスを回避
  - ESP32-P4 HP_SDMMC の IOMUX ピンは CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42 (Tab5 ピンと一致)
  - 1ビットモード + 400kHz で動作確認
- ファイル I/O は POSIX (`fopen`/`fputs`/`fflush`/`fclose`/`stat`) を使う (VFS 経由で動作)
- インクルード: `driver/sdmmc_host.h`, `esp_vfs_fat.h`, `sdmmc_cmd.h` (SD.h/SPI.h は不要)
- マウントポイント `/sdcard`、グローバル `static sdmmc_card_t* s_card = nullptr;` が必要

## 表示
- 日本語フォント: `M5.Display.setFont(&lgfx::fonts::lgfxJapanGothic_16)` を drawDisplay() 先頭に設定
