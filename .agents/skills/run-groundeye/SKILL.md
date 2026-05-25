---
name: run-groundeye
description: Manage and run the GROUNDEYE module within Alpine3D to simulate radiation balance incorporating forward scattering by snow and terrain.
---

# Run GROUNDEYE

Use this skill when the user asks to:
- Run GROUNDEYE (within Alpine3D) for simulating radiative transfer in terrain, including forward scattering from snow cover.
- Model the irradiance of solar panels in complex alpine terrain.
- Pre-process meteorological/topographic data for GROUNDEYE input.
- Post-process and plot the radiation balance outputs from GROUNDEYE experimental runs.

## Context & Components
GROUNDEYE is a specialized module integrated into the Alpine3D `EBalance` folder (specifically modifying `SnowBRDF`, `TerrainRadiationComplex`, and `SolarPanel`). The reference experimental data and modified Alpine3D source code for this are localized within the project directory `Data_Forward_Scattering/`. 

- **Model Code:** `Data_Forward_Scattering/Model Code/`
- **Model Inputs:** `Data_Forward_Scattering/Model Input Data/` 
- **Model Outputs:** `Data_Forward_Scattering/Model Output Data/`
- **Postprocessing:** `Data_Forward_Scattering/Postprocessing/`
- **Documentation:** See the `MasterTheses_vonruett.pdf` and `ReadMe.txt` in the root of `Data_Forward_Scattering/`.

## Workflow Pipeline

### 1. Identify & Prepare Input Data
- Validate existing inputs in `Data_Forward_Scattering/Model Input Data/`.
- If new elevation models (DEM) or satellite data need to be ingested for a new region, **invoke the `handle-remote-sensing-data` skill** to fetch, reproject, and format the terrain data correctly before passing it to Alpine3D/MeteoIO.

### 2. Setup & Execution
- Ensure the Alpine3D model is built using the provided code in `Data_Forward_Scattering/Model Code/` (which contains the customized `EBalance` modules, `AlpineControl.cc`, and `AlpineMain.cc`).
- Run the Alpine3D executable utilizing the proper `.ini` configuration files. Ensure execution logs are captured to trace convergence or I/O errors.
- Target all outputs (e.g., radiative fluxes, solar panel irradiance grids) into an organized timestamped folder under `analysis_results/GROUNDEYE_Outputs/` or `Data_Forward_Scattering/Model Output Data/`.

### 3. Post-Processing & Diagnostics
- Utilize the analysis scripts found in `Data_Forward_Scattering/Postprocessing/` to parse the simulated terrain irradiance.
- **Invoke the `plot-results` skill** to generate standard visual outputs (like multi-panel plots of Direct/Diffuse/Reflected irradiance, and SnowBRDF scattering distributions). Store these generated interactive or static plots cleanly inside `analysis_results/Plots/GROUNDEYE/`.
