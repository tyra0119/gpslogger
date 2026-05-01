#!/usr/bin/env python3
"""
Japan Municipality Address Database Generator
==============================================
OpenStreetMap の Overpass API から日本の市区町村データを取得し、
GPS ロガーのオフライン住所検索用 CSV を生成します。

出力: ../data/addr.csv
  列: lat,lon,prefecture,city
  並び: 緯度昇順 (ESP32 側で早期終了できるようにするため)

生成した addr.csv を SD カードの /gpsdb/addr.csv にコピーしてください。

必要なライブラリ:
  pip install requests

使い方:
  python generate_addr_db.py

注意:
  OpenStreetMap データは © OpenStreetMap contributors (ODbL ライセンス)
  https://www.openstreetmap.org/copyright
"""

import requests
import csv
import time
import sys
import json
from pathlib import Path

# =============================================================
# 設定
# =============================================================
OVERPASS_URL = "https://overpass-api.de/api/interpreter"
OUTPUT_PATH  = Path(__file__).parent.parent / "data" / "addr.csv"
CACHE_DIR    = Path(__file__).parent / ".cache"

RETRY_COUNT  = 3
RETRY_DELAY  = 10   # 秒
REQUEST_WAIT = 2    # リクエスト間隔 (Overpass 負荷軽減)


# =============================================================
# Overpass API ラッパー
# =============================================================
def overpass_query(query: str, timeout: int = 120) -> dict | None:
    """Overpass API を呼び出す。失敗時は最大 RETRY_COUNT 回リトライ。"""
    for attempt in range(RETRY_COUNT):
        try:
            resp = requests.post(
                OVERPASS_URL,
                data={"data": query},
                timeout=timeout + 15,
                headers={
                    "Accept-Charset": "utf-8",
                    "User-Agent": "gpslogger-addr-db-generator/1.0 (GPS Logger for M5Stack Tab5)",
                },
            )
            resp.raise_for_status()
            return resp.json()
        except requests.RequestException as exc:
            wait = RETRY_DELAY * (attempt + 1)
            print(f"  [retry {attempt + 1}/{RETRY_COUNT}] {exc}  ({wait}s 待機)")
            if attempt < RETRY_COUNT - 1:
                time.sleep(wait)
    return None


# =============================================================
# キャッシュ付き Overpass クエリ
# =============================================================
def cached_query(cache_key: str, query: str, timeout: int = 120) -> dict | None:
    """結果をローカルにキャッシュして再実行を省略する。"""
    CACHE_DIR.mkdir(exist_ok=True)
    cache_file = CACHE_DIR / f"{cache_key}.json"

    if cache_file.exists():
        print(f"  (キャッシュ使用: {cache_key})")
        return json.loads(cache_file.read_text(encoding="utf-8"))

    result = overpass_query(query, timeout)
    if result is not None:
        cache_file.write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
    return result


# =============================================================
# 都道府県一覧の取得
# =============================================================
def get_prefectures() -> list[dict]:
    """日本の 47 都道府県を Overpass から取得して返す。"""
    print("都道府県を取得中...")

    query = """
[out:json][timeout:60];
rel["boundary"="administrative"]["admin_level"="4"]["name"]["name:ja"];
out tags;
"""
    data = cached_query("prefectures", query)
    if not data:
        print("ERROR: 都道府県の取得に失敗しました。")
        sys.exit(1)

    prefectures = []
    for elem in data["elements"]:
        tags = elem.get("tags", {})
        name = tags.get("name", "").strip()
        # 都道府県名の末尾で判定
        if any(name.endswith(s) for s in ("都", "道", "府", "県")):
            area_id = 3600000000 + elem["id"]  # OSM の area ID 変換
            prefectures.append({"name": name, "area_id": area_id})

    prefectures.sort(key=lambda p: p["name"])
    print(f"  → {len(prefectures)} 都道府県")
    return prefectures


# =============================================================
# 市区町村の取得 (都道府県ごと)
# =============================================================
def get_municipalities(pref_name: str, area_id: int) -> list[dict]:
    """指定した都道府県内の市区町村 (admin_level=7) を取得する。"""
    safe_name = pref_name.replace("/", "_")
    query = f"""
[out:json][timeout:90];
area({area_id})->.pref;
rel["boundary"="administrative"]["admin_level"="7"](area.pref);
out center tags;
"""
    data = cached_query(f"muni_{safe_name}", query, timeout=90)
    if not data:
        print(f"  WARNING: {pref_name} の市区町村取得に失敗")
        return []

    results = []
    for elem in data["elements"]:
        if elem.get("type") != "relation":
            continue
        center = elem.get("center")
        if not center:
            continue
        tags = elem.get("tags", {})
        name = tags.get("name", "").strip()
        if not name:
            continue

        results.append({
            "lat":        round(center["lat"], 6),
            "lon":        round(center["lon"], 6),
            "prefecture": pref_name,
            "city":       name,
        })

    return results


# =============================================================
# メイン処理
# =============================================================
def main():
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)

    prefectures = get_prefectures()
    all_records: list[dict] = []

    for i, pref in enumerate(prefectures):
        print(f"[{i + 1:2d}/{len(prefectures)}] {pref['name']} ...")
        munis = get_municipalities(pref["name"], pref["area_id"])
        all_records.extend(munis)
        print(f"  → {len(munis)} 市区町村")

        # キャッシュが無い場合のみ待機 (キャッシュ利用時は即時)
        cache_key = f"muni_{pref['name'].replace('/', '_')}"
        if not (CACHE_DIR / f"{cache_key}.json").exists():
            time.sleep(REQUEST_WAIT)

    # 緯度昇順でソート (ESP32 側の早期終了に利用)
    all_records.sort(key=lambda r: r["lat"])

    # CSV 出力
    with open(OUTPUT_PATH, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["lat", "lon", "prefecture", "city"])
        writer.writeheader()
        writer.writerows(all_records)

    print(f"\n完了: {len(all_records)} レコード → {OUTPUT_PATH}")
    print("\n次のステップ:")
    print("  SD カードに /gpsdb/ フォルダを作成して")
    print(f"  {OUTPUT_PATH} を /gpsdb/addr.csv としてコピーしてください。")


if __name__ == "__main__":
    main()
