# User Manual: Seasonal Snow Simulation And Melt-Out Mapping

This manual describes how to reproduce the seasonal snow simulation workflow in BrightnessEngine and compare modelled melt-out dates against satellite-derived melt-out rasters. It covers the terrain/remote-sensing preparation, Unreal Engine seasonal simulation, melt-out export, reference raster generation, and map/statistics plotting.

This manual intentionally does not cover the hemispherical cavity benchmark, radiation benchmark captures, or SkyLight calibration experiments.

## 1. Workflow Overview

The reproducible workflow has five stages:

1. Prepare the repository and local tools.
2. Prepare map inputs: DEM, AOI, terrain metadata, and optional orthophoto/locator maps.
3. Run the seasonal snow simulation in Unreal Engine and export `Meltout_*.json` / `Meltout_*.png`.
4. Generate satellite melt-out reference rasters from Sentinel-2/Landsat.
5. Generate melt-out comparison maps and metrics.

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

## 4. Configure The Seasonal Simulation

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

## 5. Export Terrain And Geometry Inputs

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

## 6. Optional DEM Preparation

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

## 7. Run The Seasonal Simulation

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

## 8. Generate Satellite Melt-Out Reference Rasters

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

## 9. Generate Observation-Count QC Raster

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

## 10. Inspect Reference And Model Alignment

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

## 11. Create Melt-Out Comparison Maps

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

## 12. Optional Paired FSM2 Comparison

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

## 13. Optional Locator And Orthophoto Products

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

## 14. Quality-Control Checklist

Before accepting a reproduced run, verify:

- The Unreal map tag in `Meltout_*.json` matches the map directory.
- `StartTime` and `EndTime` in Unreal match the Earth Engine `--start_date` and `--end_date`.
- The reference raster CRS matches the map projection.
- The reference raster and model export overlap spatially.
- `CellSizeMeters` in model metadata is reasonable for the map resolution.
- The melt-out DOY range is realistic for the season.
- The observation-count threshold does not remove most of the AOI.
- The comparison CSV has nonzero paired cells.

## 15. Troubleshooting

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

If `plot_all_meltout_maps.py` reports zero paired cells, try:

- lowering `--min-obs-count`,
- lowering `--min-valid-frac`,
- removing `--trim-border-pct`,
- confirming that model and reference seasons use the same date window.

## 16. Reproducibility Record

For each final run, save these values in your notes:

- Git commit hash.
- Unreal Engine version.
- Map name and map tag.
- Simulation model and radiation scheme.
- Weather provider and forcing file.
- `StartTime`, `EndTime`, and `TimeStepSeconds`.
- Melt-out threshold/re-arm settings.
- Satellite sensor mode and melt-out strategy.
- Reference raster path.
- Observation-count raster path.
- Comparison command and metrics CSV path.

