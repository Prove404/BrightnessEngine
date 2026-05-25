// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Landscape.h"
#include "GameFramework/Actor.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "SimulationWeatherDataProviderBase.h"
#include "SimulationBase.h"
#include "Cells/LandscapeCell.h"
#include "Cells/DebugCell.h"
// #include "VirtualHeightfieldMesh/VirtualHeightfieldMesh.h" // VirtualHeightfieldMesh not available in UE5.6
#include "Materials/MaterialInterface.h"
#include "DegreeDay/DegreeDaySimulation.h"
#include "ShowFlags.h"
#include "CollisionQueryParams.h"
#include "UObject/SoftObjectPtr.h"
#include "Engine/StaticMesh.h"
#include "RenderingThread.h"
#include "RHIResources.h"
#include "SnowSimulationActor.generated.h"

// Forward declarations
class UTexture2D;
class UTextureCube;
class UMaterialInstanceDynamic;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UDirectionalLightComponent;
class USkyLightComponent;
class FRHIGPUBufferReadback;
class FRHIGPUTextureReadback;
class FRDGPooledBuffer;
class FRHIShaderResourceView;
class FRHIBuffer;
class FJsonObject;
struct FBelowCanopyForcingProvenance;

DECLARE_LOG_CATEGORY_EXTERN(SimulationLog, Log, All);

UENUM(BlueprintType)
enum class EDebugVisualizationType : uint8
{
	Nothing 				UMETA(DisplayName = "Nothing"),
	Index 					UMETA(DisplayName = "Cell Index (Grid ID)"),
	RadiationIndex			UMETA(DisplayName = "Radiation Index"),
	SnowHeight				UMETA(DisplayName = "Snow Height (mm)", Hidden),
	SWE 					UMETA(DisplayName = "Snow Water Equivalent (l)", Hidden),
	Position 				UMETA(DisplayName = "Position", Hidden),
	Altitude 				UMETA(DisplayName = "Altitude (cm)", Hidden),
	Area 					UMETA(DisplayName = "Area (m^2)", Hidden),
	Curvature				UMETA(DisplayName = "Curvature", Hidden),
	Aspect					UMETA(DisplayName = "Aspect (degrees)", Hidden),
	// Energy fluxes
	NetSurfaceFlux			UMETA(DisplayName = "Net Surface Flux (W/m²)", Hidden),
	NetShortwave			UMETA(DisplayName = "Net Shortwave (W/m²)", Hidden),
	NetLongwave				UMETA(DisplayName = "Net Longwave (W/m²)", Hidden),
	SensibleHeat			UMETA(DisplayName = "Sensible Heat (W/m²)", Hidden),
	LatentHeat				UMETA(DisplayName = "Latent Heat (W/m²)", Hidden),
	GroundHeatFlux			UMETA(DisplayName = "Ground Heat Flux (W/m²)", Hidden),
	// Temperatures
	SurfaceTemperature		UMETA(DisplayName = "Surface Temperature (K)", Hidden),
	SurfaceTempCelsius		UMETA(DisplayName = "Surface Temperature (°C)", Hidden),
	SnowLayer0Temp			UMETA(DisplayName = "Snow Layer 0 Temp (°C)", Hidden),
	SnowLayer1Temp			UMETA(DisplayName = "Snow Layer 1 Temp (°C)", Hidden),
	SnowLayer2Temp			UMETA(DisplayName = "Snow Layer 2 Temp (°C)", Hidden),
	SoilLayer0Temp			UMETA(DisplayName = "Soil Layer 0 Temp (°C)", Hidden),
	SoilLayer1Temp			UMETA(DisplayName = "Soil Layer 1 Temp (°C)", Hidden),
	// Mass fluxes and budget
	MeltRate				UMETA(DisplayName = "Melt Rate (mm/h)", Hidden),
	RefreezeRate			UMETA(DisplayName = "Refreeze Rate (mm/h)", Hidden),
	SublimationRate			UMETA(DisplayName = "Sublimation Rate (mm/h)", Hidden),
	Runoff					UMETA(DisplayName = "Runoff (mm)", Hidden),
	// Radiation
	DiffuseSW				UMETA(DisplayName = "Diffuse SW (W/m²)", Hidden),
	DirectSW				UMETA(DisplayName = "Direct SW (W/m²)", Hidden),
	// Snow properties
	SnowAlbedo				UMETA(DisplayName = "Snow Albedo", Hidden),
	SnowDensity				UMETA(DisplayName = "Snow Density (kg/m³)", Hidden),
	SnowLayerCount			UMETA(DisplayName = "Snow Layer Count", Hidden),
};

UENUM(BlueprintType)
enum class ERadiationNightCaptureMode : uint8
{
	SkipAndZero UMETA(DisplayName = "Skip captures at night and zero targets"),
	CaptureWithVisibilityMask UMETA(DisplayName = "Capture at night using sun visibility mask")
};

UENUM(BlueprintType)
enum class ERadiationBenchmarkIncidentMode : uint8
{
	DirectOnly UMETA(DisplayName = "Direct Only"),
	DiffuseOnly UMETA(DisplayName = "Diffuse Only"),
	DirectAndDiffuse UMETA(DisplayName = "Direct + Diffuse")
};

/** Capture method for the SkyLight calibration pass. */
UENUM(BlueprintType)
enum class ESkyLightCalibrationMethod : uint8
{
	HemisphereProbe UMETA(DisplayName = "Hemisphere Probe (upward-looking probe inside cavity)"),
	OrthographicAtlas UMETA(DisplayName = "Orthographic Atlas (top-down capture with reference strip)")
};

UENUM(BlueprintType)
enum class ERadiationBenchmarkMethod : uint8
{
	OrthographicAtlas UMETA(DisplayName = "Orthographic Atlas"),
	ProbeLattice UMETA(DisplayName = "Probe Lattice")
};

USTRUCT(BlueprintType)
struct FRadiationBenchmarkAlbedoSweepMaterial
{
	GENERATED_BODY()

	/** Material applied to the benchmark cavity/terrain surface for this sweep sample. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation Benchmark")
	TSoftObjectPtr<UMaterialInterface> Material;

	/** Analytical Lambertian albedo represented by this material. Use 0 for the black baseline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation Benchmark", meta=(ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0"))
	float Albedo = 0.0f;

	/** Optional label written to the sweep summary CSV. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation Benchmark")
	FString Label;
};

UENUM(BlueprintType)
enum class ETerrainResidualMode : uint8
{
	TotalMinusDirectMinusSky UMETA(DisplayName = "Recommended: Total - Direct - SkyOnly"),
	DiffuseMinusSkyOnly UMETA(Hidden), // Deprecated diagnostic mode (auto-remapped)
	TotalMinusDirectMinusDiffuse UMETA(DisplayName = "Fallback: Total - Direct - Diffuse")
};

UENUM(BlueprintType)
enum class ESVFMapOccluderMode : uint8
{
	TerrainOnly UMETA(DisplayName = "Terrain Only"),
	FullScene UMETA(DisplayName = "Full Scene"),
	FullSceneRayTracingGPU UMETA(DisplayName = "Full Scene RayTracing GPU"),
	BelowCanopyProbe UMETA(DisplayName = "Below-Canopy Probe")
};




UCLASS()
class SIMULATION_API ASnowSimulationActor : public AActor
{
	GENERATED_BODY()

public:
	bool IsRadiationBenchmarkOverrideActive() const;

	// Reference strip height in pixels/rows (increased from 1 to 5 to improve stability)
	// Reference strip height in pixels/rows (increased from 1 to 5 to improve stability)
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Reference", meta=(EditCondition="bEnableRadiationCapture && bUseReferenceTile", ClampMin="1", UIMin="1"))
	int32 ReferenceStripHeight = 5;

	// Optional material override for the reference strip (defaults to pure white Lambertian if null)
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Reference", meta=(EditCondition="bEnableRadiationCapture && bUseReferenceTile"))
	UMaterialInterface* ReferenceStripOverrideMaterial = nullptr;

	// Use a dual-material reference strip (split into two halves).
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Reference", meta=(EditCondition="bEnableRadiationCapture && bUseReferenceTile"))
	bool bUseDualReferenceStrip = false;

	/** Snow depth threshold (m) used to choose GroundHalf vs SnowHalf in dual-reference normalization. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Reference", meta=(EditCondition="bEnableRadiationCapture && bUseReferenceTile && bUseDualReferenceStrip", ClampMin="0.0"))
	float DualReferenceSnowDepthThreshold_m = 0.0f;

	// Material override for the ground/landscape half of the reference strip.
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Reference", meta=(EditCondition="bEnableRadiationCapture && bUseReferenceTile && bUseDualReferenceStrip"))
	UMaterialInterface* ReferenceStripGroundMaterial = nullptr;

	// Material override for the snow half of the reference strip.
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Reference", meta=(EditCondition="bEnableRadiationCapture && bUseReferenceTile && bUseDualReferenceStrip"))
	UMaterialInterface* ReferenceStripSnowMaterial = nullptr;

	// Dual-strip layout is fixed for consistency: top half = ground, bottom half = snow.

	// Deprecated: alternate columns split. Dual reference now uses a top/bottom split.
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Reference", meta=(EditCondition="bEnableRadiationCapture && bUseReferenceTile && bUseDualReferenceStrip", DisplayName="(Deprecated) Alternate Columns", ToolTip="Deprecated. Dual reference uses a top/bottom split. This toggle has no effect.", AdvancedDisplay))
	bool bReferenceStripAlternateColumns = false;

	//@TODO make cell creation algorithm independent of section size
	/** Size of one cell (in vertices) of the simulation, should be divisible by the quad section size. */
	//@TODO make cell creation algorithm independent of section size
	/** Size of one cell (in vertices) of the simulation, should be divisible by the quad section size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | General")
	int CellSize = 9;

	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif


	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Snow Simulation | General")
	/** The current date of the simulation. */
	FDateTime CurrentSimulationTime;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | General")
	/** Simulation start time. */
	FDateTime StartTime = FDateTime(2015, 10, 1);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | General")
	/** Simulation end time. */
	FDateTime EndTime = FDateTime(2016, 9, 1);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | General")
	/** Number of timesteps to be executed for each iteration. */
	int32 Timesteps = 1;



	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | General")
	/** Snow depth threshold (in meters) below which a cell is considered melted out.
	 * Note: this compares against snow depth (CpuDepthMeters), not SWE. */
	float MeltoutDepthThreshold = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | General", meta=(ClampMin="0.0"))
	/** Snow depth (m) that must be sustained to re-arm meltout tracking after a cell already
	 * recorded a meltout date. Must be >= MeltoutDepthThreshold. Set higher than the melt
	 * threshold to ignore late-spring trace showers that would otherwise wipe per-cell
	 * meltout dates. Set to a very large value (e.g. 1000) to disable re-arm entirely
	 * ("first meltout wins"). */
	float MeltoutReArmDepthThreshold = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | General", meta=(ClampMin="1"))
	/** Number of consecutive ticks the cell depth must stay >= MeltoutReArmDepthThreshold
	 * before a previously recorded meltout date is cleared. Hysteresis against transient
	 * snowfall events. */
	int32 MeltoutReArmSustainedTicks = 3;

	/** Shared terrain redistribution scheme applied to all snow models. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain Redistribution", meta = (ShowOnlyInnerProperties))
	FTerrainRedistributionSettings TerrainRedistribution;

	// Terrain-derived curvature preprocessing for redistribution
	/** Legacy mode: scales curvature by (L/L_ref)^2.
	 * This can amplify cell-size dependence and is kept only for backward compatibility with older tuning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain Redistribution | Curvature Field")
	bool bNormalizeCurvatureByCellSize = false;

	/** Reference length in meters for legacy curvature scaling (L_ref). Typical 10 m. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain Redistribution | Curvature Field", meta=(ClampMin="0.001"))
	float CurvatureReferenceMeters = 10.0f;

	/** Optional absolute clamp applied to curvature after normalization. 0 = disabled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain Redistribution | Curvature Field", meta=(ClampMin="0.0"))
	float CurvatureClampAbs = 0.0f;

	/** Radius (meters) used to smooth terrain curvature before redistribution (0 disables). Increase to damp tiny cavities that over-accumulate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain Redistribution | Curvature Field", meta=(ClampMin="0.0", UIMin="0.0", ForceUnits="m", DisplayName="CurvatureSmoothingRadiusMeters"))
	float CurvatureSmoothingRadiusMeters = 5.0f;

	// ============================================================================
	// GeoReferencing & Location
	// ============================================================================

	/** The Latitude in degrees of the top left vertex of the top left cell (Northwest). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | GeoReferencing")
	float Latitude;

	/** The Longitude in degrees of the top left vertex of the top left cell (Northwest). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | GeoReferencing")
	float Longitude;

	/** Unit vector which points north. Set to match geographic north direction. Default (0,-1,0) for landscapes where -Y is north. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | GeoReferencing", meta=(ShowOnlyInnerProperties))
	FVector North = { 0,-1,0 };

	/** Automatically override the North vector using the GeoReferencing ENU basis (recommended only if the landscape is not manually rotated). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | GeoReferencing")
	bool bAutoAlignNorthWithGeoReferencing = false;

	/** Additional offset (hours) applied before driving the SunSky actor. Positive delays sunrise/sunset relative to simulation clock. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | GeoReferencing", meta=(ClampMin="-14.0", ClampMax="14.0", ForceUnits="h"))
	float SunSkyLocalTimeOffsetHours = 0.0f;

	/** Whether to toggle the SunSky actor's daylight saving flag (adds +1h inside the actor). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | GeoReferencing")
	bool bSunSkyUseDaylightSavingTime = false;

	// Simulation model configuration (Instanced)
	// ============================================================================

	// Main simulation configuration. Select a simulation model (Strategy Pattern) to configure its specific parameters.
	UPROPERTY(EditAnywhere, Instanced, Category = "Snow Simulation | Configuration", meta=(ShowOnlyInnerProperties))
	TObjectPtr<USimulationBase> SimulationConfiguration;

	// === Unified Radiation Index Configuration (applies to both FSM2 and DegreeDay) ===

	/** Luminous Efficacy of the Sun (lm/W). Used to convert DNI (W/m²) to Lux. Default ~110. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Physical", meta=(ClampMin="0.0"))
	float SunLuminousEfficacy = 110.0f;

	/** Luminous Efficacy/Scaling Factor for the Sky. Used to convert DHI (W/m²) to SkyLight Intensity. 
	 * Units depend on SkyLight settings (Scalar vs Physical). Default 1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Physical", meta=(ClampMin="0.0"))
	float SkyLuminousEfficacy = 1.0f;

	/** Additional user multiplier for SkyLight intensity tuning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Physical", meta=(ClampMin="0.0"))
	float SkyLightIntensityMultiplier = 1.0f;

	/** When enabled, the production path converts resolved DNI/DHI from forcing into scene light intensities.
	 * Disable this to keep production SunSky / DirectionalLight / SkyLight intensities authored by the level
	 * and use forcing only for timing/provenance, not for direct light scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Physical")
	bool bProductionLightingUsesForcingIntensityScaling = false;

	/** When enabled, benchmark/validation paths convert benchmark DNI/DHI into scene light intensities.
	 * Disable this to keep authored scene intensities during cavity/hemisphere validation and benchmark captures
	 * while still allowing the benchmark path to override sun direction and bookkeeping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Physical", meta=(EditCondition="bEnableRadiationCapture"))
	bool bBenchmarkLightingUsesForcingIntensityScaling = false;

	// ═══════════════════════════════════════════════════════════════════
	// SkyLight Calibration
	// ═══════════════════════════════════════════════════════════════════
	// Adjusts SkyLightIntensityMultiplier so that an upward-looking probe
	// inside a hemispherical cavity measures exactly DHI/2.  Under isotropic
	// diffuse illumination the cavity occludes half the sky hemisphere,
	// giving a known analytical target for any SVF ≈ 0.5 position.

	/** Capture method for the skylight calibration pass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | SkyLight Calibration", meta=(EditCondition="bEnableRadiationCapture"))
	ESkyLightCalibrationMethod SkyLightCalibrationMethod = ESkyLightCalibrationMethod::HemisphereProbe;

	/** Diffuse horizontal irradiance (W/m²) used as the calibration reference.
	 * The probe should measure exactly CalibrationDHI / 2 inside the cavity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | SkyLight Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="1.0"))
	float SkyLightCalibrationDHI_Wm2 = 500.0f;

	/** Direct normal irradiance (W/m²) used by the orthographic-atlas calibration path.
	 * This should be > 0 so atmospheric diffuse can be derived from TotalNoGI - Direct. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | SkyLight Calibration", meta=(EditCondition="bEnableRadiationCapture && SkyLightCalibrationMethod==ESkyLightCalibrationMethod::OrthographicAtlas", ClampMin="0.0"))
	float SkyLightCalibrationDNI_Wm2 = 500.0f;

	/** Surface albedo applied to the cavity and reference strip during calibration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | SkyLight Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.0", ClampMax="1.0"))
	float SkyLightCalibrationAlbedo = 0.80f;

	/** Maximum number of calibration iterations. Each iteration captures, computes
	 * the scale factor, and adjusts SkyLightIntensityMultiplier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | SkyLight Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="1", ClampMax="10"))
	int32 SkyLightCalibrationMaxIterations = 5;

	/** Convergence tolerance — calibration succeeds when
	 * |measured - target| / target < this value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | SkyLight Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.001", ClampMax="0.5"))
	float SkyLightCalibrationTolerance = 0.02f;

	/** Maximum single-step adjustment factor to prevent unstable jumps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | SkyLight Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="1.0", ClampMax="10.0"))
	float SkyLightCalibrationMaxAdjustmentFactor = 4.0f;

	/** Use the dedicated radiation SkyLight (SLS_SpecifiedCubemap) for calibration.
	 * Recommended: a uniform cubemap provides isotropic illumination by construction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | SkyLight Calibration", meta=(EditCondition="bEnableRadiationCapture"))
	bool bSkyLightCalibrationUseDedicatedSkyLight = true;

	// ── Calibration results (read-only) ──

	/** True when the last calibration run converged successfully. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | SkyLight Calibration")
	bool bSkyLightCalibrationSucceeded = false;

	/** Status message from the last calibration run. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | SkyLight Calibration")
	FString SkyLightCalibrationStatus = TEXT("Not run.");

	/** SkyLightIntensityMultiplier before the last calibration. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | SkyLight Calibration")
	float SkyLightCalibrationOriginalMultiplier = 1.0f;

	/** SkyLightIntensityMultiplier after the last calibration (or restored value on failure). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | SkyLight Calibration")
	float SkyLightCalibrationFinalMultiplier = 1.0f;

	/** Measured probe irradiance (RTY units) from the last iteration. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | SkyLight Calibration")
	float SkyLightCalibrationLastMeasuredIrradiance = 0.0f;

	/** Analytical target irradiance (RTY units) = DHI/2 scaled by luminous efficacy. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | SkyLight Calibration")
	float SkyLightCalibrationTargetIrradiance = 0.0f;

	/** Use a dedicated SkyLight for radiation captures instead of the live scene-captured SunSky SkyLight.
	 * Uses the assigned cubemap when available, otherwise falls back to a built-in Engine cubemap
	 * to avoid scene-dependent sky recapture artifacts in the physics path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Physical")
	bool bUseDedicatedRadiationSkyLight = true;

	/** Optional cubemap used by the dedicated radiation SkyLight.
	 * When null, the capture falls back to a built-in Engine cubemap before
	 * falling all the way back to the scene SkyLight path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Physical", meta=(EditCondition="bUseDedicatedRadiationSkyLight"))
	TObjectPtr<UTextureCube> RadiationSkyCubemap = nullptr;

	/** Disable atmosphere/fog in radiation captures when using the dedicated radiation SkyLight.
	 * The dedicated sky then becomes the sole diffuse sky source for capture physics. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Physical", meta=(EditCondition="bUseDedicatedRadiationSkyLight"))
	bool bDisableAtmosphereWhenUsingDedicatedRadiationSkyLight = true;

	/** Global cloud toggle for the radiation diagnostics pipeline.
	 * When enabled, volumetric cloud components are hidden for every radiation capture path
	 * (atlas, probe, hemisphere, benchmark, calibration, and validation) and restored afterward. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture", DisplayName="Disable Clouds In All Captures"))
	bool bDisableVolumetricCloudsDuringRadiationCapture = true;

	/** Use direct-pass radiation index only (RTY_Direct). Forces melt to use GHI only (no DNI/DHI split). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Mode", meta=(EditCondition="bEnableRadiationCapture"))
	bool bUseDirectRadiationIndexOnly = false;

	/** Use total-pass radiation index only (RTY_Total). Ignores split direct/diffuse indices. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Mode", meta=(EditCondition="bEnableRadiationCapture"))
	bool bUseTotalRadiationIndexOnly = false;

	/** Last applied Sun Intensity (Lux) for diagnostics. */
	UPROPERTY(Transient, VisibleAnywhere, Category = "Snow Simulation | Diagnostic", meta=(AdvancedDisplay))
	float LastSunLightIntensity = 0.0f;

	/** Last applied Sky Intensity (Scalar/Lux) for diagnostics. */
	UPROPERTY(Transient, VisibleAnywhere, Category = "Snow Simulation | Diagnostic", meta=(AdvancedDisplay))
	float LastSkyLightIntensity = 0.0f;

	float GetLastSunLightIntensity() const { return LastSunLightIntensity; }
	float GetLastSkyLightIntensity() const { return LastSkyLightIntensity; }

	/** Updates Sun/Sky actor intensities immediately using provided DNI/DHI. Avoids 1-step lag. */
	void UpdateSunSkyFromForcing(float DNI, float DHI);

	/** Applies benchmark DNI/DHI to scene lights when benchmark intensity scaling is enabled; otherwise preserves authored scene intensities. */
	bool ApplyBenchmarkLightingFromForcingIfEnabled(float DNI, float DHI);

	/** Sync cached diagnostic light intensities from the currently active scene lights without applying forcing-based scaling. */
	bool SyncCachedLightIntensitiesFromScene();

	// TWeakObjectPtr for SkyLight (Sun is handled by CachedSunDirectionalLight)
	TWeakObjectPtr<class USkyLightComponent> CachedSunSkyLight;

	/** Dedicated runtime SkyLight used only inside radiation capture scope when a stable specified cubemap is available. */
	UPROPERTY(Transient, VisibleAnywhere, Category = "Snow Simulation | Diagnostic", meta=(AdvancedDisplay))
	TObjectPtr<class USkyLightComponent> RadiationSkyLight = nullptr;






	// ============================================================================
	// Unified Diagnostics Configuration
	// ============================================================================
	

    

	// Runtime simulation instance (active after Initialize/Build)
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Snow Simulation | Runtime", meta=(AdvancedDisplay))
	TObjectPtr<USimulationBase> Simulation;


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Visuals")
	/** Phether to draw the date on the screen or not. */
	bool DrawDate = true;

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions")
	void RebuildSimulation();

	/** Load diagnostics tracked cell indices from a JSON report generated by tools/select_radiation_diagnostic_cells.py. */
	UFUNCTION(CallInEditor, Category = "Snow Simulation | Diagnostics", meta=(DisplayName="Load Tracked Cells From JSON"))
	void LoadDiagnosticsTrackedCellsFromJson();

	/** Validates that required material parameters are present */
	bool ValidateMaterialParameters(UMaterialInterface* BaseMat);

	// ===================================
	// Debug - Status (Read-Only)
	// ===================================

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Debug | Status", meta=(DisplayName="Debug Cells Count", ToolTip="Number of debug cells currently initialized. Should match Total Cells when grid is built."))
	int32 DebugCellsCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Debug | Status", meta=(DisplayName="Grid Status", ToolTip="Current status of the debug grid visualization."))
	FString DebugGridStatus = TEXT("Not Initialized");

	// ===================================
	// Debug - Main Controls
	// ===================================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Overlay", meta=(DisplayName="Enable Debug Overlay", ToolTip="Master toggle for all debug visualizations. When enabled, you can select what to display using 'Debug Visualization Type'."))
	/** Enable debugging overlay. */
	bool bEnableDebugOverlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Overlay", meta=(EditCondition="bEnableDebugOverlay", DisplayName="Visualization Type", ToolTip="Select what data to display on each cell. 'Index' shows grid cell IDs useful for diagnostics tracking."))
	/** What should be visualized.  */
	EDebugVisualizationType DebugVisualizationType = EDebugVisualizationType::Nothing;

	// ===================================
	// Debug - Grid Visualization
	// ===================================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Grid", meta=(DisplayName="Render Grid Lines", ToolTip="Draw simulation grid lines over the landscape. Works in PIE and in editor viewport preview."))
	/** Render the simulation grid over the landscape. */
	bool RenderGrid = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Grid", meta=(DisplayName="Enable Editor Preview", ToolTip="Allow debug grid/overlay drawing in editor viewport without running the simulation (no PIE)."))
	/** Enable grid and cell index visualization in the editor viewport (without running simulation). */
	bool bEnableEditorGridPreview = false;

	/** Refresh grid preview in editor (rebuilds cell geometry for visualization). */
	UFUNCTION(CallInEditor, Category = "Snow Debug | Grid", meta=(DisplayName="Refresh Editor Grid Preview"))
	void RefreshEditorGridPreview();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Grid", meta=(DisplayName="Grid Z Offset", ToolTip="Vertical offset (in cm) to raise the debug grid above the landscape surface.", ClampMin="0", UIMin="0"))
	/** Offset of the grids z position. */
	float DebugGridZOffset = 50.0f;

	// ===================================
	// Debug - Cell Index Display
	// ===================================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Cell Display", meta=(EditCondition="bEnableDebugOverlay", DisplayName="Show All Cell Indices", ToolTip="When enabled, shows cell indices/debug info for all cells regardless of camera distance. WARNING: May cause performance issues on large landscapes!"))
	/** Show ALL cell indices/debug info regardless of distance (useful for finding cell indices). */
	bool bShowAllCellIndices = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Cell Display", meta=(EditCondition="bEnableDebugOverlay && !bShowAllCellIndices", DisplayName="Display Distance (cm)", ToolTip="Maximum distance from camera (in cm) at which cell debug info is displayed. Default 15000 = 150 meters.", ClampMin="100", UIMin="1000", UIMax="100000"))
	int CellDebugInfoDisplayDistance = 15000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Cell Display", meta=(EditCondition="bEnableDebugOverlay", DisplayName="Show Tracked Cells Only", ToolTip="When enabled, only displays debug info for cells listed in 'Diagnostics Tracked Cell Indices'. Useful for focusing on specific cells."))
	/** If true, only shows debug info for cells that are in the diagnostics tracking list. */
	bool bShowTrackedCellsOnly = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Cell Display", meta=(EditCondition="bEnableDebugOverlay", DisplayName="Highlight Tracked Cells", ToolTip="When enabled, cells listed in 'Diagnostics Tracked Cell Indices' are displayed in green instead of purple."))
	/** If true, cells in the diagnostics tracking list are highlighted in green. */
	bool bHighlightTrackedCells = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Cell Display", meta=(EditCondition="bEnableDebugOverlay", DisplayName="Use Occlusion Culling", ToolTip="When enabled, only displays debug info for cells with a clear line-of-sight to the camera. Reduces visual clutter but may hide some cells.", AdvancedDisplay))
	/** If true, only shows debug info for cells with line of sight to camera (avoids occlusion). If false, shows all cells within distance. */
	bool bUseOcclusionCulling = false;

	// ===================================
	// Debug - Logging & Export
	// ===================================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Logging", meta=(DisplayName="Enable Verbose Logging", ToolTip="Enable detailed per-step logging to Output Log. WARNING: May significantly slow down simulation!"))
	/** If false, verbose per-step logging stays muted to avoid slowing the sim. */
	bool bEnableDebugLogging = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Logging", meta=(DisplayName="Save Material Textures", ToolTip="Export simulation textures (snow depth, albedo) to the Screenshots folder.", AdvancedDisplay))
	/** If true, writes the textures generated by the simulation for the material to the screenshot folder. */
	bool SaveMaterialTextures = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Debug | Logging", meta=(DisplayName="Save Simulation Frames", ToolTip="Take a screenshot after each simulation step. Creates many files!", AdvancedDisplay))
	/** If true takes a screenshot each time an iteration of the simulation is executed. */
	bool SaveSimulationFrames = false;

	// PIE auto-run controls
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Advanced")
	bool bAutoRun = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Advanced", meta=(ClampMin="1", ClampMax="120"))
	int32 SimRateHz = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Advanced", meta=(ClampMin="1"))
	float SimDtSeconds = 3600.0f; // 1 hour per step

	// ============================================================================
	// Snow Material & Visual Configuration
	// ============================================================================

	// Material selection & binding behavior
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Material")
	TSoftObjectPtr<UMaterialInterface> SnowSurfaceMaterial; // default to /Game/Materials/M_VHM_Snow

	/** Optional PostProcess material for Lambertian capture in the Lambertian melt model.
	 * Should be an Unlit material that outputs DiffuseLighting only.
	 * Leave empty to use default C++-based override (recommended). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Material", meta=(EditCondition="bEnableRadiationCapture"))
	TSoftObjectPtr<UMaterialInterface> LambertianCaptureMaterial;

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Material", meta = (ClampMin = "0"))
	int32 TargetVHMSlotIndex = 0;

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Material")
	bool bOverrideExistingMaterial = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Visuals | Material")
	float SnowDisplacementScale = 5.0f;

	// Visual/surface controls
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals")
	float SnowAlbedo_WSA = 0.95f;   // white-sky albedo  (diffuse env)

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals")
	float SnowAlbedo_BSA = 0.85f;   // black-sky albedo  (direct sun)

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals", meta = (ClampMin = "0.0"))
	float SnowRoughnessBase = 0.35f;

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals", meta = (ClampMin = "0.0"))
	float SparkleIntensity = 0.25f;   // small, additive spec lobe

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals", meta = (ClampMin = "0.001"))
	float SparkleScale = 0.5f;        // world-space frequency (m^-1)

	// Physically-inspired knobs (optional; can be ignored by the material)
	// Note: Age and impurity automatically adjust albedo/roughness via heuristics
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals")
	float SnowAgeDays = 0.0f;       // aging increases roughness, darkens albedo

	/** Mean snow surface albedo (0..1) averaged over cells above SnowAgeDepthThreshold_m.
	 *  Pushed to the snow master material via Param_SnowAlbedoMean so FSM2's internal
	 *  layer-based albedo decay drives the fresh<->old texture blend directly, bypassing
	 *  the SnowAgeDays exponential for simulations (like FSM2) that don't track days since
	 *  last snowfall. Kept in sync each tick from the simulation's GetCellSnowAlbedoState(). */
	UPROPERTY(VisibleAnywhere, Category = "Snow Simulation | Visuals", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SnowAlbedoMean = 0.85f;

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals", meta = (ClampMin = "0.0"))
	float SnowAgeDepthThreshold_m = 0.005f;

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals")
	float GrainSize_um = 150.0f;    // affects albedo/roughness heuristics

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals")
	float Impurity_ppm = 0.0f;      // soot/dust → albedo reduction

	// Material parameter names (allow remap if user material differs)
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_SnowDepthTex = TEXT("SnowDepthTex");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_SnowAlbedoTex = TEXT("SnowAlbedoTex");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_SnowOriginMeters = TEXT("SnowOriginMeters");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_SnowInvSizePerMeter = TEXT("SnowInvSizePerMeter");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_SnowDisplacementScale = TEXT("SnowDisplacementScale");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_SnowMap = TEXT("SnowMap");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_CellsDimensionX = TEXT("CellsDimensionX");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_CellsDimensionY = TEXT("CellsDimensionY");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_ResolutionX = TEXT("ResolutionX");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_ResolutionY = TEXT("ResolutionY");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_MaxSnow = TEXT("MaxSnow");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_AlbedoWSA = TEXT("Albedo_WSA");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_AlbedoBSA = TEXT("Albedo_BSA");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_RoughnessBase = TEXT("SnowRoughness");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_SparkleIntensity = TEXT("SparkleIntensity");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_SparkleScale = TEXT("SparkleScale");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_SnowAgeDays = TEXT("SnowAgeDays");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_SnowAlbedoMean = TEXT("SnowAlbedoMean");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_Grain_um = TEXT("GrainSize_um");

	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Visuals | Params")
	FName Param_Impurity_ppm = TEXT("Impurity_ppm");

	// Snow Diagnostics
	// ============================================================================

	// === Radiation Diagnostics Configuration ===

	/** Enable detailed radiation index diagnostics output (separate from main diagnostics).
	 * Creates two files per simulation: *_radiation.csv (timeseries) and *_radiation_config.txt (settings).
	 * Main diagnostics will only include the final radiation index used for melt calculations. */
	/** Enable detailed radiation index diagnostics output (separate from main diagnostics).
	 * Creates two files per simulation: *_radiation.csv (timeseries) and *_radiation_config.txt (settings).
	 * Main diagnostics will only include the final radiation index used for melt calculations. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Diagnostics")
	bool bEnableRadiationDiagnostics = false;

	/** Auto-managed radiation diagnostics folder path. Manual overrides are ignored. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Diagnostics", meta=(DisplayName="Radiation Output Directory (Auto)", AdvancedDisplay, ToolTip="Automatically routed to analysis_results/Maps/<Map>/<Model>/Radiation by the diagnostics pipeline."))
	FString RadiationDiagnosticsDirectory = TEXT("analysis_results/Maps/UnknownMap/UnknownModel/Radiation");

	/** Append to existing radiation diagnostics file instead of overwriting. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Diagnostics", meta=(EditCondition="bEnableRadiationDiagnostics"))
	bool bAppendRadiationDiagnostics = false;

	/** Enable diagnostics logging to CSV (DegreeDay simulation). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Diagnostics")
	bool bEnableDiagnostics = false;

	/** Write diagnostics every N simulation steps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Diagnostics", meta=(ClampMin="1"))
	int32 DiagnosticsEveryNSteps = 1;

	/** When enabled, FSM2 diagnostics writers keep only the CSV columns required by the consolidated post-run plot bundle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Diagnostics", meta=(DisplayName="Lean Post-Run Bundle Diagnostics", ToolTip="Writes only the FSM2 diagnostics and radiation CSV columns used by the post-run bundle plots. Use this to reduce disk I/O during production runs."))
	bool bWriteLeanPostRunBundleDiagnostics = false;

	/** Auto-managed diagnostics folder path. Manual overrides are ignored. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Diagnostics", meta=(DisplayName="Diagnostics Output Directory (Auto)", AdvancedDisplay, ToolTip="Automatically routed to analysis_results/Maps/<Map>/<Model>/Diagnostics by the diagnostics pipeline."))
	FString DiagnosticsDirectory = TEXT("analysis_results/Maps/UnknownMap/UnknownModel/Diagnostics");

	/** Cell indices to track for detailed diagnostics (CSV). Leave empty to track cell 0 only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Diagnostics")
	TArray<int32> DiagnosticsTrackedCellIndices;

	/** Path to JSON containing ue_diagnostics_tracked_cell_indices. Absolute or relative to project root.
	 * Leave empty to auto-pick the newest *_diagnostic_cells.json under analysis_results/Maps/<MapTag>/Terrain. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Diagnostics")
	FString DiagnosticsTrackedCellsJsonPath;

	/** Texture containing melt-out DOY values (R16F). Nodata (-1) is mapped to 0. */
	/** Texture containing melt-out DOY values (R16F). Nodata (-1) is mapped to 0. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Diagnostics")
	UTexture2D* MeltoutDOYTexture = nullptr;

	/** GPU texture updated from CPU depth buffer (PF_R16F). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Diagnostics")
	UTexture2D* SnowDepthTexture = nullptr;

	/** GPU texture for albedo values (PF_B8G8R8A8 grayscale in RGBA for material Texture2D params). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Diagnostics")
	UTexture2D* SnowAlbedoTexture = nullptr;

	// === Radiation Capture Configuration ===

	/** Enable orthographic irradiance atlas rendering (for UE-based radiation index) */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture")
	bool bEnableRadiationCapture = false;

	/** Persistently lock rendering detail for radiation captures (no per-frame LOD toggling). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | LOD", meta=(EditCondition="bEnableRadiationCapture"))
	bool bLockCaptureLODOnce = true;

	/** SceneCapture LOD distance factor used for radiation capture views (lower = higher detail). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | LOD", meta=(EditCondition="bEnableRadiationCapture && bLockCaptureLODOnce", ClampMin="0.01", ClampMax="1.0", UIMin="0.01", UIMax="1.0"))
	float CaptureLODDistanceFactor = 0.1f;

	/** Force Landscape components to LOD0 for radiation captures. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | LOD", meta=(EditCondition="bEnableRadiationCapture && bLockCaptureLODOnce"))
	bool bForceLandscapeLOD0ForCapture = true;

	/** Force Virtual Heightfield Mesh settings toward maximum detail for radiation captures. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | LOD", meta=(EditCondition="bEnableRadiationCapture && bLockCaptureLODOnce"))
	bool bForceVHMLodForCapture = true;

	/** VHM LOD0 screen size used when forcing high detail (smaller = higher detail). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | LOD", meta=(EditCondition="bEnableRadiationCapture && bLockCaptureLODOnce && bForceVHMLodForCapture", ClampMin="0.1", UIMin="0.1"))
	float CaptureVHMLod0ScreenSize = 0.1f;

	/** VHM force-loaded LOD levels for capture stability (higher uses more VT memory). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | LOD", meta=(EditCondition="bEnableRadiationCapture && bLockCaptureLODOnce && bForceVHMLodForCapture", ClampMin="0", ClampMax="4", UIMin="0", UIMax="4"))
	int32 CaptureVHMNumForceLoadLods = 4;

	/** Enable View.LODDistanceFactor contribution for VHM so SceneCapture LOD settings affect VHM. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | LOD", meta=(EditCondition="bEnableRadiationCapture && bLockCaptureLODOnce && bForceVHMLodForCapture"))
	bool bEnableVHMViewLodFactorForCapture = true;

	/** Allow the direct-only pass to keep atmosphere/fog/volumetric-fog contributions.
	 * Leave disabled for a pure direct reference so Total-Direct preserves the non-direct shortwave residual. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture", DisplayName="Include Atmosphere/Fog In Direct Pass"))
	bool bDirectCaptureIncludesAtmosphere = false;

	/** When reference-tile calibration is active, keep atmosphere/fog in the direct pass too so the strip direct beam
	 * stays energy-consistent with Total/TotalNoGI. This targets cases where Direct exceeds TotalNoGI on the strip and
	 * collapses the derived diffuse residual. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture && bUseReferenceTile", DisplayName="Match Direct Pass Atmosphere To Reference Calibration"))
	bool bMatchDirectPassAtmosphereToReferenceCalibration = true;

	/** Include atmosphere/fog/volumetric fog in the non-direct decomposition passes (Total/Diffuse/DiffuseNoGI).
	 * Kept separate from the direct-pass toggle so older saved actors with bDirectCaptureIncludesAtmosphere=false
	 * still preserve atmospheric diffuse in the non-direct residual. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture", DisplayName="Include Atmosphere/Fog In Non-Direct Passes"))
	bool bNonDirectCapturesIncludeAtmosphere = true;

	/** Controls nighttime behavior for the radiation capture pipeline. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture"))
	ERadiationNightCaptureMode RadiationNightCaptureMode = ERadiationNightCaptureMode::SkipAndZero;

	/** Number of real engine frames to render before recording the final radiation capture.
	 * Larger values give Lumen extra time to converge after large lighting jumps. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0", ClampMax="16"))
	int32 RadiationPrimingFrameCount = 0;

	/** Manual exposure value (EV100) for radiation captures. Ensures deterministic linear HDR values.
	 * EV100=0 means no exposure compensation (physically accurate luminance values).
	 * Higher values brighten the image (e.g., EV100=10 multiplies by 2^10=1024).
	 * Lower values darken (e.g., EV100=-2 divides by 4). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture", ClampMin="-10.0", ClampMax="20.0"))
	float RadiationCaptureEV100 = 0.0f;

	/** Allow per-actor overrides of key Lumen CVars during off-screen captures. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Lumen", meta=(EditCondition="bEnableRadiationCapture"))
	bool bOverrideLumenCaptureCVars = true;

	/** Value applied to r.Lumen.DiffuseIndirect.TraceStepFactor while captures run. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Lumen", meta=(EditCondition="bEnableRadiationCapture && bOverrideLumenCaptureCVars", ClampMin="0.1", ClampMax="10.0"))
	float LumenCaptureTraceStepFactor = 0.5f;

	/** Value applied to r.Lumen.TraceDistanceScale while captures run. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Lumen", meta=(EditCondition="bEnableRadiationCapture && bOverrideLumenCaptureCVars", ClampMin="0.1", ClampMax="10.0"))
	float LumenCaptureTraceDistanceScale = 1.0f;

	/** Value applied to r.Lumen.DiffuseIndirect.Scale while captures run. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Lumen", meta=(EditCondition="bEnableRadiationCapture && bOverrideLumenCaptureCVars", ClampMin="0.0", ClampMax="10.0"))
	float LumenCaptureDiffuseIndirectScale = 1.0f;

	/** Toggles r.Lumen.TraceMeshSDFs (1 = enabled) to enforce higher quality indirect bounces. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Lumen", meta=(EditCondition="bEnableRadiationCapture && bOverrideLumenCaptureCVars"))
	bool bLumenCaptureForceMeshSDFs = true;

	/** Toggles r.UsePreExposure during captures (1 = enabled). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Lumen", meta=(EditCondition="bEnableRadiationCapture && bOverrideLumenCaptureCVars"))
	bool bCaptureUsePreExposure = true;

	/** Enable Lumen convergence warm-up frames to build up diffuse/indirect lighting before final capture.
	 * This temporarily re-enables Lumen temporal history for a few frames to allow skylight and indirect
	 * bounces to accumulate, then disables it again for the final capture to prevent cross-timestep lag. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Lumen", meta=(EditCondition="bEnableRadiationCapture && bOverrideLumenCaptureCVars"))
	bool bEnableLumenConvergence = true;

	/** Number of warm-up frames to run with Lumen temporal history enabled. Higher values allow more
	 * diffuse/indirect energy to accumulate, useful for scenes with high skylight contrast or complex GI.
	 * 5 frames is typically enough for outdoor scenes; increase to 8-10 for complex indoor/canyon scenarios. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Lumen", meta=(EditCondition="bEnableRadiationCapture && bOverrideLumenCaptureCVars && bEnableLumenConvergence", ClampMin="1", ClampMax="20"))
	int32 LumenConvergenceFrames = 5;

	/** History weight to use during Lumen convergence (0.0 = no temporal, 1.0 = full temporal).
	 * Higher values (0.8-0.95) converge faster but may overshoot; lower values (0.5-0.7) are more stable. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Lumen", meta=(EditCondition="bEnableRadiationCapture && bOverrideLumenCaptureCVars && bEnableLumenConvergence", ClampMin="0.0", ClampMax="1.0"))
	float LumenConvergenceHistoryWeight = 0.9f;

	/** Enable debug logging for radiation capture consistency verification */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Debug", meta=(EditCondition="bEnableRadiationCapture"))
	bool bDebugRadiationCapture = false;

	/** Reference to the primary SunSky directional light component (cached during UpdateSunSkyActors). */
	UPROPERTY(Transient)
	TWeakObjectPtr<UDirectionalLightComponent> CachedSunDirectionalLight;

	UDirectionalLightComponent* GetSunDirectionalLight() const { return CachedSunDirectionalLight.Get(); }

	/** Test pixel X coordinate for consistency verification (0 to GridDimX-1) */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Debug", meta=(EditCondition="bEnableRadiationCapture && bDebugRadiationCapture"))
	int32 DebugPixelX = 0;

	/** Test pixel Y coordinate for consistency verification (0 to GridDimY-1) */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Debug", meta=(EditCondition="bEnableRadiationCapture && bDebugRadiationCapture"))
	int32 DebugPixelY = 0;

	/** Export per-capture CSV diagnostics for reference-strip mapping checks (cell pixel vs top strip values). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Debug", meta=(EditCondition="bEnableRadiationCapture"))
	bool bExportReferenceMappingDebug = false;

	/** Cell index used by reference mapping debug export (-1 = first tracked diagnostics cell). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Debug", meta=(EditCondition="bEnableRadiationCapture && bExportReferenceMappingDebug"))
	int32 ReferenceMappingDebugCellIndex = -1;

	/** Render target for TOTAL irradiance (DirectionalLight + SkyLight + Lumen GI) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RT_Total = nullptr;

	/** Render target for DIRECT-ONLY irradiance (DirectionalLight + Shadows only, no GI/Sky) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RT_Direct = nullptr;

	/** Render target for DIFFUSE-ONLY irradiance (SkyLight + GI, no direct sun). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RT_Diffuse = nullptr;

	/** Render target for the no-GI reference pass (sun + sky, but no GI/reflections).
	 * RTY_DiffuseNoGI is derived later as max(TotalNoGI - Direct, 0). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RT_DiffuseNoGI = nullptr;

	/** Render target for render-side surface state used by dual-reference selection. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RT_SurfaceState = nullptr;

	/** Scalar render target for TOTAL pass (R16f single-channel).
	 * GPU-converted from RT_Total RGB using equal tristimulus weights: Y = (R + G + B) / 3.
	 * Not BT.709 photopic — see RGBToLuminance.usf for rationale. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RTY_Total = nullptr;

	/** Scalar render target for DIRECT-ONLY pass (R16f single-channel).
	 * GPU-converted from RT_Direct RGB using equal tristimulus weights: Y = (R + G + B) / 3. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RTY_Direct = nullptr;

	/** Diffuse irradiance render target (R16f single-channel).
	 * GPU-computed: RTY_Diffuse = max(RTY_Total - RTY_Direct, epsilon)
	 * OR GPU-converted directly from RT_Diffuse when bUseDiffuseCapture is enabled.
	 * Represents indirect lighting only (skylight + GI + inter-reflections).
	 * Optional 3x3 Gaussian blur applied if bBlurDiffuse is enabled (suppresses GI speckle). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RTY_Diffuse = nullptr;

	/** Luminance-only render target for sky diffuse without terrain GI (R16f single-channel).
	 * GPU-derived as max(luminance(TotalNoGI) - luminance(Direct), 0). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RTY_DiffuseNoGI = nullptr;

	/** Luminance-only render target for TotalNoGI (sun + sky, GI/reflections removed).
	 * This is the stable calibration basis for the 3-way split. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RTY_TotalNoGI = nullptr;

	/** Single-channel render-side surface-state proxy derived from RT_SurfaceState. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RTY_SurfaceState = nullptr;

	/** Temporary buffer for diffuse blur pass (R16f single-channel).
	 * Only used when bBlurDiffuse is enabled. */
	UPROPERTY(Transient)
	UTextureRenderTarget2D* RTY_DiffuseTemp = nullptr;

	/** Accumulation buffer for RTY_Total when time integration is enabled. */
	UPROPERTY(Transient)
	UTextureRenderTarget2D* RTY_TotalIntegration = nullptr;

	/** Accumulation buffer for RTY_Direct when time integration is enabled. */
	UPROPERTY(Transient)
	UTextureRenderTarget2D* RTY_DirectIntegration = nullptr;

	/** Accumulation buffer for RTY_Diffuse when time integration is enabled. */
	UPROPERTY(Transient)
	UTextureRenderTarget2D* RTY_DiffuseIntegration = nullptr;

	/** Accumulation buffer for RTY_DiffuseNoGI when time integration is enabled. */
	UPROPERTY(Transient)
	UTextureRenderTarget2D* RTY_DiffuseNoGIIntegration = nullptr;

	/** Accumulation buffer for RTY_TotalNoGI when time integration is enabled. */
	UPROPERTY(Transient)
	UTextureRenderTarget2D* RTY_TotalNoGIIntegration = nullptr;

	/** Enable 3x3 Gaussian blur on RTY_Diffuse to suppress GI speckle artifacts.
	 * Disabled by default. Useful when Lumen GI produces noisy indirect lighting. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture"))
	bool bBlurDiffuse = false;

	/** Capture a diffuse-only render target and derive RTY_Diffuse from it (instead of Total-Direct).
	 * Reference-tile calibrated mode forces Total-Direct because the dedicated diffuse pass under-captures sun-driven atmospheric diffuse at low solar elevation. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture"))
	bool bUseDiffuseCapture = false;

	/** Capture a no-GI pass for terrain residual estimation.
	 * The raw capture keeps sun + sky but disables GI/reflections; sky-only diffuse is then derived as TotalNoGI - Direct. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture"))
	bool bCaptureDiffuseNoGIForTerrain = false;

	/** Use RGBA32f render targets for the raw HDR capture passes instead of RGBA16f.
	 * Helps diagnose half-float saturation when Total/Diffuse values approach the 65504 ceiling.
	 * Increases memory use and readback cost. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture", AdvancedDisplay))
	bool bUseRGBA32fRadiationTargets = true;

	/** Terrain residual mode used to derive RTY_Terrain from captures.
	 * Recommended mode requires SkyOnly (RTY_DiffuseNoGI) capture.
	 * Fallback mode works when SkyOnly is not available. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture"))
	ETerrainResidualMode TerrainResidualMode = ETerrainResidualMode::TotalMinusDirectMinusSky;

	/** Enable 3x3 Gaussian blur on the terrain residual after RTY_Total - RTY_Direct - RTY_DiffuseNoGI.
	 * Useful for suppressing GI speckle amplification in RTY_Terrain and terrain-share maps. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture"))
	bool bBlurTerrainResidual = false;

	/** Clamp for extracted radiation index values (prevents outliers from reflections/steep slopes). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.1"))
	float MaxRadiationIndexClamp = 5.0f;

	/** Sun elevation threshold (degrees) that controls how quickly UE radiation is forced to zero after sunset. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.0", ClampMax="90.0", UIMin="0.0", UIMax="15.0", Units="deg"))
	float RadiationSunVisibilityThreshold = 0.6f;

	/** Enable time-integrated radiation capture.
	 * Instead of snapping a single instantaneous frame at the start of the hour,
	 * the system will sub-step the sun position multiple times across the hour,
	 * accumulate the radiation, and use the mean value.
	 * Crucial for accurate dawn/dusk transitions with 1h steps. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | TimeIntegration", meta=(EditCondition="bEnableRadiationCapture"))
	bool bEnableTimeIntegratedRadiation = false;

	/** Center the integration window on the simulation timestamp (reduces apparent sun lag). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | TimeIntegration", meta=(EditCondition="bEnableRadiationCapture && bEnableTimeIntegratedRadiation"))
	bool bCenterTimeIntegrationWindow = false;

	/** Step size in minutes for the radiation integration loop.
	 * Smaller values = smoother dawn/dusk transitions but slower simulation.
	 * Recommended: 10-15 minutes (4-6 samples per hour). */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | TimeIntegration", meta=(EditCondition="bEnableRadiationCapture && bEnableTimeIntegratedRadiation", ClampMin="1", ClampMax="60"))
	int32 RadiationIntegrationSubstepMinutes = 10;

	/** Force a SkyLight recapture before each radiation capture sample.
	 * Useful for diagnosing stale SkyLight state in diffuse and sky-only passes.
	 * Expensive when time integration is enabled. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRecaptureSkyBeforeRadiationSample = false;

	/** Use a telephoto perspective capture (small FOV) instead of orthographic to enable Lumen diffuse GI. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture"))
	bool bUseTelephotoCapture = false;

	/** Vertical FOV (degrees) when telephoto capture is enabled. Smaller FOV ≈ more orthographic. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture && bUseTelephotoCapture", ClampMin="1.0", ClampMax="60.0"))
	float TelephotoFOVDegrees = 10.0f;

	/** Yaw offset (degrees) applied to top-down capture alignment.
	 * Default -90 keeps reference strip at RT top and aligns RT-X with landscape +X. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture", ClampMin="-180.0", ClampMax="180.0", UIMin="-180.0", UIMax="180.0", AdvancedDisplay))
	float CaptureYawOffsetDegrees = -90.0f;

	/** Dimensionless direct radiation index field (R16f single-channel).
	 * GPU-computed: r_dir(x,y) = RTY_Direct(x,y) / (R_direct + 1e-6)
	 * Where R_direct is the mean from reference patch (y=0).
	 * Values typically 0.0 to 2.0: <1.0 = shadowed, 1.0 = reference, >1.0 = enhanced (rare). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RTF_DirIndex = nullptr;

	/** Dimensionless diffuse/non-direct radiation index field (R16f single-channel).
	 * GPU-computed: r_diff(x,y) = RTY_Diffuse(x,y) / (R_diffuse + 1e-6)
	 * Where RTY_Diffuse still includes GI/terrain bounce.
	 * Kept for DegreeDay and legacy split-index paths; FSM2 flux-calibrated mode uses RTY_DiffuseNoGI + terrain residual instead. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RTF_DiffIndex = nullptr;

	// === Raw Geometry-Only Index (Optional Diagnostic) ===

	/** Enable computation/export of the raw total UE index texture (RTF_rUE_raw).
	 * When disabled, the texture is not allocated and pass 6 is skipped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bEnableRadiationCapture"))
	bool bEnableRawRadiationIndexTexture = false;

	/** Raw total radiation index (R16f): r_UE_raw = RTY_Total / R_total.
	 * Pure engine-relative geometry index (dimensionless), no meteorological forcing.
	 * Computed only when bEnableRawRadiationIndexTexture is enabled.
	 * Values: 0.0 (fully occluded) to 2.0+ (enhanced by reflections). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	UTextureRenderTarget2D* RTF_rUE_raw = nullptr;

	/** Occluder set used when exporting the dedicated geometric sky-view-factor map. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF")
	ESVFMapOccluderMode SVFMapOccluderMode = ESVFMapOccluderMode::TerrainOnly;

	/** Number of azimuth directions used to solve the horizon profile per cell. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF", meta=(ClampMin="4", ClampMax="360", UIMin="8", UIMax="72"))
	int32 SVFAzimuthSampleCount = 16;

	/** Binary-search refinements used to solve the minimum visible sky angle in each azimuth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF", meta=(ClampMin="1", ClampMax="12", UIMin="3", UIMax="8"))
	int32 SVFBinarySearchIterations = 5;

	/** Maximum horizon elevation considered blocked; values near 90 degrees approach a closed sky. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF", meta=(ClampMin="1.0", ClampMax="89.9", UIMin="45.0", UIMax="89.5"))
	float SVFMaxHorizonElevationDegrees = 89.0f;

	/** Horizontal trace reach used to search for skyline occluders. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF", meta=(ClampMin="1.0", UIMin="100.0", Units="m"))
	float SVFTraceMaxDistanceMeters = 2000.0f;

	/** Small lift above the cell centroid to avoid self-intersection with the source surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF", meta=(ClampMin="0.0", UIMin="0.0", Units="cm"))
	float SVFTraceOriginLiftCm = 10.0f;

	/** Use the local terrain normal as the hemisphere axis when computing terrain SVF.
	 * This matches HORAYZON's sky_view_factor definition and Lambertian diffuse exchange on tilted terrain.
	 * Disable to fall back to the legacy horizontal-hemisphere diagnostic. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF")
	bool bSVFUseSurfaceNormalHemisphere = true;

	/** Also export the complementary SVF definition alongside the primary terrain SVF product.
	 * When the primary export is surface-normal, the companion export is horizontal, and vice versa. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF")
	bool bExportComplementarySVFVariant = true;

	/** Allow the requested FullSceneRayTracingGPU mode to fall back to the CPU full-scene tracer when GPU ray tracing is unavailable or not implemented. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF")
	bool bAllowGeometryRayTracingGPUFallbackToCPU = true;

	/** Log a small CPU-vs-GPU visibility sample when building geometry diagnostics with the GPU ray-tracing path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF", AdvancedDisplay)
	bool bLogGeometryRayTracingGpuCpuMicroDiagnostic = true;

	/** Number of rays compared in the GPU-vs-CPU geometry micro-diagnostic. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Terrain | SVF", meta=(EditCondition="bLogGeometryRayTracingGpuCpuMicroDiagnostic", ClampMin="1", ClampMax="32", UIMin="4", UIMax="16"), AdvancedDisplay)
	int32 GeometryRayTracingGpuCpuMicroDiagnosticSampleCount = 8;

	/** Export geometric direct-shadow comparison maps alongside dated radiation exports. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Capture")
	bool bExportDirectGeometryMaps = true;

	/** Occluder set used when exporting the dated geometric SWdir and sun-mask maps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bExportDirectGeometryMaps"))
	ESVFMapOccluderMode SWdirMapOccluderMode = ESVFMapOccluderMode::TerrainOnly;

	/** Maximum direct-shadow trace reach used for the geometric SWdir export. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bExportDirectGeometryMaps", ClampMin="1.0", UIMin="100.0", Units="m"))
	float SWdirTraceMaxDistanceMeters = 2000.0f;

	/** Small lift above the cell centroid to avoid self-shadowing in geometric direct-ray traces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Capture", meta=(EditCondition="bExportDirectGeometryMaps", ClampMin="0.0", UIMin="0.0", Units="cm"))
	float SWdirTraceOriginLiftCm = 10.0f;

	/** Export below-canopy probe radiation maps at each ExportTargetTimestep.
	 * Works with any radiation index (UEScene, SwiftGeom, Hock3, Pellicciotti) —
	 * the SVF/SWdir/Wm2 products are purely analytical and do not require bEnableRadiationCapture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy")
	bool bExportBelowCanopyProbeMaps = false;

	/** Occluder set used by the below-canopy probe export. FullScene is recommended for forest canopies. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeMaps"))
	ESVFMapOccluderMode BelowCanopyProbeOccluderMode = ESVFMapOccluderMode::FullScene;

	/** Vertical clearance above the snow-displaced cell surface used for below-canopy probe tracing.
	 *  Default 300 cm keeps the probe above even deep snowpacks (Totalp peak ~2.1 m). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeMaps", ClampMin="0.0", UIMin="0.0", Units="cm"))
	float BelowCanopyProbeHeightCm = 300.0f;

	/** Number of azimuth directions used by the below-canopy diffuse sky-view probe. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeMaps", ClampMin="4", ClampMax="360", UIMin="8", UIMax="72"))
	int32 BelowCanopyProbeAzimuthSampleCount = 16;

	/** Binary-search refinements used by the below-canopy diffuse sky-view probe. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeMaps", ClampMin="1", ClampMax="12", UIMin="3", UIMax="8"))
	int32 BelowCanopyProbeBinarySearchIterations = 5;

	/** Horizontal trace reach used by the below-canopy probe export. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeMaps", ClampMin="1.0", UIMin="100.0", Units="m"))
	float BelowCanopyProbeTraceMaxDistanceMeters = 2000.0f;

	/** Export an additional sparse-probe scene-bounce term for below-canopy validation.
	 * This remains export-only and does not feed the melt-model radiation indices. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy")
	bool bExportBelowCanopySceneBounceMaps = false;

	/** Export a hemispherical luminance diagnostic for one selected below-canopy probe cell.
	 * This writes five upward-looking perspective faces plus a reprojected fisheye image.
	 * Works without bEnableRadiationCapture — Lumen converges cleanly with no RTY contamination. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy")
	bool bExportBelowCanopyProbeHemisphere = false;

	/** Grid-space X index of the selected below-canopy hemisphere probe cell. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeHemisphere", ClampMin="0", UIMin="0"))
	int32 BelowCanopyHemisphereProbeCellX = 0;

	/** Grid-space Y index of the selected below-canopy hemisphere probe cell. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeHemisphere", ClampMin="0", UIMin="0"))
	int32 BelowCanopyHemisphereProbeCellY = 0;

	/** Resolution of each transient perspective face used to build the selected-probe hemisphere export. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeHemisphere", ClampMin="16", UIMin="32", UIMax="1024"))
	int32 BelowCanopyHemisphereCaptureResolution = 256;

	/** Output resolution of the reprojected fisheye image for the selected below-canopy probe. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeHemisphere", ClampMin="32", UIMin="64", UIMax="2048"))
	int32 BelowCanopyHemisphereFisheyeResolution = 512;

	/** Also export the underlying five perspective face luminance grids used to build the fisheye image. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeHemisphere"))
	bool bExportBelowCanopyHemisphereFaceGrids = true;

	/** Also export a low-material companion hemisphere capture for the selected below-canopy probe cell.
	 * This uses a temporary dark material override on the active terrain render surface plus the landscape so
	 * the hemisphere plotter can derive `TotalDark` and `Total - TotalDark` material-pair diagnostics. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeHemisphere"))
	bool bExportBelowCanopyHemisphereLowMaterialCompanion = false;

	/** Low-albedo override material used for the optional below-canopy hemisphere companion capture.
	 * When unset, the scene-bounce low-albedo material is reused if available. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeHemisphere && bExportBelowCanopyHemisphereLowMaterialCompanion"))
	TSoftObjectPtr<UMaterialInterface> BelowCanopyHemisphereLowAlbedoMaterial;

	/** Spacing in grid cells between sparse scene-bounce probes. Larger spacing is faster but smoother. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps", ClampMin="1", UIMin="1", UIMax="64"))
	int32 BelowCanopySceneBounceProbeSpacingCells = 8;

	/** Restrict scene-bounce ProbeLattice sampling and dense exports to the GROUNDEYE comparison AOI.
	 * The full terrain scene remains visible to the capture, but probe anchors are concentrated inside this grid window. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps"))
	bool bBelowCanopySceneBounceUseGroundeyeAoi = false;

	/** Inclusive minimum simulation-grid X index for the GROUNDEYE AOI ProbeLattice window. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps && bBelowCanopySceneBounceUseGroundeyeAoi", ClampMin="0", UIMin="0"))
	int32 BelowCanopySceneBounceGroundeyeAoiMinX = 76;

	/** Inclusive maximum simulation-grid X index for the GROUNDEYE AOI ProbeLattice window. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps && bBelowCanopySceneBounceUseGroundeyeAoi", ClampMin="0", UIMin="0"))
	int32 BelowCanopySceneBounceGroundeyeAoiMaxX = 324;

	/** Inclusive minimum simulation-grid Y index for the GROUNDEYE AOI ProbeLattice window. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps && bBelowCanopySceneBounceUseGroundeyeAoi", ClampMin="0", UIMin="0"))
	int32 BelowCanopySceneBounceGroundeyeAoiMinY = 141;

	/** Inclusive maximum simulation-grid Y index for the GROUNDEYE AOI ProbeLattice window. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps && bBelowCanopySceneBounceUseGroundeyeAoi", ClampMin="0", UIMin="0"))
	int32 BelowCanopySceneBounceGroundeyeAoiMaxY = 259;

	/** Resolution of each transient perspective face used to integrate the below-canopy scene-bounce probes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps", ClampMin="8", UIMin="16", UIMax="256"))
	int32 BelowCanopySceneBounceCaptureResolution = 48;

	/** Hard safety budget for sparse scene-bounce probes. The exporter auto-coarsens spacing when the requested lattice would exceed this probe count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps", ClampMin="1", UIMin="64", UIMax="8192"))
	int32 BelowCanopySceneBounceMaxProbeCount = 512;

	/** Vertical clearance above the highest terrain cell used for the open-sky reference probe. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps", ClampMin="0.0", UIMin="0.0", Units="cm"))
	float BelowCanopySceneBounceReferenceClearanceCm = 5000.0f;

	/** Run an additional low-material sparse-probe sweep during below-canopy scene-bounce export.
	 * The primary high pass uses the live scene materials; the secondary low pass temporarily overrides
	 * the active terrain render surface plus the landscape to suppress texture leakage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps"))
	bool bBelowCanopySceneBounceUseMaterialPair = false;

	/** In material-pair mode, skip the legacy TotalNoGI residual pass and treat the low-material sweep as the primary terrain-share diagnostic.
	 * This reduces probe captures from three sweeps (Total, TotalNoGI, TotalDark) to two (Total, TotalDark). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps && bBelowCanopySceneBounceUseMaterialPair"))
	bool bBelowCanopySceneBounceLeanMaterialPairMode = true;

	/** Low-albedo override material used for the secondary below-canopy scene-bounce probe sweep.
	 * This should be a simple dark Lambertian-style material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps && bBelowCanopySceneBounceUseMaterialPair"))
	TSoftObjectPtr<UMaterialInterface> BelowCanopySceneBounceLowAlbedoMaterial;

	/** Export scaled W/m2 dense maps for the below-canopy scene-bounce run.
	 * Leave this off for faster manuscript-focused runs that only need RTY-relative products. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps"))
	bool bBelowCanopySceneBounceExportWm2Products = false;

	/** Export EXR mirrors of the below-canopy scene-bounce dense maps.
	 * This is disabled by default because the PNG/CSV products are usually sufficient for diagnostics and plotting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps"))
	bool bBelowCanopySceneBounceExportExrProducts = false;

	/** Export the canonical sparse probe table used to build the dense below-canopy scene-bounce maps.
	 * This stores per-probe RTY values before interpolation so plotting and post-processing can be rerun without another UE export. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps"))
	bool bBelowCanopySceneBounceExportSparseProbeTable = true;

	/** Run the below-canopy plotting script after export. This is visualization-only and does not affect melt-model inputs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopyProbeMaps || bExportBelowCanopySceneBounceMaps"))
	bool bGenerateBelowCanopyPlots = true;

	/** Also emit an interactive HTML summary when Plotly is available in the selected Python environment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bGenerateBelowCanopyPlots"))
	bool bGenerateBelowCanopyInteractiveHtml = true;

	/** Python executable used for below-canopy plotting. Leave as `python` when available on PATH. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bGenerateBelowCanopyPlots"))
	FString BelowCanopyPlotPythonExecutable = TEXT("python");

	// === Explicit Render Target Exports ===

	/** Specific timesteps at which below-canopy products are written to disk.
	 * Used by both the UEScene radiation pipeline (bEnableRadiationCapture) and the
	 * standalone analytical/hemisphere path (bExportBelowCanopyProbeMaps / bExportBelowCanopyProbeHemisphere). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export")
	TArray<FDateTime> ExportTargetTimesteps;

	/** Also export a PNG screenshot from the matching level-editor viewport camera when atlas radiation maps are written. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture"))
	bool bExportEditorViewportScreenshot = true;

	/** Include the orthographic atlas radiation export in the dedicated map-export action. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationMapExportIncludeAtlas = true;

	/** Include dated UE SWdir geometry exports in the dedicated map-export action. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationMapExportIncludeSWdir = true;

	/** Include dated UE SVF geometry exports in the dedicated map-export action. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationMapExportIncludeSVF = true;

	/** Include the sparse below-canopy probe-lattice export in the dedicated map-export action. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationMapExportIncludeProbeLattice = true;

	/** Include the selected below-canopy hemisphere fisheye export in the dedicated map-export action. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationMapExportIncludeHemisphere = false;

	/** Include the HPEval-method SVF and below-canopy SWR fisheye-derived maps in the dedicated map-export action.
	 * Captures an upward-looking hemisphere image at each probe cell (or sparse subset) and applies the
	 * HPEval ring-integration formula (Jonas et al. 2019) to derive SVF_flat and below-canopy SWR.
	 * Produces two grid CSV maps: SVF_HPEval and SWR_BelowCanopy_HPEval. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationMapExportIncludeHPEval = false;

	/** Spacing in grid cells between HPEval hemisphere probes. 1 = every cell (slow), 4 = every 4th cell (fast).
	 * The dense SVF/SWR grids are filled by bilinear interpolation between sampled probe cells. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture && bRadiationMapExportIncludeHPEval", ClampMin="1", UIMin="1", UIMax="32"))
	int32 HPEvalProbeSpacingCells = 4;

	/** Resolution of each transient perspective face used during HPEval hemisphere captures.
	 * Higher values improve SVF accuracy at the cost of capture time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture && bRadiationMapExportIncludeHPEval", ClampMin="16", UIMin="32", UIMax="512"))
	int32 HPEvalHemisphereCaptureResolution = 128;

	/** Output resolution of the equidistant fisheye image used for HPEval ring integration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture && bRadiationMapExportIncludeHPEval", ClampMin="32", UIMin="64", UIMax="512"))
	int32 HPEvalHemisphereFisheyeResolution = 256;

	/** Number of equal-area zenith rings used when integrating the fisheye image for SVF.
	 * 9 is the HPEval default; increasing this improves angular resolution at marginal cost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Map Export", meta=(EditCondition="bEnableRadiationCapture && bRadiationMapExportIncludeHPEval", ClampMin="3", ClampMax="36", UIMin="6", UIMax="18"))
	int32 HPEvalZenithRings = 9;

	/** Legacy toggle retained for compatibility only.
	 * HPEval is no longer auto-run from probe-lattice exports because the extra fisheye capture sweep
	 * roughly doubles the capture workload and can trigger VRAM OOM during simulation-time exports.
	 * Use the dedicated Run Radiation Map Export action with bRadiationMapExportIncludeHPEval instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | BelowCanopy", meta=(EditCondition="bExportBelowCanopySceneBounceMaps", DisplayName="Legacy HPEval Piggyback (Ignored)"))
	bool bIncludeHPEvalInProbeLatticeExport = false;

	/** Benchmark target surfaces that should receive the temporary Lambertian override during the dedicated benchmark capture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture"))
	TArray<TObjectPtr<AActor>> RadiationBenchmarkSurfaceActors;

	/** Apply a temporary benchmark-surface material override during dedicated cavity captures. Disable this when the map is already authored with the intended Lambertian benchmark material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture"))
	bool bUseRadiationBenchmarkSurfaceOverride = false;

	/** Material used for the optional benchmark surface override. Expected to be a simple Lambertian-style material with editable albedo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && bUseRadiationBenchmarkSurfaceOverride"))
	TSoftObjectPtr<UMaterialInterface> RadiationBenchmarkSurfaceMaterial;

	/** In probe-lattice benchmark mode, run a paired-material capture using distinct high- and low-albedo override materials.
	 * This is intended for sensitivity experiments and should not be used as a strict physical terrain-interreflection decomposition. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && RadiationBenchmarkMethod==ERadiationBenchmarkMethod::ProbeLattice"))
	bool bRadiationBenchmarkProbeLatticeUseMaterialPair = false;

	/** High-albedo material used for the primary pass of the probe-lattice paired-material benchmark. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && bRadiationBenchmarkProbeLatticeUseMaterialPair && RadiationBenchmarkMethod==ERadiationBenchmarkMethod::ProbeLattice"))
	TSoftObjectPtr<UMaterialInterface> RadiationBenchmarkProbeLatticeHighAlbedoMaterial;

	/** Low-albedo material used for the secondary pass of the probe-lattice paired-material benchmark. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && bRadiationBenchmarkProbeLatticeUseMaterialPair && RadiationBenchmarkMethod==ERadiationBenchmarkMethod::ProbeLattice"))
	TSoftObjectPtr<UMaterialInterface> RadiationBenchmarkProbeLatticeLowAlbedoMaterial;

	/** Run the probe-lattice benchmark as a serial albedo sweep using the materials listed below. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && RadiationBenchmarkMethod==ERadiationBenchmarkMethod::ProbeLattice"))
	bool bRadiationBenchmarkProbeLatticeUseAlbedoSweep = false;

	/** Albedo/material samples used by the probe-lattice albedo sweep. Include an albedo=0 material for the preferred black-cavity baseline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && bRadiationBenchmarkProbeLatticeUseAlbedoSweep && RadiationBenchmarkMethod==ERadiationBenchmarkMethod::ProbeLattice"))
	TArray<FRadiationBenchmarkAlbedoSweepMaterial> RadiationBenchmarkProbeLatticeAlbedoSweepMaterials;

	/** Export representative fisheyes for every albedo-sweep sample. Disabled by default to keep the sweep lean. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && bRadiationBenchmarkProbeLatticeUseAlbedoSweep && RadiationBenchmarkMethod==ERadiationBenchmarkMethod::ProbeLattice"))
	bool bRadiationBenchmarkProbeLatticeAlbedoSweepExportDiagnosticHemisphere = false;

	/** Use a lean validation path for albedo sweeps: capture only RTY Total, skip TotalNoGI and scene-bounce exports, and write only the Total RTY map plus aggregate sweep outputs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && bRadiationBenchmarkProbeLatticeUseAlbedoSweep && RadiationBenchmarkMethod==ERadiationBenchmarkMethod::ProbeLattice"))
	bool bRadiationBenchmarkProbeLatticeAlbedoSweepLeanTotalOnly = true;

	/** Export a benchmark fisheye set from representative cavity locations during probe-lattice runs.
	 * The set currently includes bottom-center, sun-facing side, opposite-sun side, and near-top cavity viewpoints. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && RadiationBenchmarkMethod==ERadiationBenchmarkMethod::ProbeLattice", DisplayName="Export Benchmark Fisheye Set"))
	bool bRadiationBenchmarkProbeLatticeExportDiagnosticHemisphere = true;

	/** Scalar parameter written on the benchmark material when applying the requested albedo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && bUseRadiationBenchmarkSurfaceOverride"))
	FName RadiationBenchmarkScalarAlbedoParameter = TEXT("Albedo");

	/** Vector parameter written on the benchmark material when applying the requested albedo as grayscale base color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && bUseRadiationBenchmarkSurfaceOverride"))
	FName RadiationBenchmarkVectorAlbedoParameter = TEXT("BaseColor");

	/** Requested Lambertian surface albedo for the benchmark capture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.0", ClampMax="1.0"))
	float RadiationBenchmarkAlbedo = 0.80f;

	/** Incident radiation mode used for the benchmark capture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture"))
	ERadiationBenchmarkIncidentMode RadiationBenchmarkIncidentMode = ERadiationBenchmarkIncidentMode::DirectOnly;

	/** Sun elevation used when the benchmark capture drives the directional light manually. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && RadiationBenchmarkIncidentMode!=ERadiationBenchmarkIncidentMode::DiffuseOnly", ClampMin="0.0", ClampMax="89.9", UIMin="0.0", UIMax="89.9", Units="deg"))
	float RadiationBenchmarkSunElevationDeg = 15.0f;

	/** Sun azimuth used when the benchmark capture drives the directional light manually. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && RadiationBenchmarkIncidentMode!=ERadiationBenchmarkIncidentMode::DiffuseOnly", ClampMin="-180.0", ClampMax="180.0", UIMin="-180.0", UIMax="180.0", Units="deg"))
	float RadiationBenchmarkSunAzimuthDeg = 180.0f;

	/** Direct-normal irradiance used to drive the benchmark sun light when direct radiation is enabled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && RadiationBenchmarkIncidentMode!=ERadiationBenchmarkIncidentMode::DiffuseOnly", ClampMin="0.0"))
	float RadiationBenchmarkDNI_Wm2 = 800.0f;

	/** Horizontal diffuse irradiance used to drive the benchmark sky light when diffuse radiation is enabled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && RadiationBenchmarkIncidentMode!=ERadiationBenchmarkIncidentMode::DirectOnly", ClampMin="0.0"))
	float RadiationBenchmarkDHI_Wm2 = 200.0f;

	/** Benchmark backend to use when running the unified cavity benchmark action. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture"))
	ERadiationBenchmarkMethod RadiationBenchmarkMethod = ERadiationBenchmarkMethod::OrthographicAtlas;

	/** Compute and export SVF diagnostics alongside the benchmark capture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationBenchmarkExportSVF = true;

	/** Use a surface-normal-aligned hemisphere when computing benchmark SVF gates.
	 * This is kept separate from the normal terrain SVF export settings so cavity benchmarks can be configured independently. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationBenchmarkUseSurfaceNormalSVF = true;

	/** Allowed MAE of the cavity SVF field to the analytical hemispherical-cavity target (0.5). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation", meta=(ClampMin="0.0"))
	float RadiationBenchmarkSVFValidationMaeTolerance = 0.02f;

	/** Export the raw SVF validation geometry products (SVF, mean horizon, mask, altitude, slope). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	bool bRadiationBenchmarkSVFValidationExportMaps = true;

	/** True when the most recent cavity SVF validation completed successfully. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	bool bLastRadiationBenchmarkSVFValidationSucceeded = false;

	/** Status text from the most recent cavity SVF validation run. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	FString LastRadiationBenchmarkSVFValidationStatus;

	/** Report path written by the most recent cavity SVF validation run. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	FString LastRadiationBenchmarkSVFValidationReportPath;

	/** Mean SVF over the cavity validation domain from the most recent run. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	float LastRadiationBenchmarkSVFValidationMeanSVF = 0.0f;

	/** Standard deviation of SVF over the cavity validation domain from the most recent run. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	float LastRadiationBenchmarkSVFValidationStdSVF = 0.0f;

	/** MAE of SVF to the analytical hemispherical-cavity target (0.5) from the most recent run. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	float LastRadiationBenchmarkSVFValidationMaeToHalf = 0.0f;

	/** Full-domain mean SVF from the most recent cavity SVF validation run. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	float LastRadiationBenchmarkSVFValidationMeanSVFFullDomain = 0.0f;

	/** Full-domain MAE of SVF to 0.5 from the most recent cavity SVF validation run. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	float LastRadiationBenchmarkSVFValidationMaeToHalfFullDomain = 0.0f;

	/** Domain label used for the last cavity SVF validation statistics. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	FString LastRadiationBenchmarkSVFValidationDomainLabel;

	/** SVF definition used for the last cavity SVF validation run. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation")
	FString LastRadiationBenchmarkSVFValidationDefinition = TEXT("SurfaceNormalHemisphere");

	/** Restore the original benchmark-surface materials after the capture completes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationBenchmarkRestoreSurfaceMaterials = true;

	/** Run the benchmark plotting script automatically after exporting summary/maps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationBenchmarkGeneratePlot = true;

	/** Python executable used for the benchmark plotting script. Leave as `python` when available on PATH. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark", meta=(EditCondition="bEnableRadiationCapture && bRadiationBenchmarkGeneratePlot"))
	FString RadiationBenchmarkPythonExecutable = TEXT("python");

	/** Target absolute sky-only reference luminance to reach during cavity-based sky calibration.
	 * The auto-calibration adjusts SkyLightIntensityMultiplier until ReferenceLuminance_DiffuseNoGI
	 * from a DiffuseOnly hemispherical-cavity benchmark lands near this working point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.0001"))
	float RadiationBenchmarkSkyCalibrationTargetReferenceLuminance = 1.0f;

	/** Relative tolerance for the target sky-only reference luminance, expressed as a fraction of the target. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.0"))
	float RadiationBenchmarkSkyCalibrationRelativeTolerance = 0.05f;

	/** Analytical target for the cavity's mean normalized sky-only response.
	 * This is used as a validation gate before any SkyLightIntensityMultiplier update is accepted. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.0"))
	float RadiationBenchmarkSkyCalibrationTargetNormalizedDiffuseNoGI = 0.5f;

	/** Allowed absolute deviation from the analytical cavity mean normalized sky-only response. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.0"))
	float RadiationBenchmarkSkyCalibrationNormalizedDiffuseTolerance = 0.05f;

	/** Allowed MAE of the benchmark SVF map to the analytical hemispherical-cavity target (0.5). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.0"))
	float RadiationBenchmarkSkyCalibrationSVFMaeTolerance = 0.02f;

	/** Maximum number of benchmark iterations when solving for SkyLightIntensityMultiplier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="1", ClampMax="10"))
	int32 RadiationBenchmarkSkyCalibrationMaxIterations = 3;

	/** Clamp on each multiplicative SkyLightIntensityMultiplier adjustment to avoid unstable jumps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="1.0"))
	float RadiationBenchmarkSkyCalibrationMaxAdjustmentFactor = 4.0f;

	/** Relative tolerance used by the hemisphere-based light calibration against physical open-sky irradiance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.0"))
	float RadiationBenchmarkHemisphereCalibrationRelativeTolerance = 0.05f;

	/** Maximum number of benchmark iterations when solving benchmark-light scales from the hemisphere reference probe. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="1", ClampMax="10"))
	int32 RadiationBenchmarkHemisphereCalibrationMaxIterations = 3;

	/** Clamp on each multiplicative hemisphere-calibration light adjustment to avoid unstable jumps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="1.0"))
	float RadiationBenchmarkHemisphereCalibrationMaxAdjustmentFactor = 4.0f;

	/** Relative tolerance for the probe-lattice cavity diffuse solve.
	 * The calibration converges when the cavity-interior DiffuseNoGI response matches
	 * the analytical hemispherical-cavity target within this relative tolerance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="0.0"))
	float RadiationBenchmarkProbeLatticeCalibrationRelativeTolerance = 0.05f;

	/** Maximum number of benchmark iterations when solving SkyLightIntensityMultiplier from the sparse probe-lattice path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="1", ClampMax="10"))
	int32 RadiationBenchmarkProbeLatticeCalibrationMaxIterations = 3;

	/** Clamp on each multiplicative probe-lattice SkyLightIntensityMultiplier adjustment to avoid unstable jumps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration", meta=(EditCondition="bEnableRadiationCapture", ClampMin="1.0"))
	float RadiationBenchmarkProbeLatticeCalibrationMaxAdjustmentFactor = 4.0f;

	/** When true, the probe-lattice cavity benchmark uses the dedicated uniform benchmark skylight.
	 * Disable this to test parity against the same production scene-skylight path used by terrain exports. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration", meta=(EditCondition="bEnableRadiationCapture"))
	bool bRadiationBenchmarkProbeLatticeUseDedicatedSkyLight = true;

	/** True when the most recent cavity-based sky calibration completed successfully and kept the updated multiplier. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration")
	bool bLastRadiationBenchmarkSkyCalibrationSucceeded = false;

	/** Last observed sky-only reference luminance during the cavity calibration pipeline. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration")
	float LastRadiationBenchmarkSkyCalibrationReferenceLuminance = 0.0f;

	/** Last observed DHI / ReferenceLuminance_DiffuseNoGI scale from the cavity calibration pipeline. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration")
	float LastRadiationBenchmarkSkyCalibrationScaleDiffuseNoGI = 0.0f;

	/** Last observed mean normalized sky-only cavity response. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration")
	float LastRadiationBenchmarkSkyCalibrationMeanNormalizedDiffuseNoGI = 0.0f;

	/** Last observed SVF MAE-to-0.5 during the cavity calibration pipeline. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration")
	float LastRadiationBenchmarkSkyCalibrationSVFMaeToHalf = 0.0f;

	/** SkyLightIntensityMultiplier before the last calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration")
	float LastRadiationBenchmarkSkyCalibrationOriginalMultiplier = 0.0f;

	/** SkyLightIntensityMultiplier after the last calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration")
	float LastRadiationBenchmarkSkyCalibrationFinalMultiplier = 0.0f;

	/** Report path written by the last cavity-based sky calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration")
	FString LastRadiationBenchmarkSkyCalibrationReportPath;

	/** Summary CSV path from the last benchmark iteration inside the cavity calibration pipeline. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration")
	FString LastRadiationBenchmarkSkyCalibrationSummaryPath;

	/** Status text from the last cavity-based sky calibration attempt, including the failure reason when it aborts. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Sky Calibration")
	FString LastRadiationBenchmarkSkyCalibrationStatus;

	/** Summary CSV from the most recent hemisphere benchmark capture. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere")
	FString LastRadiationBenchmarkHemisphereSummaryPath;

	/** True when the most recent hemisphere-based light calibration completed successfully. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration")
	bool bLastRadiationBenchmarkHemisphereCalibrationSucceeded = false;

	/** Status text from the last hemisphere-based light calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration")
	FString LastRadiationBenchmarkHemisphereCalibrationStatus;

	/** Report path written by the last hemisphere-based light calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration")
	FString LastRadiationBenchmarkHemisphereCalibrationReportPath;

	/** Last open-sky reference direct irradiance measured by the hemisphere benchmark family. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration")
	float LastRadiationBenchmarkHemisphereReferenceDirectIrradiance = 0.0f;

	/** Last open-sky reference sky-only irradiance measured by the hemisphere benchmark family. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration")
	float LastRadiationBenchmarkHemisphereReferenceDiffuseNoGIIrradiance = 0.0f;

	/** Last open-sky reference total irradiance measured by the hemisphere benchmark family. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration")
	float LastRadiationBenchmarkHemisphereReferenceTotalIrradiance = 0.0f;

	/** Last probe direct component normalized by the open-sky direct reference. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere")
	float LastRadiationBenchmarkHemisphereNormalizedDirect = 0.0f;

	/** Last probe sky-only component normalized by the open-sky sky reference. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere")
	float LastRadiationBenchmarkHemisphereNormalizedDiffuseNoGI = 0.0f;

	/** Last probe terrain-bounce component normalized by the open-sky no-GI reference. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere")
	float LastRadiationBenchmarkHemisphereNormalizedTerrain = 0.0f;

	/** Last probe total component normalized by the open-sky total reference. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere")
	float LastRadiationBenchmarkHemisphereNormalizedTotal = 0.0f;

	/** Sun luminous-efficacy value before the last hemisphere-based light calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration")
	float LastRadiationBenchmarkHemisphereOriginalSunLuminousEfficacy = 0.0f;

	/** Sun luminous-efficacy value after the last hemisphere-based light calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration")
	float LastRadiationBenchmarkHemisphereFinalSunLuminousEfficacy = 0.0f;

	/** Sky-light multiplier before the last hemisphere-based light calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration")
	float LastRadiationBenchmarkHemisphereOriginalSkyLightIntensityMultiplier = 0.0f;

	/** Sky-light multiplier after the last hemisphere-based light calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Hemisphere Calibration")
	float LastRadiationBenchmarkHemisphereFinalSkyLightIntensityMultiplier = 0.0f;

	/** True when the most recent probe-lattice sky calibration completed successfully and kept the updated multiplier. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	bool bLastRadiationBenchmarkProbeLatticeCalibrationSucceeded = false;

	/** Status text from the last probe-lattice sky calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	FString LastRadiationBenchmarkProbeLatticeCalibrationStatus;

	/** Report path written by the last probe-lattice sky calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	FString LastRadiationBenchmarkProbeLatticeCalibrationReportPath;

	/** Summary CSV path from the last sparse probe-lattice benchmark iteration. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	FString LastRadiationBenchmarkProbeLatticeSummaryPath;

	/** Aggregate summary CSV path from the last probe-lattice albedo sweep. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	FString LastRadiationBenchmarkProbeLatticeAlbedoSweepSummaryPath;

	/** Legacy reference-plane luminance diagnostic from the last sparse probe-lattice benchmark iteration.
	 * The current cavity-direct calibration does not solve against this value and it is usually zero. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	float LastRadiationBenchmarkProbeLatticeReferencePlaneMeanLuminance = 0.0f;

	/** Legacy reference-plane irradiance diagnostic from the last sparse probe-lattice benchmark iteration.
	 * The current cavity-direct calibration does not solve against this value and it is usually zero. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	float LastRadiationBenchmarkProbeLatticeReferencePlaneIrradiance = 0.0f;

	/** Analytical diffuse scale candidate from the last sparse probe-lattice benchmark iteration.
	 * Computed as target cavity DiffuseNoGI divided by the observed cavity-interior DiffuseNoGI. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	float LastRadiationBenchmarkProbeLatticeScaleDiffuseNoGI = 0.0f;

	/** Last observed mean normalized diffuse-no-GI cavity response from the sparse probe-lattice benchmark. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	float LastRadiationBenchmarkProbeLatticeMeanNormalizedDiffuseNoGI = 0.0f;

	/** Last observed mean normalized terrain-bounce cavity response from the sparse probe-lattice benchmark. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	float LastRadiationBenchmarkProbeLatticeMeanNormalizedTerrain = 0.0f;

	/** Last observed mean normalized total cavity response from the sparse probe-lattice benchmark. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	float LastRadiationBenchmarkProbeLatticeMeanNormalizedTotal = 0.0f;

	/** Last observed SVF MAE-to-0.5 during the sparse probe-lattice calibration pipeline. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	float LastRadiationBenchmarkProbeLatticeSVFMaeToHalf = 0.0f;

	/** SkyLightIntensityMultiplier before the last sparse probe-lattice calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	float LastRadiationBenchmarkProbeLatticeOriginalMultiplier = 0.0f;

	/** SkyLightIntensityMultiplier after the last sparse probe-lattice calibration attempt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Benchmark | Probe Lattice Calibration")
	float LastRadiationBenchmarkProbeLatticeFinalMultiplier = 0.0f;

	/** Keep track of which timesteps have already been exported. */
	UPROPERTY(Transient)
	TArray<FDateTime> ExportedTargetTimes;

	/** Wall-clock timestamp tag used to isolate whole-grid radiation map exports for the current run. */
	UPROPERTY(Transient)
	FString RadiationExportRunTimestampTag;

	void ExportRadiationMapsForTimestep(const FDateTime& Time, const FString& OverrideTargetDir = FString());
	void ExportRenderTargetToDisk(class UTextureRenderTarget2D* RT, const FString& Filename, const FString& TargetDirectory = FString());
	bool ExportEditorViewportScreenshot(const FString& Filename, const FString& TargetDirectory = FString());

	/** Returns a sanitized map identifier used in output folder/file names. */
	FString GetCurrentMapTag() const;

	/** Returns a sanitized melt-model identifier used in output folder/file names. */
	FString GetCurrentMeltModelTag() const;

	/** Returns a sanitized radiation-scheme identifier used in output folder/file names. */
	FString GetCurrentRadiationSchemeTag() const;

	/** Returns "DynGeom" if dynamic surface geometry is enabled, "StatGeom" otherwise. */
	FString GetSnowGeometryTag() const;

	/** Returns a combined run tag: <map>_<meltmodel>_<radiation>_<geometry>[_<timestamp>]. */
	FString BuildRunTag(const FString& TimestampTag = FString()) const;

	/** Absolute output root for current map organization, independent of melt-model tag. */
	FString GetMapOutputBaseDirectory() const;

	/** Absolute output root for current map/model organization. */
	FString GetRunOutputBaseDirectory() const;

	/** Absolute output directory for a run category (e.g. Diagnostics, Meltout, Radiation). */
	FString GetOutputCategoryDirectory(const FString& Category) const;

	/** Absolute output directory for whole-grid radiation map exports created during the current run. */
	FString GetRadiationMapExportDirectory() const;

	/** Absolute root for button-triggered radiation map exports, organized by map only. */
	FString GetRadiationMapExportRootDirectory() const;

	/** Absolute directory for one product family inside the current button-triggered radiation map export run. */
	FString GetRadiationMapExportProductDirectory(const FString& ProductCategory) const;

	// === Reference Tile Configuration ===

	/** Enable reference tile border for normalized radiation index.
	 * When enabled, top border rows (y=0..ReferenceStripHeight-1) of render targets are reserved for a horizontal,
	 * unoccluded white reference plane. This provides R_total and R_direct reference
	 * luminance values for computing dimensionless radiation ratios.
	 * Option A: border strip at the top edge inside the same atlas. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Reference", meta=(EditCondition="bEnableRadiationCapture"))
	bool bUseReferenceTile = true;

	/** Height offset (meters) applied to keep the reference plane above local terrain. */
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Radiation | Capture | Reference", meta=(EditCondition="bEnableRadiationCapture && bUseReferenceTile", ClampMin="0.0", UIMin="0.0", Units="m"))
	float ReferencePlaneHeightOffsetMeters = 300.0f;

	/** Cached reference luminance from TOTAL pass (R + G + B) / 3.
	 * Updated each capture. Used for computing radiation index = L_cell / L_reference. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_Total = 0.0f;

	/** Cached reference luminance from DIRECT-ONLY pass (R + G + B) / 3.
	 * Updated each capture. Used for computing direct/diffuse ratios. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_Direct = 0.0f;

	/** Cached reference diffuse luminance from RTY_Diffuse.
	 * Updated each capture. Used for computing diffuse normalization. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_Diffuse = 0.0f;

	/** Cached reference diffuse luminance from RTY_DiffuseNoGI (sky-only).
	 * Updated each capture when DiffuseNoGI is available. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_DiffuseNoGI = 0.0f;

	/** Cached reference luminance from RTY_TotalNoGI (sun + sky, no GI/reflections). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_TotalNoGI = 0.0f;

	/** Reference luminance for the ground/landscape half of a dual strip. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_Total_Ground = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_Direct_Ground = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_Diffuse_Ground = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_DiffuseNoGI_Ground = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_TotalNoGI_Ground = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_SurfaceState_Ground = 0.0f;

	/** Reference luminance for the snow half of a dual strip. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_Total_Snow = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_Direct_Snow = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_Diffuse_Snow = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_DiffuseNoGI_Snow = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_TotalNoGI_Snow = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_SurfaceState_Snow = 0.0f;

	/** Full-strip mean render-side surface-state value. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	float ReferenceLuminance_SurfaceState = 0.0f;

	/** True once a real reference luminance has been read back from the GPU. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Radiation | Capture | Outputs")
	bool bReferenceLuminanceValid = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Debug")
	/** Number of simulation cells per dimension in x. */
	int32 CellsDimensionX;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Debug")
	/** Number of simulation cells per dimension in y. */
	int32 CellsDimensionY;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Debug")
	/** Landscape scale. */
	FVector LandscapeScale;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Debug")
	/** Overall landscape resolution in x dimension. */
	float OverallResolutionX;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Debug")
	/** Overall landscape resolution in y dimension. */
	float OverallResolutionY;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Debug")
	/** Total number of simulation cells. */
	int32 NumCells;

	/** Weather data provider for the simulation. */
	USimulationWeatherDataProviderBase* ClimateDataComponent;

	// Weather forcing system
	// Selection mode toggle: true = inline instance, false = class-based
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Weather", meta=(Tooltip="If true, InlineWeatherProvider is used; if false, WeatherProviderClass is used."))
	bool UseInlineWeatherProvider = true;

	// Inline instance edited in Details when inline mode is enabled
	UPROPERTY(EditAnywhere, Instanced, Category = "Snow Simulation | Weather", meta=(EditCondition="UseInlineWeatherProvider", EditConditionHides, ShowInnerProperties))
	USimulationWeatherDataProviderBase* InlineWeatherProvider = nullptr;

	// Class to instantiate at runtime when class mode is enabled
	UPROPERTY(EditAnywhere, Category = "Snow Simulation | Weather", meta=(EditCondition="!UseInlineWeatherProvider", HideEditConditionToggle, Tooltip="If UseInlineWeatherProvider=false, WeatherProviderClass is used."))
	TSubclassOf<USimulationWeatherDataProviderBase> WeatherProviderClass;

	// Convenience: When no provider is specified, use CSV provider by default if CSV settings exist

	// Runtime-resolved provider actually used
	UPROPERTY(Transient, VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Weather", meta=(AdvancedDisplay))
	USimulationWeatherDataProviderBase* WeatherProvider;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Weather")
	float TimeStepSeconds = 3600.0f; // 1 hour default

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Weather")
	bool bLoopTime = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Weather", meta=(EditCondition="!bLoopTime"))
	bool bAutoStopAtEndTime = true;

	/** Default constructor. */
	ASnowSimulationActor();

	/** Returns the simulation start time. */
	FDateTime GetRunStartTime() const { return StartTime; }

	/** Returns the simulation end time. */
	FDateTime GetRunEndTime() const { return EndTime; }

	/** Called when the game starts or when spawned */
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	
	/** Called every frame */
	virtual void Tick( float DeltaSeconds ) override;

	/** Allow ticking in editor when any debug visualization is enabled. */
	virtual bool ShouldTickIfViewportsOnly() const override;

	/** Initializes the simulation. */
	void Initialize();

	/** Resolves a concrete simulation instance, never returning the abstract base. */
	USimulationBase* ResolveSimulation();

	/** Builds and initializes the simulation instance based on SimulationModel. */
	void BuildSimulationInstance();

	/** Fills the CPU depth buffer with a debug gradient and uploads it to the GPU. */
	/** Fills the CPU depth buffer with a debug gradient and uploads it to the GPU. */
	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions")
	void DebugFillDepth(float MaxDepthMeters = 0.2f);

	/** Prints comprehensive status information for debugging. */
	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions")
	void PrintStatus();

	/** Called by simulations to update the CPU depth buffer (meters). */
	void UpdateCpuDepthMeters(const TArray<float>& InDepthMeters);

	/** Applies a cached depth snapshot to every runtime depth consumer before standalone exports. */
	void ApplyDepthSnapshotToRuntime(const TArray<float>& SnapshotDepthMeters, const FDateTime& TargetTime, const TCHAR* SourceLabel);

	/** Captures the current per-cell snow depth (meters) and stores it under TargetTime so a later
	 *  standalone export can restore the physics state the sim had when it first crossed that date.
	 *  Uses SnowSim->DepthMeters when available, otherwise falls back to CpuDepthMeters.
	 *  Also persists the snapshot to disk so it survives editor session restarts. */
	void CacheDepthSnapshotForTargetTime(const FDateTime& TargetTime);

	/** Restores the cached per-cell snow depth for TargetTime into the CPU and rendered snow state.
	 *  Checks in-memory cache first; falls back to the disk snapshot written
	 *  by CacheDepthSnapshotForTargetTime. Returns true if a snapshot was found and applied. */
	bool RestoreDepthSnapshotForTargetTime(const FDateTime& TargetTime);

	/** Returns the deterministic file path used to persist a depth snapshot for the given target time. */
	FString GetDepthSnapshotCachePath(const FDateTime& TargetTime) const;

	/** Called by simulations to update the CPU albedo buffer (0-1 range). */
	void UpdateCpuAlbedo(const TArray<float>& InAlbedo);

	/** Returns the weather provider currently supplying forcing data (component takes priority). */
	USimulationWeatherDataProviderBase* GetActiveWeatherProvider() const;
	USimulationWeatherDataProviderBase* EnsureActiveWeatherProviderReady();

	/** Returns the timestamp (UTC) that should be used when querying weather providers. */
	FDateTime GetWeatherQueryTimeUtc() const;

	// === Radiation Texture Accessors (for Simulation and Materials) ===

	/**
	 * Returns raw total radiation index texture (dimensionless, ~0-2).
	 * Only available when bEnableRawRadiationIndexTexture is enabled.
	 * Pure geometry-only index, independent of meteorological forcing.
	 *
	 * Usage: Material parameter, simulation sampling
	 * Values: 0.0 (shadow) to 2.0+ (enhanced by reflections)
	 * @return RTF_rUE_raw render target or nullptr when disabled/uninitialized
	 */
	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Radiation")
	UTextureRenderTarget2D* GetRawRadiationIndexTexture() const { return RTF_rUE_raw; }

	/** Returns the scene capture component used for radiation captures. */
	USceneCaptureComponent2D* GetRadiationCaptureComponent() const { return RadiationCapture; }
	/** Internal: toggles lambertian capture show-flag override for radiation passes. */
	void SetLambertianCaptureShowFlagOverride(bool bEnable) { bLambertianCaptureShowFlagOverride = bEnable; }

	/**
	 * Returns whether valid radiation indices are available.
	 * Radiation indices are available after radiation capture has been performed.
	 *
	 * @return true if radiation indices have been computed and cached
	 */
	bool HasValidRadiationIndices() const
	{
		if (!bEnableRadiationCapture || CachedRadiationIndices.Num() == 0)
		{
			return false;
		}

		return !bUseReferenceTile || bReferenceLuminanceValid;
	}

	float GetMaxRadiationIndexClamp() const { return FMath::Max(MaxRadiationIndexClamp, 0.1f); }
	float GetCachedCaptureCosSolarZenith() const { return CachedCaptureCosSolarZenith; }
	float GetCachedSunVisibility() const { return CachedSunVisibility; }
	float GetCachedClearSkyReference_Wm2() const { return CachedClearSkyReference_Wm2; }
	void SetCachedClearSkyReference_Wm2(float Value) { CachedClearSkyReference_Wm2 = Value; }



	/**
	 * Returns cached radiation indices from the raw radiation texture.
	 *
	 * @return Reference to array of radiation index values (dimensionless, ~0-2)
	 */
	const TArray<float>& GetCachedRadiationIndices() const { return CachedRadiationIndices; }

	/** Returns the cached Direct radiation indices (e.g. RTY_Direct or Irradiance). */
	const TArray<float>& GetCachedDirectIndices() const { return CachedDirectIndices; }

	/** Returns the cached Diffuse radiation indices (e.g. RTY_Diffuse or Irradiance). */
	const TArray<float>& GetCachedDiffuseIndices() const { return CachedDiffuseIndices; }

	/** Returns the current simulation cell spacing in meters. */
	float GetMetersPerCell() const { return MetersPerCell; }

	/** Returns cached luminance from RTY_Total. */
	const TArray<float>& GetCachedTotalRTY() const { return CachedRTY_Total; }

	/** Returns cached luminance from RTY_Direct. */
	const TArray<float>& GetCachedDirectRTY() const { return CachedRTY_Direct; }

	/** Returns cached luminance from RTY_Diffuse. */
	const TArray<float>& GetCachedDiffuseRTY() const { return CachedRTY_Diffuse; }

	/** Returns cached luminance from RTY_DiffuseNoGI (sky-only diffuse). */
	const TArray<float>& GetCachedDiffuseNoGIRTY() const { return CachedRTY_DiffuseNoGI; }

	/** Returns cached luminance from RTY_TotalNoGI (sun + sky, no GI/reflections). */
	const TArray<float>& GetCachedTotalNoGIRTY() const { return CachedRTY_TotalNoGI; }

	/** Returns cached render-side surface-state values used by dual-reference selection. */
	const TArray<float>& GetCachedSurfaceStateRTY() const { return CachedRTY_SurfaceState; }

	/** Returns cached terrain residual luminance (RTY_Terrain) used for interreflection diagnostics. */
	const TArray<float>& GetCachedTerrainResidualRTY() const { return CachedRTY_Terrain; }

	/** Returns the most recently exported or computed geometric sky-view-factor map. */
	const TArray<float>& GetCachedSVFMap() const { return CachedSVF_UE_Map; }

private:
	friend class UDegreeDaySimulation;

	/** The current step of the simulation (in hours). */
	int32 CurrentSimulationStep = 0;

	/** Visual accumulator for PIE stepping (seconds). */
	float VisualAccumulator = 0.0f;

	/** Accumulates simulated seconds until a weather step (TimeStepSeconds) is executed. */
	float SimulatedSecondsAccumulator = 0.0f;

	/** Minimum and maximum snow water equivalent (SWE) of the landscape. */
	float MinSWE, MaxSWE;

	/** Max snow from the initial conditions. */
	float InitialMaxSnow;

	/** Landscape cells. */
	TArray<FLandscapeCell> LandscapeCells;

	/** Cells for debugging. */
	TArray<FDebugCell> DebugCells;

	/** Structure to cache detailed simulation data for debug visualization */
	struct FCellDebugData
	{
		float NetSurfaceFlux_Wm2 = 0.0f;
		float NetShortwave_Wm2 = 0.0f;
		float NetLongwave_Wm2 = 0.0f;
		float SensibleHeat_Wm2 = 0.0f;
		float LatentHeat_Wm2 = 0.0f;
		float GroundHeatFlux_Wm2 = 0.0f;
		float SurfaceTemperatureK = 0.0f;
		float SnowLayer0TempK = 0.0f;
		float SnowLayer1TempK = 0.0f;
		float SnowLayer2TempK = 0.0f;
		float SoilLayer0TempK = 0.0f;
		float SoilLayer1TempK = 0.0f;
		float MeltMass_kgm2 = 0.0f;
		float RefreezeMass_kgm2 = 0.0f;
		float SublimationMass_kgm2 = 0.0f;
		float Runoff_kgm2 = 0.0f;
		float RadiationIndex = 1.0f;
		float DiffuseSW_Wm2 = 0.0f;
		float DirectSW_Wm2 = 0.0f;
		float SnowAlbedo = 0.0f;
		float SnowDensity_kgm3 = 0.0f;
		int32 SnowLayerCount = 0;
		float TimeStep_Hours = 1.0f; // For rate conversions
	};

	/** Cached debug data per cell (updated each simulation step) */
	TArray<FCellDebugData> CellDebugData;

	/** Slope of the terrain. */
	UTexture2D* SlopeTexture;

	/** CPU-side snow depth buffer in meters (R16F layout). */
	TArray<FFloat16> CpuDepthMeters;

	/** Per-target-date snapshots of the full-precision depth array captured when the sim tick
	 *  first crossed each entry in ExportTargetTimesteps. Enables the standalone radiation export
	 *  to restore the correct physics state for each target date. */
	TMap<FDateTime, TArray<float>> DepthSnapshotsByTargetTime;

	/** CPU-side albedo buffer (0-1 range, R16F layout). */
	TArray<FFloat16> CpuAlbedo;

	/** Melt-out Day of Year tracking (initialized to -1). */
	TArray<int16> MeltoutDOY;

	/** Tracks whether a cell has ever contained snow above the melt-out threshold this season. */
	TArray<uint8> MeltoutHadSnow;

	/** Melt-out Quality Control flags (0=unset, 1=good). */
	TArray<uint8> MeltoutQC;

	/** Per-cell counter of consecutive ticks with depth >= MeltoutReArmDepthThreshold.
	 * Used by the re-arm hysteresis to ignore transient late-season snowfall events. */
	TArray<int16> MeltoutAboveThresholdStreak;

	/** Resets melt-out tracking arrays for a new season. */
	void ResetMeltoutStateForNewSeason();

	/** Helper to compute Day of Year from DateTime. */
	int32 GetDayOfYear(const FDateTime& DateTime);

	/** Flag to track if melt-out has been exported for the current season. */
	bool bHasExportedMeltout = false;
	bool bAutoStopTriggered = false;

	void RequestEndPlay();

public:
	/** Updates the MeltoutDOYTexture with current melt-out data. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Snow Simulation | Actions")
	void UpdateMeltoutDOYTexture();

#if WITH_EDITOR
	/** Exports melt-out DOY data to disk as a 16-bit PNG/EXR with JSON metadata. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Snow Simulation | Actions")
	void ExportMeltoutToDisk(FString Filename = TEXT("MeltoutDOY"), bool bRotate90 = false);

	/** Exports dated SWdir/SunMask geometry maps for every timestamp listed in ExportTargetTimesteps without running the snow simulation. */
	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void ExportSWdirMapsForTargetTimesteps();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Export SWdir UE Maps For Target Timesteps"))
	void ExportSWdirMapsForTargetTimestepsBtn() { ExportSWdirMapsForTargetTimesteps(); }

	/** Runs the dedicated radiation map-export action for the selected target timesteps and selected product toggles. */
	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void RunRadiationMapExport();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Radiation | Map Export", meta=(DisplayName="Run Radiation Map Export"))
	void RunRadiationMapExportBtn() { RunRadiationMapExport(); }

	/** Exports below-canopy probe and/or scene-bounce maps for every timestamp listed in ExportTargetTimesteps without running the snow simulation. */
	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void ExportBelowCanopyMapsForTargetTimesteps();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Export Below-Canopy Maps For Target Timesteps"))
	void ExportBelowCanopyMapsForTargetTimestepsBtn() { ExportBelowCanopyMapsForTargetTimesteps(); }

	/** Exports a selected-probe below-canopy hemispherical image for every timestamp listed in ExportTargetTimesteps without running the snow simulation. */
	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void ExportBelowCanopyProbeHemisphereForTargetTimesteps();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Export Below-Canopy Probe Hemisphere For Target Timesteps"))
	void ExportBelowCanopyProbeHemisphereForTargetTimestepsBtn() { ExportBelowCanopyProbeHemisphereForTargetTimesteps(); }

	/** Re-run the below-canopy plotting script for the latest exported below-canopy run without re-exporting maps. */
	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void PlotLatestBelowCanopyRun();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Plot Latest Below-Canopy Run"))
	void PlotLatestBelowCanopyRunBtn() { PlotLatestBelowCanopyRun(); }

	/** Exports Slope and Aspect data to disk as 16-bit PNG/EXR. */
	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void ExportTerrainDerivatives(FString Filename = TEXT("Terrain"), bool bRotate90 = true);

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Export Terrain Derivatives"))
	void ExportTerrainDerivativesBtn() { ExportTerrainDerivatives(); }

	/** Exports geometric comparison maps derived from per-cell tracing (SVF, SWdir, SunMask, slope factor). */
	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void ExportSVFMap(FString Filename = TEXT("SVF_UE"), bool bRotate90 = true);

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Export SVF/SWdir UE Maps"))
	void ExportSVFMapBtn() { ExportSVFMap(); }

	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Radiation | SkyLight Calibration")
	void RunSkyLightCalibration();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Radiation | SkyLight Calibration", meta=(DisplayName="Run SkyLight Calibration"))
	void RunSkyLightCalibrationBtn() { RunSkyLightCalibration(); }

	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void RunRadiationBenchmark();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Radiation | Benchmark", meta=(DisplayName="Run Radiation Benchmark"))
	void RunRadiationBenchmarkBtn() { RunRadiationBenchmark(); }

	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void RunRadiationBenchmarkSVFValidation();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Radiation | Benchmark | SVF Validation", meta=(DisplayName="Run Cavity SVF Validation"))
	void RunRadiationBenchmarkSVFValidationBtn() { RunRadiationBenchmarkSVFValidation(); }

	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void RunRadiationBenchmarkCapture();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Run Radiation Benchmark Capture"))
	void RunRadiationBenchmarkCaptureBtn() { RunRadiationBenchmarkCapture(); }

	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void RunRadiationBenchmarkSkyCalibration();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Run Radiation Benchmark Sky Calibration"))
	void RunRadiationBenchmarkSkyCalibrationBtn() { RunRadiationBenchmarkSkyCalibration(); }

	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void RunRadiationBenchmarkHemisphereCapture();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Run Radiation Benchmark Hemisphere Capture"))
	void RunRadiationBenchmarkHemisphereCaptureBtn() { RunRadiationBenchmarkHemisphereCapture(); }

	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void RunRadiationBenchmarkHemisphereCalibration();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Run Radiation Benchmark Hemisphere Validation"))
	void RunRadiationBenchmarkHemisphereCalibrationBtn() { RunRadiationBenchmarkHemisphereCalibration(); }

	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void RunRadiationBenchmarkProbeLatticeCalibration();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Run Radiation Benchmark Probe-Lattice Calibration"))
	void RunRadiationBenchmarkProbeLatticeCalibrationBtn() { RunRadiationBenchmarkProbeLatticeCalibration(); }

	UFUNCTION(BlueprintCallable, Category = "Snow Simulation | Diagnostics")
	void RunRadiationBenchmarkProbeLatticeAlbedoSweep();

	UFUNCTION(CallInEditor, Category = "Snow Simulation | Actions", meta=(DisplayName="Run Probe-Lattice Albedo Sweep"))
	void RunRadiationBenchmarkProbeLatticeAlbedoSweepBtn() { RunRadiationBenchmarkProbeLatticeAlbedoSweep(); }
#endif

private:
	/** Applies actor-level diagnostics settings to a simulation config/runtime instance. */
	void ApplyUnifiedDiagnosticsSettings(USimulationBase* TargetSimulation, const FString& DiagnosticsDirectoryPath) const;

	/** Applies actor-level terrain redistribution settings to a simulation config/runtime instance. */
	void ApplyUnifiedTerrainRedistributionSettings(USimulationBase* TargetSimulation) const;

	/** Pushes DiagnosticsTrackedCellIndices from the actor to runtime/config simulations. */
	void SyncTrackedCellsToSimulations();

	/** Optional debug MID handle. Landscape writes now go through ALandscapeProxy parameter setters and leave this null. */
	UPROPERTY(Transient)
	UMaterialInstanceDynamic* SnowMID = nullptr;

	/** Tracks the last runtime albedo texture object pushed into the VHM MID so we can refresh render state on swaps. */
	UPROPERTY(Transient)
	UTexture2D* LastAppliedVHMAlbedoTexture = nullptr;

	/** One-shot diagnostics for the first albedo upload/bind in PIE. */
	UPROPERTY(Transient)
	bool bLoggedInitialAlbedoUpload = false;

	UPROPERTY(Transient)
	bool bLoggedInitialVHMAlbedoBinding = false;

	/** Cell size in meters (world). */
	float MetersPerCell = 0.0f;

	/** True when editor Start/End times are authored in local time and must be converted to UTC for forcing. */
	bool bSimulationTimesRepresentLocal = false;

	/** Cached offset (Local = UTC + Offset). When active, weather providers are queried with Time - Offset. */
	FTimespan SimulationLocalToUtcOffset = FTimespan::Zero();

	/** Flag to track if material validation passed - prevents tick if false. */
	bool bMaterialValidationPassed = false;

	/** Cached capture LOD settings so render state is dirtied only when the lock configuration changes. */
	bool bRadiationCaptureLODSettingsApplied = false;
	float AppliedRadiationCaptureLODDistanceFactor = -1.0f;
	bool bAppliedForceLandscapeLOD0ForCapture = false;
	bool bAppliedForceVHMLodForCapture = false;
	float AppliedCaptureVHMLod0ScreenSize = -1.0f;
	int32 AppliedCaptureVHMNumForceLoadLods = INDEX_NONE;
	bool bAppliedEnableVHMViewLodFactorForCapture = false;
	TWeakObjectPtr<ALandscape> AppliedRadiationCaptureLODLandscape;

	/** The landscape of the world. */
	ALandscape* Landscape;

	struct FTimeIntegrationConfig
	{
		bool bEnabled = false;
		int32 SampleCount = 1;
		double SampleIntervalSeconds = 0.0;
		double EffectiveStepSeconds = 0.0;
		FDateTime StepStartTime;
	};

	FTimeIntegrationConfig BuildTimeIntegrationConfig(const FDateTime& SimTime, const FTimespan& StepDuration) const;
	bool ShouldUseDedicatedRadiationSkyLightForCapture() const;
	UTextureCube* ResolveRadiationSkyCubemapForCapture() const;
	void EnsureRadiationSkyLightComponent();
	void ConfigureRadiationSkyLightComponent();
	void EnsureTimeIntegrationTargets(int32 Width, int32 Height);
	void ResetTimeIntegrationTargets();
	void AccumulateLuminanceTexture(UTextureRenderTarget2D* Source, UTextureRenderTarget2D* Accumulator, float SampleWeight);
	void CopyIntegrationResult(UTextureRenderTarget2D* Source, UTextureRenderTarget2D* Dest);
	void ClearRenderTargetTexture(UTextureRenderTarget2D* Target);
	void ClearNighttimeRadiationState();

	/**
	* Updates the material with data from the simulation.
	*/
	void UpdateMaterialTexture();

	/** Performs a simulation substep; advances when accumulated seconds >= TimeStepSeconds. */
	void StepSimulation(float dtSeconds);

	/**
	* Sets up VHM integration if VHM actor is found.
	*/
	void SetupVHMIntegration();

	/**
	* Sets up VHM material parameters with given origin and size.
	*/
	void SetupVHMMaterialParameters(UPrimitiveComponent* PrimComponent, AActor* FoundActor, const FVector2D& OriginMeters, const FVector2D& SizeMeters);

	/**
	* Sets up landscape binding as fallback when VHM is not available.
	*/
	void SetupLandscapeBinding();

	/** Per-target parameter mapping to preserve VHM-specific vs Landscape-specific mappings */
	struct FRenderBinding
	{
		enum class ETarget { VHM, Landscape };
		ETarget Target;
		FVector2D OriginMeters;
		FVector2D SizeMeters;
		FVector2D InvSizePerMeter;
		TWeakObjectPtr<UPrimitiveComponent> PrimaryComponent;
		bool bInitialized = false;
	};

	/**
	* Applies snow parameters using the stored render binding.
	*/
	void ApplySnowParams(const FRenderBinding& Binding, UTexture2D* SnowTex2D);

private:
	/** Material instance for VHM. */
	UMaterialInstanceDynamic* VHMMaterialInstance;

	/** Retry counter for VHM bounds not ready. */
	int32 VHMBoundsRetryCount = 0;

	FRenderBinding ActiveRenderBinding;

	// === Radiation Capture Components ===

	/** Orthographic scene capture for irradiance atlas rendering */
	UPROPERTY(Transient)
	USceneCaptureComponent2D* RadiationCapture = nullptr;

	/** Reference plane mesh component for unoccluded radiation measurement.
	 * Positioned outside grid bounds, renders as pure white Lambertian surface.
	 * Captured in bottom border row (y=0) of render targets. */
	UPROPERTY(Transient)
	UStaticMeshComponent* ReferencePlaneMesh = nullptr;

	/** Secondary reference plane mesh for dual-strip mode (snow material). */
	UPROPERTY(Transient)
	UStaticMeshComponent* ReferencePlaneMeshSnow = nullptr;

	/** Column meshes used for alternating dual-strip mode. */
	UPROPERTY(Transient)
	TArray<UStaticMeshComponent*> ReferenceStripColumnMeshes;

	/** Mesh asset used to create the reference plane for irradiance capture */
	UPROPERTY(EditDefaultsOnly, Category = "Snow Simulation | Radiation | Capture | Reference")
	TSoftObjectPtr<UStaticMesh> ReferencePlaneMeshAsset;

	/** Cached ShowFlags for TOTAL pass (all lighting enabled) */
	FEngineShowFlags CachedShowFlags_Total = FEngineShowFlags(EShowFlagInitMode::ESFIM_Game);

	/** Cached ShowFlags for DIRECT-ONLY pass (only directional light + shadows) */
	FEngineShowFlags CachedShowFlags_Direct = FEngineShowFlags(EShowFlagInitMode::ESFIM_Game);

	/** Cached ShowFlags for DIFFUSE-ONLY pass (sky + GI, no direct sun) */
	FEngineShowFlags CachedShowFlags_Diffuse = FEngineShowFlags(EShowFlagInitMode::ESFIM_Game);

	/** Cached ShowFlags for SKY-ONLY pass (sky only, no GI, no direct sun) */
	FEngineShowFlags CachedShowFlags_DiffuseNoGI = FEngineShowFlags(EShowFlagInitMode::ESFIM_Game);

	/** Cached ShowFlags for render-side surface-state capture. */
	FEngineShowFlags CachedShowFlags_SurfaceState = FEngineShowFlags(EShowFlagInitMode::ESFIM_Game);

	/** Apply Lambertian-friendly show flag overrides (used when no capture material is provided). */
	void ApplyLambertianShowFlagOverride(FEngineShowFlags& Flags) const;

	/** Applies a single atmosphere/fog policy across all radiation decomposition passes. */
	void ApplyAtmosphereShowFlagPolicy(FEngineShowFlags& Flags) const;

	/** True when Lambertian capture fallback should override show flags. */
	bool bLambertianCaptureShowFlagOverride = false;

	/** True while a benchmark capture should override SunSky/time-driven lighting with manual light direction and, when enabled, benchmark-driven irradiance. */
	bool bUseRadiationBenchmarkLightingOverride = false;

	/** True while a synthetic radiation benchmark is isolating itself from snow-model-specific logic and output tagging. */
	bool bRadiationBenchmarkCaptureActive = false;

	/** True only while a probe-lattice albedo-sweep child capture is running. */
	bool bRadiationBenchmarkProbeLatticeAlbedoSweepCaptureActive = false;

	/** True when the benchmark capture should preserve atmosphere-matched direct subtraction
	 * so DiffuseNoGI can be derived consistently as TotalNoGI - Direct for calibration. */
	bool bRadiationBenchmarkPreserveDirectAtmosphereForCalibration = false;

	/** True when the benchmark capture should stay in-memory and skip benchmark artifact exports. */
	bool bRadiationBenchmarkLightweightCapture = false;

	/** True when benchmark TotalNoGI/Direct passes should use strict semantics even if that differs from terrain exports. */
	bool bRadiationBenchmarkStrictNoGIForBenchmark = false;

	/** True when probe-lattice benchmark/calibration should build and export a fresh benchmark SVF field.
	 * Benchmarks normally skip this and rely on the separate SVF validation path instead. */
	bool bRadiationBenchmarkProbeLatticeBuildFreshSVF = false;

	float RadiationBenchmarkOverrideSunElevationDeg = 0.0f;
	float RadiationBenchmarkOverrideSunAzimuthDeg = 180.0f;
	float RadiationBenchmarkOverrideDNI_Wm2 = 0.0f;
	float RadiationBenchmarkOverrideDHI_Wm2 = 0.0f;

	/**
	* Sets up radiation capture components and render targets.
	*/
	void SetupRadiationCapture();

	/** Applies persistent high-detail LOD settings for capture once configuration exists. */
	void ApplyPersistentRadiationCaptureLODSettings();

	/**
	* Updates radiation captures with dual-pass rendering.
	* Supports time-integration.
	* @param SimTime Current simulation date/time (end of step)
	* @param StepDuration Duration of the simulation step (for sub-stepping)
	*/
	void TickRadiationCaptures(const FDateTime& SimTime, const FTimespan& StepDuration);

	/**
	* Configures ShowFlags for TOTAL irradiance pass (all lighting).
	*/
	void ConfigureShowFlags_Total(FEngineShowFlags& Flags);

	/**
	* Configures ShowFlags for DIRECT-ONLY irradiance pass (directional light only).
	* The optional legacy reference-tile atmosphere match keeps direct-pass atmosphere/fog
	* enabled when strip-based calibration is active; hemisphere benchmark captures should
	* generally disable that behavior so Direct remains a pure sun term.
	*/
	void ConfigureShowFlags_Direct(FEngineShowFlags& Flags, bool bAllowReferenceTileAtmosphereMatch = true);

	/**
	* Configures ShowFlags for DIFFUSE-ONLY irradiance pass (sky + GI, no direct sun).
	*/
	void ConfigureShowFlags_Diffuse(FEngineShowFlags& Flags);

	/**
	* Configures ShowFlags for SKY-ONLY irradiance pass (sky only, no GI, no direct sun).
	*/
	void ConfigureShowFlags_DiffuseNoGI(FEngineShowFlags& Flags);

	/**
	* Configures ShowFlags for render-side surface-state capture (material/base color only).
	*/
	void ConfigureShowFlags_SurfaceState(FEngineShowFlags& Flags);

	struct FBelowCanopyHemisphereFaceCapture
	{
		FString Label;
		FVector FaceForward = FVector::ForwardVector;
		FVector FaceUp = FVector::UpVector;
		TArray<FLinearColor> Pixels;
	};

	struct FBelowCanopyHemispherePassCapture
	{
		TArray<FBelowCanopyHemisphereFaceCapture> FaceCaptures;
		TArray<float> FisheyeLuminance;
		float Irradiance = 0.0f;
	};

	struct FBelowCanopyHemisphereCaptureResult
	{
		int32 FaceResolution = 0;
		int32 FisheyeResolution = 0;
		FVector ProbeOrigin = FVector::ZeroVector;
		FVector ProbeUpAxis = FVector::UpVector;
		FVector ProbeForwardAxis = FVector::ForwardVector;
		FVector ProbeRightAxis = FVector::RightVector;
		FBelowCanopyHemispherePassCapture Total;
		FBelowCanopyHemispherePassCapture TotalNoGI;
		FBelowCanopyHemispherePassCapture Direct;
		TArray<float> DiffuseNoGIFisheyeLuminance;
		TArray<float> TerrainFisheyeLuminance;
		float DiffuseNoGIIrradiance = 0.0f;
		float TerrainIrradiance = 0.0f;
		float SkyFraction = 0.0f;          // cosine-weighted sky fraction (HPEval per-pixel threshold)
	};

	struct FRadiationBenchmarkRunResult
	{
		bool bSucceeded = false;
		int32 Iteration = 0;
		FString TargetDir;
		FString RunTag;
		FString SummaryCsvPath;
		FString PlotPath;
		FString DiagnosticsLabel = TEXT("unknown");
		float SkyLightIntensityMultiplierBefore = 0.0f;
		float RequestedDNI_Wm2 = 0.0f;
		float RequestedDHI_Wm2 = 0.0f;
		float ReferenceLuminance_Total = 0.0f;
		float ReferenceLuminance_DiffuseNoGI = 0.0f;
		float ReferenceLuminance_TotalNoGI = 0.0f;
		double EffectiveReferenceLuminance_DiffuseNoGI = 0.0;
		double MeanDiffuseNoGI = 0.0;
		double MeanTerrain = 0.0;
		double MeanTotal = 0.0;
		double MeanNormalizedDiffuseNoGI = 0.0;
		double MeanNormalizedTerrain = 0.0;
		double MeanNormalizedTotal = 0.0;
		double MeanSVF = 0.0;
		double StdSVF = 0.0;
		double MaeSVFToHalf = 0.0;
		double MeanSVFFullDomain = 0.0;
		double StdSVFFullDomain = 0.0;
		double MaeSVFToHalfFullDomain = 0.0;
		double ScaleDiffuseNoGICandidate = 0.0;
		bool bHasSkyOnlyField = false;
		bool bHasTotalNoGIField = false;
		bool bSkyOnlyCollapsed = false;
		bool bIndirectCollapsed = false;
		bool bUsedTotalNoGIAsSkyReferenceFallback = false;
		bool bUsedSVFInteriorMask = false;
		FString SVFAnalysisDomainLabel = TEXT("full_domain");
		FString SVFDefinitionLabel = TEXT("HorizontalHemisphere");
	};

	struct FRadiationBenchmarkHemisphereRunResult
	{
		bool bSucceeded = false;
		int32 Iteration = 0;
		FString TargetDir;
		FString RunTag;
		FString SummaryCsvPath;
		float RequestedDNI_Wm2 = 0.0f;
		float RequestedDHI_Wm2 = 0.0f;
		float TargetDirectHorizontalIrradiance = 0.0f;
		float TargetDiffuseHorizontalIrradiance = 0.0f;
		float TargetTotalNoGIIrradiance = 0.0f;
		float SunLuminousEfficacyBefore = 0.0f;
		float SkyLightIntensityMultiplierBefore = 0.0f;
		float ProbeDirectIrradiance = 0.0f;
		float ProbeDiffuseNoGIIrradiance = 0.0f;
		float ProbeTerrainIrradiance = 0.0f;
		float ProbeTotalNoGIIrradiance = 0.0f;
		float ProbeTotalIrradiance = 0.0f;
		float ReferenceDirectIrradiance = 0.0f;
		float ReferenceDiffuseNoGIIrradiance = 0.0f;
		float ReferenceTotalNoGIIrradiance = 0.0f;
		float ReferenceTotalIrradiance = 0.0f;
		float CalibrationReferenceDirect = 0.0f;
		FString CalibrationReferenceDirectSource = TEXT("OpenSkyHemisphereValidationOnly");
		double NormalizedDirect = 0.0;
		double NormalizedDiffuseNoGI = 0.0;
		double NormalizedTerrain = 0.0;
		double NormalizedTotal = 0.0;
	};

	struct FRadiationBenchmarkProbeLatticeRunResult
	{
		bool bSucceeded = false;
		int32 Iteration = 0;
		FString TargetDir;
		FString RunTag;
		FString SummaryCsvPath;
		float ReferenceDirectIrradiance = 0.0f;
		float ReferenceTotalIrradiance = 0.0f;
		float ReferenceTotalNoGIIrradiance = 0.0f;
		float RequestedDNI_Wm2 = 0.0f;
		float RequestedDHI_Wm2 = 0.0f;
		float SkyLightIntensityMultiplierBefore = 0.0f;
		float TargetDiffuseHorizontalIrradiance = 0.0f;
		float ReferencePlaneMeanLuminance = 0.0f;
		float ReferencePlaneIrradiance = 0.0f;
		double ScaleDiffuseNoGICandidate = 0.0;
		double MeanCapturedTotalRTY = 0.0;
		double MeanCapturedTotalNoGIRTY = 0.0;
		double MeanSceneBounceRTY = 0.0;
		double MeanTotalWithSceneBounceRTY = 0.0;
		double MeanCapturedTotalLowMaterialRTY = 0.0;
		double MeanHighMinusLowMaterialDeltaRTY = 0.0;
		double MeanResidualTerrainFraction = 0.0;
		double MeanMaterialPairTerrainFraction = 0.0;
		double MeanNormalizedTotalRTY = 0.0;
		double MeanNormalizedTotalNoGIRTY = 0.0;
		double MeanCapturedTotalNoGI = 0.0;
		double MeanSceneBounce = 0.0;
		double P95SceneBounce = 0.0;
		double P99SceneBounce = 0.0;
		double MaxSceneBounce = 0.0;
		double MeanTotalWithSceneBounce = 0.0;
		bool bUsedMaterialPair = false;
		double MeanCapturedTotalLowMaterial = 0.0;
		double MeanHighMinusLowMaterialDelta = 0.0;
		double AnalyticalTargetDiffuseNoGI = 0.0;
		double AnalyticalTargetTerrain = 0.0;
		double AnalyticalTargetTotal = 0.0;
		double MeanNormalizedDiffuseNoGI = 0.0;
		double MeanNormalizedTerrain = 0.0;
		double MeanNormalizedTotal = 0.0;
		double RelativeErrorDiffuseNoGI = 0.0;
		double RelativeErrorTerrain = 0.0;
		double RelativeErrorTotal = 0.0;
		double MeanSVF = 0.0;
		double StdSVF = 0.0;
		double MaeSVFToHalf = 0.0;
		bool bUsedSVFInteriorMask = false;
		FString SVFAnalysisDomainLabel = TEXT("full_domain");
		FString SVFDefinitionLabel = TEXT("HorizontalHemisphere");
		int32 EffectiveProbeSpacingCells = 0;
		int32 SparseProbeCountX = 0;
		int32 SparseProbeCountY = 0;
		FString SkyLightingPathLabel = TEXT("DedicatedBenchmarkSkyLight");
		int32 DiagnosticProbeCellX = INDEX_NONE;
		int32 DiagnosticProbeCellY = INDEX_NONE;
		bool bDiagnosticProbeCaptureSucceeded = false;
		FString DiagnosticProbeFailureReason;
		double DiagnosticProbeDirect = 0.0;
		double DiagnosticProbeDiffuseNoGI = 0.0;
		double DiagnosticProbeTerrain = 0.0;
		double DiagnosticProbeTotalNoGI = 0.0;
		double DiagnosticProbeTotal = 0.0;
		double DiagnosticProbeTotalLowMaterial = 0.0;
		double DiagnosticProbeHighMinusLowMaterialDelta = 0.0;
		int32 NearestSparseSampleCellX = INDEX_NONE;
		int32 NearestSparseSampleCellY = INDEX_NONE;
		bool bNearestSparseSampleCaptured = false;
		double NearestSparseSampleDirect = 0.0;
		double NearestSparseSampleDiffuseNoGI = 0.0;
		double NearestSparseSampleTerrain = 0.0;
		double NearestSparseSampleTotalNoGI = 0.0;
		double NearestSparseSampleTotal = 0.0;
		double NearestSparseSampleTotalLowMaterial = 0.0;
		double NearestSparseSampleHighMinusLowMaterialDelta = 0.0;
	};

	struct FRadiationBenchmarkProbeLatticeAlbedoSweepSample
	{
		int32 SweepIndex = INDEX_NONE;
		int32 SourceIndex = INDEX_NONE;
		float Albedo = 0.0f;
		FString Label;
		FString MaterialPath;
		FRadiationBenchmarkProbeLatticeRunResult Result;
	};

	TArray<FRadiationBenchmarkProbeLatticeAlbedoSweepSample> CachedRadiationBenchmarkProbeLatticeAlbedoSweepSamples;

	void ApplyRadiationBenchmarkSunOverride();
	bool CollectRadiationBenchmarkSurfaceComponents(TArray<UPrimitiveComponent*>& OutComponents) const;
	bool CollectBelowCanopyRenderSurfaceComponents(TArray<UPrimitiveComponent*>& OutComponents, bool bIncludeLandscapeLeakProtection) const;
	bool CaptureBelowCanopyHemisphereAtProbeOrigin(
		const FVector& ProbeOrigin,
		const FVector& ProbeNormal,
		int32 FaceResolution,
		int32 FisheyeResolution,
		FBelowCanopyHemisphereCaptureResult& OutCaptureResult,
		FString& OutFailureReason,
		UTextureRenderTarget2D* ExternalProbeTarget = nullptr);
	bool WriteBelowCanopyProbeHemisphereProducts(
		const FDateTime& Time,
		const FString& TargetDir,
		const FString& RunTag,
		int32 ProbeCellX,
		int32 ProbeCellY,
		int32 ProbeCellIndex,
		const FBelowCanopyHemisphereCaptureResult& CaptureResult,
		bool bTotalOnly = false,
		float EffectiveProbeHeightCm = -1.0f);
	bool ExecuteRadiationBenchmarkCapture(FRadiationBenchmarkRunResult* OutResult = nullptr);
	bool ExecuteRadiationBenchmarkHemisphereCapture(FRadiationBenchmarkHemisphereRunResult* OutResult = nullptr);
	bool ExecuteRadiationBenchmarkProbeLatticeCapture(FRadiationBenchmarkProbeLatticeRunResult* OutResult = nullptr, const FString& RunTimestampTagOverride = FString());
	void PopulateRadiationBenchmarkRunResult(FRadiationBenchmarkRunResult& OutResult, const TArray<float>* SVFValues) const;
	bool WriteRadiationBenchmarkMetadata(const FString& TargetDir, const FString& RunTag, const FDateTime& CaptureTime) const;
	bool WriteRadiationBenchmarkSummary(const FString& TargetDir, const FString& RunTag, const FDateTime& CaptureTime, const TArray<float>* SVFValues, const FString& SVFBackendLabel, const FString& SVFBackendReason, FString& OutSummaryCsvPath) const;
	bool WriteRadiationBenchmarkHemisphereSummary(const FString& TargetDir, const FString& RunTag, const FDateTime& CaptureTime, const FRadiationBenchmarkHemisphereRunResult& Result, FString& OutSummaryCsvPath) const;
	bool WriteRadiationBenchmarkProbeLatticeSummary(const FString& TargetDir, const FString& RunTag, const FDateTime& CaptureTime, const FRadiationBenchmarkProbeLatticeRunResult& Result, FString& OutSummaryCsvPath) const;
	bool WriteRadiationBenchmarkProbeLatticeAlbedoSweepSummary(const FString& TargetDir, const FString& SweepTag, const FDateTime& CaptureTime, const TArray<FRadiationBenchmarkProbeLatticeAlbedoSweepSample>& Samples, FString& OutSummaryCsvPath) const;
	bool RunRadiationBenchmarkPlotScript(const FString& SummaryCsvPath, const FString& TargetDir, FString& OutPlotPath) const;
	bool WriteRadiationBenchmarkSkyCalibrationReport(const FString& TargetDir, const FString& ReportTag, const TArray<FRadiationBenchmarkRunResult>& IterationResults, bool bCalibrationSucceeded, const FString& FinalStatus, FString& OutReportPath) const;
	bool WriteRadiationBenchmarkHemisphereCalibrationReport(const FString& TargetDir, const FString& ReportTag, const TArray<FRadiationBenchmarkHemisphereRunResult>& IterationResults, bool bCalibrationSucceeded, const FString& FinalStatus, FString& OutReportPath) const;
	bool WriteRadiationBenchmarkProbeLatticeCalibrationReport(const FString& TargetDir, const FString& ReportTag, const TArray<FRadiationBenchmarkProbeLatticeRunResult>& IterationResults, bool bCalibrationSucceeded, const FString& FinalStatus, FString& OutReportPath) const;
	bool RunBelowCanopyPlotScript(const FString& MapsDir, FString& OutPlotPath) const;
	bool RunBelowCanopyProbeHemispherePlotScript(const FString& MapsDir, FString& OutPlotPath, const FString& SpecificRunTag = FString()) const;
	bool CaptureBenchmarkReferencePlaneIrradiance(UStaticMeshComponent* ReferencePlaneComponent, float ReferencePlaneAlbedo, float& OutMeanLuminance, float& OutIrradiance, FString& OutFailureReason);

	/**
	* Reads a single pixel from a render target and returns RGBA as FLinearColor.
	* Uses GPU readback for deterministic verification.
	* @param RT The render target to read from
	* @param X Pixel X coordinate
	* @param Y Pixel Y coordinate
	* @return Linear HDR color value at (X,Y), or black if failed
	*/
	FLinearColor ReadPixelFromRT(UTextureRenderTarget2D* RT, int32 X, int32 Y);

	/**
	* Debug utility: Verifies frame-to-frame consistency of radiation captures.
	* Reads the same pixel twice and compares bit-identical values.
	*/
	void DebugVerifyRadiationConsistency();

	/**
	* Converts grid cell coordinates (i,j) to world-space position.
	* Returns the center of the cell in world coordinates (cm).
	* @param CellX Grid cell index in X (0 to CellsDimensionX-1)
	* @param CellY Grid cell index in Y (0 to CellsDimensionY-1)
	* @return World position (cm) of cell center
	*/
	FVector GridCellToWorldPosition(int32 CellX, int32 CellY) const;

	/**
	* Converts world-space position to grid cell coordinates (i,j).
	* Returns fractional coordinates (use FMath::FloorToInt for indices).
	* @param WorldPosition World position in cm
	* @return Fractional grid coordinates (X, Y)
	*/
	FVector2D WorldPositionToGridCell(const FVector& WorldPosition) const;

	/**
	* Runtime validation: Verifies that grid cell (i,j) maps to pixel (i,j).
	* Samples a test point and checks for correct alignment.
	* Logs warnings if mapping is incorrect.
	*/
	void ValidateGridToPixelMapping();

	/** Writes a CSV snapshot of strip/cell luminance values to validate reference-strip mapping. */
	void ExportReferenceMappingDebugSnapshot(const FDateTime& CaptureTime, const TCHAR* StageLabel = TEXT("PostReadback"), bool bForceRenderSync = false, bool bSampleSceneCaptureTargets = false);

	/**
	* Sets up the reference plane mesh for unoccluded radiation measurement.
	* Creates a small horizontal plane positioned to be captured in the y=0 border row.
	*/
	void SetupReferencePlane();

	/**
	* Samples reference tile luminance from both render targets and updates cached values.
	* Reads from y=0 border row (reference tile), averages multiple pixels for stability.
	* Updates ReferenceLuminance_Total and ReferenceLuminance_Direct.
	*/
	void UpdateReferenceLuminance(bool bQueueNewReadback = true, bool bBlockUntilReady = false);

	/**
	* Gets the current reference luminance for TOTAL pass.
	* @return Reference luminance (R+G+B)/3, or 1.0 if reference tile disabled
	*/
	float GetReferenceLuminance_Total() const { return bUseReferenceTile ? ReferenceLuminance_Total : 1.0f; }

	/**
	* Gets the current reference luminance for DIRECT-ONLY pass.
	* @return Reference luminance (R+G+B)/3, or 1.0 if reference tile disabled
	*/
	float GetReferenceLuminance_Direct() const { return bUseReferenceTile ? ReferenceLuminance_Direct : 1.0f; }

	/**
	* Gets the current reference luminance for DIFFUSE pass.
	* @return Reference luminance (luminance of RTY_Diffuse), or 1.0 if reference tile disabled
	*/
	float GetReferenceLuminance_Diffuse() const { return bUseReferenceTile ? ReferenceLuminance_Diffuse : 1.0f; }

	/**
	* Dispatches GPU compute shader to collapse RGB render targets to a scalar channel.
	* Converts RT_Total → RTY_Total and RT_Direct → RTY_Direct using equal tristimulus
	* weights ((R+G+B)/3). Uses RDG (Render Dependency Graph) for efficient GPU execution.
	*/
	void ConvertRGBToLuminance();

	/** Converts the render-side surface-state capture to a single-channel proxy texture. */
	void ConvertSurfaceStateToLuminance();

	/**
	* Dispatches GPU compute shader to calculate diffuse irradiance.
	* Computes RTY_Diffuse = max(RTY_Total - RTY_Direct, epsilon).
	* Optionally applies 3x3 Gaussian blur if bBlurDiffuse is enabled.
	* Avoids CPU stalls by executing entirely on GPU.
	*/
	void ComputeDiffuseIrradiance();

	/**
	* Computes dimensionless radiation index fields from luminance textures.
	* Calculates reference means (R_direct, R_diffuse) from y=0 border row (CPU).
	* Dispatches GPU compute shader to compute per-pixel indices:
	*   r_dir(x,y) = RTY_Direct(x,y) / (R_direct + epsilon)
	*   r_diff(x,y) = RTY_Diffuse(x,y) / (R_diffuse + epsilon)
	* Stores results in RTF_DirIndex and RTF_DiffIndex (R16f textures).
	* These split-index textures remain the legacy/diagnostic path; FSM2 flux-calibrated mode reconstructs from RTY captures directly.
	* Requires bUseReferenceTile to be enabled.
	*/
	void ComputeRadiationIndices();

	/**
	* Computes raw geometry-only radiation indices when enabled.
	* Creates RTF_rUE_raw = RTY_Total / R_total (pure UE engine-relative index).
	* These indices are dimensionless and independent of meteorological forcing.
	*/
	void ComputeRawRadiationIndices();

	/**
	* Computes calibrated physical irradiance fields when forcing data is available.
	*
	* Input: DNI, DHI, SolarZenith from weather forcing (FForcingRadiation struct)
	/** Initializes GPU readback resources for radiation extraction. */
	void InitializeRadiationExtraction();

	/** Dispatches compute shader to extract per-cell radiation indices into a GPU buffer. */
	void ExtractRadiationIndicesToBuffer();

	/** Extracts RTY_Direct and RTY_Diffuse luminance values per cell to GPU buffers */
	void ExtractRTYLuminanceToBuffers();

	/** Attempts to read back RTY luminance values from GPU into CachedRTY arrays */
	bool ReadbackRTYLuminance(bool bBlockUntilReady = false);

	/** Attempts to read back radiation indices from GPU into CachedRadiationIndices. */
	bool ReadbackRadiationIndices();

	/** Releases GPU readback resources for the radiation extraction system. */
	void CleanupRadiationExtraction();

	/** Cached pixel value from previous frame for consistency checking */
	FLinearColor PreviousDebugPixelValue;

	/** Frame counter for consistency verification */
	int32 DebugFrameCounter = 0;

	/** Cached radiation indices array for simulation access */
	TArray<float> CachedRadiationIndices;
	TArray<float> CachedDirectIndices;
	TArray<float> CachedDiffuseIndices;

	/** Cached RTY luminance values per cell for diagnostics */
	TArray<float> CachedRTY_Direct;
	TArray<float> CachedRTY_Diffuse;
	TArray<float> CachedRTY_Total;
	TArray<float> CachedRTY_DiffuseNoGI;
	TArray<float> CachedRTY_TotalNoGI;
	TArray<float> CachedRTY_SurfaceState;
	TArray<float> CachedRTY_Terrain;
	TArray<float> CachedSVF_UE_Map;
	TArray<float> CachedSunMask_UE_Map;
	TArray<float> CachedSWdir_UE_Map;

	/** Guard state to prevent redundant capture reinitialization when configuration is unchanged. */
	bool bRadiationCaptureInitialized = false;
	int32 CachedRadiationCaptureWidth = 0;
	int32 CachedRadiationCaptureHeight = 0;

	/** Countdown for per-frame Lumen priming passes before emitting the final capture. */
	int32 RadiationPrimingCooldown = 0;
	bool bRadiationPrimingActive = false;
	int64 LastRadiationPrimingFrame = INDEX_NONE;
	bool bRadiationPrimingSimTimeValid = false;
	FDateTime RadiationPrimingSimTime;
	FDateTime RadiationPrimingStartTime;
	bool bRadiationPrimingStartTimeValid = false;
	FDateTime LastRadiationCaptureSimTime;
	bool bLastRadiationCaptureTimeValid = false;
	bool bCachedUseReferenceTile = false;
	bool bUseDiffuseCaptureThisFrame = false;
	bool bUseDiffuseNoGICaptureThisFrame = false;

	/** GPU readback buffer used to copy radiation indices from GPU to CPU. */
	FRHIGPUBufferReadback* RadiationReadbackBuffer = nullptr;
	FRHIGPUBufferReadback* DirectIndexReadbackBuffer = nullptr;
	FRHIGPUBufferReadback* DiffuseIndexReadbackBuffer = nullptr;

	/** GPU readback helper for RTY_DiffuseNoGI. */
	FRHIGPUBufferReadback* RTYDiffuseNoGIReadback = nullptr;

	/** GPU readback helper for RTY_TotalNoGI. */
	FRHIGPUBufferReadback* RTYTotalNoGIReadback = nullptr;

	/** GPU readback helper for RTY_SurfaceState. */
	FRHIGPUBufferReadback* RTYSurfaceStateReadback = nullptr;

	/** True if the latest RTY extraction pass queued an RTY_DiffuseNoGI readback. */
	bool bRTYDiffuseNoGIReadbackQueued = false;

	/** True if the latest RTY extraction pass queued an RTY_TotalNoGI readback. */
	bool bRTYTotalNoGIReadbackQueued = false;

	/** True if the latest RTY extraction pass queued an RTY_SurfaceState readback. */
	bool bRTYSurfaceStateReadbackQueued = false;

	/** GPU readback helper for reference luminance (total channel). */
	FRHIGPUTextureReadback* ReferenceTotalReadback = nullptr;

	/** GPU readback helper for reference luminance (direct channel). */
	FRHIGPUTextureReadback* ReferenceDirectReadback = nullptr;

	/** GPU readback helper for reference luminance (diffuse channel). */
	FRHIGPUTextureReadback* ReferenceDiffuseReadback = nullptr;

	/** GPU readback helper for reference luminance (diffuse no-GI channel). */
	FRHIGPUTextureReadback* ReferenceDiffuseNoGIReadback = nullptr;

	/** GPU readback helper for reference luminance (TotalNoGI channel). */
	FRHIGPUTextureReadback* ReferenceTotalNoGIReadback = nullptr;

	/** GPU readback helper for reference render-side surface state. */
	FRHIGPUTextureReadback* ReferenceSurfaceStateReadback = nullptr;

	/** GPU readback buffers for RTY_Direct and RTY_Diffuse luminance values per cell */
	FRHIGPUBufferReadback* RTYDirectReadback = nullptr;
	FRHIGPUBufferReadback* RTYDiffuseReadback = nullptr;
	FRHIGPUBufferReadback* RTYTotalReadback = nullptr;

	/** Mapping from simulation cells to capture pixel coordinates (computed per configuration). */
	TArray<FIntPoint> RadiationCellPixelCoords;
	FBufferRHIRef RadiationPixelCoordBuffer;
	TRefCountPtr<FRHIShaderResourceView> RadiationPixelCoordSRV;

	/** Output file used by reference-strip mapping debug snapshots. */
	FString ReferenceMappingDebugCsvPath;

	/** Pooled buffer reference used during radiation extraction passes. */
	TRefCountPtr<FRDGPooledBuffer> RadiationIndexBuffer;

	/** Tracks pending reference readback operations. */
	bool bReferenceReadbackPending = false;

	/** True if a diffuse reference readback was queued for the pending readback. */
	bool bReferenceDiffuseReadbackQueued = false;

	/** True if a diffuse-noGI reference readback was queued for the pending readback. */
	bool bReferenceDiffuseNoGIReadbackQueued = false;

	/** True if a TotalNoGI reference readback was queued for the pending readback. */
	bool bReferenceTotalNoGIReadbackQueued = false;

	/** True if a surface-state reference readback was queued for the pending readback. */
	bool bReferenceSurfaceStateReadbackQueued = false;

	/** Width of the queued reference readback row. */
	int32 ReferenceReadbackWidth = 0;

	/** Cached top-edge reference-strip luminance rows from the latest successful reference readback. */
	TArray<float> CachedReferenceStripRTY_Total;
	TArray<float> CachedReferenceStripRTY_Direct;
	TArray<float> CachedReferenceStripRTY_Diffuse;
	TArray<float> CachedReferenceStripRTY_DiffuseNoGI;
	TArray<float> CachedReferenceStripRTY_TotalNoGI;
	int32 CachedReferenceStripWidth = 0;
	int32 CachedReferenceStripHeight = 0;

	struct FGeometryProductTiming
	{
		FString Product;
		FString TriggerTimestampIso;
		FString RequestedMode;
		FString BackendUsed;
		FString BackendReason;
		bool bSucceeded = false;
		bool bUsedGpu = false;
		bool bUsedFallback = false;
		int32 GridWidth = 0;
		int32 GridHeight = 0;
		int32 CellCount = 0;
		int32 SampleCount = 0;
		int32 BinarySearchIterations = 0;
		double TraceMaxDistanceMeters = 0.0;
		double TraceOriginLiftCm = 0.0;
		double ComputeSeconds = 0.0;
		double CellsPerSecond = 0.0;
	};

	FGeometryProductTiming LastSVFGeometryTiming;
	FGeometryProductTiming LastSWdirGeometryTiming;

	FString GetGeometryOccluderModeLabel(ESVFMapOccluderMode Mode) const;
	bool IsTerrainOnlyGeometryMode(ESVFMapOccluderMode Mode) const;
	bool IsRayTracingGPUOccluderMode(ESVFMapOccluderMode Mode) const;
	bool IsBelowCanopyProbeExportActive() const;
	bool CanUseGeometryRayTracingGPU(FString* OutReason = nullptr) const;
	void AddGeometryTimingToJson(TSharedPtr<FJsonObject> JsonObj, const FGeometryProductTiming& Timing) const;
	void AppendGeometryTimingMetadata(FString& Metadata, const FString& Prefix, const FGeometryProductTiming& Timing) const;
	bool BuildSVFMap(
		TArray<float>& OutSVFValues,
		TArray<float>* OutMeanHorizonDegrees = nullptr,
		FString* OutBackendLabel = nullptr,
		FString* OutBackendReason = nullptr,
		bool bUseSurfaceNormalHemisphere = false);
	bool BuildSVFMapCpu(
		TArray<float>& OutSVFValues,
		TArray<float>* OutMeanHorizonDegrees,
		ESVFMapOccluderMode Mode,
		bool bUseSurfaceNormalHemisphere = false);
	bool BuildSVFMapRayTracingGPU(
		TArray<float>& OutSVFValues,
		TArray<float>* OutMeanHorizonDegrees,
		FString* OutFailureReason = nullptr,
		bool bUseSurfaceNormalHemisphere = false);
	FCollisionQueryParams BuildSVFTraceParams(bool bTerrainOnly) const;
	bool IsLandscapeHitForSVF(const FHitResult& Hit) const;
	bool IsSkyRayVisible(UWorld* World,
		const FVector& Origin,
		const FVector& RayDirection,
		float TraceDistanceCm,
		const FCollisionQueryParams& TraceParams,
		bool bTerrainOnly) const;
	bool IsSVFRayVisible(UWorld* World,
		const FVector& Origin,
		const FVector& HorizontalDirection,
		const FVector& UpVector,
		float HorizontalDistanceCm,
		float ElevationRadians,
		const FCollisionQueryParams& TraceParams,
		bool bTerrainOnly) const;
	float ResolveSWdirCosZenith(const FVector& SunToLight) const;
	bool BuildSWdirMap(
		TArray<float>& OutSunMaskValues,
		TArray<float>& OutSWdirValues,
		TArray<float>* OutSlopeFactorValues = nullptr,
		FString* OutBackendLabel = nullptr,
		FString* OutBackendReason = nullptr);
	bool BuildSWdirMapCpu(
		TArray<float>& OutSunMaskValues,
		TArray<float>& OutSWdirValues,
		TArray<float>* OutSlopeFactorValues,
		ESVFMapOccluderMode Mode);
	bool BuildSWdirMapRayTracingGPU(
		TArray<float>& OutSunMaskValues,
		TArray<float>& OutSWdirValues,
		TArray<float>* OutSlopeFactorValues,
		FString* OutFailureReason = nullptr);
	bool ExportDirectGeometryMapsForRun(
		const FDateTime& Time,
		const FString& TargetDir,
		const FString& RunTag,
		FString* OutModeLabel = nullptr,
		float* OutCosZenith = nullptr,
		FString* OutBackendLabel = nullptr,
		FString* OutBackendReason = nullptr);
	bool ExportSVFGeometryMapForRun(
		const FDateTime& Time,
		const FString& TargetDir,
		const FString& RunTag,
		FString* OutBackendLabel = nullptr,
		FString* OutBackendReason = nullptr);
	bool ResolveBelowCanopyForcingForTime(
		const FDateTime& Time,
		float CosZenith,
		float& OutDirectHorizontalWm2,
		float& OutDiffuseHorizontalWm2,
		FBelowCanopyForcingProvenance* OutProvenance = nullptr);
	bool BuildBelowCanopyProbeMapsForTime(
		const FDateTime& Time,
		TArray<float>& OutDirectWm2,
		TArray<float>& OutDiffuseWm2,
		TArray<float>& OutTotalWm2,
		float* OutDirectHorizontalWm2 = nullptr,
		float* OutDiffuseHorizontalWm2 = nullptr,
		TArray<float>* OutSunMaskValues = nullptr,
		TArray<float>* OutSWdirValues = nullptr,
		TArray<float>* OutSVFValues = nullptr,
		TArray<float>* OutMeanHorizonDegrees = nullptr,
		FString* OutDirectBackendLabel = nullptr,
		FString* OutDirectBackendReason = nullptr,
		FString* OutDiffuseBackendLabel = nullptr,
		FString* OutDiffuseBackendReason = nullptr,
		FBelowCanopyForcingProvenance* OutForcingProvenance = nullptr);
	bool ExportBelowCanopyProbeMapsForRun(
		const FDateTime& Time,
		const FString& TargetDir,
		const FString& RunTag);
	bool ExportBelowCanopyProbeHemisphereForRun(
		const FDateTime& Time,
		const FString& TargetDir,
		const FString& RunTag);
	bool ExportBelowCanopySceneBounceMapsForRun(
		const FDateTime& Time,
		const FString& TargetDir,
		const FString& RunTag);
	bool ExportHPEvalMapsForRun(
		const FDateTime& Time,
		const FString& TargetDir,
		const FString& RunTag,
		int32 ProbeSpacingCells     = 0,   // 0 = use HPEvalProbeSpacingCells
		int32 FaceResolution        = 0,   // 0 = use HPEvalHemisphereCaptureResolution
		int32 FisheyeResolution     = 0);  // 0 = use HPEvalHemisphereFisheyeResolution
	bool RunHPEvalPlotScript(const FString& RunDir, FString& OutPlotPath) const;
	bool PrepareRadiationMapExportLightingForTime(
		const FDateTime& TargetTime,
		bool bNeedSceneCaptureLighting,
		float& OutCosZenith);

	/** Sun visibility mask captured when the current reference readback was enqueued. */
	float ReferenceReadbackSunVisibility = 1.0f;

	/** Cached cosine of the solar zenith angle used during the latest capture. */
	float CachedCaptureCosSolarZenith = 1.0f;

	/** Cached scalar (0-1) describing whether the sun contributes to UE radiation for the latest capture. */
	float CachedSunVisibility = 1.0f;

	/** Internal selective-export guard used by the dedicated radiation map-export action. */
	bool bRadiationMapExportSelectiveProbeMapActive = false;

	/** Internal selective-export toggle for below-canopy probe SWdir products. */
	bool bRadiationMapExportSelectiveProbeMapIncludeSWdir = true;

	/** Internal selective-export toggle for below-canopy probe SVF products. */
	bool bRadiationMapExportSelectiveProbeMapIncludeSVF = true;

	/** Tracks whether the configured component-based weather provider has been initialized outside BeginPlay. */
	TWeakObjectPtr<USimulationWeatherDataProviderBase> InitializedClimateDataComponent;

	/** Tracks whether the configured standalone weather provider has been initialized outside BeginPlay. */
	TWeakObjectPtr<USimulationWeatherDataProviderBase> InitializedWeatherProvider;

	/** Cached I_s (W/m²): Potential clear-sky direct radiation on horizontal surface.
	 * Used as normalization reference for the low sun angle fallback mode.
	 * Updated by DegreeDay simulation during Step(). */
	float CachedClearSkyReference_Wm2 = 0.0f;

	/** Frame counter used to avoid double extraction per frame. */
	int32 LastRadiationExtractionFrame = INDEX_NONE;

	/**
	* Renders appropriate debug information.
	*/
	void DoRenderGrid();

	/** Renders the debug information from the simulation. */
	void DoRenderDebugInformation();

	/** Updates the debug data cache from simulation output */
	void UpdateDebugDataCache();

	/** Returns true when the on-screen debug overlay should be active. */
	bool IsDebugOverlayActive() const;

#if WITH_SUNSKY_SUPPORT
	/** Updates all SunSky actors in the world to match the current simulation time. */
	void UpdateSunSkyActors();
#if WITH_SUNSKY_SUPPORT
	void UpdateSunSkyActorsAtTime(const FDateTime& Time);
#endif
#endif

	/** Logs min/max/mean of CpuDepthMeters. */
	void LogDepthStats();

	/** Uploads CpuDepthMeters to SnowDepthTexture as PF_R16F via render thread. */
	void UploadDepthToTexture();

	/** Uploads CpuAlbedo to SnowAlbedoTexture as PF_B8G8R8A8 grayscale in RGBA via render thread. */
	void UploadAlbedoToTexture();
	bool ShouldUploadAlbedoTexture() const;

	/** Samples the current cosine of the solar zenith from the active Sun directional light. */
	float SampleSunCosine() const;

	/** Converts a cosine of solar zenith into a 0-1 visibility scalar used for night clamping. */
	float EvaluateSunVisibility(float CosZenith) const;
	float GetSunVisibilityThresholdCos() const;

	/** Recomputes the per-cell -> capture pixel mapping for the current capture configuration. */
	void UpdateRadiationCellPixelMapping(float GridWidthWorld, float TotalHeightWorld, const FVector& CaptureCenterWorld, const FVector& CaptureLocation, const FRotator& CaptureRotation, int32 RTWidth, int32 RTHeight);

	/** Uploads the CPU-side pixel coordinate array to a GPU structured buffer. */
	void UploadRadiationCellPixelCoords();

	/** Releases GPU resources associated with the cell/pixel mapping. */
	void ReleaseRadiationPixelCoordResources();

	/**
	* Returns the cell at the given x and y position or a nullptr if the indices are out of bounds.
	*
	* @param X
	* @param Y
	* @return the cell at the given x and y position or a nullptr if the indices are out of bounds.
	*/
	FLandscapeCell* GetCellChecked(int X, int Y)
	{
		return GetCellChecked(X + Y * CellsDimensionX);
	}

	/**
	* Returns the cell at the given index or nullptr if the index is out of bounds.
	*
	* @param Index the index of the cell
	* @return the cell at the given index or nullptr if the index is out of bounds
	*/
	FLandscapeCell* GetCellChecked(int Index)
	{
		return (Index >= 0 && Index < LandscapeCells.Num()) ? &LandscapeCells[Index] : nullptr;
	}
};
