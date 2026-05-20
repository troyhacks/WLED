# M5Stack Core S3 Display Usermod

Display usermod for the ILI9342C 320x240 TFT display on the M5Stack Core S3, using LovyanGFX.

## Pin Mapping (M5Stack Core S3)

| ESP32-S3 | ILI9342C | Description     |
|----------|----------|-----------------|
| G37      | MOSI     | SPI Data        |
| G36      | SCLK     | SPI Clock       |
| G3       | CS       | Chip Select     |
| G35      | DC       | Data/Command    |

Reset is controlled via the AW9523B GPIO expander (P1_1). Backlight is powered via AXP2101 PMU (DLDO1).

## Building

In `platformio_override.ini` for your M5Stack Core S3 environment:

```ini
build_flags =
    -D USERMOD_M5STACK_CORE_S3_DISPLAY

lib_deps =
    https://github.com/lovyan03/LovyanGFX
```

## Features

- SSID and IP address in header bar
- 16-band graphic equalizer bars (differential drawing)
- Real audio reactive data when Audioreactive usermod is enabled
- Simulated bouncing bars when no audio data
- Rainbow color per bar (red → violet)
- Auto sleep after 5 minutes of inactivity

## Display Notes

- Uses LovyanGFX with `SPI3_HOST` (HSPI)
- Native landscape 320x240 resolution
- BGR color order, display inversion enabled
- Backlight always on (controlled by AXP2101 DLDO1)
