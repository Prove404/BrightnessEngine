# BrightnessEngine

BrightnessEngine is an Unreal Engine project for snow, terrain, and radiation simulation experiments. The project combines Unreal runtime systems, custom simulation plugins, shader code, weather forcing data, and prepared content maps/assets used for research-oriented snow modelling workflows.

## Repository Contents

- `Source/` contains the main Unreal project module and editor target definitions.
- `Plugins/UnrealSnow/` contains simulation, weather data, shader utility, and editor plugins.
- `Shaders/` contains project shader code used by the simulation pipeline.
- `Config/` contains Unreal Engine project configuration.
- `Content/` contains Unreal maps, materials, blueprints, textures, and other binary project assets.
- `ForcingData/` contains example and prepared meteorological forcing files.
- `RemoteSensingScripts/` contains remote sensing and DEM processing scripts used for data preparation and melt-out raster analysis.

## Requirements

- Unreal Engine 5.x
- Git LFS
- Visual Studio with C++ tooling for Unreal Engine development

After cloning, install Git LFS support and fetch LFS assets:

```powershell
git lfs install
git lfs pull
```

Open `BrightnessEngine.uproject` from Unreal Engine or regenerate project files if required by your local engine installation.

## Remote Sensing Scripts

The remote sensing scripts are kept in `RemoteSensingScripts/` as project utilities, separate from local agent workflow files. They include DEM download/preparation tools, orthophoto texture download helpers, and Sentinel-2 melt-out raster analysis scripts.

## Notes On Assets

This repository contains Unreal binary assets and third-party-derived content. The source code in this repository is licensed under the repository license, while external assets, marketplace content, engine content, Cesium assets, Megascans/Fab content, and other third-party materials remain governed by their respective licenses and terms.

## License

The project source code is released under the MIT License. See `LICENSE` for details.
