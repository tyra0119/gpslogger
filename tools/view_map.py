#!/usr/bin/env python3
"""GPS ログ CSV を Leaflet.js の HTML マップに変換する。

使い方:
  python tools/view_map.py <csv_or_dir> [output.html]

  csv_or_dir : CSV ファイル or CSV が入ったディレクトリ (gps_*.csv を読む)
  output.html: 出力先 (省略時は map.html)

インターネット接続が必要 (OpenStreetMap タイルと Leaflet CDN を使用)。
"""

import sys
import os
import csv
import json
from pathlib import Path


COLORS = [
    "#e74c3c", "#3498db", "#2ecc71", "#f39c12",
    "#9b59b6", "#1abc9c", "#e67e22", "#34495e",
]

HTML_TEMPLATE = """\
<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<title>GPS ログ マップ</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<style>
  html,body{margin:0;padding:0;height:100%;font-family:sans-serif}
  #map{height:100vh}
  #panel{
    position:absolute;top:10px;right:10px;z-index:1000;
    background:rgba(255,255,255,.92);padding:10px 14px;
    border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.2);
    font-size:13px;max-width:240px
  }
  #panel h3{margin:0 0 8px;font-size:14px}
  .leg{display:flex;align-items:center;margin:4px 0}
  .leg-color{width:28px;height:4px;border-radius:2px;margin-right:8px;flex-shrink:0}
  #footer{
    position:absolute;bottom:10px;left:10px;z-index:1000;
    background:rgba(255,255,255,.85);padding:5px 10px;
    border-radius:6px;font-size:12px;color:#555
  }
</style>
</head>
<body>
<div id="map"></div>
<div id="panel"><h3>GPS ログ</h3><div id="legend"></div></div>
<div id="footer"></div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
const TRACKS = __TRACKS_JSON__;
const CENTER = __CENTER_JSON__;

const map = L.map('map').setView(CENTER, 13);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
  attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
  maxZoom: 19
}).addTo(map);

const allLL = [];
let totalPts = 0;
const legend = document.getElementById('legend');

TRACKS.forEach(function(track) {
  if (!track.pts.length) return;

  const lls = track.pts.map(function(p){ return [p.lat, p.lon]; });
  L.polyline(lls, {color: track.color, weight: 3, opacity: 0.85}).addTo(map);
  lls.forEach(function(ll){ allLL.push(ll); });

  track.pts.forEach(function(p, i) {
    const isFirst = i === 0;
    const isLast  = i === track.pts.length - 1;
    const r     = (isFirst || isLast) ? 7 : 4;
    const fill  = isFirst ? '#27ae60' : isLast ? '#c0392b' : track.color;
    const popup = p.ts + '<br>'
      + '速度: ' + p.spd.toFixed(1) + ' km/h<br>'
      + '高度: ' + p.alt.toFixed(0) + ' m<br>'
      + '衛星: ' + p.sat + '  HDOP: ' + p.hdop.toFixed(1);
    L.circleMarker([p.lat, p.lon], {
      radius: r, color: '#fff', weight: 1.5,
      fillColor: fill, fillOpacity: 0.9
    }).bindPopup(popup).addTo(map);
  });

  totalPts += track.pts.length;

  const item = document.createElement('div');
  item.className = 'leg';
  item.innerHTML = '<div class="leg-color" style="background:' + track.color + '"></div>'
    + '<span>' + track.name + ' (' + track.pts.length + '件)</span>';
  legend.appendChild(item);
});

if (allLL.length) map.fitBounds(allLL, {padding: [30, 30]});

document.getElementById('footer').textContent =
  '合計 ' + totalPts + ' 件  ●緑=始点  ●赤=終点';
</script>
</body>
</html>
"""


FIELDNAMES = ["timestamp", "latitude", "longitude", "altitude_m", "speed_kmh", "satellites", "hdop"]

def load_csv(path):
    points = []
    skipped = 0
    with open(path, newline="", encoding="utf-8-sig") as f:
        first = f.readline()
        # ヘッダー行か判定: "timestamp" を含まなければデータ行として扱う
        has_header = "timestamp" in first
        f.seek(0)
        reader = csv.DictReader(f, fieldnames=None if has_header else FIELDNAMES)
        if not has_header:
            print(f"  ヘッダーなし CSV: フィールド名を自動設定")
        for row in reader:
            if row.get("timestamp") == "timestamp":
                continue  # ヘッダー行をスキップ
            try:
                lat = float(row["latitude"])
                lon = float(row["longitude"])
                if lat == 0.0 and lon == 0.0:
                    skipped += 1
                    continue
                points.append({
                    "ts":  row["timestamp"],
                    "lat": lat,
                    "lon": lon,
                    "alt": float(row.get("altitude_m", 0) or 0),
                    "spd": float(row.get("speed_kmh",  0) or 0),
                    "sat": int(row.get("satellites",   0) or 0),
                    "hdop": float(row.get("hdop",      99) or 99),
                })
            except (ValueError, KeyError):
                skipped += 1
    if skipped:
        print(f"  スキップ: {skipped} 行")
    return points


def collect_files(arg):
    p = Path(arg)
    if p.is_file():
        return [p]
    if p.is_dir():
        files = sorted(p.glob("gps_*.csv"))
        return files if files else sorted(p.glob("*.csv"))
    return []


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    input_arg = sys.argv[1]
    output    = sys.argv[2] if len(sys.argv) >= 3 else "map.html"

    files = collect_files(input_arg)
    if not files:
        print(f"ERROR: CSV が見つかりません: {input_arg}")
        sys.exit(1)

    all_tracks = []
    for f in files:
        pts = load_csv(f)
        if pts:
            all_tracks.append({"name": f.stem, "pts": pts})
            print(f"  {f.name}: {len(pts)} ポイント")

    if not all_tracks:
        print("ERROR: 有効なポイントがありません。")
        sys.exit(1)

    for i, t in enumerate(all_tracks):
        t["color"] = COLORS[i % len(COLORS)]

    all_pts = [p for t in all_tracks for p in t["pts"]]
    center  = [
        sum(p["lat"] for p in all_pts) / len(all_pts),
        sum(p["lon"] for p in all_pts) / len(all_pts),
    ]

    html = HTML_TEMPLATE \
        .replace("__TRACKS_JSON__", json.dumps(all_tracks, ensure_ascii=False)) \
        .replace("__CENTER_JSON__", json.dumps(center))

    with open(output, "w", encoding="utf-8") as f:
        f.write(html)

    abs_out = os.path.abspath(output)
    print(f"生成: {abs_out}  ({len(all_pts)} ポイント合計)")
    print(f"ブラウザで開く: file:///{abs_out.replace(os.sep, '/')}")


if __name__ == "__main__":
    main()
