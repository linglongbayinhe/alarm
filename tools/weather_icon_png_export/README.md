# Export firmware weather icons to PNG

Uses the same C source as the device (`main/display/display_weather_icon_renderer.c`) so PNGs match on-screen pixels (40×40, RGB565 rasterized to 24-bit PNG).

## Requirements

- GCC or Clang on PATH (e.g. [MSYS2](https://www.msys2.org/) MinGW64: `pacman -S mingw-w64-x86_64-gcc`)
- `stb_image_write.h` in this folder (already present; from [nothings/stb](https://github.com/nothings/stb), public domain)

## Build

From this directory:

```bash
gcc -O2 -I../../main/display ../../main/display/display_weather_icon_renderer.c export_weather_icons.c -o export_weather_icons
```

Windows PowerShell (if `gcc` is MinGW):

```powershell
gcc -O2 -I../../main/display ../../main/display/display_weather_icon_renderer.c export_weather_icons.c -o export_weather_icons.exe
```

## Run

```bash
mkdir out
./export_weather_icons out
```

Default output directory is `out` if omitted.

Produces `weather_unknown.png` … `weather_windy.png` (16 files, enum 0–15).

## Notes

- Resolution is fixed at **40×40** (`DISPLAY_WEATHER_ICON_RENDER_SIZE`). For sharper assets in LVGL, regenerate at higher resolution by changing the define in the renderer (firmware + this tool) or upscale PNGs externally (may reintroduce blur).
