# LicencePlates – source

HORIZON licence plates for **Forza Horizon 2** on the Azimuth emulator.
Made by **Philipp55112**. Please do not reupload without credit.

This repository contains the source of the plugin `PlateLight.dll` (version 1.0.0) and the Python tools used to build the plate data.
The ready-to-use mod (models, textures, menus) is the release zip `LicencePlates_1.0.0.zip`.

## Features

- HORIZON plates on 335 cars (EU or US style), with embossed normal map and gloss map
- Your own car shows your plate; drivatars and traffic keep the HORIZON plate, even when they drive the same model
- In-game plate editor in the garage: up to 8 fields (A–Z, 0–9, space, 3 stickers), live update on the car, remove / restore the plate, 16 languages
- Working plate lamps on 173 cars: glowing lamp glass, real plate light on your car, colour and range per car, light cone by lamp size, correct lamps for tuning bumpers
- Plates fall off in crashes (Cosmetic and Simulation damage) and come back on repair

Known game issue: leaving photo mode can show a very long loading screen. This also happens without any mods.

## Repository layout

| Path | Content |
| --- | --- |
| `plugin/platelight_eo.cpp` | main plugin: lamps, player car detection, own plate resources (alias), plate removal, fall-off physics |
| `plugin/plate_editor.cpp/.h` | builds the player plate textures (BC7) from `PlateEditor_eu.bin` / `_us.bin` |
| `plugin/include`, `plugin/minhook` | MinHook (hooking library) |
| `plugin/ext` | bc7enc / rgbcx (BC7 texture encoder) |
| `plugin/build.sh` | build script |
| `tools/` | Python scripts that generated the mod data (lamp glass meshes, `PlateHide.csv`, `PlateLampSize.csv`, `PlateParts.csv`, player materials) |

## Building the plugin

Requirements: Python 3 and zig as cross compiler (`pip install ziglang`). Works on Linux and Windows.

```
cd plugin
./build.sh
```

Result: `plugin/PlateLight.dll`. Copy it to `<Azimuth>\mods\Forza Horizon 2\LicencePlates\plugins\`.
FH2 Extra Options loads it.

## Notes on the tools

The scripts in `tools/` are kept as a reference of how the data was made.
They expect the extracted FH2 car archives and some intermediate files at fixed paths from the original work environment, so they do not run out of the box.
Comments in the source and the tools are partly in German.

## Credits and licences

- LicencePlates: Philipp55112
- MinHook: Copyright (C) 2009-2017 Tsuda Kageyu, BSD 2-clause licence (`plugin/minhook/LICENSE.txt`)
- bc7enc / rgbcx: Richard Geldreich, Jr., see `plugin/ext/LICENSE`
- Forza Horizon 2 is a trademark of Microsoft. This project is a fan mod and is not affiliated with Microsoft or Playground Games.
