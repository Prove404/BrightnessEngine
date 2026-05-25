#pragma once

#include "CoreMinimal.h"
#include "FSM2Parameters.generated.h"

UENUM(BlueprintType)
enum class EFSM2DensityScheme : uint8
{
    FixedDensity UMETA(DisplayName = "Fixed density"),
    AgeCompaction UMETA(DisplayName = "Age-dependent compaction"),
    OverburdenCompaction UMETA(DisplayName = "Overburden compaction")
};

UENUM(BlueprintType)
enum class EFSM2GrainGrowthScheme : uint8
{
    None UMETA(DisplayName = "Constant"),
    TemperatureDependent UMETA(DisplayName = "Temperature dependent"),
    TemperatureGradient UMETA(DisplayName = "Temperature gradient dependent")
};

UENUM(BlueprintType)
enum class EFSM2HydrologyScheme : uint8
{
    FreeDrain UMETA(DisplayName = "Free draining"),
    Bucket UMETA(DisplayName = "Bucket storage"),
    Darcy UMETA(DisplayName = "Darcy drainage")
};

UENUM(BlueprintType)
enum class EFSM2ShortwavePartition : uint8
{
    DiffuseOnly UMETA(DisplayName = "Diffuse only"),
    DiffuseAndDirect UMETA(DisplayName = "Diffuse + direct")
};

UENUM(BlueprintType)
enum class EFSM2HeightReference : uint8
{
    AboveGround UMETA(DisplayName = "Above ground"),
    AboveCanopyTop UMETA(DisplayName = "Above canopy top")
};

UENUM(BlueprintType)
enum class EFSM2SnowFractionScheme : uint8
{
    Linear UMETA(DisplayName = "Linear"),
    HyperbolicTangent UMETA(DisplayName = "Hyperbolic tangent"),
    Asymptotic UMETA(DisplayName = "Asymptotic")
};

UENUM(BlueprintType)
enum class EFSM2AlbedoScheme : uint8
{
    DiagnosticTemperature UMETA(DisplayName = "Diagnostic temperature"),
    PrognosticAge UMETA(DisplayName = "Prognostic age")
};

UENUM(BlueprintType)
enum class EFSM2SnowConductivityScheme : uint8
{
    Fixed UMETA(DisplayName = "Fixed"),
    DensityDependent UMETA(DisplayName = "Density dependent")
};



USTRUCT(BlueprintType)
struct FFSM2PhysicalConstants
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float SpecificHeatAir_JkgK = 1005.0f; // cp

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float WaterVapourMolecularRatio = 0.622f; // eps

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float SaturationVapourPressure0_Pa = 611.213f; // e0

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float Gravity_mps2 = 9.81f; // g

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float SpecificHeatIce_JkgK = 2100.0f; // hcap_ice

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float SpecificHeatWater_JkgK = 4180.0f; // hcap_wat

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float ThermalConductivityAir_WmK = 0.025f; // hcon_air

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float ThermalConductivityClay_WmK = 1.16f; // hcon_clay

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float ThermalConductivityIce_WmK = 2.24f; // hcon_ice

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float ThermalConductivitySand_WmK = 1.57f; // hcon_sand

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float ThermalConductivityWater_WmK = 0.56f; // hcon_wat

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float SolarConstant_Wm2 = 1367.0f; // I0

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float LatentHeatFusion_Jkg = 3.34e5f; // Lf

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float LatentHeatVapor_Jkg = 2.501e6f; // Lv

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float WaterDynamicViscosity_Pas = 1.78e-3f; // mu_wat

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float GasConstantDryAir_JkgK = 287.0f; // Rair

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float GasConstantWaterVapour_JkgK = 462.0f; // Rwat

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float DensityIce_kgm3 = 917.0f; // rho_ice

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float DensityWater_kgm3 = 1000.0f; // rho_wat

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float StefanBoltzmann_Wm2K4 = 5.67e-8f; // sb

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float MeltingPoint_K = 273.15f; // Tm

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float VonKarman = 0.4f; // vkman

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constants")
    float RainHeatCapacity_Jkg = 4180.0f; // Specific heat of liquid water for rain sensible heat (J/kg/K)
};

USTRUCT(BlueprintType)
struct FFSM2LayerConfiguration
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers", meta = (ClampMin = "1", ClampMax = "3"))
    int32 MaxSnowLayers = 3; // Nsmax

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers", meta = (ClampMin = "1", ClampMax = "3"))
    int32 MaxActiveLayers = 3;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers", meta = (ClampMin = "0.0"))
    float MinimumLayerThickness_m = 0.1f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers", meta = (ClampMin = "0.0"))
    float MaximumLayerThickness_m = 0.4f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers", meta = (ClampMin = "0"))
    int32 SoilLayerCount = 4; // Nsoil

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers")
    TArray<float> SoilLayerThicknesses_m {0.1f, 0.2f, 0.4f, 0.8f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers")
    TArray<float> MinimumSnowLayerThicknesses_m {0.1f, 0.2f, 0.4f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers")
    int32 CanopyLayerCount = 1; // Ncnpy (Fortran default CANMOD=1)

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers")
    float VegetationFractionUpper = 0.5f; // fvg1 (used when CANMOD=2)

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers")
    float SubcanopyMeasurementHeight_m = 1.5f; // zsub
};

USTRUCT(BlueprintType)
struct FFSM2SnowParameters
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Schemes")
    EFSM2DensityScheme DensityScheme = EFSM2DensityScheme::AgeCompaction;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Schemes")
    EFSM2GrainGrowthScheme GrainGrowthScheme = EFSM2GrainGrowthScheme::TemperatureDependent;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Schemes")
    EFSM2HydrologyScheme HydrologyScheme = EFSM2HydrologyScheme::Bucket;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Albedo")
    EFSM2AlbedoScheme AlbedoScheme = EFSM2AlbedoScheme::PrognosticAge;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Cover")
    EFSM2SnowFractionScheme SnowCoverFractionScheme = EFSM2SnowFractionScheme::Linear;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Cover", meta = (ClampMin = "0.0"))
    float MinimumSnowfallRate_mmph = 0.0f;

    /** If true, apply terrain redistribution of snowfall based on slope/curvature metadata from the DEM grid. */
    UPROPERTY()
    bool bApplySlopeCurvatureRedistribution = true;

    UPROPERTY()
    float MinSnowfallForRedistribution_mm = 1.0f;

    /** Slope (deg) where redistribution starts reducing snowfall accumulation. Blöschl (1991) uses 10°. */
    UPROPERTY()
    float SlopeRedistributionStartDeg = 10.0f;

    /** Slope (deg) where snowfall accumulation tends toward zero before curvature correction. Blöschl (1991) uses 60°. */
    UPROPERTY()
    float SlopeRedistributionZeroDeg = 60.0f;

    /** Linear gain applied to unitless curvature in redistribution factor (factor = 1 + gain * curvature). Blöschl (1991) uses a3 = 50 m. */
    UPROPERTY()
    float CurvatureRedistributionGain = 50.0f;

    /** Number of cells to fade redistribution to neutral factor (1.0) near domain boundaries. */
    UPROPERTY()
    int32 RedistributionEdgeFadeCells = 5;

    /** Clamp lower bound for redistribution factor after slope+curvature weighting. */
    UPROPERTY()
    float MinRedistributionFactor = 0.0f;

    /** Clamp upper bound for redistribution factor after slope+curvature weighting. Blöschl (1991) implies a 2x cap for gullies. */
    UPROPERTY()
    float MaxRedistributionFactor = 2.0f;

    /** If true, normalize redistribution factors so total snowfall mass is conserved over the domain. */
    UPROPERTY()
    bool bConserveMassDuringRedistribution = true;

    /** If true, exclude edge cells from redistribution mass-conservation normalization. */
    UPROPERTY()
    bool bExcludeEdgeCellsFromMassConservation = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Density", meta = (ClampMin = "0.0"))
    float MinSnowDensity_kgm3 = 100.0f; // Align floor with fresh-snow density rhof

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Density", meta = (ClampMin = "0.0"))
    float MaxSnowDensity_kgm3 = 917.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Albedo", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MinimumSnowAlbedo = 0.5f; // asmn

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Albedo", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MaximumSnowAlbedo = 0.85f; // asmx

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Density", meta = (ClampMin = "0.0"))
    float MaxColdSnowDensity_kgm3 = 300.0f; // rcld

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Density", meta = (ClampMin = "0.0"))
    float MaxMeltSnowDensity_kgm3 = 500.0f; // rmlt

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Density", meta = (ClampMin = "0.0"))
    float FixedSnowDensity_kgm3 = 300.0f; // rfix

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Density", meta = (ClampMin = "0.0"))
    float FreshSnowDensity_kgm3 = 100.0f; // rhof

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Density", meta = (ClampMin = "0.0"))
    float WindPackedSnowDensity_kgm3 = 300.0f; // rhow

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Temperature")
    float MinSnowTemperature_K = 243.15f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Compaction", meta = (ClampMin = "0.0"))
    float ColdCompactionTimescale_s = 720000.0f; // DEPRECATED: Use CompactionTimescale_s

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Compaction", meta = (ClampMin = "0.0"))
    float MeltCompactionTimescale_s = 720000.0f; // DEPRECATED: Use CompactionTimescale_s

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Compaction", meta = (ClampMin = "0.0"))
    float ReferenceViscosity_PaS = 3.7e7f; // eta0

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Compaction", meta = (ClampMin = "0.0"))
    float CompactionTimescale_s = 200.0f * 3600.0f; // trho

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Metamorphism", meta = (ClampMin = "0.0"))
    float ThermalMetamorphismRate_s = 2.8e-6f; // snda

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grain", meta = (ClampMin = "0.000001", ClampMax = "0.01"))
    float FreshSnowGrainRadius_m = 5.0e-5f; // rgr0

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grain", meta = (ClampMin = "0.0"))
    float SnowfallRefreshAlbedo_kgm2 = 10.0f; // Salb

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Albedo", meta = (ClampMin = "0.0"))
    float ColdSnowAlbedoTimescale_s = 1000.0f * 3600.0f; // tcld

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Albedo", meta = (ClampMin = "0.0"))
    float MeltSnowAlbedoTimescale_s = 100.0f * 3600.0f; // tmlt

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Albedo")
    float AlbedoDecayTemperature_C = -2.0f; // Talb

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ClampMin = "1"))
    int32 HydrologySubsteps = 10; // nhyd

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ClampMin = "0.0"))
    float DrainageTimescale_s = 7200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MaximumDrainageFraction = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ClampMin = "0.0", ClampMax = "0.2"))
    float IrreducibleWaterFraction = 0.03f; // Wirr

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ClampMin = "1"))
    int32 DarcyIterations = 10;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ClampMin = "1"))
    int32 DarcyNonlinearIterations = 10; // Match Fortran TRIDIAG loop count

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ClampMin = "0.0"))
    float SnowCoverFractionDepthScale_m = 0.1f; // hfsn

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ClampMin = "0.0"))
    float DarcySaturationExponent = 3.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roughness", meta = (ClampMin = "0.0"))
    float SnowRoughnessLength_m = 0.001f; // z0sn

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal")
    EFSM2SnowConductivityScheme ConductivityScheme = EFSM2SnowConductivityScheme::DensityDependent;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal", meta = (ClampMin = "0.0"))
    float FixedSnowConductivity_WmK = 0.24f; // kfix


    UPROPERTY()
    float ThermalConductivityA_WmK_DEPRECATED = 0.021f;

    UPROPERTY()
    float ThermalConductivityB_WmK_DEPRECATED = 2.5e-4f;
};

USTRUCT(BlueprintType)
struct FFSM2RadiationParameters
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SnowEmissivity = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float FreshSnowAlbedo = 0.85f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float OldSnowAlbedo = 0.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float GroundAlbedo = 0.2f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (ClampMin = "0.0"))
    float AlbedoDecayTimeConstant_h = 120.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation")
    bool bUseSlopeAdjustedShortwave = false;

    /** Use Unreal Engine's Lumen-based radiation index to correct incoming shortwave radiation.
     * This accounts for terrain shadows, reflections, and local topographic effects.
     * When enabled, replaces geometric slope correction with physically-based rendering results. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation")
    bool bUseUERadiationIndex = false;

    /** If true, use reference-strip calibrated UE shortwave components (direct/diffuse/terrain) in W/m². */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex"))
    bool bUseFluxCalibratedUERadiation = true;

    /** If true, include terrain residual/interreflection shortwave using RTY_Terrain calibration. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation"))
    bool bUseTerrainInterreflection = true;

    /** Guard calibrated reference-strip scaling under low-sun or dim-reference conditions. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation"))
    bool bUseReferenceStripGuard = true;

    /** Minimum sun elevation (degrees) required to trust reference-strip calibration. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation && bUseReferenceStripGuard", ClampMin = "0.0", ClampMax = "90.0"))
    float ReferenceStripMinSunElevation_deg = 5.0f;

    /** Minimum reference luminance required to trust reference-strip calibration. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation && bUseReferenceStripGuard", ClampMin = "0.0"))
    float ReferenceStripMinLuminance = 0.1f;

    /** If true, override the depth/cover-based dual-reference half when its direct solution is implausible but the alternate half is plausible. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation", DisplayName = "Use Dual Reference Plausibility Guard"))
    bool bUseDualReferencePlausibilityGuard = true;

    /** Maximum acceptable direct radiation index for dual-reference half selection before switching to the alternate half. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation && bUseDualReferencePlausibilityGuard", ClampMin = "0.1"))
    float DualReferenceMaxPlausibleDirectIndex = 5.0f;

    /** Max allowed total reference scale before terrain residual is disabled for stability. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation && bUseTerrainInterreflection", ClampMin = "0.1"))
    float ReferenceStripMaxTotalScale = 10.0f;

    /** If true, convert UE flux-calibrated shortwave back toward incident irradiance before FSM2 applies (1-albedo).
     *  Default off: the same-material dual-reference-strip construction cancels surface albedo directly in the
     *  RTY ratio, so a separate scalar neutralisation factor is no longer needed and would double-correct. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation"))
    bool bNeutralizeCaptureAlbedo = false;

    /** Assumed albedo of a single-material reference strip used during RTY->W/m² calibration. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation && bNeutralizeCaptureAlbedo", ClampMin = "0.01", ClampMax = "1.0"))
    float ReferenceStripAssumedAlbedo = 1.0f;

    /** Assumed albedo of the ground half when a dual reference strip is used. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation && bNeutralizeCaptureAlbedo", ClampMin = "0.01", ClampMax = "1.0"))
    float ReferenceStripAssumedGroundAlbedo = 0.2f;

    /** Assumed albedo of the snow half when a dual reference strip is used. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation && bNeutralizeCaptureAlbedo", ClampMin = "0.01", ClampMax = "1.0"))
    float ReferenceStripAssumedSnowAlbedo = 0.85f;

    /** Lower bound for per-cell albedo when neutralizing captured radiance. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation && bNeutralizeCaptureAlbedo", ClampMin = "0.01", ClampMax = "1.0"))
    float MinSurfaceAlbedoForNeutralization = 0.05f;

    /** Clamp for the albedo neutralization multiplier to avoid unstable spikes. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (EditCondition = "bUseUERadiationIndex && bUseFluxCalibratedUERadiation && bNeutralizeCaptureAlbedo", ClampMin = "1.0"))
    float MaxAlbedoNeutralizationFactor = 3.0f;
};

USTRUCT(BlueprintType)
struct FFSM2AtmosphereParameters
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Atmosphere")
    float SurfacePressure_Pa = 90000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Atmosphere")
    float BulkTransferCoefficient = 0.0013f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Atmosphere")
    float MinimumWindSpeed_mps = 0.1f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Atmosphere")
    float TemperatureMeasurementHeight_m = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Atmosphere")
    float WindMeasurementHeight_m = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Atmosphere")
    EFSM2HeightReference MeasurementHeightReference = EFSM2HeightReference::AboveGround;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation")
    EFSM2ShortwavePartition ShortwavePartition = EFSM2ShortwavePartition::DiffuseOnly; // SWPART=0 (Fortran default)

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation")
    float LatitudeDegrees = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation")
    float SolarNoonLocalTime = 12.0f;
};

USTRUCT(BlueprintType)
struct FFSM2VegetationParameters
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Albedo")
    float DenseCanopyAlbedo = 0.1f; // acn0

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Albedo")
    float SnowCoveredDenseCanopyAlbedo = 0.3f; // acns

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optics")
    float CanopyElementReflectivity = 0.27f; // avg0

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optics")
    float CanopySnowReflectivity = 0.65f; // avgs

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Heat")
    float VegetationHeatCapacityPerVAI_Jm2K = 3.6e4f; // cvai

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Unloading")
    float ExponentialUnloadingTimeScale_s = 240.0f * 3600.0f; // eunl

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture")
    float SnowFreeMoistureConductance_ms = 0.01f; // gsnf

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
    float CanopyBaseHeight_m = 2.0f; // hbas

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
    float CanopyHeight_m = 0.0f; // vegh

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optics")
    float LightExtinctionCoefficient = 0.5f; // kext

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aerodynamics")
    float LeafBoundaryResistance = 20.0f; // leaf

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Unloading")
    float MeltUnloadingFraction = 0.4f; // munl

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storage")
    float SnowCapacityPerVAI_kgm2 = 4.4f; // svai

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Unloading")
    float TemperatureUnloadingParameter = 1.87e5f; // Tunl

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Unloading")
    float WindUnloadingParameter_m = 1.56e5f; // Uunl

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aerodynamics")
    float CanopyWindDecay = 2.5f; // wcan

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ShortwaveTransmittance = 0.75f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiation")
    float LongwaveAddition_Wm2 = 35.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SnowInterceptionFraction = 0.15f;
};

USTRUCT(BlueprintType)
struct FFSM2SoilParameters
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Composition", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ClayFraction = 0.3f; // fcly

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Composition", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SandFraction = 0.6f; // fsnd

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface")
    float SaturatedSurfaceConductance_ms = 0.01f; // gsat

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface")
    float SnowFreeRoughnessLength_m = 0.1f; // z0sf

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal")
    float GroundConductivity_WmK = 1.25f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal")
    // FSM2 default spin-up soil temperature (Tprf = 285 K)
    float GroundTemperature_K = 285.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal")
    float ThermalConductivity_WmK = 1.5f; // hcon_soil

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal")
    float VolumetricHeatCapacityDry_Jm3K = 2.2e6f; // hcap_soil

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology")
    float SaturatedMatricPotential_m = 0.0f; // sathh

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology")
    float CriticalVolumetricWater = 0.0f; // Vcrit

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ClampMin = "0.0"))
    float SurfaceMoistureConductance_ms = 1e-6f; // gs1 - surface moisture conductance for latent heat limitation

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology")
    float SaturatedVolumetricWater = 0.0f; // Vsat

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology")
    float ClappHornbergerExponent = 0.0f; // b

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Initial")
    // FSM2 namelist default (Tprf = 285 K)
    float InitialTemperature_K = 285.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Initial", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float InitialMoisture_Vol = 0.3f;
};

USTRUCT(BlueprintType)
struct FFSM2ModuleSettings
{
    GENERATED_BODY()

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Modules")
    bool bEnableGroundHeatFlux = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Modules")
    bool bEnableRefreezing = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Modules")
    bool bEnableCanopy = false;
};

USTRUCT(BlueprintType)
struct FFSM2DiagnosticSettings
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Diagnostics", meta = (AdvancedDisplay))
    bool bEnableDiagnostics = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Diagnostics", meta = (ClampMin = "1", AdvancedDisplay))
    int32 DiagnosticsEveryNSteps = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Diagnostics")
    bool bAppendDiagnostics = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Diagnostics", meta = (DisplayName = "Lean Post-Run Bundle Diagnostics", ToolTip = "When enabled, FSM2 writes only the state and radiation CSV columns consumed by the post-run plot bundle. This reduces diagnostics disk I/O while keeping bundle plots working."))
    bool bWriteLeanPostRunBundleDiagnostics = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Diagnostics")
    FString DiagnosticsFileName = TEXT("FSM2Diagnostics.csv");

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Diagnostics", meta = (AdvancedDisplay))
    FString DiagnosticsDirectory = TEXT("analysis_results");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Diagnostics")
    bool bTrackAllCellsWhenEmpty = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Diagnostics", meta = (AdvancedDisplay))
    TArray<int32> DiagnosticsTrackedCellIndices;
};

USTRUCT(BlueprintType)
struct FFSM2EnsembleParameters
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ensemble")
    float PrecipitationMultiplier = 1.0f; // Pmlt

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ensemble")
    float TemperatureOffset_K = 0.0f; // Tadd

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ensemble|Lapse Rate")
    bool bDisableLapseRateAdjustments = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ensemble|Lapse Rate")
    bool bApplyPrecipLapseBelowStation = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ensemble|Lapse Rate")
    float TemperatureLapseRate_CPer100m = -0.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ensemble|Lapse Rate")
    float PrecipitationLapseRate_FractionPerKm = 0.30f;
};

USTRUCT(BlueprintType)
struct FFSM2ModelParameters
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Constants")
    FFSM2PhysicalConstants PhysicalConstants;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Layers")
    FFSM2LayerConfiguration Layers;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Snow")
    FFSM2SnowParameters Snow;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Radiation")
    FFSM2RadiationParameters Radiation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Atmosphere")
    FFSM2AtmosphereParameters Atmosphere;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Vegetation")
    FFSM2VegetationParameters Vegetation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Soil")
    FFSM2SoilParameters Soil;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "FSM2|Modules")
    FFSM2ModuleSettings Modules;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Diagnostics")
    FFSM2DiagnosticSettings Diagnostics;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Ensemble")
    FFSM2EnsembleParameters Ensemble;
};
