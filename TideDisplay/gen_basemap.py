"""
gen_basemap.py  —  TideDisplay basemap generator
Fetches ESRI World Imagery satellite tiles (no roads) and writes basemap_img.h
matched exactly to the NOAA WMS bounding box used by TideDisplay.ino.

Run from E:\Projects\Arduino\Clock\TideDisplay\:
    pip install requests Pillow
    python gen_basemap.py
"""

import math, io, sys
import requests
from PIL import Image

# ── Must match TideDisplay.ino constants exactly ───────────────────────────────
LOCATION_LAT    = 32.2163
LOCATION_LON    = -80.7526
RADAR_RADIUS_MI = 15.0      # change this if you change it in the .ino
SCREEN_W        = 320
SCREEN_H        = 240

# ── Reproduce the exact bounding-box formula from showRadar() ──────────────────
lat_rad      = math.radians(LOCATION_LAT)
lat_half_deg = RADAR_RADIUS_MI / 69.0
lon_half_deg = (RADAR_RADIUS_MI * (SCREEN_W / SCREEN_H)) / (69.0 * math.cos(lat_rad))

LON_MIN = LOCATION_LON - lon_half_deg
LON_MAX = LOCATION_LON + lon_half_deg
LAT_MIN = LOCATION_LAT - lat_half_deg
LAT_MAX = LOCATION_LAT + lat_half_deg

print("Bounding box:")
print("  lat %.5f to %.5f" % (LAT_MIN, LAT_MAX))
print("  lon %.5f to %.5f" % (LON_MIN, LON_MAX))

# ── Tile math ──────────────────────────────────────────────────────────────────
def lon_to_x(lon, z): return int((lon + 180) / 360 * 2**z)
def lat_to_y(lat, z): return int((1 - math.log(math.tan(math.radians(lat)) + 1/math.cos(math.radians(lat))) / math.pi) / 2 * 2**z)
def x_to_lon(x, z):  return x / 2**z * 360 - 180
def y_to_lat(y, z):  return math.degrees(math.atan(math.sinh(math.pi * (1 - 2*y/2**z))))

ZOOM = 12
tx0 = lon_to_x(LON_MIN, ZOOM);  tx1 = lon_to_x(LON_MAX, ZOOM)
ty0 = lat_to_y(LAT_MAX, ZOOM);  ty1 = lat_to_y(LAT_MIN, ZOOM)
cols = tx1 - tx0 + 1;  rows = ty1 - ty0 + 1
TILE = 256
print("\nFetching %dx%d ESRI satellite tiles at zoom %d..." % (cols, rows, ZOOM))

# ── ESRI World Imagery — satellite only, no roads ─────────────────────────────
mosaic  = Image.new("RGB", (cols * TILE, rows * TILE))
headers = {"User-Agent": "Mozilla/5.0 (TideDisplay basemap generator)"}

total = cols * rows
count = 0
for ri, ty in enumerate(range(ty0, ty1 + 1)):
    for ci, tx in enumerate(range(tx0, tx1 + 1)):
        count += 1
        url = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%d/%d/%d" % (ZOOM, ty, tx)
        try:
            r = requests.get(url, headers=headers, timeout=15)
            r.raise_for_status()
            tile = Image.open(io.BytesIO(r.content)).convert("RGB")
            mosaic.paste(tile, (ci * TILE, ri * TILE))
            print("  tile %d/%d  col=%d row=%d  OK" % (count, total, tx, ty))
        except Exception as e:
            print("  tile col=%d row=%d  FAILED: %s" % (tx, ty, e))
            sys.exit(1)

# ── Crop mosaic to exact bounding box, resize to 320x240 ──────────────────────
mw, mh    = mosaic.size
full_lon0 = x_to_lon(tx0,     ZOOM);  full_lon1 = x_to_lon(tx1 + 1, ZOOM)
full_lat1 = y_to_lat(ty0,     ZOOM);  full_lat0 = y_to_lat(ty1 + 1, ZOOM)

def px_x(lon): return int((lon - full_lon0) / (full_lon1 - full_lon0) * mw)
def px_y(lat): return int((full_lat1 - lat) / (full_lat1 - full_lat0) * mh)

crop = (px_x(LON_MIN), px_y(LAT_MAX), px_x(LON_MAX), px_y(LAT_MIN))
print("\nCropping %s -> box %s" % (str(mosaic.size), str(crop)))
img = mosaic.crop(crop).resize((SCREEN_W, SCREEN_H), Image.LANCZOS)
print("Final image: %s" % str(img.size))

# ── Convert to RGB565 ──────────────────────────────────────────────────────────
rgb565 = []
for r, g, b in img.getdata():
    rgb565.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

# ── Write basemap_img.h ───────────────────────────────────────────────────────
rows_of_hex = []
for i in range(0, len(rgb565), 16):
    chunk = rgb565[i:i+16]
    rows_of_hex.append("  " + ", ".join("0x%04X" % v for v in chunk))

array_body = ",\n".join(rows_of_hex)

out = "// Auto-generated -- ESRI World Imagery satellite basemap (no roads)\n"
out += "// bbox: lat %.5f-%.5f  lon %.5f-%.5f\n" % (LAT_MIN, LAT_MAX, LON_MIN, LON_MAX)
out += "// RADAR_RADIUS_MI=%.1f  zoom=%d\n" % (RADAR_RADIUS_MI, ZOOM)
out += "// Regenerate: python gen_basemap.py\n"
out += "#pragma once\n"
out += "#include <Arduino.h>\n"
out += "#define BASEMAP_W %d\n" % SCREEN_W
out += "#define BASEMAP_H %d\n" % SCREEN_H
out += "const uint16_t basemap_img[BASEMAP_W * BASEMAP_H] PROGMEM = {\n"
out += array_body + "\n"
out += "};\n"

with open("basemap_img.h", "w") as f:
    f.write(out)

print("\nDone -- basemap_img.h written (%d bytes)" % (len(rgb565) * 2))
print("Copy it into your TideDisplay sketch folder and recompile.")