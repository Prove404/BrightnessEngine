# User Manual: Seasonal Snow Simulation And Melt-Out Mapping

This manual describes how to reproduce the seasonal snow simulation workflow in BrightnessEngine and compare modelled melt-out dates against satellite-derived melt-out rasters. It covers the terrain/remote-sensing preparation, Unreal Engine seasonal simulation, melt-out export, reference raster generation, and map/statistics plotting.

This manual intentionally does not cover the hemispherical cavity benchmark, radiation benchmark captures, or SkyLight calibration experiments.

## 1. Workflow Overview

The reproducible workflow has six stages:

1. Prepare the repository and local tools.
2. Set up the Unreal scene from the DEM: landscape, materials, RVT/VHM, georeferencing, SunSky, and simulation actor.
3. Prepare map inputs: AOI, terrain metadata, and optional orthophoto/locator maps.
4. Run the seasonal snow simulation in Unreal Engine and export `Meltout_*.json` / `Meltout_*.png`.
5. Generate satellite melt-out reference rasters from Sentinel-2/Landsat.
6. Generate melt-out comparison maps and metrics.

The expected output structure is:

```text
analysis_results/
  Maps/
    <MapTag>/
      Terrain/
      <ModelTag>/
        Meltout/
          Meltout_*.json
          Meltout_*.png
          Meltout_*_Status.png
      RS_Data/
        Meltout/
          Meltout_Reference_*.tif
          SourceObservationCounts_*.tif
      Comparison/
        Meltout/
          Maps/
            *_meltout_map.png
            meltout_map_comparison_metrics.csv
            meltout_stats_summary.png
```

`<MapTag>` is normally `Totalp`, `Finse`, or another map name derived by the simulation actor. The current project opens `Totalp` by default.

## 2. Prerequisites

Install:

- Unreal Engine 5.x with C++ support.
- Visual Studio with the Unreal C++ workload.
- Git LFS.
- Python 3.10+ or a Conda environment with geospatial packages.
- Google Earth Engine access for the Google Cloud project used by the scripts.

Clone and fetch LFS assets:

```powershell
git clone https://github.com/Prove404/BrightnessEngine.git
cd BrightnessEngine
git lfs install
git lfs pull
```

Install Python dependencies in your preferred environment:

```powershell
python -m pip install earthengine-api rasterio numpy pandas matplotlib requests pillow
```

On Windows, `rasterio` is often easier to install through Conda:

```powershell
conda create -n brightness-rs python=3.11 rasterio numpy pandas matplotlib requests pillow -c conda-forge
conda activate brightness-rs
python -m pip install earthengine-api
```

Authenticate Earth Engine once:

```powershell
earthengine authenticate
```

## 3. Open The Unreal Project

Open `BrightnessEngine.uproject` in Unreal Engine. If Unreal asks to rebuild modules, accept the rebuild.

Recommended editor setup:

- Open the seasonal map, for example `/Game/Maps/Totalp`.
- Select the `BP_SnowSimulationActor` instance in the level.
- Use the Details panel for all simulation settings.
- Keep benchmark and calibration actions disabled/unused for this workflow.

Do not use these actor actions for this manual:

- `Run Radiation Benchmark`
- `Run Cavity SVF Validation`
- `Run Radiation Benchmark Capture`
- `Run Radiation Benchmark Sky Calibration`
- `Run Radiation Benchmark Hemisphere Capture`
- `Run Radiation Benchmark Hemisphere Validation`
- `Run Probe-Lattice Albedo Sweep`
- `Run SkyLight Calibration`

## 4. Set Up The Seasonal Scene

Use this section when recreating the map scene from a DEM or when checking that an existing map is aligned correctly. The simulation assumes that the landscape, snow/VHM materials, georeferencing, SunSky, and `BP_SnowSimulationActor` all describe the same projected terrain grid.

### 4.1 Prepare The DEM Heightmap

Start from a projected DEM covering the simulation AOI. Use a projected CRS in meters:

- Totalp / Swiss sites: commonly `EPSG:2056`.
- Finse / Norway sites: commonly `EPSG:25832`.

If needed, prepare the DEM outside Unreal:

```powershell
python RemoteSensingScripts/prepare_dem_for_horayzon.py `
  --input_dem path\to\source_dem.tif `
  --out_dem analysis_results/Terrain/prepared_dem.tif
```

For new downloads, use the site-specific download helpers:

```powershell
python RemoteSensingScripts/fetch_geonorge_dtm1.py `
  --out_dem analysis_results/Terrain/DTM1_Finse_prepared.tif

python RemoteSensingScripts/fetch_swissalti3d.py `
  --out_dem analysis_results/Terrain/swissalti3d_prepared.tif
```

Before importing into Unreal, record:

- CRS / EPSG code.
- DEM pixel size in meters.
- DEM upper-left projected coordinate.
- DEM bounds.
- Elevation unit and vertical datum.

These values are needed later to validate the exported `Terrain.json`, AOI GeoJSON, and melt-out metadata.

### 4.2 Create Or Validate The Landscape

For a new map:

1. Create a new Unreal level for the site, for example `Totalp` or `Finse`.
2. Import the DEM as a Landscape heightmap.
3. Set the landscape horizontal scale so one Unreal landscape vertex step matches the DEM pixel size.
4. Keep the landscape actor rotation at `0, 0, 0` unless you have a specific reason to rotate the map.
5. Use a landscape material compatible with the snow simulation material parameters.
6. Add any static terrain context, vegetation, or scene meshes required for the seasonal radiation run.

Unreal uses centimeters internally. A DEM cell size of `1 m` corresponds to `100 cm` horizontal spacing. The simulation actor converts its grid spacing from landscape scale and `CellSize`, so the effective simulation-cell size is:

```text
CellSizeMeters = LandscapeScaleX_cm * CellSize / 100
```

For an existing map:

1. Select the Landscape actor and confirm its location, rotation, and scale.
2. Confirm that the landscape extents match the DEM/AOI expected for the map.
3. Confirm that the landscape material exposes the snow depth/albedo parameters used by the simulation.
4. Keep the landscape origin stable once terrain/radiation/reference rasters have been generated; changing it invalidates previous georeferenced exports.

### 4.3 Set Up Landscape Material, Snow Material, RVT, And VHM

The seasonal simulation has two visual/rendering surfaces that must stay aligned:

- The Landscape material writes/reads terrain height and ground appearance.
- The snow surface material receives the runtime snow depth/albedo textures from `BP_SnowSimulationActor` and displaces/colours the snow surface, normally through a Virtual Heightfield Mesh.

Project assets used by the existing maps include:

- Landscape material examples: `Content/SnowDepth/Materials/M_Landscape_Mod.uasset` and site instances such as `MI_Landscape_Mod_Totalp`, `MI_Landscape_Mod_Finse`, and `MI_Landscape_Mod_Ducan`.
- Height/RVT support materials: `Content/SnowDepth/Materials/M_Land_Height.uasset`, `Content/SnowDepth/Materials/M_Land_HeightToRVT.uasset`, `Content/M_Land_Height.uasset`, and `Content/M_Land_HeightToRVT.uasset`.
- Runtime Virtual Textures: `Content/RVT_Land_Height.uasset`, `Content/RVT_Land_Height_MinMax.uasset`, and site variants under `Content/SnowDepth/Textures/RVTs/`.
- Snow/VHM material examples: `Content/SnowDepth/Materials/M_VHM_Snow.uasset`, `MI_VHM_Snow`, and site instances such as `MI_VHM_Snow_Mod_Totalp`, `MI_VHM_Snow_Mod_Finse`, and `MI_VHM_Snow_Mod_Ducan`.

For the Landscape:

1. Assign the site landscape material instance, for example `MI_Landscape_Mod_Totalp`.
2. Make sure the landscape material writes or samples the same height/RVT assets used by the snow material.
3. Assign the Runtime Virtual Texture volume/assets used by the material graph.
4. Confirm landscape layer info assets are assigned for the site, for example `Snow_LayerInfo` and `GrassDry_LayerInfo` / `Grass_LayerInfo`.
5. Keep RVT bounds covering the full DEM/landscape footprint.

For the Runtime Virtual Texture setup:

1. Add or validate an RVT Volume covering the complete landscape.
2. Assign the terrain height RVT, usually `RVT_Land_Height`.
3. Assign the min/max height RVT when the material instance expects it, usually `RVT_Land_Height_MinMax` or the site-specific variant.
4. Align the RVT Volume transform with the Landscape bounds, not just the visible AOI.
5. Build/update virtual texture data if Unreal indicates stale RVT content.

For the Virtual Heightfield Mesh:

1. Add or validate a Virtual Heightfield Mesh actor covering the same area as the Landscape.
2. Set its Runtime Virtual Texture / height source to the same landscape height RVT.
3. Assign the snow material instance to the VHM, for example `MI_VHM_Snow_Mod_Totalp`.
4. Ensure the VHM bounds match the landscape bounds and are not clipped around the AOI.
5. Keep VHM LOD settings stable during reproducible runs. The actor can force high-detail VHM LOD during radiation captures with `bForceVHMLodForCapture`.

In `BP_SnowSimulationActor`, set the material binding fields:

- `Snow Simulation | Visuals | Material | SnowSurfaceMaterial`: the snow/VHM material instance used for the snow surface.
- `Snow Simulation | Visuals | Material | TargetVHMSlotIndex`: material slot on the VHM receiving the snow material, normally `0`.
- `Snow Simulation | Visuals | Material | bOverrideExistingMaterial`: enabled when the actor should replace the VHM material at runtime.
- `Snow Simulation | Visuals | Material | SnowDisplacementScale`: visual displacement multiplier for snow depth.
- `Snow Simulation | Visuals | Params`: keep parameter names at their defaults unless the material graph was renamed.

The default parameter names expected by the simulation actor are:

```text
SnowDepthTex
SnowAlbedoTex
SnowOriginMeters
SnowInvSizePerMeter
SnowDisplacementScale
SnowMap
CellsDimensionX
CellsDimensionY
ResolutionX
ResolutionY
MaxSnow
Albedo_WSA
Albedo_BSA
SnowRoughness
SparkleIntensity
SparkleScale
SnowAgeDays
SnowAlbedoMean
GrainSize_um
Impurity_ppm
```

Do not rename these material parameters unless you also update the corresponding `Snow Simulation | Visuals | Params` fields on the actor.

Material validation checklist:

- The VHM snow material receives a changing `SnowDepthTex` after simulation starts.
- Snow depth displacement appears in the same cells as the debug grid.
- Snow albedo changes are visible if using an FSM2 or albedo-aware model.
- The snow surface is not offset relative to the landscape.
- The VHM does not disappear at camera distance or during radiation captures.
- The RVT height/min-max values cover the complete landscape.

If no VHM is available, the actor has a landscape material binding fallback, but the seasonal visual workflow is intended to use the VHM path.

### 4.4 Add Georeferencing

Add or validate the Unreal georeferencing setup for the map:

1. Enable/use Unreal's GeoReferencing system for the level if it is not already present.
2. Set the projected CRS to the DEM CRS.
3. Set the projected origin so Unreal world coordinates map to the DEM grid.
4. Keep all exported terrain and reference rasters in the same CRS.

In `BP_SnowSimulationActor`, set:

- `Snow Simulation | GeoReferencing | Latitude`: latitude of the top-left/NW cell reference point.
- `Snow Simulation | GeoReferencing | Longitude`: longitude of the top-left/NW cell reference point.
- `Snow Simulation | GeoReferencing | North`: world-space direction for geographic north.
- `Snow Simulation | GeoReferencing | bAutoAlignNorthWithGeoReferencing`: enable only when the landscape is not manually rotated and the GeoReferencing ENU basis should drive the north vector.

The default `North = (0, -1, 0)` assumes that negative Unreal Y points north. If the landscape is rotated, update `North` manually and verify with a solar-shadow check.

### 4.5 Add SunSky And Lighting Actors

The seasonal simulation updates the Sun/Sky state from the simulation time and weather forcing. The level should contain:

- A SunSky actor or equivalent sun/sky setup.
- A Directional Light used as the sun.
- A SkyLight for sky illumination.
- Optional atmosphere/cloud components if they are part of the production scene.

Recommended seasonal settings:

- `SunSkyLocalTimeOffsetHours = 0.0` unless the forcing timestamps require a known local-time correction.
- `bSunSkyUseDaylightSavingTime = false` for reproducible UTC-style seasonal runs.
- `bProductionLightingUsesForcingIntensityScaling = false` unless the run explicitly uses forcing-derived light intensities.
- `bUseDedicatedRadiationSkyLight = true` for UE radiation capture variants.
- `bDisableAtmosphereWhenUsingDedicatedRadiationSkyLight = true` when using the dedicated radiation SkyLight path.

Do not use the SkyLight calibration actions for this seasonal reproduction workflow.

### 4.6 Place And Configure `BP_SnowSimulationActor`

Place one `BP_SnowSimulationActor` in the level and assign or validate:

- The Landscape reference used by the actor.
- `CellSize`, chosen so simulation cells are an appropriate aggregation of the landscape vertex grid.
- `SnowSurfaceMaterial`, `TargetVHMSlotIndex`, and material parameter names.
- `StartTime`, `EndTime`, and weather provider.
- `SimulationConfiguration`.
- Radiation capture options only for model variants that use UE radiation fields.

After placement:

1. Click `Rebuild Simulation`.
2. Check that `Debug Cells Count` is nonzero.
3. Temporarily enable the debug grid or cell-index overlay if alignment needs visual inspection.
4. Confirm that cell indexing follows the same orientation assumed by the exported rasters.

### 4.7 Validate Scene Alignment

Before running a full season, export terrain metadata once:

1. Click `Export Terrain Derivatives`.
2. Inspect the generated files under `analysis_results/Maps/<MapTag>/Terrain/`.
3. Confirm that `Terrain.json` reports the expected projection, bounds, cell size, and map tag.
4. Confirm that the generated AOI GeoJSON covers the intended terrain footprint.

Then run a short simulation or debug export and check that:

- Melt-out / terrain rasters are not transposed.
- Snow displacement and albedo update on the VHM during a test step.
- The VHM snow surface, landscape material, and debug grid occupy the same cells.
- North in the map matches north in the satellite reference.
- Sun movement is plausible for the site latitude, longitude, date, and time.
- The model output and reference raster overlap when inspected with `inspect_rasters.py`.

## 5. Configure The Seasonal Simulation

In `BP_SnowSimulationActor`, set the general simulation window:

- `Snow Simulation | General | StartTime`: season start, for example `2016-10-01 00:00:00`.
- `Snow Simulation | General | EndTime`: season end, for example `2017-08-01 00:00:00`.
- `Snow Simulation | Weather | TimeStepSeconds`: normally `3600`.
- `Snow Simulation | Weather | bAutoStopAtEndTime`: enabled.
- `Snow Simulation | General | MeltoutDepthThreshold`: normally `0.02` m.
- `Snow Simulation | General | MeltoutReArmDepthThreshold`: normally `0.10` m.
- `Snow Simulation | General | MeltoutReArmSustainedTicks`: normally `3`.

Choose a weather provider:

- CSV provider: uses `ForcingData/CSV/meteoFinse20162018.csv` by default.
- ERA5 provider: uses `ForcingData/ERA5/weatherERA5.json`.
- MeteoSwiss provider: uses `ForcingData/MeteoSwiss/weatherMeteoSwiss.json`.

Choose a model in `Snow Simulation | Configuration | SimulationConfiguration`:

- `DegreeDaySimulation` for degree-day / radiation-index seasonal runs.
- `FSM2SnowSimulation` for FSM2 seasonal runs.

For a normal seasonal production run, enable the radiation path only when the model variant needs UE radiation fields. Leave the benchmark/calibration buttons unused.

## 6. Export Terrain And Geometry Inputs

Before comparing melt-out rasters, export the map grid and terrain metadata from the selected `BP_SnowSimulationActor`.

In the actor Details panel:

1. Click `Rebuild Simulation`.
2. Click `Export Terrain Derivatives`.
3. If the run uses UE terrain radiation maps, click `Export SVF/SWdir UE Maps`.
4. If you need dated radiation map products for target dates, populate `ExportTargetTimesteps` and click `Run Radiation Map Export`.

Expected terrain files are written under:

```text
analysis_results/Maps/<MapTag>/Terrain/
```

The terrain export should include JSON metadata and terrain derivative rasters/images. The remote sensing scripts use these files for AOI discovery, CRS, grid alignment, and comparison support.

## 7. Optional DEM Preparation

Use the remote sensing utilities when you need to recreate DEM inputs or prepare a DEM for HORAYZON/terrain workflows.

Finse / Norway DTM1:

```powershell
python RemoteSensingScripts/fetch_geonorge_dtm1.py `
  --out_dem analysis_results/Terrain/DTM1_Finse_prepared.tif
```

SwissALTI3D:

```powershell
python RemoteSensingScripts/fetch_swissalti3d.py `
  --out_dem analysis_results/Terrain/swissalti3d_prepared.tif
```

Prepare an existing DEM for HORAYZON-style terrain workflows:

```powershell
python RemoteSensingScripts/prepare_dem_for_horayzon.py `
  --input_dem path\to\input_dem.tif `
  --out_dem analysis_results/Terrain/prepared_dem.tif
```

These scripts are optional if the Unreal map and terrain exports already exist.

## 8. Run The Seasonal Simulation

Run the simulation in Unreal after the actor is configured:

1. Press Play in Editor or run the simulation mode used by the project.
2. Let the model advance from `StartTime` to `EndTime`.
3. Confirm that the actor auto-stops at the end date.
4. Select the actor and click `ExportMeltoutToDisk`.

The melt-out export writes:

```text
analysis_results/Maps/<MapTag>/<ModelTag>/Meltout/Meltout_*.json
analysis_results/Maps/<MapTag>/<ModelTag>/Meltout/Meltout_*.png
analysis_results/Maps/<MapTag>/<ModelTag>/Meltout/Meltout_*_Status.png
```

The JSON metadata stores grid size, cell size in meters, projected origin, map tag, melt model tag, radiation scheme tag, DOY scaling, and georeferencing information. The comparison scripts read the JSON and PNG together.

Run each model/radiation variant you want to compare separately. For example:

- FSM2 with default radiation.
- FSM2 with UE radiation fields.
- Degree-day variants used in the seasonal study.

## 9. Generate Satellite Melt-Out Reference Rasters

The reference raster is generated from Sentinel-2 and Landsat 8/9 imagery through Google Earth Engine. Use the same season dates as the Unreal simulation.

For a Totalp-style season:

```powershell
python RemoteSensingScripts/MeltOutRasters/download_meltout_reference.py `
  --aoi_geojson analysis_results/Maps/Totalp/Terrain/AOI_Totalp_from_Terrain.geojson `
  --ref_dem_tif analysis_results/Maps/Totalp/Terrain/Terrain_Totalp.tif `
  --start_date 2016-10-01 `
  --end_date 2017-08-01 `
  --strategy midpoint `
  --scale 30 `
  --crs EPSG:2056 `
  --out_tif analysis_results/Maps/Totalp/RS_Data/Meltout/Meltout_Reference_s2_landsat_2017_midpoint.tif
```

For a Finse-style season, use the Finse AOI/DEM paths and CRS, commonly `EPSG:25832`.

If you omit `--aoi_geojson` or `--ref_dem_tif`, the scripts look for the newest suitable files under `analysis_results/Terrain`. For reproducible runs, prefer explicit paths.

The melt-out strategy controls how the date is assigned between last snow and first ground:

- `midpoint`: default; midpoint between last observed snow and first observed ground.
- `first_ground`: conservative snow-duration estimate.
- `last_snow`: conservative snow-free estimate.

## 10. Generate Observation-Count QC Raster

Generate a matching observation-count raster so low-observation cells can be excluded from comparisons.

```powershell
python RemoteSensingScripts/MeltOutRasters/plot_source_observation_counts.py `
  --aoi_geojson analysis_results/Maps/Totalp/Terrain/AOI_Totalp_from_Terrain.geojson `
  --ref_dem_tif analysis_results/Maps/Totalp/Terrain/Terrain_Totalp.tif `
  --start_date 2016-10-01 `
  --end_date 2017-08-01 `
  --scale 30 `
  --crs EPSG:2056 `
  --out_tif analysis_results/Maps/Totalp/RS_Data/Meltout/SourceObservationCounts_s2_landsat_2017.tif `
  --out_png analysis_results/Maps/Totalp/RS_Data/Meltout/SourceObservationCounts_s2_landsat_2017.png
```

The output records valid optical observations per pixel. Later comparison steps can require a minimum observation count.

## 11. Inspect Reference And Model Alignment

Use `inspect_rasters.py` before producing final maps:

```powershell
python RemoteSensingScripts/MeltOutRasters/inspect_rasters.py `
  analysis_results/Maps/Totalp/RS_Data/Meltout/Meltout_Reference_s2_landsat_2017_midpoint.tif `
  analysis_results/Maps/Totalp/<ModelTag>/Meltout/Meltout_*.json
```

Check:

- CRS is the expected map CRS.
- Raster dimensions are plausible.
- Bounds overlap.
- UE cell size and projected origin match the terrain export.
- No unexpected rotation or transpose is reported.

## 12. Create Melt-Out Comparison Maps

Create spatial comparison maps for every `Meltout_*.json` under a map directory:

```powershell
python RemoteSensingScripts/MeltOutRasters/plot_all_meltout_maps.py `
  --map-dir analysis_results/Maps/Totalp `
  --reference-tif analysis_results/Maps/Totalp/RS_Data/Meltout/Meltout_Reference_s2_landsat_2017_midpoint.tif `
  --obs-count-tif analysis_results/Maps/Totalp/RS_Data/Meltout/SourceObservationCounts_s2_landsat_2017.tif `
  --out-dir analysis_results/Maps/Totalp/Comparison/Meltout/Maps `
  --coarse-resolution 30 `
  --min-valid-frac 0.5 `
  --min-obs-count 5 `
  --diff-lim 30
```

Outputs:

- One `*_meltout_map.png` per model run.
- `meltout_map_comparison_metrics.csv`.
- `meltout_stats_summary.png`.

Each map contains:

- Satellite reference melt-out DOY.
- Model melt-out DOY.
- Model minus reference difference in days.
- Optional observation-count panel.
- Bias, RMSE, correlation, paired-cell count, and within-7-day percentage.

## 13. Optional Paired FSM2 Comparison

If you ran both FSM2 default-radiation and FSM2 UE-radiation variants, create a paired coarse-grid comparison:

```powershell
python RemoteSensingScripts/MeltOutRasters/compare_fsm2_meltout_paired_coarse.py `
  --map-dir analysis_results/Maps/Totalp `
  --reference-tif analysis_results/Maps/Totalp/RS_Data/Meltout/Meltout_Reference_s2_landsat_2017_midpoint.tif `
  --obs-count-tif analysis_results/Maps/Totalp/RS_Data/Meltout/SourceObservationCounts_s2_landsat_2017.tif `
  --out-dir analysis_results/Maps/Totalp/Comparison/Meltout/FSM2_Paired `
  --coarse-resolution 30 `
  --trim-border-pct 5 `
  --min-valid-frac 0.7 `
  --min-obs-count 20
```

Outputs:

- `fsm2_meltout_paired_coarse_30m.png`.
- `fsm2_meltout_paired_coarse_metrics.csv`.
- `fsm2_meltout_paired_coarse_metrics.json`.
- GeoTIFF difference rasters for model-reference and UE-default comparisons.

## 14. Optional Locator And Orthophoto Products

Create locator maps for reporting:

```powershell
python RemoteSensingScripts/MeltOutRasters/download_locator_maps.py `
  --aoi_geojson analysis_results/Maps/Totalp/Terrain/AOI_Totalp_from_Terrain.geojson `
  --out_dir analysis_results/Maps/Totalp/RS_Data/Locator
```

Download an orthophoto texture aligned to the map AOI:

```powershell
python RemoteSensingScripts/download_orthophoto_texture.py `
  --map_tag Totalp
```

These products are useful for figures and map context; they are not required for the seasonal model run itself.

## 15. Quality-Control Checklist

Before accepting a reproduced run, verify:

- The Unreal map tag in `Meltout_*.json` matches the map directory.
- The landscape scale, `CellSize`, and `CellSizeMeters` metadata agree.
- The landscape material, RVT assets, VHM actor, and snow material cover the same terrain extent.
- The actor `SnowSurfaceMaterial` points to the intended VHM snow material instance.
- Runtime snow textures drive the expected material parameters (`SnowDepthTex`, `SnowAlbedoTex`, and alignment parameters).
- The actor `Latitude`, `Longitude`, `North`, and GeoReferencing CRS match the DEM.
- SunSky follows the expected sun path for the selected site and date range.
- `StartTime` and `EndTime` in Unreal match the Earth Engine `--start_date` and `--end_date`.
- The reference raster CRS matches the map projection.
- The reference raster and model export overlap spatially.
- `CellSizeMeters` in model metadata is reasonable for the map resolution.
- The melt-out DOY range is realistic for the season.
- The observation-count threshold does not remove most of the AOI.
- The comparison CSV has nonzero paired cells.

## 16. Troubleshooting

If Earth Engine authentication fails, run:

```powershell
earthengine authenticate
```

If `rasterio` fails to import on Windows, use a Conda environment from `conda-forge`.

If no satellite images are found, relax cloud filters or check the AOI/season dates.

If the comparison scripts cannot find model files, confirm that `Meltout_*.json` files exist below:

```text
analysis_results/Maps/<MapTag>/<ModelTag>/Meltout/
```

If maps appear shifted or rotated, inspect the UE metadata with `inspect_rasters.py` and confirm:

- CRS.
- projected origin.
- cell size.
- `PixelLayoutTransform`.
- landscape rotation and actor `North` vector.

If snow does not appear on the terrain:

- confirm that a VHM actor exists and overlaps the Landscape,
- confirm that the VHM uses the expected snow material instance,
- confirm that `SnowSurfaceMaterial` and `TargetVHMSlotIndex` are set on `BP_SnowSimulationActor`,
- confirm that `bOverrideExistingMaterial` is enabled if the actor should bind the material at runtime,
- confirm that the material parameter names match the actor defaults,
- confirm that RVT height/min-max assets are assigned and cover the landscape.

If `plot_all_meltout_maps.py` reports zero paired cells, try:

- lowering `--min-obs-count`,
- lowering `--min-valid-frac`,
- removing `--trim-border-pct`,
- confirming that model and reference seasons use the same date window.

## 17. Reproducibility Record

For each final run, save these values in your notes:

- Git commit hash.
- Unreal Engine version.
- Map name and map tag.
- DEM source, CRS, pixel size, bounds, and vertical datum.
- Landscape location, rotation, scale, and simulation `CellSize`.
- Landscape material instance, RVT assets, VHM actor/material, and snow material instance.
- Snow visual parameters: `SnowDisplacementScale`, albedo controls, and any non-default material parameter names.
- GeoReferencing projected origin and actor latitude/longitude/north vector.
- SunSky time offset, daylight-saving setting, and production lighting intensity mode.
- Simulation model and radiation scheme.
- Weather provider and forcing file.
- `StartTime`, `EndTime`, and `TimeStepSeconds`.
- Melt-out threshold/re-arm settings.
- Satellite sensor mode and melt-out strategy.
- Reference raster path.
- Observation-count raster path.
- Comparison command and metrics CSV path.
