---
name: manage-diagnostics-pipeline
description: Manage and oversee the diagnostics pipeline for the simulation. Ensure all required variables are tracked without redundancy, and that output files are descriptively named and cleanly organized in designated folders.
---

# Manage Diagnostics Pipeline

Use this skill when the user asks to:
- Add, modify, or remove variables from the C++ or Fortran diagnostics output (e.g., CSV or NetCDF files).
- Refactor the diagnostics logging frequency, tracked indices, or tracked areas.
- Re-organize how and where simulation run outputs are saved on disk.

## Principle
A good diagnostics pipeline produces exactly what is needed for post-processing and plotting without bloating the hard drive. Output datasets must be self-describing, reliably placed in consistent directory structures, and strictly versioned or timestamped to prevent accidental overwrites of valuable baseline runs.

## Workflow

### 1. Variable Tracking & Avoidance of Redundancy
- **Check Needs:** Before adding a new variable to the diagnostics output (like `SnowMass_kgm2` or `Albedo`), check whether it is strictly required by the downstream Python plotting scripts (handled via `plot-results`).
- **Eliminate Duplicates:** Avoid logging intermediate variables if they can be trivially derived from other logged variables during post-processing (e.g., do not log Bulk Density if both Snow Mass and Snow Depth are already logged, unless debugging a specific calculation).
- **Format:** Adhere to consistent naming conventions and SI units for columns across all models (e.g., `SnowDepth_m`, `SurfaceTemperatureK`, `MeltRate_mmph`).

### 2. File Naming Conventions
- Diagnostics output files MUST be descriptive. Do not use generic names like `output.csv`.
- Include the model name, variant/experiment type, and an exact timestamp.
- **Example:** `FSM2_Baseline_20261014_112549.csv` or `DegreeDay_ClearSky_20261014_120036.csv`.

### 3. Folder Organization
All runs should be neatly routed to their permanent architectural homes under the `analysis_results/` or `Saved/` directory, systematically divided **first by the Map/Terrain name**, and **then by the melt model or simulation module**.

- **Structure Example:** `analysis_results/[MapName]/[ModelName]/Outputs/`
- **Unreal Engine Outputs (C++):** 
  - Save FSM2 data to: `analysis_results/[MapName]/FSM2_UE/Outputs/`
  - Save DegreeDay data to: `analysis_results/[MapName]/DegreeDay_UE/Outputs/`
  - Alternatively, if dumping raw intermediate tests, use `Saved/Diagnostics/`.
- **Fortran Outputs (FSM2 Baseline):**
  - Save raw states and fluxes to: `analysis_results/[MapName]/FSM2_Fortran/Outputs/<Variant_Timestamp>/`
- **Terrain & Radiation Diagnostics:**
  - Route HORAYZON outputs to: `analysis_results/[MapName]/HORAYZON/Outputs/`
  - Route Alpine3D/GROUNDEYE outputs to: `analysis_results/[MapName]/GROUNDEYE/Outputs/`

### 4. Implementation Guidelines (Unreal Engine C++)
- In Unreal Engine, diagnostics tracking is typically managed by `USTRUCTs` storing arrays of data per tick.
- Ensure the `bEnableDiagnostics` flag is exposed in the `SnowSimulationActor` and properly gates the disk I/O.
- Use the `DiagnosticsEveryNSteps` parameter wisely to downsample the output to hourly or daily intervals, keeping file sizes manageable for multi-year simulations.
