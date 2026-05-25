---
name: handle-ue-radiation
description: Manage and debug Unreal Engine's radiative transfer pipeline. Oversee radiation indices calculate by leveraging UE's render engine to resolve inter-reflections and manage render targets used for cell luminance extraction.
---

# Handle UE Radiation

Use this skill when the user asks to:
- Debug or modify the UE rendering of direct and diffuse radiation, or radiation indices (RI) for snowmelt evaluation.
- Diagnose missing or buggy luminance extraction from Render Target Textures (e.g. `RTY_Direct`, `RTY_Diffuse`, `RTY_Total`).
- Resolve anomalies in radiative transfer, including Global Illumination (GI) inter-reflections, shadow culling, distance culling, or LOD interference.
- Edit the extraction buffer and offset logic for the calibration/reference strip.

## Core Concepts & Pipeline

The Unreal Engine simulation uses real-time graphics rendering (typically Lumen GI) to model complex physical phenomena (forward scattering, inter-reflections of light from terrain).

1. **Illumination Sources:**
   - **Direct Radiation:** Modeled via a `DirectionalLight` simulating the Sun.
   - **Diffuse Radiation:** Modeled via a `SkyLight` / `SkyAtmosphere`.
   - **Inter-reflections (Terrain Bounce):** Modeled via Unreal's Global Illumination system (Lumen or Hardware Ray Tracing). This simulates how light bounces off the surrounding geometry (e.g., highly reflective snow-covered slopes illuminating nearby shaded valleys).

2. **Render Target Extraction (`RTY_*`):**
   - The simulation employs an orthographic top-down `SceneCaptureComponent2D` to map the terrain into 2D Render Targets.
   - These are typically processed into textures and read back onto the CPU as luminance arrays to determine how much light each grid cell receives.

3. **Calibration & Reference Strip:**
   - To translate UE's unbounded HDR luminance into physical \( W/m^2 \) components, a "reference strip" (a pure white, unshadowed horizontal plane) is rendered at the edge or above the terrain.
   - The reference strip luminance is captured and divided against the known physical input (from weather forcing like ERA5) to create a calibration scale factor.
   - **Anomaly Check:** Always verify the reference strip is positioned properly (e.g., Z-height is above all shadows) and skipping logic in UV mapping is correct.

4. **Radiation Indices (RI):**
   - The RI is the ratio between a cell's incoming radiation and the reference horizontal radiation.
   - Computed for Direct, Diffuse, and Total.

## Debugging & Anomaly Checks

### A. Blank / Zero Target Values
If `RTY_Direct`, `RTY_Diffuse`, or specific cell extractions are reading `0.0` or pitch black:
- **Reference Strip Height:** Verify the calibration reference strip is high enough to escape terrain shadow.
- **Exposure Settings:** Auto-exposure (Eye Adaptation) MUST be disabled on the SceneCapture. It must be locked to a fixed manual exposure to prevent wild scalar scaling across time.
- **ShowFlags:** Ensure the SceneCapture component's `ShowFlags` are explicitly enabling lighting, post-processing (if needed for GI), and not culling the actors.

### B. UE Lighting Setup Quirks (GI & Inter-reflections)
- **Diffuse Light vs Direct Light Separation:** UE's SkyLight might still produce directional scattering. When isolating components (e.g., turning off the Sun to capture only Diffuse), ensure indirect lighting caches/Lumen cards are correctly flushed/updated so that disabled light doesn't "bleed" into the next capture.
- **Lumen & Material Albedo:** Lumen requires materials to have physically plausible BaseColor values (albedos) to bounce light. If snow materials are set to absolutely pure white (1.0, 1.0, 1.0), Lumen bounce can become unbounded and act as an infinite light multiplier. Ensure snow albedos are clamped (e.g., 0.8 to 0.9) to prevent blown-out inter-reflection artifacts.
- **Orthographic vs Perspective Projection:** Orthographic cameras treat Lumen/Virtual Shadow Maps differently. Hardware Ray Tracing (HIT) and Lumen Screen Traces often perform poorly or break entirely in an orthographic projection. If inter-reflections are missing or artifacts emerge, ensure the projection size and near/far clip planes precisely bound the landscape, or consider switching to a Perspective Telephoto capture.

### C. Terrain/Cell Mismatches
- **UV Mapping:** Check that the mapping from 2D Render Target pixels `(X, Y)` to absolute world coordinates or simulation `CellIndex` is aligned.
- **Padding Offsets:** If the reference strip is drawn along the border of the render target, remember to shift the Y-axis extraction index.
- **LODs:** High camera distances can force the landscape mesh into a low Level-Of-Detail (LOD). This causes shadows to simplify or geometry to deform, drastically altering cell shadow resolution. Ensure the Virtual Heightfield Mesh (VHM) or landscape LOD settings are clamped if physical precision is required.

## Relevant Paths/Files
- `Source/BrightnessEngine/SnowSimulationActor.cpp` (TickRadiationCaptures, Map/Extraction logic)
- `Source/BrightnessEngine/Radiation...` (Classes or structs handling the indices)
- *(Note: Diagnostics/Analysis plotting for the radiation values are handled by the `plot-results` skill in `.agents/skills/plot-results/scripts/`)*
