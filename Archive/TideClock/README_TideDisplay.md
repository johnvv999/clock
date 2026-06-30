# TideDisplay – ESP32 + 3.2" ILI9341

## Files
| File | Purpose |
|------|---------|
| `TideDisplay.ino` | Main sketch |
| `wave_img.h` | Great Wave RGB565 bitmap (100×136) |

## Required Libraries (Arduino IDE)
Install via Library Manager:
- **TFT_eSPI** by Bodmer
- **ArduinoJson** by Benoit Blanchon

## TFT_eSPI Setup (IMPORTANT)
Edit `Documents/Arduino/libraries/TFT_eSPI/User_Setup.h`:

```cpp
// Uncomment this line:
#define ILI9341_DRIVER

// Set your ESP32 SPI pins (typical wiring):
#define TFT_MISO  19
#define TFT_MOSI  23
#define TFT_SCLK  18
#define TFT_CS    15   // Chip select
#define TFT_DC     2   // Data/Command
#define TFT_RST    4   // Reset (or -1 if tied to ESP reset)
#define TFT_BL    32   // Backlight (optional)

#define LOAD_GLCD   // Font 1 (built-in)
#define LOAD_FONT2  // Font 2
#define LOAD_FONT4  // Font 4
#define LOAD_FONT6  // Font 6
#define LOAD_FONT7  // Font 7
#define LOAD_FONT8  // Font 8
#define LOAD_GFXFF  // FreeFonts
#define SMOOTH_FONT
```

## Display Orientation
- `tft.setRotation(3)` = landscape with USB/connector on the LEFT
- If image appears mirrored, try rotation `1` instead

## Layout (320 × 240)
```
┌──────────┬────────────────────┬──────────┐
│  Great   │  HIGH  Jun 28      │  WEATHER │
│  Wave    │  2:14 AM           │  HHI     │
│  Image   │  LOW   Jun 28      │          │
│ (100px)  │  8:45 AM           │   85°F   │
│          │  HIGH  Jun 28      │          │
│          │  3:02 PM           │ ─────── │
│          │  LOW   Jun 28      │Feels Like│
│          │  9:18 PM           │   91°F   │
└──────────┴────────────────────┴──────────┘
   ← 100px →← 112px →          ← 106px →
```

## APIs Used
- **Tides**: NOAA CO-OPS (free, no key) — Station 8665530 Hilton Head Island
- **Weather**: Open-Meteo (free, no key) — lat 32.1354, lon -80.9274

## Colours
- Low tide: Olive green `#808000`
- High tide: Medium blue `#3399FF`
- Current temp: White (textSize 4)
- Feels-like temp: Bright red (textSize 3, ~75% of current)
- Background: Black
