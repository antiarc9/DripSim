# DripSim

Water on the skin of your character in Skyrim Special Edition. The drops merge into beads, run down along the real pose (the gravity direction comes from the live skeleton) and dry afterwards. Version 1.2.0.

DripSim is an SKSE plugin. `DripSim.dll` is the framework (body capture, gravity field, the film that is drawn on the skin). `DripSimRain.dll` is the rain mode, `DripSimCream.dll` the cream mode. Other effects can be added as small modes against `api/DripSimAPI.h`.

## Cream mode
A thick, opaque, slightly warm white fluid on the player's skin (face and body; hands and feet optional). A shot is a few strands from a source in front of the target and below it; each strand is cast against the real, posed skin (needs API 5: `IHost::SurfaceRaycast`), so it lands as a compact blob, an arc or a short thread across depending on how it flew. Where it is thickest, blobs gather; after a short hold they creep down the live pose, leaving a streak as wide as themselves that thins out with age. It does not dry; it stays for `StayTime` seconds and then fades out.
- Keys (`Data\SKSE\Plugins\DripSimCream.ini`): `KeySplash=Shift+C` (a shot at the face or the upper torso), `KeyPour=Shift+V` (poured from above), `KeyClean=Ctrl+Shift+C` (wipes it off)
- Mod event: `akRef.SendModEvent("DripSim_Cream", "splash" | "pour" | "clean", strength 0..1)`
- Shot: `StrandsMin`, `StrandsMax`, `StrandInterval`, `StrandLength` (cm), `StrandWidth` (mm), `Unevenness`, `BlobShare`, `ShotDistanceMin/Max` (cm), `ShotElevationMin/Max` (degrees), `ShotSide` (degrees), `Sweep`, `FaceShare`, `AimHeight`, `HoldTime`
- Streaks: `TrailFade`, `TrailFloor`; flow: `Size`, `Viscosity`, `FlowSpeed`, `SetTime`, `FlowTime`, `DripBody`, `DripHands`, `DripFeet`, `DripFace`, `MaxBlobs`, `MaxRunners`
- Look: `Opacity`, `Gloss`, `Sheen`, `Shininess`, `Shade`, `ContactShadow`, `NormalStrength`, `Color=246,243,234`, `WarmColor=241,229,192`, `Warmth`, `Clearing`, `ClearTime`; `StayTime`, `FadeTime`; pour: `PourRate`, `PourTime`, `PourSpread`; skin parts: `Face`, `Hands`, `Feet`
- Preview: `DripSimPreview --mode cream --variant 2 --at 1,4,10,20` (variant 1 = shot at the torso, 2 = pour; the preview has the body only)

## Requirements (to play)
- Skyrim Special Edition 1.5.97 (the only version tested so far)
- SKSE64, Address Library for SKSE Plugins, RaceMenu
- A CBBE-style body (tested with CBBE 3BA)
- Microsoft Visual C++ Redistributable 2015-2022 (x64)

## Build
- Visual Studio 2022 or later (C++23), CMake 3.21 or later, vcpkg
- `scripts\build_plugin.cmd` (sets the paths for the author's machine: edit `VS` and `VCPKG_ROOT` at the top)
- This repository holds the source only: no built files, no settings files, no textures. The built plugin is a separate download.

The offline preview tool (`DripSimPreview`) needs a `body_pack.bin` made from your own body mesh with `plugin/tools/preview/export_pack.py`. The mesh data of someone else's body mod is not part of this repository.

## License
Copyright (C) 2026 AntiArc

This program is free software: you can redistribute it and modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the [LICENSE](LICENSE) file.

DripSim is linked with [CommonLibSSE-NG](plugin/lib/CommonLibSSE-NG) (GPL-3.0-or-later, with the exceptions in its `EXCEPTIONS.md`), so DripSim is under the GPL too. Third-party libraries come through vcpkg (fmt, spdlog and others) under their own licenses.
