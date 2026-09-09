# ESP Emote Gen Player ST7789 Experiment

Standalone ESP-IDF experiment for validating `esp_emote_gen_player` on this
project's ST7789 320x240 display wiring before integrating it into the main
application.

This project intentionally does not modify or depend on the main LVGL runtime.
It initializes the LCD panel directly, creates an `esp_emote_gen_player`, and
flushes player buffers with `esp_lcd_panel_draw_bitmap()`.

## Expected asset

Default partition mode expects an Emote Gen asset pack in the `emote_gen`
partition. The current validation asset is:

```text
assets/normal.bin
```

Flash it to the `emote_gen` partition address from `partitions.csv`:

```text
esptool.py --chip esp32 write_flash 0x310000 assets/normal.bin
```

The `emote_gen` partition starts at `0x310000` and is `0x280000` bytes. The
current `normal.bin` is about 1.33 MB, so it fits in this partition.

Path mode is still available from `idf.py menuconfig` for later `/ext` tests.
When using path mode, the external FATFS mount uses the same W25Q128 pins as the
main project:

```text
MOSI=GPIO32 MISO=GPIO25 SCLK=GPIO33 CS=GPIO27
```

The experiment does not format external FATFS on mount failure. If mounting
fails, it logs a warning and continues without assets so validation cannot erase
the main project's cache by accident.

## Manual test flow

```powershell
cd experiments/emote_gen_player_st7789
idf.py set-target esp32
idf.py menuconfig
idf.py build flash monitor
```

In `menuconfig`, confirm `Emote Gen asset source` is `Partition label`.
If `menuconfig` cannot open because dependency resolution fails, this project
also pins the same defaults in `sdkconfig.defaults`:

```text
CONFIG_EMOTE_EXPERIMENT_ASSET_SOURCE_PARTITION=y
CONFIG_EMOTE_EXPERIMENT_ASSET_PARTITION_LABEL="emote_gen"
CONFIG_EMOTE_EXPERIMENT_MMAP_ENABLE=y
CONFIG_EMOTE_EXPERIMENT_INITIAL_ANIM_NAME="normal"
```

The common failure:

```text
fatal: cannot use bare repository ... safe.bareRepository is 'explicit'
```

is a Git/component-manager cache issue before Kconfig loads. Fix Git/component
cache first, then rerun `idf.py reconfigure` or `idf.py menuconfig`.

No build is run by Codex; this directory is only a validation scaffold.

## Success criteria

- LCD initializes with the same orientation/color profile as the main project.
- Asset pack mounts successfully.
- Animation named `normal` starts; if missing, the app tries `idle`.
- Flush callback logs FPS once per second.
- Device can play for at least 5 minutes without reboot or heap decline.

If `normal` is not found, check the monitor output:

```text
Asset entry[0]: name=... file=...
```

Then set `EMOTE_EXPERIMENT_INITIAL_ANIM_NAME` to the actual entry name.

## Notes

- This is not a production backend.
- ESP-IDF v6.0 does not ship the legacy `json` component required by the
  upstream player. This experiment provides a local `components/json`
  compatibility component for the cJSON subset used by `index.json` parsing.
- If path mode is too slow, switch to partition + mmap mode for the next test.
- If this standalone project cannot play reliably, do not integrate Emote Gen
  into the main application.
