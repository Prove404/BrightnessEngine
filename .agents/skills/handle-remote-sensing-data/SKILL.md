---
name: handle-remote-sensing-data
description: Process, normalize, and validate remote sensing data (e.g., Sentinel 2 melt-out dates, DEMs, ERA5) against simulation outputs.
---

# Handle Remote Sensing Data

Use this skill when the user asks to:
- Process or ingest remote sensing datasets (like Sentinel 2 imagery, DEM GeoTIFFs, or ERA5 forcing data).
- Automatically fetch Elevation Data (DEM) given a location from repositories like SwissALTI, GeoNorge, or Google Earth Engine, and process it into a UE-ready format (e.g. 16-bit PNG heightmaps).
- Align and georeference simulation data with real-world spatial data.
- Perform validation of snowmelt models against observed satellite data.

## Principle
Prefer reproducible pipelines. Always explicitly document Coordinate Reference Systems (CRS), nodata values, spatial resolution, and temporal domains. Use scripts like GDAL or rasterio under the hood for raster operations to avoid manual errors.

## Inputs
- Raw raster or vector files (e.g., `.tif`, `.nc`, `.shp`, `.csv`).
- Output grids or point data extracted from Unreal Engine or FSM2.

## Typical Operations

### A. Data Normalization
- Resampling DEMs or satellite imagery to match the simulation grid resolution.
- Reprojecting datasets to a common CRS (e.g., matching the project's local projection or UTM zone).
- Handling nodata values and masking clouds/shadows in optical imagery (like Sentinel 2).

### B. UE-Ready DEM Pipeline
1. Receive a geographic location (coordinates) or bounding box from the user.
2. Query and download the corresponding elevation data from appropriate repositories based on the location:
   - **SwissALTI3D** (for Switzerland)
   - **GeoNorge** (for Norway)
   - **Google Earth Engine (GEE)** (global fallback, e.g., SRTM or Copernicus DEM)
   - *Requirement:* To prevent downloading unnecessary data, strictly compute the bounding box encompassing the Area of Interest (AOI) **plus** the required search radius (e.g., for horizon/shadow computations). Fetch only the tiles that intersect this expanded bounding box.
3. Process the raw elevation data using GDAL or rasterio:
   - Mosaic tiles if necessary.
   - Resample to the required uniform resolution (e.g., 1 meter).
   - Normalize and convert the GeoTIFF to a 16-bit grayscale PNG heightmap compatible with Unreal Engine Landscape or Virtual Heightfield Mesh.
   - Adjust pixel dimensions to match Unreal's recommended sizes (e.g., 1009x1009, 2017x2017).
   - Generate a metadata file (e.g. JSON) containing:
     - The projected CRS.
     - The coordinates of the top-left pixel (origin) to align exactly with Unreal Engine's GeoReferencing plugin.
     - The Z scale factor required to make the landscape height match the original DEM elevation.
     - The snow multiplication factor (to be input into the UE snow material, ensuring snow depth is correctly scaled and visualized).
4. Save the final UE-ready DEM PNG and associated metadata into the designated `Saved/DEMs/` or `analysis_results/DEMs/` folder.

### C. Melt-out Date Extraction (Sentinel 2)
1. Ingest timeseries of Sentinel 2 fractional snow cover (FSC) or binary snow maps.
2. Filter for cloud cover and interpolate missing dates.
3. Determine the specific day-of-year (DOY) when each pixel transitions from snow-covered to snow-free.
4. Export the resulting "melt-out date" raster for comparison.

### D. Validation & Comparison
1. Load the UE/FSM2 simulated snow water equivalent (SWE) or snow depth grids.
2. Determine the simulated melt-out date for each cell.
3. Compute spatially distributed error metrics (e.g., RMSE, Mean Bias Error) against the Sentinel 2 observed melt-out dates.
4. Generate discrepancy maps highlighting areas where the simulation melts too early or too late.

## Requirements
- When doing Python data processing, rely on `rasterio`, `numpy`, `xarray`, `geopandas`, and `gdal`.
- Ensure geographic origin coordinates match exactly between Unreal Engine's GeoReferencing plugin and the external GIS data.
- Never overwrite original raw data. Always write to a distinct `processed/` or `artifacts/` folder.
