#pragma once

#include "SnowSimulation.h"
#include "Misc/DateTime.h"
#include "DegreeDaySimulation.generated.h"

class FSimulationComputeShader;
class FSnowPixelShader;
class UTextureRenderTarget2D;
struct FDebugCell;
struct FLandscapeCell;
struct FClimateData;
class UWorld;
class ASnowSimulationActor;

/**
 * Radiation forcing components derived from GHI and solar geometry.
 * All radiation values in W/mÂ².
 */
USTRUCT(BlueprintType)
struct SIMULATION_API FForcingRadiation
{
	GENERATED_BODY()

	/** Global Horizontal Irradiance (W/mÂ²) - total shortwave on horizontal surface */
	UPROPERTY(BlueprintReadWrite, Category = "Radiation")
	float GHI = 0.0f;

	/** Diffuse Horizontal Irradiance (W/mÂ²) - diffuse component on horizontal surface */
	UPROPERTY(BlueprintReadWrite, Category = "Radiation")
	float DHI = 0.0f;

	/** Direct Normal Irradiance (W/mÂ²) - direct beam normal to sun direction */
	UPROPERTY(BlueprintReadWrite, Category = "Radiation")
	float DNI = 0.0f;

	/** Diffuse fraction (0-1) - DHI/GHI ratio */
	UPROPERTY(BlueprintReadWrite, Category = "Radiation")
	float DiffuseFraction = 0.0f;

	/** Solar zenith angle (radians) - angle from vertical to sun */
	UPROPERTY(BlueprintReadWrite, Category = "Radiation")
	float SolarZenith_rad = 0.0f;

	/** Cosine of solar zenith angle (0-1) */
	UPROPERTY(BlueprintReadWrite, Category = "Radiation")
	float CosSolarZenith = 0.0f;

	FForcingRadiation() = default;

	FForcingRadiation(float InGHI, float InSolarZenith_rad, float InDiffuseFraction = -1.0f)
		: GHI(InGHI)
		, DiffuseFraction(InDiffuseFraction)
		, SolarZenith_rad(InSolarZenith_rad)
	{
		CosSolarZenith = FMath::Max(FMath::Cos(SolarZenith_rad), 1e-6f);
	}
};

/**
 * Method for calculating radiation index in melt calculations.
 */
UENUM(BlueprintType)
enum class ERadiationIndexMethod : uint8
{
	/** Swift's algorithm for solar radiation on mountain slopes (geometric, parametric) */
	Swift UMETA(DisplayName = "Swift (Geometric)"),

	/** Sample radiation from UE scene lighting (accounts for shadows, inter-reflections, GI) */
	UnrealEngine UMETA(DisplayName = "Unreal Engine (Scene-Based)")
};

/**
 * Melt formulation used by the DegreeDay simulation.
 */
UENUM(BlueprintType)
enum class EDegreeDayMeltModel : uint8
{
	/** Existing enhanced temperature-index formulation (kept for backward compatibility; hidden). */
	Enhanced UMETA(DisplayName = "Enhanced Temperature Index", Hidden),

	/** Hock (1999) Model 2: MF + a*I term driven by potential clear-sky direct radiation. */
	HockModel2 UMETA(DisplayName = "Hock Model 2 (MF + a*I)"),

	/** Hock (1999) Model 3 exact formulation using cloudiness ratio Gs/Is. */
	HockModel3Exact UMETA(DisplayName = "Hock Model 3 (Exact)"),

	/** Legacy UE+SWR entry mapped to Pellicciotti-style additive formulation (hidden to reduce clutter). */
	HockModel3_UE_SWR UMETA(DisplayName = "Pellicciotti Model D (Legacy UE+SWR)", Hidden),
	PellicciottiModelD UMETA(DisplayName = "Pellicciotti Model D"),
	PellicciottiModelD_RI UMETA(DisplayName = "Pellicciotti Model D (Lambertian)"),
	PellicciottiModelD_PBR UMETA(DisplayName = "Pellicciotti Model D (PBR Material)"),
	PellicciottiModelD_FluxCalibrated UMETA(DisplayName = "Pellicciotti Model D (Flux-Calibrated)")
};

/**
 * Degree-day snow simulation backed by a compute shader pipeline.
 */
UCLASS(Blueprintable, BlueprintType, EditInlineNew)
class SIMULATION_API UDegreeDaySimulation : public USnowSimulation
{
	GENERATED_BODY()

public:
	// Ensure grid/texture setup from the base simulation is preserved
	virtual void Initialize_Implementation(int32 GX, int32 GY, float CellM) override;

	/** Critical slope for snow deposition in degrees. BlÃ¶schl (1991) uses zero accumulation above 60Â°. */
	UPROPERTY()
	float SlopeThreshold = 60.0f;

	/** Temperature threshold below which all precipitation is snow (Â°C). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Configuration", DisplayName = "TSnow A")
	float TSnowA = 0.0f;

	/** Temperature threshold above which all precipitation is rain (Â°C). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Configuration", DisplayName = "TSnow B")
	float TSnowB = 2.0f;

	/** Threshold A air temperature above which some snow starts melting (Â°C). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Configuration", DisplayName = "TMelt A")
	float TMeltA = 0.0f;

	/** Threshold B air temperature above which all snow starts melting (Â°C). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Configuration", DisplayName = "TMelt B")
	float TMeltB = 2.0f;

	/** Fresh snow albedo (0-1). Reflectivity of newly fallen snow. Typical range: 0.7-0.9.
	 * Higher values = more reflection = less melt. Standard value is 0.8 for clean fresh snow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Albedo", DisplayName = "Fresh Snow Albedo")
	float FreshSnowAlbedo = 0.8f;

	/** Old/dirty snow albedo (0-1). Minimum albedo after aging and rain events. Typical range: 0.4-0.6.
	 * Lower values = less reflection = more melt. Snow albedo ages toward this value over time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Albedo", DisplayName = "Old Snow Albedo")
	float OldSnowAlbedo = 0.4f;

	/** Albedo aging rate (daysâ»Â¹). Controls how fast snow albedo decreases from fresh to old.
	 * Higher values = faster aging. Typical range: 0.1-0.3. k_e=0.2 means albedo halves in ~3.5 days. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Albedo", DisplayName = "k_e (Albedo Aging Rate)")
	float k_e = 0.2f;

	/** Degree-day factor: melt rate in mm/(day*C). Controls how fast snow melts per degree above TMelt A.
	 * Set to 32.0 following Dunn et al. (1999) Model E calibration (range 29-35 mm/day/C).
	 * Eggleston et al (1971) suggests 0.4-0.6 inch/(day*F) = 18.288-27.432 mm/day/C for k_m. Closer to Dunn et al. (1999) than B.Neukom's 5.0 mm/day/C.
	 * This enhanced temperature-index model includes RadiationIndex, albedo, and rain-on-snow melt.
	 * Reference: Dunn & Colohan (1999) https://doi.org/10.1016/S0022-1694(99)00095-5 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Configuration", DisplayName = "k_m")
	float k_m = 32.0f;

	/** Melt formulation selector (Enhanced vs Hock models). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Configuration")
	EDegreeDayMeltModel DegreeDayMeltModel = EDegreeDayMeltModel::PellicciottiModelD;

	/** Threshold temperature (Â°C) used by the Hock models for the positive temperature term (T_pos). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Hock1999", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::HockModel2 || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3Exact"))
	float HockMeltThresholdC = 0.0f;

	/** Base melt factor MF (mm dâ»Â¹ Â°Câ»Â¹) for snow in Hock Model 2/3.
	 * Hock (1999) Table 1: Model 2 = 1.8, Model 3 = 2.1 mm dâ»Â¹ Â°Câ»Â¹
	 * These are daily values that will be applied to compute daily melt, then scaled to timestep.
	 * Default 1.8 mm dâ»Â¹ Â°Câ»Â¹ (Model 2 value for snow) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Hock1999", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::HockModel2 || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3Exact", ClampMin="0.0"))
	float HockSnowMeltFactor = 1.8f;

	/** Radiation coefficient a (mm hâ»Â¹ Â°Câ»Â¹ mÂ² Wâ»Â¹) for snow in Hock Model 2/3.
	 * Units: mm hâ»Â¹ Â°Câ»Â¹ mÂ² Wâ»Â¹ = mm hâ»Â¹ Â°Câ»Â¹ (W mâ»Â²)â»Â¹
	 * NOTE: Table header explicitly states "radiation factors a in mÂ² Wâ»Â¹ mm hâ»Â¹ Â°Câ»Â¹" (HOURLY!)
	 * Hock (1999) Table 1: Model 2 = 0.6Ã—10â»Â³, Model 3 = 0.7Ã—10â»Â³ mm hâ»Â¹ Â°Câ»Â¹ mÂ² Wâ»Â¹
	 * In code, this is multiplied by 24 to convert to mm dâ»Â¹ to match daily MF units.
	 * Default 0.0006 mm hâ»Â¹ Â°Câ»Â¹ mÂ² Wâ»Â¹ (Model 2 value for snow)
	 *
	 * NOTE: This coefficient applies to the full term IÂ·(G_s/I_s) in Model 3, not just I.
	 * The equation is: M = [MF + aÂ·IÂ·(G_s/I_s)]Â·T+ where G_s/I_s is the cloudiness ratio.
	 * Albedo is implicitly included in this coefficient (do NOT multiply by (1-Î±) separately). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Hock1999", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::HockModel2 || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3Exact", ClampMin="0.0"))
	float HockSnowRadiationFactor = 0.0006f;

	/** Clear-sky transmissivity applied when computing potential direct solar radiation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Hock1999", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::HockModel2 || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3Exact", ClampMin="0.0", ClampMax="1.0"))
	float HockClearSkyTransmissivity = 0.75f;

	// === Pellicciotti et al. (2005/2017) Model D parameters ===

	/** Temperature factor TF (mm h^-1 C^-1) for Pellicciotti Model D. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Pellicciotti", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3_UE_SWR", ClampMin="0.0"))
	float PellicciottiTemperatureFactor = 0.05f;

	/** Shortwave radiation factor SRF (m^2 mm W^-1 h^-1) for Pellicciotti Model D. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Pellicciotti", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3_UE_SWR", ClampMin="0.0"))
	float PellicciottiShortwaveFactor = 0.0094f;

	/** Shortwave coefficient applied to the RI-scaled term for the Pellicciotti RI variant. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Pellicciotti", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3_UE_SWR", ClampMin="0.0"))
	float PellicciottiShortwaveFactor_RI = 0.0094f;

	/** Threshold temperature TT (deg C). Melt is zero when T <= TT in Pellicciotti models. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Pellicciotti", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3_UE_SWR"))
	float PellicciottiTempThresholdC = 0.0f;

	/** Positive daily maximum temperature sum (deg C) required to decay albedo to OldSnowAlbedo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Pellicciotti", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3_UE_SWR", ClampMin="1.0"))
	float PellicciottiAlbedoTempSumForOldSnow = 50.0f;

	/** Minimum temperature sum used inside log10 term to avoid log(0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Pellicciotti", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3_UE_SWR", ClampMin="0.1"))
	float PellicciottiAlbedoMinTempSum = 1.0f;

	/** Ice albedo applied when no snow remains (Pellicciotti albedo parameterisation). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Pellicciotti", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel==EDegreeDayMeltModel::HockModel3_UE_SWR", ClampMin="0.0", ClampMax="1.0"))
	float PellicciottiIceAlbedo = 0.30f;

	/** If true, use (1 - albedo) absorptivity for Model D PBR (same as Lambertian). If false, PBR uses absorptivity = 1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Pellicciotti", meta=(EditCondition="DegreeDayMeltModel==EDegreeDayMeltModel::PellicciottiModelD_PBR", DisplayName="PBR Absorptivity Uses Albedo"))
	bool bUseAlbedoAbsorptivityInPBR = false;

	/** @deprecated Use OldSnowAlbedo instead. Kept for backward compatibility. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Deprecated")
	float SnowAlbedo = 0.4f;

	// === Radiation Index Configuration ===

	/** Method for calculating radiation index in melt calculations.
	 * Swift: Geometric algorithm based on slope/aspect (fast, no scene rendering required).
	 * UnrealEngine: Scene-based radiation from UE captures (requires bEnableRadiationCapture=true in SnowSimulationActor).
	 *
	 * NOTE: For Pellicciotti Model D (Radiation Index/PBR), set this to UnrealEngine to use captured scene radiation.
	 * Swift radiation is always computed for diagnostics regardless of this setting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Radiation")
	ERadiationIndexMethod RadiationIndexMethod = ERadiationIndexMethod::Swift;

	/** Reference luminance value for radiation calibration (typically 1.0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Radiation", meta=(ClampMin="0.1", ClampMax="10.0"))
	float ReferenceLuminance = 1.0f;

	/** If true, fallback to Swift radiation when sun elevation is below threshold. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Radiation")
	bool bUseLowSunAngleFallback = false;

	/** Sun elevation threshold (degrees) for low-angle fallback. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Radiation", meta=(EditCondition="bUseLowSunAngleFallback", ClampMin="0.0", ClampMax="30.0"))
	float LowSunAngleThreshold_deg = 15.0f;

	/** Guard reference strip calibration when sun is low or reference luminance is too dim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Radiation")
	bool bUseReferenceStripGuard = true;

	/** Minimum sun elevation (degrees) required to trust reference strip scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Radiation", meta=(EditCondition="bUseReferenceStripGuard", ClampMin="0.0", ClampMax="90.0", Units="deg"))
	float ReferenceStripMinSunElevation_deg = 5.0f;

	/** Minimum reference luminance required to trust reference strip scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Radiation", meta=(EditCondition="bUseReferenceStripGuard", ClampMin="0.0"))
	float ReferenceStripMinLuminance = 0.1f;

	/** If true, compute terrain as residual from scaled total: Total_scaled - Direct_scaled - Diffuse_scaled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Radiation", meta=(EditCondition="bUseReferenceStripGuard"))
	bool bUseTotalReferenceForTerrainResidual = false;

	/** Maximum allowed total reference scale before falling back to Direct+Diffuse only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Radiation", meta=(EditCondition="bUseReferenceStripGuard", ClampMin="0.1"))
	float ReferenceStripMaxTotalScale = 10.0f;

	/** If true, use split direct/diffuse forcing (DNI/DHI) scaled by UE indices for Pellicciotti Model D. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Radiation")
	bool bUseSplitRadiationForPellicciotti = false;

	/** If true, use the weather forcing snow fraction directly instead of the temperature-based heuristic. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Forcing Adjustments")
	bool bUseWeatherSnowFraction = true;

	/** If true, skip temperature and precipitation lapse-rate adjustments (use forcing values as-is). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Forcing Adjustments")
	bool bDisableLapseRateAdjustments = false;

	/** If true, apply precipitation lapse-rate adjustments both above and below the measurement altitude. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Forcing Adjustments")
	bool bApplyPrecipLapseBelowStation = true;

	/** Multiplicative precipitation lapse rate in fraction per km (0.30 = +30%/km), following BlÃ¶schl (1991). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Forcing Adjustments", meta=(ClampMin="0.0"))
	float PrecipitationLapseRate_FractionPerKm = 0.30f;

	/** If true, apply slope/curvature redistribution to solid precipitation before deposition. */
	UPROPERTY()
	bool bApplySlopeCurvatureRedistribution = true;

	/** Minimum step snowfall (mm) required before slope/curvature redistribution is applied.
	 * Use this to suppress single-cell redistribution artifacts during trace snowfall events. */
	UPROPERTY()
	float MinSnowfallForRedistribution_mm = 1.0f;

	/** Curvature gain in redistribution factor (1 + gain*curvature). BlÃ¶schl (1991) uses a3 = 50 m. */
	UPROPERTY()
	float CurvatureRedistributionGain = 50.0f;

	/** Slope (deg) where redistribution starts to reduce accumulation. BlÃ¶schl (1991) uses 10Â°. */
	UPROPERTY()
	float SlopeRedistributionStartDeg = 10.0f;

	/** Slope (deg) where accumulation is effectively zero due to redistribution. BlÃ¶schl (1991) uses 60Â° in the Austrian Alps. */
	UPROPERTY()
	float SlopeRedistributionZeroDeg = 60.0f;

	/** Number of cells from the boundary over which redistribution fades back to 1.0 (0 disables). */
	UPROPERTY()
	int32 RedistributionEdgeFadeCells = 5;

	/** Internal lower clamp for redistribution factors. */
	UPROPERTY()
	float MinRedistributionFactor = 0.0f;

	/** Internal upper clamp for redistribution factors. */
	UPROPERTY()
	float MaxRedistributionFactor = 2.0f;

	/** If true, normalize redistribution factors to conserve total mass across the domain.
	 * When enabled, snow is redistributed from steep/convex to flat/concave areas without changing total domain mass.
	 * When disabled, redistribution factors directly scale snowfall deposition (can create/destroy snowfall mass).
	 * Only applies when bApplySlopeCurvatureRedistribution is true. */
	UPROPERTY()
	bool bConserveMassDuringRedistribution = true;

	/** If true, exclude edge cells from the redistribution mass-conservation normalization. */
	UPROPERTY()
	bool bExcludeEdgeCellsFromMassConservation = true;

	float ComputeSnowRedistributionFactor(int32 CellIdx, float InclinationRad) const;

	float GetRedistributionEdgeWeight(int32 CellIdx) const;
	bool IsRedistributionEdgeCell(int32 CellIdx) const;

	/** If true, use GPU compute shaders for parallel cell processing (20-50x faster for large grids).
	 * If false, falls back to CPU sequential processing (better for debugging and diagnostics).
	 * GPU path uses UE5.6 compute shaders to process all cells in parallel on the graphics card. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DegreeDay|Performance")
	bool bUseGPUAcceleration = true;

	/** If true, dynamically update aspect and inclination based on current snow surface geometry.
	 * When enabled, aspect and slope are recomputed each step from the VHM (snow + terrain).
	 * When disabled, uses static terrain geometry from initialization.
	 * Recommended: true for simulations with significant snow redistribution or accumulation. */
	UPROPERTY()
	bool bUseDynamicSurfaceGeometry = false;



	/** Enable diagnostics logging to CSV (managed by ASnowSimulationActor). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Diagnostics | Internal (Auto)", meta=(AdvancedDisplay))
	bool bEnableDiagnostics = false;

	/** Write diagnostics every N simulation steps (managed by ASnowSimulationActor). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Diagnostics | Internal (Auto)", meta=(ClampMin="1", AdvancedDisplay))
	int32 DiagnosticsEveryNSteps = 1;

	/** Append to existing diagnostics file instead of creating new (set via Actor properties, not directly) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Diagnostics")
	bool bAppendDiagnostics = false;

	/** Enable verbose per-step/per-cell UE_LOG inside hot simulation loops. Off by default for sim speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Diagnostics", meta=(AdvancedDisplay))
	bool bEnableHotPathLogs = false;

	/** Directory for diagnostics output (managed by ASnowSimulationActor pipeline). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Diagnostics | Internal (Auto)", meta=(AdvancedDisplay))
	FString DiagnosticsDirectory = TEXT("analysis_results/DegreeDay_OutputsUE");

	/** Cell indices tracked for diagnostics (managed by ASnowSimulationActor). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snow Simulation | Diagnostics | Internal (Auto)", meta=(AdvancedDisplay))
	TArray<int32> DiagnosticsTrackedCellIndices;

	/** Legacy marker overlay for tracked cells. Prefer actor-level "Snow Debug" overlay for unified diagnostics debugging. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Diagnostics | Visualization (Legacy)", meta=(AdvancedDisplay, DisplayName="Enable Legacy Cell Markers"))
	bool bShowDiagnosticCells = false;

	/** Radius of legacy debug spheres drawn at diagnostic cell locations (in cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Diagnostics | Visualization (Legacy)", meta=(ClampMin="10.0", ClampMax="1000.0", AdvancedDisplay))
	float DiagnosticCellMarkerRadius = 100.0f;

	/** Color of legacy diagnostic cell markers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Diagnostics | Visualization (Legacy)", meta=(AdvancedDisplay))
	FColor DiagnosticCellColor = FColor::Red;

	/** Show labels for legacy diagnostic cell markers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Degree Day | Diagnostics | Visualization (Legacy)", meta=(AdvancedDisplay))
	bool bShowDiagnosticCellLabels = true;

	// === Radiation Diagnostics (set via Actor, not directly here) ===
	// Properties deprecated: now accessed via OwningSimulationActor

	virtual FString GetSimulationName() const override;

	virtual void Simulate(ASnowSimulationActor* SimulationActor, int32 CurrentSimulationStep, int32 Timesteps,
		bool SaveSnowMap, bool CaptureDebugInformation, TArray<FDebugCell>& DebugCells) override;

virtual void Initialize(ASnowSimulationActor* SimulationActor, const TArray<FLandscapeCell>& Cells,
		float InitialMaxSnow, UWorld* World) override;

	virtual UTexture* GetSnowMapTexture() override;

	virtual float GetMaxSnow() override;

	// Per-step accumulation + simple degree-day melt (legacy CPU path retained for debugging/testing)
	virtual void Step(float DtSeconds, const FWeatherForcingData& W, TArray<float>& OutDepthMeters) override;

	/** Public accessor for albedo state array (for material texture binding) */
	virtual TArray<float> GetCellSnowAlbedoState() const override { return CellSnowAlbedoState; }
	const TArray<float>& GetCellSnowAlbedoStateRef() const { return CellSnowAlbedoState; }
	const TArray<float>& GetCellDaysSinceSnowfallRef() const { return CellDaysSinceSnowfall; }

	// Lightweight const accessors used by the debug overlay (no copies unless necessary)
	const TArray<float>& GetCellSnowWaterEquivalentLiters() const { return CellSnowWaterEquivalentLiters; }
	const TArray<float>& GetCellAreaSqMeters() const { return CellAreaSqMeters; }
	const TArray<float>& GetCellLastRadiationIndex() const { return CellLastRadiationIndex; }
	const TArray<float>& GetCellLastRadiationIndexUE() const { return CellLastRadiationIndex_UE; }
	const TArray<float>& GetCellLastRadiationIndexSwift() const { return CellLastRadiationIndex_Swift; }
	/** Update cached RTY luminance (direct/diffuse) coming from the SnowSimulationActor capture readback. */
	void UpdateRTYLuminance(const TArray<float>& InTotalRTY, const TArray<float>& InDirectRTY, const TArray<float>& InDiffuseRTY, const TArray<float>& InDiffuseNoGIRTY, const TArray<float>& InTotalNoGIRTY, const TArray<float>& InTerrainRTY);
	const TArray<float>& GetCellLastAccumulationDepth() const { return CellLastAccumulationDepth_m; }
	const TArray<float>& GetCellLastMeltDepth() const { return CellLastMeltDepth_m; }
	const TArray<float>& GetCellLastMeltFactor() const { return CellLastMeltFactor; }
	const TArray<float>& GetCellLastComputedSnowRate() const { return CellLastComputedSnowRate; }
	const TArray<float>& GetCellLastLocalAirTempC() const { return CellLastLocalAirTempC; }
	const TArray<float>& GetCellLastWeatherSnowFrac() const { return CellLastWeatherSnowFrac; }
	const TArray<float>& GetCellLastEffectiveSnowFrac() const { return CellLastEffectiveSnowFrac; }
	const TArray<uint8>& GetCellLastSlopeFiltered() const { return CellLastSlopeFiltered; }
	const FForcingRadiation& GetLastTimestepRadiation() const { return LastTimestepForcingRad; }
	bool HadValidRadiationForcing() const { return bLastTimestepHadValidRadiationForcing; }

	/** DEPRECATED: No longer needed. Radiation indices are now cached in SnowSimulationActor
	 * and accessed directly via GetCachedRadiationIndices() for efficient memory usage.
	 * See SnowSimulationActor::ExtractRadiationIndicesToBuffer() for the new implementation. */
	// float SampleSceneRadiationIndex(int32 CellIndex) const;

	/**
	 * Computes DNI (Direct Normal Irradiance) and DHI (Diffuse Horizontal Irradiance) from GHI.
	 *
	 * Uses Erbs et al. (1982) correlation to estimate diffuse fraction if not provided:
	 *   k_t = GHI / (S_0 * cos(Î¸_z))  [clearness index]
	 *
	 *   For k_t â‰¤ 0.22:  F_d = 1.0 - 0.09 * k_t
	 *   For k_t â‰¤ 0.80:  F_d = 0.9511 - 0.1604*k_t + 4.388*k_tÂ² - 16.638*k_tÂ³ + 12.336*k_tâ´
	 *   For k_t > 0.80:  F_d = 0.165
	 *
	 * Then computes:
	 *   DHI = F_d * GHI
	 *   DNI = (1 - F_d) * GHI / max(cos(Î¸_z), 1e-6)
	 *
	 * @param InOut Radiation struct to populate (GHI and SolarZenith_rad must be set on input)
	 * @param ExtraterrestrialIrradiance Solar constant in W/mÂ² (default 1361.0)
	 *
	 * Reference: Erbs, D.G., Klein, S.A., Duffie, J.A. (1982). "Estimation of the diffuse
	 *            radiation fraction for hourly, daily and monthly-average global radiation."
	 *            Solar Energy, 28(4), 293-302. doi:10.1016/0038-092X(82)90302-4
	 */
	static void ComputeDNI_DHI(FForcingRadiation& InOut, float ExtraterrestrialIrradiance = 1361.0f);

	/**
	 * Gets the current solar position from the world's primary directional light.
	 * The directional light is typically controlled by a georeferenced SunSky actor.
	 *
	 * @param OutSolarZenith_rad Solar zenith angle in radians (0 = sun at zenith, PI/2 = horizon)
	 * @param OutCosSolarZenith Cosine of solar zenith angle (for direct irradiance calculations)
	 * @param OutSolarAzimuth_rad Solar azimuth angle in radians (0 = North, PI/2 = East, PI = South, 3PI/2 = West)
	 * @return true if a valid directional light was found, false otherwise (fallback values set)
	 */
	bool GetSolarZenithFromWorld(float& OutSolarZenith_rad, float& OutCosSolarZenith, float& OutSolarAzimuth_rad) const;

private:
	FSimulationComputeShader* SimulationComputeShader = nullptr;
	FSnowPixelShader* SimulationPixelShader = nullptr;
	UTextureRenderTarget2D* RenderTarget = nullptr;

	// Diagnostics tracking
	struct FDegreeDayCellDiagnostics
	{
		int32 StepIndex = 0;
		int32 CellIndex = -1;
		double SimulationTimeSeconds = 0.0;
		FDateTime Timestamp;

		// State variables
		float SnowDepth_m = 0.0f;

		// Forcing data
		float ForcingAirTemperatureC = 0.0f;
		float ForcingPrecipRate_kgm2s = 0.0f;
		float ForcingPrecipRate_mmph = 0.0f;
		float ForcingSnowFrac = 0.0f;
		float WeatherSnowFracRaw = 0.0f;
		float ForcingSWdown_Wm2 = 0.0f;
		float ForcingLWdown_Wm2 = 0.0f;
		float ForcingWindSpeed_mps = 0.0f;
		float ForcingRH = 0.0f;
		float ForcingPressure_Pa = 0.0f;
		float ForcingCloudCover_01 = 0.0f;  // Cloud cover fraction from forcing (0-1)

		// Computed values
		float SnowAccumulation_m = 0.0f;
		float MeltAmount_m = 0.0f;
		float MeltFactor = 0.0f;
		float MeltRate_mps = 0.0f;
		float MeltRate_mmph = 0.0f;
		float ComputedSnowRate = 0.0f;  // Locally calculated snow fraction (0-1), different from forcing SnowFrac
		float SnowAlbedo = 0.0f;
		float RadiationComponent = 0.0f;  // Radiation component actually used for melt calculation
		float RadiationIndex_Swift = 0.0f;  // Swift geometric radiation (always computed for comparison)
		float RadiationIndex_UE = 0.0f;  // UE scene-based radiation (when available)
		float CloudinessRatio = 1.0f;  // Gs/Is for Model 3 Exact, r_UE for Model 3 variations
		float CellSlopeDegrees = 0.0f;

		// Mass conservation diagnostics
		float RedistributionFactor = 1.0f;     // Cell-specific redistribution factor based on slope/curvature (before normalization)
		float MassConservationFactor = 1.0f;   // Domain-wide normalization factor to conserve total mass

		// Radiation diagnostics
		float r_UE_raw = 0.0f;            // Raw radiation index (dimensionless, always available)
		float GHI_Wm2 = 0.0f;             // Global Horizontal Irradiance from forcing (W/mÂ²)
		float DNI_Wm2 = 0.0f;             // Direct Normal Irradiance (W/mÂ²)
		float DNI_Horiz_Wm2 = 0.0f;       // Direct Horizontal Irradiance (W/m^2)
		float DHI_Wm2 = 0.0f;             // Diffuse Horizontal Irradiance (W/mÂ²)
		float DiffuseFraction = 0.0f;     // Diffuse fraction (DHI/GHI)

		// Physical Light Settings (Diagnostics)
		float SunLightIntensity = 0.0f;   // Applied DirectionalLight Intensity (Lux)
		float SkyLightIntensity = 0.0f;   // Applied SkyLight Intensity (Scale/Unit)

		// Separate Radiation Components (UE Capture)
		float RadiationDirect_UE = 0.0f;  // Absolute Direct Irradiance (W/mÂ²)
		float RadiationDiffuse_UE = 0.0f; // Absolute Diffuse Irradiance (W/mÂ²)
		float RadiationTotal_UE = 0.0f;   // Absolute Total Irradiance (W/mÂ², calibrated)
		float RadiationTotal_UsedForMelt = 0.0f; // Total shortwave actually used in the melt law
		float RadiationTerrain_UE = 0.0f; // Residual terrain/interreflection (W/mÂ²)
		float RadiationIndex_Direct = 0.0f; // Normalized Direct Index (Direct_UE / DNI_Horiz)
		float RadiationIndex_Diffuse = 0.0f; // Normalized Diffuse Index (Diffuse_UE / DHI)


		float I_Wm2 = 0.0f;               // Potential clear-sky direct radiation at cell (W/mÂ², accounts for slope/aspect)
		float Is_Wm2 = 0.0f;              // Potential clear-sky direct radiation on horizontal surface (W/mÂ²)
		float RTY_Total = 0.0f;           // Total luminance from UE (unnormalized)
		float RTY_Direct = 0.0f;          // Direct radiation index from UE (luminance, unnormalized)
		float RTY_Diffuse = 0.0f;         // Diffuse radiation index from UE (luminance, unnormalized)
		float RTY_DiffuseNoGI = 0.0f;     // Sky-only diffuse luminance from UE (unnormalized)
		float RTY_TotalNoGI = 0.0f;       // TotalNoGI luminance from UE (sun + sky, no GI)
		float RTY_Terrain = 0.0f;         // Terrain residual luminance (unnormalized)
		float SolarZenith_deg = 0.0f;     // Solar zenith angle (degrees, 0=overhead, 90=horizon)
		float SolarElevation_deg = 0.0f;  // Solar elevation angle (degrees, 0=horizon, 90=overhead)
		float SolarAzimuth_deg = 0.0f;    // Solar azimuth angle (degrees, 0=North, 90=East, 180=South, 270=West)
		float CosSolarZenith = 0.0f;      // cos(zenith angle)
		float SolarZenith_Forcing_deg = 0.0f;
		float SolarElevation_Forcing_deg = 0.0f;
		float CosSolarZenith_Forcing = 0.0f;
		float CosSolarZenith_UECapture = 0.0f;
		float SunVisibility_UECapture = 1.0f;
		bool bHasSolarForcing = false;
		float Qsw_abs_Wm2 = 0.0f;         // Absorbed shortwave radiation: (1-alpha) * Etot (W/mÂ²)
		float CellAltitude_m = 0.0f;
		float MeasurementAltitude_m = 0.0f;
		float GeoOriginElevation_m = 0.0f;
		float AltitudeDelta_m = 0.0f;

		// Terrain geometry for radiation analysis
		float CellAspect_deg = 0.0f;        // Compass direction slope faces (0-360Â°, 0=North)
		float CellInclination_deg = 0.0f;   // Slope angle from horizontal (0-90Â°)
		float CellLatitude_deg = 0.0f;      // Geographic latitude

		// Reference patch luminance (for validating UE radiation normalization)
		float ReferenceLuminance_Total = 0.0f;  // Mean total luminance of reference strip
		float ReferenceLuminance_Direct = 0.0f; // Mean direct luminance of reference strip
		float ReferenceLuminance_Diffuse = 0.0f; // Mean diffuse luminance of reference strip
		float ReferenceLuminance_DiffuseNoGI = 0.0f; // Mean sky-only diffuse luminance of reference strip
		float ReferenceLuminance_TotalNoGI = 0.0f; // Mean TotalNoGI luminance of reference strip
		float ReferenceLuminance_Terrain = 0.0f; // Mean terrain residual luminance of reference strip
		float ReferenceScale_Total = 0.0f;      // Calibration scale (Total W/mÂ² per RTY)
		float ReferenceScale_Direct = 0.0f;     // Calibration scale (Direct W/mÂ² per RTY)
		float ReferenceScale_Diffuse = 0.0f;    // Calibration scale (Diffuse W/mÂ² per RTY)
		float ReferenceScale_TotalNoGI = 0.0f;  // Calibration scale (TotalNoGI W/mÂ² per RTY)
		bool bSkyOnlyReferenceUsable = false;   // Sky-only reference passed relative/absolute plausibility guard
		bool bSkyOnlyReferenceMeetsMinLuminance = false; // Sky-only reference also passed min-luminance gate
		bool bSkyOnlyDiffuseScalingUsed = false; // Diffuse scaling switched to RTY_DiffuseNoGI for this timestep
		float SkyOnlyReferenceRatio = 0.0f;    // RefDiffuseNoGI / RefDiffuse
		float SkyOnlyRTYRatio = 0.0f;          // RTY_DiffuseNoGI / RTY_Diffuse
		bool bReferenceValid = false;           // True if reference values are valid for this timestep

		void Reset()
		{
			StepIndex = 0;
			CellIndex = -1;
			SimulationTimeSeconds = 0.0;
			Timestamp = FDateTime();
			SnowDepth_m = 0.0f;
			ForcingAirTemperatureC = 0.0f;
			ForcingPrecipRate_kgm2s = 0.0f;
			ForcingPrecipRate_mmph = 0.0f;
			ForcingSnowFrac = 0.0f;
			WeatherSnowFracRaw = 0.0f;
			ForcingSWdown_Wm2 = 0.0f;
			ForcingLWdown_Wm2 = 0.0f;
			ForcingWindSpeed_mps = 0.0f;
			ForcingRH = 0.0f;
			ForcingPressure_Pa = 0.0f;
			SnowAccumulation_m = 0.0f;
			MeltAmount_m = 0.0f;
			MeltFactor = 0.0f;
			MeltRate_mps = 0.0f;
			ComputedSnowRate = 0.0f;
			SnowAlbedo = 0.0f;
			RadiationComponent = 0.0f;
			CellSlopeDegrees = 0.0f;
			r_UE_raw = 0.0f;
			GHI_Wm2 = 0.0f;
			DNI_Wm2 = 0.0f;
			DNI_Horiz_Wm2 = 0.0f;
			DHI_Wm2 = 0.0f;
			DiffuseFraction = 0.0f;
			RadiationDirect_UE = 0.0f;
			RadiationDiffuse_UE = 0.0f;
			RadiationTotal_UE = 0.0f;
			RadiationTotal_UsedForMelt = 0.0f;
			RadiationTerrain_UE = 0.0f;
			RadiationIndex_Direct = 0.0f;
			RadiationIndex_Diffuse = 0.0f;
			I_Wm2 = 0.0f;
			Is_Wm2 = 0.0f;
			RTY_Total = 0.0f;
			RTY_Direct = 0.0f;
			RTY_Diffuse = 0.0f;
			RTY_DiffuseNoGI = 0.0f;
			RTY_TotalNoGI = 0.0f;
			RTY_Terrain = 0.0f;
			SolarZenith_deg = 0.0f;
			SolarElevation_deg = 0.0f;
			SolarAzimuth_deg = 0.0f;
			CosSolarZenith = 0.0f;
			SolarZenith_Forcing_deg = 0.0f;
			SolarElevation_Forcing_deg = 0.0f;
			CosSolarZenith_Forcing = 0.0f;
			CosSolarZenith_UECapture = 0.0f;
			SunVisibility_UECapture = 1.0f;
			bHasSolarForcing = false;
			Qsw_abs_Wm2 = 0.0f;
			CellAltitude_m = 0.0f;
			MeasurementAltitude_m = 0.0f;
			GeoOriginElevation_m = 0.0f;
			AltitudeDelta_m = 0.0f;
			ReferenceLuminance_Total = 0.0f;
			ReferenceLuminance_Direct = 0.0f;
			ReferenceLuminance_Diffuse = 0.0f;
			ReferenceLuminance_DiffuseNoGI = 0.0f;
			ReferenceLuminance_TotalNoGI = 0.0f;
			ReferenceLuminance_Terrain = 0.0f;
			ReferenceScale_Total = 0.0f;
			ReferenceScale_Direct = 0.0f;
			ReferenceScale_Diffuse = 0.0f;
			ReferenceScale_TotalNoGI = 0.0f;
			bSkyOnlyReferenceUsable = false;
			bSkyOnlyReferenceMeetsMinLuminance = false;
			bSkyOnlyDiffuseScalingUsed = false;
			SkyOnlyReferenceRatio = 0.0f;
			SkyOnlyRTYRatio = 0.0f;
			bReferenceValid = false;
		}
	};

	void EnsureDiagnosticsFileInitialized();
	void WriteDiagnostics(const TArray<FDegreeDayCellDiagnostics>& CellDiagnostics);
	void SaveCellLocationSnapshot(const TArray<FDegreeDayCellDiagnostics>& CellDiagnostics);
	void DrawDiagnosticCellsDebug();
	void WriteMeltModelConfigFile();

	// Radiation diagnostics methods
	void EnsureRadiationDiagnosticsFileInitialized();
	void WriteRadiationDiagnostics(const TArray<FDegreeDayCellDiagnostics>& CellDiagnostics);
	void WriteRadiationConfigFile();
	void EnsureRadiativeTransferDiagnosticsFileInitialized();
	void WriteRadiativeTransferDiagnostics(const TArray<FDegreeDayCellDiagnostics>& CellDiagnostics);
	FString RadiationDiagnosticsFilename;
	FString RadiativeTransferDiagnosticsFilename;
	FString RadiationConfigFilename;
	FString MeltModelConfigFilename;

	// Per-cell geom + state
	TArray<float> CellAreaSqMeters;
	TArray<float> CellAreaXYSqMeters;
	TArray<float> CellAltitudeCm;
	TArray<float> CellAspectRad;
	TArray<float> CellInclinationRad;
	TArray<float> CellLatitudeRad;

	// Base terrain cell corners for dynamic surface geometry computation
	TArray<FVector> CellBaseP0;  // Corner positions from base terrain (no snow)
	TArray<FVector> CellBaseP1;
	TArray<FVector> CellBaseP2;
	TArray<FVector> CellBaseP3;
	TArray<float> CellSnowWaterEquivalentLiters;
	TArray<float> CellInterpolatedSWE_Liters;
	TArray<float> CellSnowAlbedoState;
	TArray<float> CellDaysSinceSnowfall;
	TArray<float> CellAlbedoTempSum;
	TArray<float> CellDailyMaxTemp;
	TArray<float> CellLastRadiationIndex;
	TArray<float> CellLastRadiationIndex_Swift;  // Always computed for diagnostics
	TArray<float> CellLastRadiationIndex_UE;     // UE scene-based (when available)
	TArray<float> CellLastRadiationIndex_UE_Raw; // Raw UE radiation index (unclamped/unmodified) for diagnostics
	TArray<float> CellLastCloudinessRatio;       // Gs/Is for Model 3 Exact, r_UE for Model 3 variations
	TArray<float> CellLastNetShortwaveAbsorbed_Wm2;
	TArray<float> CellLastPotentialDirect_Wm2;   // I: Potential clear-sky direct radiation at cell
	TArray<float> CellLastPotentialHorizontal_Wm2; // Is: Potential clear-sky direct radiation on horizontal surface
	TArray<float> CellLastRTY_Total;             // Total luminance from UE (unnormalized)
	TArray<float> CellLastRTY_Direct;            // Direct luminance from UE (unnormalized)
	TArray<float> CellLastRTY_Diffuse;           // Diffuse luminance from UE (unnormalized)
	TArray<float> CellLastRTY_DiffuseNoGI;       // Sky-only diffuse luminance from UE (unnormalized)
	TArray<float> CellLastRTY_TotalNoGI;         // TotalNoGI luminance from UE (sun + sky, no GI)
	TArray<float> CellLastRTY_Terrain;           // Terrain residual luminance from UE (unnormalized)
	bool bHasDiffuseNoGI = false;
	bool bHasTerrainRTY = false;
	TArray<float> CellLastAccumulationDepth_m;
	TArray<float> CellLastMeltDepth_m;
	TArray<float> CellLastMeltFactor;
	TArray<float> CellLastComputedSnowRate;
	TArray<float> CellLastLocalAirTempC;
	TArray<uint8> CellLastSlopeFiltered;
	TArray<float> CellLastWeatherSnowFrac;
	TArray<float> CellLastEffectiveSnowFrac;
	TArray<float> CellLastRedistributionFactor;      // Per-cell redistribution factor (before normalization)
	
	// Separate Radiation Component Arrays (Persistent Storage for Diagnostics)
	TArray<float> CellLastRadiationDirect_UE;
	TArray<float> CellLastRadiationDiffuse_UE;
	TArray<float> CellLastRadiationTotal_UE;
	TArray<float> CellLastRadiationTerrain_UE;
	TArray<float> CellLastRadiationIndex_Direct;
	TArray<float> CellLastRadiationIndex_Diffuse;
	TArray<float> CellLastReferenceScale_Total;
	TArray<float> CellLastReferenceScale_Direct;
	TArray<float> CellLastReferenceScale_Diffuse;
	TArray<float> CellLastReferenceScale_TotalNoGI;
	TArray<float> CellLastReferenceLuminance_Total;
	TArray<float> CellLastReferenceLuminance_Direct;
	TArray<float> CellLastReferenceLuminance_Diffuse;
	TArray<float> CellLastReferenceLuminance_DiffuseNoGI;
	TArray<float> CellLastReferenceLuminance_TotalNoGI;
	TArray<float> CellLastReferenceLuminance_Terrain;
	
	float LastMassConservationFactor = 1.0f;         // Domain-wide conservation factor from last timestep

	// Cached radiation forcing data from last timestep (for diagnostics)
	FForcingRadiation LastTimestepForcingRad;
	bool bLastTimestepHadValidRadiationForcing = false;

	float CurrentMaxSnowMM = 0.0f;
	float MeasurementAltitudeCm = 0.0f;
	float GeoReferencingOriginUpCm = 0.0f;
	float CellSpacingMeters = 0.0f;  // Distance between cell centers (for curvature computation)
	int32 LastAlbedoUpdateDayOfYear = -1;
	TArray<FClimateData> LegacyClimateData;

	// Weak reference to owning simulation actor (used for accessing cached radiation indices in UE method)
	TWeakObjectPtr<ASnowSimulationActor> OwningSimulationActor;

	void InitializeCellState(const TArray<FLandscapeCell>& Cells);
	void PerformDegreeDayStep(float DtSeconds, const FWeatherForcingData& W, int32 InDayOfYear, float MeasurementAltitude);
	void UpdateDepthFromSnowWaterEquivalent();
	void UpdateDynamicSurfaceGeometry(const FVector& North);
	float ComputeSolarRadiationIndex(float Inclination, float Aspect, float Latitude, float DayOfYear) const;
	float SolarFunc2(float L, float D) const;
	float SolarFunc3(float V, float W, float X, float Y, float R1, float D) const;
	float ComputePotentialDirectRadiationHock(float LatitudeRad, float SlopeRad, float AspectRad, float SolarDeclinationRad,
		float HourAngleRad, float ExtraterrestrialNormal, float PressurePa) const;
	void ComputeSolarAngles(float LatitudeRad, float SolarDeclinationRad, float HourAngleRad, float& OutCosZenith,
		float& OutSinZenith, float& OutSolarAzimuthRad) const;
	static float ComputeSolarDeclinationRad(int32 DayOfYear);
	FString GetMeltModelName() const;

	static float ComputeEquationOfTimeMinutes(int32 DayOfYear);
	static float ComputeLocalSolarTimeHours(const FDateTime& Timestamp, int32 DayOfYear, float LongitudeDegrees);
	float ConvertSWEToDepthMeters(float SnowWaterEquivalentLiters, float AreaSquareMeters) const;

	bool bDiagnosticsFileInitialized = false;
	bool bDiagnosticsHeaderWritten = false;
	bool bRadiationDiagnosticsHeaderWritten = false;
	bool bRadiativeTransferDiagnosticsHeaderWritten = false;
	FString DiagnosticsFilePath;
	int32 SimulationStepCounter = 0;
	double ElapsedSimulationSeconds = 0.0;
};
