---
name: plot-results
description: Generate consistent, high-quality, and reproducible plots (PNG, interactive HTML) for radiation, snow melt models, meteorology, and other validation metrics.
---

# Plot Results

Use this skill when the user asks to:
- Visualize simulation outputs (e.g., Radiation Index, Degree-Day Melt, SWE).
- Plot meteorological forcing data (e.g., ERA5, Frost API weather data).
- Compare different simulation runs or evaluate runs against validation datasets (Sentinel-2 melt-out, etc.).
- Convert CSV data into graphs.

## Principle
Maintain a tidy, centralized, and visually consistent approach to data visualization. Plots should be clear, labeled accurately, and publication-ready or highly interactive for analysis. 

## Inputs
- Simulation output CSV files or JSON data.
- Raster/TIF grids for spatial plots (though heavy GIS processing belongs to `handle-remote-sensing-data`).
- External observation data (e.g., weather station CSVs).

## Typical Operations

### A. Time-Series Plots
- **Radiation Fluxes**: Plotting direct, diffuse, and terrain radiation components over time for individual cells or aggregated areas. 
- **Weather/Forcing**: Visualizing temperature, precipitation, and incoming solar radiation series.
- **Melt Progression**: Displaying Snow Water Equivalent (SWE) or melt-out evolution.

### B. Spatial/Raster Visualization
- **Cell Maps**: Rendering the location of specific grid cells on top of a DEM.
- **Difference Maps**: Plotting spatial differences (error maps) between UE outputs and validation data (e.g., Sentinel-2).

### C. Formatting and Styling Guidelines
- **Libraries**: Use `matplotlib` and `seaborn` for static, publication-ready PNG outputs. Use `plotly` for interactive HTML dashboards that require zooming and hovering over data points.
- **Style**: Ensure legible font sizes, clear legends, appropriate color maps (e.g., `viridis` or `cividis` for continuous values, divergent colormaps for differences/errors).
- **Output Directories**: Always save generated plots to a dedicated directory structured **first by Map/Terrain name**, and **then by the model**. 
  - Structure Example: `analysis_results/[MapName]/[ModelName]/Plots/` (e.g., `analysis_results/Finse/FSM2_UE/Plots/`)
  - Cross-model comparisons should go to: `analysis_results/[MapName]/Comparison/Plots/`
  - Do not overwrite original data.

## Behavior
1. Check the data format before plotting.
2. If the user requests interactive analysis, provide a `plotly` HTML file alongside the static PNG.
3. Keep the plotting scripts in `.agents/skills/plot-results/scripts/` to keep the main project clean. If legacy plotting scripts are found in `Scripts/`, migrate them here if appropriate.
