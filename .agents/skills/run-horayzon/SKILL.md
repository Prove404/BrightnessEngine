---
name: run-horayzon
description: Manage and run the HORAYZON algorithm to compute terrain parameters (Sky View Factor, Horizon) and shadow maps/SWdir correction factors from DEM data. 
---

# Run HORAYZON

Use this skill when the user asks to:
- Compute geometric Sky View Factor (SVF) or topographic openness from a Digital Elevation Model (DEM) using HORAYZON.
- Generate shadow maps or direct shortwave correction factors for specific sun positions.
- Plot and visualize HORAYZON results.

## Setup & Dependencies
HORAYZON requires a specific conda environment or external libraries (Intel Embree, TBB, etc.) to compile and run.
Ensure the `horayzon` python package is installed in your active environment before running these scripts.
The source code is located at `HORAYZON/`.
Furthermore, to efficiently process high-resolution elevation data (e.g., <5m resolution like SwissALTI3D), the height map meshing utility (`hmm`) executable must be installed and properly path-configured locally to allow outer domain terrain simplification into a Triangulated Irregular Network (TIN).

## Scripts

Located in `.agents/skills/run-horayzon/scripts/`:

### `run_horayzon_pipeline.py`
A comprehensive wrapper script to handle the I/O and execution of HORAYZON on a given DEM GeoTIFF.
**Capabilities:**
- Finds and Reads a DEM (using rasterio/gdal or xarray).
- Invoke handle-remote-sensing-data if a not suitable DEM is available
- Computes terrain parameters (Sky View Factor) and Horizon for planar gridded DEMs.
- Computes shadow maps and direct shortwave correction factors for a given solar azimuth and elevation.
- Generates standard visualizations (matplotlib) and saves them to `analysis_results/HORAYZON_Outputs/`.
  - **Diagnostic Plots:** Ensure scripts generate multi-panel figures reproducing the style from the HORAYZON paper:
    1. A 4-panel plot showing: **Elevation** (terrain colormap), **Slope** (Oranges colormap), **Aspect** (twilight/circular colormap), and **Sky View Factor** (YlGnBu colormap).
    2. A 2-panel plot comparing **Elevation** and the computed **Shadow Map / SW_dir correction factor** for a specific time/sun position (viridis colormap).

## Input/Output Handling
- **Inputs:** High-resolution DEM GeoTIFFs (e.g., placed in `DEM/` or `ForcingData/`). 
- **Outputs:** The wrapper scripts will output NetCDF files containing the SVF and shadow maps, placed in `analysis_results/HORAYZON_Outputs/`.

## Workflow
1. **Prepare DEM:** Ensure your DEM is in a suitable projected coordinate system and has sufficient resolution.
   - *High-Resolution Handling (<5m):* If using very high-resolution datasets (e.g. 1m or 2m), you MUST apply terrain simplification to prevent out-of-memory errors, as detailed in the HORAYZON paper. Keep the inner domain at full resolution, but simplify the outer boundary zone into a Triangulated Irregular Network (TIN) utilizing `hmm`.
2. **Execute HORAYZON Pipeline:** Run the `run_horayzon_pipeline.py` script, passing the location, desired sun position, search radius, and enabling TIN simplification flags where appropriate. Ensure the `hmm_ex` path is correctly adapted if TIN simplification is invoked.

