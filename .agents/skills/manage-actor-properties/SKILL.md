---
name: manage-actor-properties
description: Organize, clean up, and document Unreal Engine UPROPERTY definitions in the SnowSimulationActor and related classes to ensure a tidy and intuitive Details panel.
---

# Manage Actor Properties

Use this skill when the user asks to:
- Clean up the UE Details panel for the snow simulation actor or other Simulation classes.
- Rearrange, group, or clarify `UPROPERTY` declarations.
- Remove redundant or outdated configuration parameters.

## Principle
Maintain a clean, logical, and user-friendly interface in the Unreal Editor Details panel. Parameters should be logically grouped using the `Category` specifier, protected against invalid inputs using `meta` tags, and well-documented with tooltips so the user understands every toggle.

## Targets
Primary files to refactor under this skill:
- `Source/BrightnessEngine/SnowSimulationActor.h`
- `Source/BrightnessEngine/DegreeDaySimulation.h` (or similar active model headers)

## Guidelines

### A. Categorization Layout
Group related parameters using strict and consistent `Category` names. Use subcategories (with the `|` operator) for fine-grained control. Suggested structure:

- `Simulation|General`: Core timeline settings (Start/End time, Simulation Step).
- `Simulation|Forcing`: Everything related to input meteorology (Weather JSON, CSV paths, ERA5).
- `Simulation|Radiation`: Sky scanning parameters, RT settings, calibration factors, and bounds.
- `Simulation|Melt Models`: Settings specific to Degree-Day or FSM2 (Albedo min/max, threshold temps).
- `Simulation|Diagnostics`: CSV output paths, tracked cell indices, log intervals.
- `Simulation|Advanced`: Deep technical tweaks, component references, or debug flags.

### B. Specifiers & Meta Tags
- **Visibility**: Use `EditAnywhere` for configs. Use `VisibleAnywhere` for components or read-only debug state. Use `AdvancedDisplay` for obscure settings to hide them under the fold.
- **Clamping**: ALWAYS use `meta = (ClampMin = "...", UIMin = "...")` for physical bounds (e.g., scale > 0.0, percentages 0-1) to avoid editor crashes or bad logic.
- **Enums**: Favor `UENUM()` classes over multiple mutually-exclusive `bool` flags.
- **EditConditions**: Use `meta = (EditCondition = "bSomeFlag")` to grey out properties that are irrelevant when a certain mode is disabled.

### C. Documentation
Provide brief, functional comments above every exposed `UPROPERTY`. In Unreal, a standard `///` or `/** */` comment immediately preceding the property becomes its in-editor tooltip.

```cpp
/// The path to the JSON file containing meteorological forcing data.
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation|Forcing")
FString WeatherDataPath;
```

### D. Redundancy Checks
Before creating a new property, check if an existing one can be expanded or reused. Retire properties that are no longer used by the simulation's implementation.
