# DripSim

Water on the skin of your character in Skyrim Special Edition. The drops merge into beads, run down along the real pose (the gravity direction comes from the live skeleton) and dry afterwards. Version 1.2.1.

DripSim is an SKSE plugin. `DripSim.dll` is the framework (body capture, gravity field, the film that is drawn on the skin). `DripSimRain.dll` is the rain mode, `DripSimSoak.dll` the water after a swim (the same water code, built twice). Other effects can be added as small modes against `api/DripSimAPI.h`.

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
