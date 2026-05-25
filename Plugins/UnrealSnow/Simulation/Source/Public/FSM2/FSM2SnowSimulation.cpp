#include "FSM2SnowSimulation.h"
#include "GeoReferencingSystem.h"
#include "SnowSimulationActor.h"
#include "SimulationWeatherDataProviderBase.h"
#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"

#if WITH_EDITOR && WITH_SUNSKY_SUPPORT
#include "EngineUtils.h"
#include "SunPosition.h"
#endif

namespace
{
	constexpr float FreezePoint_K = 273.15f;
	constexpr float KelvinEpsilon = 1e-3f;
    constexpr float MinHeatCapacity_Jm2K = 1.0f;
    // Use a thin numerical floor so very shallow layers (millimetre scale) are not artificially inflated.
    constexpr float MinimumLayerThickness_m = 1.0e-4f;
    constexpr float SaturationTolerance = 1e-6f;
    constexpr float MinRoughnessLength_m = 1.0e-5f;
    constexpr float DualReferenceSnowCoverThreshold = 1.0e-3f;
    constexpr float FSM2_DiffuseNoGIReferenceAbsoluteFloor = 1.0e-4f;
    constexpr float FSM2_DiffuseNoGIReferenceRelativeFloor = 1.0e-2f;
    constexpr float FSM2_DiffuseNoGIReferenceMinTotalFraction = 1.0e-2f;
    constexpr uint8 ReferenceInvalidReason_None = 0u;
    constexpr uint8 ReferenceInvalidReason_UnusableSelectedValues = 1u;
    constexpr uint8 ReferenceInvalidReason_SunBelowMinElevation = 2u;
    constexpr uint8 ReferenceInvalidReason_TotalBelowMinLuminance = 3u;
    constexpr uint8 ReferenceInvalidReason_DirectBelowMinLuminance = 4u;
    constexpr uint8 ReferenceInvalidReason_DiffuseBelowMinLuminance = 5u;
    constexpr uint8 ReferenceInvalidReason_ActorReferenceUnavailable = 6u;
    constexpr uint8 ReferenceInvalidReason_NoIncomingShortwave = 7u;

    inline const TCHAR* GetReferenceInvalidReasonText(uint8 ReasonCode)
    {
        switch (ReasonCode)
        {
        case ReferenceInvalidReason_None:
            return TEXT("Valid");
        case ReferenceInvalidReason_UnusableSelectedValues:
            return TEXT("SelectedValuesInvalid");
        case ReferenceInvalidReason_SunBelowMinElevation:
            return TEXT("SunBelowMinElevation");
        case ReferenceInvalidReason_TotalBelowMinLuminance:
            return TEXT("TotalBelowMinLuminance");
        case ReferenceInvalidReason_DirectBelowMinLuminance:
            return TEXT("DirectBelowMinLuminance");
        case ReferenceInvalidReason_DiffuseBelowMinLuminance:
            return TEXT("DiffuseBelowMinLuminance");
        case ReferenceInvalidReason_ActorReferenceUnavailable:
            return TEXT("ActorReferenceUnavailable");
        case ReferenceInvalidReason_NoIncomingShortwave:
            return TEXT("NoIncomingShortwave");
        default:
            return TEXT("Unknown");
        }
    }

    inline bool FSM2_HasUsableSkyOnlyReference(float SkyOnlyReference, float DiffuseReference, float TotalReference)
    {
        if (!FMath::IsFinite(SkyOnlyReference) || SkyOnlyReference <= FSM2_DiffuseNoGIReferenceAbsoluteFloor)
        {
            return false;
        }

        const float SafeDiffuseReference = FMath::IsFinite(DiffuseReference)
            ? FMath::Max(0.0f, DiffuseReference)
            : 0.0f;
        const float SafeTotalReference = FMath::IsFinite(TotalReference)
            ? FMath::Max(0.0f, TotalReference)
            : 0.0f;
        if (SafeDiffuseReference <= FSM2_DiffuseNoGIReferenceAbsoluteFloor
            || SafeTotalReference <= FSM2_DiffuseNoGIReferenceAbsoluteFloor)
        {
            return false;
        }

        return SkyOnlyReference >= (SafeDiffuseReference * FSM2_DiffuseNoGIReferenceRelativeFloor)
            && SkyOnlyReference >= (SafeTotalReference * FSM2_DiffuseNoGIReferenceMinTotalFraction);
    }

    inline bool IsSnowFreeForDualReference(const ASnowSimulationActor* Actor, int32 SnowLayerCount, float SnowCoverFraction, float SnowDepthMeters)
    {
        const float DepthThresholdMeters = Actor
            ? FMath::Max(0.0f, Actor->DualReferenceSnowDepthThreshold_m)
            : 0.0f;

        const bool bDepthValid = FMath::IsFinite(SnowDepthMeters);
        const bool bCoverValid = FMath::IsFinite(SnowCoverFraction);
        const bool bHasSnowDepth = bDepthValid && (SnowDepthMeters > DepthThresholdMeters);
        const bool bHasSnowCover = bCoverValid && (SnowCoverFraction > DualReferenceSnowCoverThreshold);

        // FSM2 can retain an ultra-thin numerical layer while depth/cover are effectively zero.
        // Prefer physical depth/cover for reference selection; use layer count only if both are invalid.
        if (!bDepthValid && !bCoverValid)
        {
            return SnowLayerCount <= 0;
        }

        return !(bHasSnowDepth || bHasSnowCover);
    }

    inline float ComputeDualReferenceSnowBlendWeight(const ASnowSimulationActor* Actor, int32 SnowLayerCount, float SnowCoverFraction, float SnowDepthMeters)
    {
        const bool bCoverValid = FMath::IsFinite(SnowCoverFraction);
        const bool bDepthValid = FMath::IsFinite(SnowDepthMeters);
        const float CoverWeight = bCoverValid ? FMath::Clamp(SnowCoverFraction, 0.0f, 1.0f) : 0.0f;

        float DepthWeight = 0.0f;
        if (bDepthValid)
        {
            const float DepthThresholdMeters = Actor
                ? FMath::Max(0.0f, Actor->DualReferenceSnowDepthThreshold_m)
                : 0.0f;
            if (DepthThresholdMeters > KINDA_SMALL_NUMBER)
            {
                const float BlendStartMeters = 0.5f * DepthThresholdMeters;
                const float BlendSpanMeters = FMath::Max(BlendStartMeters, KINDA_SMALL_NUMBER);
                const float Alpha = FMath::Clamp((SnowDepthMeters - BlendStartMeters) / BlendSpanMeters, 0.0f, 1.0f);
                DepthWeight = Alpha * Alpha * (3.0f - 2.0f * Alpha);
            }
            else
            {
                DepthWeight = SnowDepthMeters > 0.0f ? 1.0f : 0.0f;
            }
        }

        if (bCoverValid || bDepthValid)
        {
            // Bias toward the snow strip whenever either state indicator says snow is present.
            return FMath::Clamp(FMath::Max(CoverWeight, DepthWeight), 0.0f, 1.0f);
        }

        return SnowLayerCount > 0 ? 1.0f : 0.0f;
    }

    inline float BlendDualReferenceValue(float GroundValue, float SnowValue, float SnowWeight, float FullStripFallback)
    {
        const bool bGroundValid = FMath::IsFinite(GroundValue) && GroundValue >= 0.0f;
        const bool bSnowValid = FMath::IsFinite(SnowValue) && SnowValue >= 0.0f;
        const float ClampedSnowWeight = FMath::Clamp(SnowWeight, 0.0f, 1.0f);

        if (bGroundValid && bSnowValid)
        {
            return FMath::Lerp(GroundValue, SnowValue, ClampedSnowWeight);
        }
        if (bSnowValid)
        {
            return SnowValue;
        }
        if (bGroundValid)
        {
            return GroundValue;
        }
        return FMath::Max(0.0f, FullStripFallback);
    }

    inline bool TryComputeDirectReferenceIndex(float SampleDirectRTY, float ReferenceDirectLuminance, float& OutDirectIndex)
    {
        if (!FMath::IsFinite(SampleDirectRTY) || SampleDirectRTY < 0.0f
            || !FMath::IsFinite(ReferenceDirectLuminance) || ReferenceDirectLuminance <= KINDA_SMALL_NUMBER)
        {
            OutDirectIndex = 0.0f;
            return false;
        }

        OutDirectIndex = SampleDirectRTY / ReferenceDirectLuminance;
        return FMath::IsFinite(OutDirectIndex) && OutDirectIndex >= 0.0f;
    }

    inline bool TryComputeRenderSurfaceSnowBlendWeight(
        float SurfaceStateValue,
        float GroundReferenceValue,
        float SnowReferenceValue,
        float& OutSnowBlendWeight)
    {
        if (!FMath::IsFinite(SurfaceStateValue) || SurfaceStateValue < 0.0f
            || !FMath::IsFinite(GroundReferenceValue) || GroundReferenceValue < 0.0f
            || !FMath::IsFinite(SnowReferenceValue) || SnowReferenceValue < 0.0f)
        {
            OutSnowBlendWeight = 0.0f;
            return false;
        }

        const float ReferenceDelta = SnowReferenceValue - GroundReferenceValue;
        if (!FMath::IsFinite(ReferenceDelta) || FMath::Abs(ReferenceDelta) <= 1.0e-4f)
        {
            OutSnowBlendWeight = 0.0f;
            return false;
        }

        OutSnowBlendWeight = FMath::Clamp((SurfaceStateValue - GroundReferenceValue) / ReferenceDelta, 0.0f, 1.0f);
        return FMath::IsFinite(OutSnowBlendWeight);
    }

    inline float StabilityPsiM(float z, float rL)
    {
        const float Zeta = FMath::Clamp(z * rL, -2.0f, 1.0f);
        if (Zeta > 0.0f)
        {
            return -5.0f * Zeta;
        }

        const float Arg = FMath::Max(1.0f - 16.0f * Zeta, 1.0e-6f);
        const float X = FMath::Pow(Arg, 0.25f);
        const float HalfPi = 0.5f * PI;
        return 2.0f * FMath::Loge((1.0f + X) * 0.5f)
            + FMath::Loge((1.0f + X * X) * 0.5f)
            - 2.0f * FMath::Atan(X)
            + HalfPi;
    }

    inline float StabilityPsiH(float z, float rL)
    {
        const float Zeta = FMath::Clamp(z * rL, -2.0f, 1.0f);
        if (Zeta > 0.0f)
        {
            return -5.0f * Zeta;
        }

        const float Arg = FMath::Max(1.0f - 16.0f * Zeta, 1.0e-6f);
        const float X = FMath::Pow(Arg, 0.25f);
        return 2.0f * FMath::Loge((1.0f + X * X) * 0.5f);
    }
}

void FFSM2SnowColumn::Reset()
{
    LayerCount = 0;
    for (int32 i = 0; i < GFSM2MaxLayers; ++i)
    {
        Thickness_m[i] = 0.0f;
        Temperature_K[i] = FreezePoint_K;
        IceMass_kgm2[i] = 0.0f;
        LiquidMass_kgm2[i] = 0.0f;
        Density_kgm3[i] = 300.0f;
        GrainRadius_m[i] = 5.0e-5f;
        AgeHours[i] = 0.0f;
    }
}

void FFSM2SoilColumn::Reset()
{
    LayerCount = 0;
    for (int32 i = 0; i < GFSM2MaxSoilLayers; ++i)
    {
        Thickness_m[i] = 0.0f;
        // Align soil spin-up with FSM2 default profile (Tprf = 285 K)
        Temperature_K[i] = 285.0f;
        Moisture_VolumeFraction[i] = 0.3f;
    }
}

void FFSM2CanopyState::Reset()
{
    SnowStorage_kgm2 = 0.0f;
    LiquidStorage_kgm2 = 0.0f;
    Temperature_K = FreezePoint_K;
    SpecificHumidity = 0.0f;
}

void FFSM2ColumnState::Reset()
{
    Snow.Reset();
    Soil.Reset();
    Canopy.Reset();
    Runoff_kgm2 = 0.0f;
    SnowAlbedo = 0.0f;
    SurfaceAlbedo = 0.0f;
    SnowCoverFraction = 0.0f;
    DiffuseShortwave_Wm2 = 0.0f;
    DirectShortwave_Wm2 = 0.0f;
    TerrainShortwave_Wm2 = 0.0f;
    SurfaceTemperature_K = 285.0f;
}

UFSM2SnowSimulation::UFSM2SnowSimulation()
{
    ModelParameters.Snow.FreshSnowDensity_kgm3 = 100.0f;

    if (ModelParameters.Layers.SoilLayerThicknesses_m.Num() == 0)
    {
        ModelParameters.Layers.SoilLayerThicknesses_m = {0.1f, 0.2f, 0.4f, 0.8f};
    }

    // Keep snow surface roughness consistent with Fortran default (z0sn = 0.001 m)
    if (ModelParameters.Snow.SnowRoughnessLength_m < 0.001f)
    {
        ModelParameters.Snow.SnowRoughnessLength_m = 0.001f;
    }

    SanitizeLayerConfiguration();
}

void UFSM2SnowSimulation::PostLoad()
{
    Super::PostLoad();

    // Same-material dual-reference-strip construction cancels surface albedo in the RTY ratio,
    // so the scalar neutralisation factor would double-correct. Force it off for any saved
    // config that has it enabled alongside the UE radiation index path.
    if (ModelParameters.Radiation.bUseUERadiationIndex
        && ModelParameters.Radiation.bNeutralizeCaptureAlbedo)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[FSM2] Loaded config had bNeutralizeCaptureAlbedo=true with bUseUERadiationIndex=true. "
                 "Forcing bNeutralizeCaptureAlbedo=false: dual-reference same-material construction "
                 "already cancels surface albedo in the RTY ratio."));
        ModelParameters.Radiation.bNeutralizeCaptureAlbedo = false;
    }
}

FString UFSM2SnowSimulation::GetSimulationName() const
{
	return TEXT("FSM2");
}

void UFSM2SnowSimulation::Initialize(ASnowSimulationActor* SimulationActor, const TArray<FLandscapeCell>& Cells, float InitialMaxSnow, UWorld* World)
{
	const int32 DimX = SimulationActor ? SimulationActor->CellsDimensionX : 0;
	const int32 DimY = SimulationActor ? SimulationActor->CellsDimensionY : 0;
	float CellMeters = SimulationActor ? SimulationActor->GetMetersPerCell() : 0.0f;
	if (CellMeters <= KINDA_SMALL_NUMBER && Cells.Num() > 0)
	{
		CellMeters = FMath::Sqrt(FMath::Max(0.0f, Cells[0].AreaXY)) / 100.0f;
	}
	CellSpacingMeters = FMath::Max(CellMeters, KINDA_SMALL_NUMBER);
	InitializeGrid(DimX, DimY, CellSpacingMeters);
	EnsureStateSize(DimX * DimY);
	ResetState();
	SetTerrainMetadata(Cells, DimX, DimY);

    bDiagnosticsFileInitialized = false;
    bDiagnosticsHeaderWritten = false;
    DiagnosticsFilePath.Empty();
    bRadiationDiagnosticsHeaderWritten = false;
	RadiationDiagnosticsFilePath.Empty();
    RadiationConfigFilePath.Empty();
    SimulationStepCounter = 0;
    ElapsedSimulationSeconds = 0.0;
    MeasurementAltitudeCm = 0.0f;
    CellSpacingMeters = FMath::Max(CellSpacingMeters, KINDA_SMALL_NUMBER);

	// Store reference to simulation actor for accessing UE radiation indices
	OwningSimulationActor = SimulationActor;
}

void UFSM2SnowSimulation::Initialize_Implementation(int32 GX, int32 GY, float CellMeters)
{
	Super::Initialize_Implementation(GX, GY, CellMeters);
    CellSpacingMeters = FMath::Max(CellMeters, KINDA_SMALL_NUMBER);
    SanitizeLayerConfiguration();
	EnsureStateSize(GX * GY);
	ResetState();
    RefreshEnvironmentalMetadata(GetWorld());

    bDiagnosticsFileInitialized = false;
    bDiagnosticsHeaderWritten = false;
    DiagnosticsFilePath.Empty();
    bRadiationDiagnosticsHeaderWritten = false;
    RadiationDiagnosticsFilePath.Empty();
    RadiationConfigFilePath.Empty();
    SimulationStepCounter = 0;
    ElapsedSimulationSeconds = 0.0;
    MeasurementAltitudeCm = 0.0f;

    // Log parameter verification to ensure values match Fortran FSM2
    UE_LOG(LogTemp, Warning, TEXT("=== FSM2 PARAMETER VERIFICATION ==="));
    UE_LOG(LogTemp, Warning, TEXT("Scheme Settings:"));
    UE_LOG(LogTemp, Warning, TEXT("  GrainGrowth: %d (0=None, 1=TempDependent, 2=TempGradient) [Fortran SGRAIN=1]"),
        (int)ModelParameters.Snow.GrainGrowthScheme);
    UE_LOG(LogTemp, Warning, TEXT("  AlbedoScheme: %d (0=DiagTemp, 1=PrognosticAge) [Fortran ALBEDO=2]"),
        (int)ModelParameters.Snow.AlbedoScheme);
    UE_LOG(LogTemp, Warning, TEXT("  DensityScheme: %d (0=Fixed, 1=Age, 2=Overburden) [Fortran DENSTY=1]"),
        (int)ModelParameters.Snow.DensityScheme);
    UE_LOG(LogTemp, Warning, TEXT("  HydrologyScheme: %d (0=FreeDrain, 1=Bucket, 2=Darcy) [Fortran HYDROL=1]"),
        (int)ModelParameters.Snow.HydrologyScheme);
    UE_LOG(LogTemp, Warning, TEXT("  ConductivityScheme: %d (0=Fixed, 1=DensityDependent) [Fortran CONDCT=1]"),
        (int)ModelParameters.Snow.ConductivityScheme);

    UE_LOG(LogTemp, Warning, TEXT("Critical Parameters:"));
    UE_LOG(LogTemp, Warning, TEXT("  PrecipMultiplier: %.3f [Fortran default=1.0]"),
        ModelParameters.Ensemble.PrecipitationMultiplier);
    UE_LOG(LogTemp, Warning, TEXT("  TempOffset: %.3f K [Fortran default=0.0]"),
        ModelParameters.Ensemble.TemperatureOffset_K);
    UE_LOG(LogTemp, Warning, TEXT("  LapseAdjustmentsDisabled: %d | ApplyPrecipBelowStation: %d"),
        ModelParameters.Ensemble.bDisableLapseRateAdjustments ? 1 : 0,
        ModelParameters.Ensemble.bApplyPrecipLapseBelowStation ? 1 : 0);
    UE_LOG(LogTemp, Warning, TEXT("  TempLapseRate: %.3f C/100m | PrecipLapseRate: %.3f fraction/km"),
        ModelParameters.Ensemble.TemperatureLapseRate_CPer100m,
        ModelParameters.Ensemble.PrecipitationLapseRate_FractionPerKm);
    UE_LOG(LogTemp, Warning, TEXT("  FreshSnowDensity: %.1f kg/m3 [Fortran rhof=100]"),
        ModelParameters.Snow.FreshSnowDensity_kgm3);
    UE_LOG(LogTemp, Warning, TEXT("  FreshGrainRadius: %.2e m [Fortran rgr0=5e-5]"),
        ModelParameters.Snow.FreshSnowGrainRadius_m);

    UE_LOG(LogTemp, Warning, TEXT("Albedo Parameters:"));
    UE_LOG(LogTemp, Warning, TEXT("  MinAlbedo: %.2f [Fortran asmn=0.5]"),
        ModelParameters.Snow.MinimumSnowAlbedo);
    UE_LOG(LogTemp, Warning, TEXT("  MaxAlbedo: %.2f [Fortran asmx=0.85]"),
        ModelParameters.Snow.MaximumSnowAlbedo);
    UE_LOG(LogTemp, Warning, TEXT("  ColdTimescale: %.0f h [Fortran tcld=1000]"),
        ModelParameters.Snow.ColdSnowAlbedoTimescale_s / 3600.0f);
    UE_LOG(LogTemp, Warning, TEXT("  MeltTimescale: %.0f h [Fortran tmlt=100]"),
        ModelParameters.Snow.MeltSnowAlbedoTimescale_s / 3600.0f);
    UE_LOG(LogTemp, Warning, TEXT("  AlbedoDecayTemp: %.1f C [Fortran Talb=-2]"),
        ModelParameters.Snow.AlbedoDecayTemperature_C);

    UE_LOG(LogTemp, Warning, TEXT("Density Parameters:"));
    UE_LOG(LogTemp, Warning, TEXT("  MaxColdDensity: %.0f kg/m3 [Fortran rcld=300]"),
        ModelParameters.Snow.MaxColdSnowDensity_kgm3);
    UE_LOG(LogTemp, Warning, TEXT("  MaxMeltDensity: %.0f kg/m3 [Fortran rmlt=500]"),
        ModelParameters.Snow.MaxMeltSnowDensity_kgm3);
    UE_LOG(LogTemp, Warning, TEXT("  CompactionTime: %.0f h [Fortran trho=200]"),
        ModelParameters.Snow.CompactionTimescale_s / 3600.0f);

    UE_LOG(LogTemp, Warning, TEXT("Terrain-Derived Controls:"));
    UE_LOG(LogTemp, Warning, TEXT("  SlopeAdjustedSW: %d [Fortran N/A - flat terrain]"),
        ModelParameters.Radiation.bUseSlopeAdjustedShortwave);
    UE_LOG(LogTemp, Warning, TEXT("  UERadiationIndex: %d | FluxCalibrated: %d | TerrainInterreflection: %d"),
        ModelParameters.Radiation.bUseUERadiationIndex,
        ModelParameters.Radiation.bUseFluxCalibratedUERadiation,
        ModelParameters.Radiation.bUseTerrainInterreflection);
    UE_LOG(LogTemp, Warning, TEXT("  NeutralizeCaptureAlbedo: %d | RefAlb(single/ground/snow)=%.2f/%.2f/%.2f | MinAlb=%.2f | MaxFactor=%.2f"),
        ModelParameters.Radiation.bNeutralizeCaptureAlbedo ? 1 : 0,
        ModelParameters.Radiation.ReferenceStripAssumedAlbedo,
        ModelParameters.Radiation.ReferenceStripAssumedGroundAlbedo,
        ModelParameters.Radiation.ReferenceStripAssumedSnowAlbedo,
        ModelParameters.Radiation.MinSurfaceAlbedoForNeutralization,
        ModelParameters.Radiation.MaxAlbedoNeutralizationFactor);
    UE_LOG(LogTemp, Warning, TEXT("  SnowRedistribution: %d | DynamicGeometry: %d | MinSnowfall: %.2f mm | ConserveMass: %d | Start/ZeroSlope: %.1f/%.1f deg | CurvGain: %.2f | FactorRange: %.2f-%.2f"),
        ModelParameters.Snow.bApplySlopeCurvatureRedistribution ? 1 : 0,
        bUseDynamicSurfaceGeometry ? 1 : 0,
        ModelParameters.Snow.MinSnowfallForRedistribution_mm,
        ModelParameters.Snow.bConserveMassDuringRedistribution ? 1 : 0,
        ModelParameters.Snow.SlopeRedistributionStartDeg,
        ModelParameters.Snow.SlopeRedistributionZeroDeg,
        ModelParameters.Snow.CurvatureRedistributionGain,
        ModelParameters.Snow.MinRedistributionFactor,
        ModelParameters.Snow.MaxRedistributionFactor);

    UE_LOG(LogTemp, Warning, TEXT("Other Key Parameters:"));
    UE_LOG(LogTemp, Warning, TEXT("  IrreducibleWater: %.3f [Fortran Wirr=0.03]"),
        ModelParameters.Snow.IrreducibleWaterFraction);
    UE_LOG(LogTemp, Warning, TEXT("  ThermalMetamorphism: %.2e s^-1 [Fortran snda=2.8e-6]"),
        ModelParameters.Snow.ThermalMetamorphismRate_s);
    UE_LOG(LogTemp, Warning, TEXT("  SnowRoughness: %.4f m [Fortran z0sn=0.001]"),
        ModelParameters.Snow.SnowRoughnessLength_m);
    UE_LOG(LogTemp, Warning, TEXT("================================="));
}

void UFSM2SnowSimulation::Simulate(ASnowSimulationActor* SimulationActor, int32 CurrentSimulationStep, int32 Timesteps, bool SaveSnowMap, bool CaptureDebugInformation, TArray<FDebugCell>& DebugCells)
{
	if (!SimulationActor)
	{
		return;
	}

	UWorld* World = SimulationActor->GetWorld();
	RefreshEnvironmentalMetadata(World);

	FWeatherForcingData Forcing;
	if (SimulationActor->WeatherProvider)
	{
		Forcing = SimulationActor->WeatherProvider->GetWeatherForcing(SimulationActor->GetWeatherQueryTimeUtc());
        MeasurementAltitudeCm = SimulationActor->WeatherProvider->GetMeasurementAltitude();
	}

	Step(SimulationActor->TimeStepSeconds, Forcing, DepthMeters);
	UploadDepthToTexture();
}

void UFSM2SnowSimulation::Step(float DtSeconds, const FWeatherForcingData& Forcing, TArray<float>& OutDepthMeters)
{
    if (DtSeconds <= 0.0f || OutDepthMeters.Num() == 0)
    {
        return;
    }

    // Log parameters once at first step
    static bool bParametersLogged = false;
    if (!bParametersLogged)
    {
        bParametersLogged = true;
        UE_LOG(LogTemp, Warning, TEXT(""));
        UE_LOG(LogTemp, Warning, TEXT("=== FSM2 PARAMETER VERIFICATION ==="));
        UE_LOG(LogTemp, Warning, TEXT("Scheme Settings:"));
        UE_LOG(LogTemp, Warning, TEXT("  GrainGrowth: %d (0=None, 1=TempDependent, 2=TempGradient) [Fortran SGRAIN=1]"),
            (int)ModelParameters.Snow.GrainGrowthScheme);
        UE_LOG(LogTemp, Warning, TEXT("  AlbedoScheme: %d (0=DiagTemp, 1=PrognosticAge) [Fortran ALBEDO=2]"),
            (int)ModelParameters.Snow.AlbedoScheme);
        UE_LOG(LogTemp, Warning, TEXT("  DensityScheme: %d (0=Fixed, 1=Age, 2=Overburden) [Fortran DENSTY=1]"),
            (int)ModelParameters.Snow.DensityScheme);
        UE_LOG(LogTemp, Warning, TEXT("  HydrologyScheme: %d (0=FreeDrain, 1=Bucket, 2=Darcy) [Fortran HYDROL=1]"),
            (int)ModelParameters.Snow.HydrologyScheme);
        UE_LOG(LogTemp, Warning, TEXT("  ConductivityScheme: %d (0=Fixed, 1=DensityDependent) [Fortran CONDCT=1]"),
            (int)ModelParameters.Snow.ConductivityScheme);

        UE_LOG(LogTemp, Warning, TEXT("Critical Parameters:"));
        UE_LOG(LogTemp, Warning, TEXT("  PrecipMultiplier: %.3f [Fortran default=1.0]"),
            ModelParameters.Ensemble.PrecipitationMultiplier);
        UE_LOG(LogTemp, Warning, TEXT("  TempOffset: %.3f K [Fortran default=0.0]"),
            ModelParameters.Ensemble.TemperatureOffset_K);
        UE_LOG(LogTemp, Warning, TEXT("  LapseAdjustmentsDisabled: %d | ApplyPrecipBelowStation: %d"),
            ModelParameters.Ensemble.bDisableLapseRateAdjustments ? 1 : 0,
            ModelParameters.Ensemble.bApplyPrecipLapseBelowStation ? 1 : 0);
        UE_LOG(LogTemp, Warning, TEXT("  TempLapseRate: %.3f C/100m | PrecipLapseRate: %.3f fraction/km"),
            ModelParameters.Ensemble.TemperatureLapseRate_CPer100m,
            ModelParameters.Ensemble.PrecipitationLapseRate_FractionPerKm);
        UE_LOG(LogTemp, Warning, TEXT("  FreshSnowDensity: %.1f kg/m3 [Fortran rhof=100]"),
            ModelParameters.Snow.FreshSnowDensity_kgm3);
        UE_LOG(LogTemp, Warning, TEXT("  FreshGrainRadius: %.2e m [Fortran rgr0=5e-5]"),
            ModelParameters.Snow.FreshSnowGrainRadius_m);

        UE_LOG(LogTemp, Warning, TEXT("Albedo Parameters:"));
        UE_LOG(LogTemp, Warning, TEXT("  MinAlbedo: %.2f [Fortran asmn=0.5]"),
            ModelParameters.Snow.MinimumSnowAlbedo);
        UE_LOG(LogTemp, Warning, TEXT("  MaxAlbedo: %.2f [Fortran asmx=0.85]"),
            ModelParameters.Snow.MaximumSnowAlbedo);
        UE_LOG(LogTemp, Warning, TEXT("  ColdTimescale: %.0f h [Fortran tcld=1000]"),
            ModelParameters.Snow.ColdSnowAlbedoTimescale_s / 3600.0f);
        UE_LOG(LogTemp, Warning, TEXT("  MeltTimescale: %.0f h [Fortran tmlt=100]"),
            ModelParameters.Snow.MeltSnowAlbedoTimescale_s / 3600.0f);
        UE_LOG(LogTemp, Warning, TEXT("  AlbedoDecayTemp: %.1f C [Fortran Talb=-2]"),
            ModelParameters.Snow.AlbedoDecayTemperature_C);

        UE_LOG(LogTemp, Warning, TEXT("Density Parameters:"));
        UE_LOG(LogTemp, Warning, TEXT("  MaxColdDensity: %.0f kg/m3 [Fortran rcld=300]"),
            ModelParameters.Snow.MaxColdSnowDensity_kgm3);
        UE_LOG(LogTemp, Warning, TEXT("  MaxMeltDensity: %.0f kg/m3 [Fortran rmlt=500]"),
            ModelParameters.Snow.MaxMeltSnowDensity_kgm3);
        UE_LOG(LogTemp, Warning, TEXT("  CompactionTime: %.0f h [Fortran trho=200]"),
            ModelParameters.Snow.CompactionTimescale_s / 3600.0f);

        UE_LOG(LogTemp, Warning, TEXT("Terrain-Derived Controls:"));
        UE_LOG(LogTemp, Warning, TEXT("  SlopeAdjustedSW: %d [Fortran N/A - flat terrain]"),
            ModelParameters.Radiation.bUseSlopeAdjustedShortwave);
        UE_LOG(LogTemp, Warning, TEXT("  UERadiationIndex: %d | FluxCalibrated: %d | TerrainInterreflection: %d"),
            ModelParameters.Radiation.bUseUERadiationIndex,
            ModelParameters.Radiation.bUseFluxCalibratedUERadiation,
            ModelParameters.Radiation.bUseTerrainInterreflection);
        UE_LOG(LogTemp, Warning, TEXT("  NeutralizeCaptureAlbedo: %d | RefAlb(single/ground/snow)=%.2f/%.2f/%.2f | MinAlb=%.2f | MaxFactor=%.2f"),
            ModelParameters.Radiation.bNeutralizeCaptureAlbedo ? 1 : 0,
            ModelParameters.Radiation.ReferenceStripAssumedAlbedo,
            ModelParameters.Radiation.ReferenceStripAssumedGroundAlbedo,
            ModelParameters.Radiation.ReferenceStripAssumedSnowAlbedo,
            ModelParameters.Radiation.MinSurfaceAlbedoForNeutralization,
            ModelParameters.Radiation.MaxAlbedoNeutralizationFactor);

        UE_LOG(LogTemp, Warning, TEXT("Other Key Parameters:"));
        UE_LOG(LogTemp, Warning, TEXT("  IrreducibleWater: %.3f [Fortran Wirr=0.03]"),
            ModelParameters.Snow.IrreducibleWaterFraction);
        UE_LOG(LogTemp, Warning, TEXT("  ThermalMetamorphism: %.2e s^-1 [Fortran snda=2.8e-6]"),
            ModelParameters.Snow.ThermalMetamorphismRate_s);
        UE_LOG(LogTemp, Warning, TEXT("  SnowRoughness: %.4f m [Fortran z0sn=0.001]"),
            ModelParameters.Snow.SnowRoughnessLength_m);
        UE_LOG(LogTemp, Warning, TEXT("================================="));
        UE_LOG(LogTemp, Warning, TEXT(""));
    }

    RefreshEnvironmentalMetadata(GetWorld());

    EnsureStateSize(OutDepthMeters.Num());
    CachedAlbedo.SetNum(OutDepthMeters.Num(), EAllowShrinking::No);

    SanitizeLayerConfiguration();

    if (ASnowSimulationActor* Actor = OwningSimulationActor.Get())
    {
        if (Actor->WeatherProvider)
        {
            MeasurementAltitudeCm = Actor->WeatherProvider->GetMeasurementAltitude();
        }
    }

    const float PressureSource = (Forcing.Pressure_Pa > 100.0f)
        ? Forcing.Pressure_Pa
        : ModelParameters.Atmosphere.SurfacePressure_Pa;
    const float Pressure = FMath::Max(10100.0f, PressureSource);
	const float BaseAirTempK = FMath::Clamp(Forcing.Temperature_K, 200.0f, 320.0f);

    // DEBUG: Log input temperature for specific date to debug flux discrepancy
    // Relaxed check: any time within the 12th hour of May 15th 2017
    if (Forcing.Timestamp.GetYear() == 2017 && Forcing.Timestamp.GetMonth() == 5 && Forcing.Timestamp.GetDay() == 15 && Forcing.Timestamp.GetHour() == 12)
    {
         UE_LOG(LogTemp, Warning, TEXT("[FSM2 DEBUG] Step 2017-05-15 12:XX: Forcing.Temperature_K=%.2f (%.2f C), AirTempK=%.2f"), 
             Forcing.Temperature_K, Forcing.Temperature_K - 273.15f, BaseAirTempK);
    }
    const float Wind = FMath::Max(0.0f, Forcing.Wind_mps);
    const float BaseRelativeHumidity = FMath::Clamp(Forcing.RH_01, 0.0f, 1.0f);
    const float BasePrecipRate = FMath::Max(0.0f, Forcing.PrecipRate_kgm2s);
    const float BaseExplicitSnow_kgm2 = FMath::Max(0.0f, Forcing.SnowRate_kgm2s) * DtSeconds;
    const float BaseExplicitRain_kgm2 = FMath::Max(0.0f, Forcing.RainRate_kgm2s) * DtSeconds;
    const float BaseExplicitTotal_kgm2 = BaseExplicitSnow_kgm2 + BaseExplicitRain_kgm2;
    const float BasePrecipitation_kgm2 = Forcing.bHasExplicitPrecipitation
        ? BaseExplicitTotal_kgm2
        : (BasePrecipRate * DtSeconds);
    float BaseSnowFractionForRedistribution = 0.0f;
    if (BasePrecipitation_kgm2 > KINDA_SMALL_NUMBER)
    {
        constexpr float RedistributionTwarmC = 0.5f;
        constexpr float RedistributionTcoldC = -0.5f;
        if (Forcing.bHasExplicitPrecipitation && BaseExplicitTotal_kgm2 > KINDA_SMALL_NUMBER)
        {
            BaseSnowFractionForRedistribution = FMath::Clamp(BaseExplicitSnow_kgm2 / BaseExplicitTotal_kgm2, 0.0f, 1.0f);
        }
        else
        {
            BaseSnowFractionForRedistribution = FMath::Clamp(Forcing.SnowFrac_01, 0.0f, 1.0f);
        }

        const float BaseTempC = BaseAirTempK - FreezePoint_K;
        if (BaseTempC >= RedistributionTwarmC)
        {
            BaseSnowFractionForRedistribution = 0.0f;
        }
        else if (BaseTempC <= RedistributionTcoldC)
        {
            BaseSnowFractionForRedistribution = 1.0f;
        }
    }
    const float BaseSnowfallForRedistribution_kgm2 = BasePrecipitation_kgm2 * BaseSnowFractionForRedistribution;
    const float FreshDensity = FMath::Clamp(ModelParameters.Snow.FreshSnowDensity_kgm3, ModelParameters.Snow.MinSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3);
    const float FreshGrain = FMath::Clamp(ModelParameters.Snow.FreshSnowGrainRadius_m, 1.0e-6f, 1.0e-2f);
    const int32 CellCount = OutDepthMeters.Num();

    TArray<float> SnowRedistributionFactors;
    float SnowRedistributionConservationFactor = 1.0f;
    const bool bApplySnowRedistribution = ModelParameters.Snow.bApplySlopeCurvatureRedistribution
        && bHasTerrainMetadata
        && BaseSnowfallForRedistribution_kgm2 >= ModelParameters.Snow.MinSnowfallForRedistribution_mm
        && CellCount > 0;

    if (ModelParameters.Snow.bApplySlopeCurvatureRedistribution && !bHasTerrainMetadata && BaseSnowfallForRedistribution_kgm2 >= ModelParameters.Snow.MinSnowfallForRedistribution_mm)
    {
        static bool bLoggedMissingTerrainMetadata = false;
        if (!bLoggedMissingTerrainMetadata)
        {
            bLoggedMissingTerrainMetadata = true;
            UE_LOG(LogTemp, Warning, TEXT("[FSM2] Snow redistribution enabled but terrain metadata is unavailable; falling back to neutral factors."));
        }
    }

    auto GetRedistributionEdgeWeight = [this](int32 CellIdx) -> float
    {
        const int32 EdgeFadeCells = ModelParameters.Snow.RedistributionEdgeFadeCells;
        if (EdgeFadeCells <= 0 || GridX <= 0 || GridY <= 0)
        {
            return 1.0f;
        }

        const int32 X = CellIdx % GridX;
        const int32 Y = CellIdx / GridX;
        if (X < 0 || Y < 0 || X >= GridX || Y >= GridY)
        {
            return 1.0f;
        }

        const int32 DistToEdge = FMath::Min(
            FMath::Min(X, GridX - 1 - X),
            FMath::Min(Y, GridY - 1 - Y));

        if (DistToEdge >= EdgeFadeCells)
        {
            return 1.0f;
        }

        return FMath::Clamp(static_cast<float>(DistToEdge) / static_cast<float>(EdgeFadeCells), 0.0f, 1.0f);
    };

    if (bApplySnowRedistribution)
    {
        SnowRedistributionFactors.SetNumUninitialized(CellCount);

        const float StartDeg = FMath::Max(0.0f, ModelParameters.Snow.SlopeRedistributionStartDeg);
        const float ZeroDeg = FMath::Max(StartDeg + KINDA_SMALL_NUMBER, ModelParameters.Snow.SlopeRedistributionZeroDeg);
        const float CurvatureGain = ModelParameters.Snow.CurvatureRedistributionGain;
        const float MinFactor = FMath::Max(0.0f, ModelParameters.Snow.MinRedistributionFactor);
        const float MaxFactor = FMath::Max(MinFactor + KINDA_SMALL_NUMBER, ModelParameters.Snow.MaxRedistributionFactor);

        double TotalBefore = 0.0;
        double TotalAfter = 0.0;

        for (int32 Idx = 0; Idx < CellCount; ++Idx)
        {
            const float SlopeDeg = TerrainSlopeDegrees.IsValidIndex(Idx) ? TerrainSlopeDegrees[Idx] : 0.0f;
            const float Curvature = TerrainCurvature.IsValidIndex(Idx) ? TerrainCurvature[Idx] : 0.0f;

            const float SlopeFactor = (SlopeDeg <= StartDeg)
                ? 0.0f
                : FMath::Clamp((SlopeDeg - StartDeg) / (ZeroDeg - StartDeg), 0.0f, 1.0f);
            const float CurvatureFactor = 1.0f + CurvatureGain * Curvature;
            const float RawFactor = (1.0f - SlopeFactor) * CurvatureFactor;
            const float ClampedFactor = FMath::Clamp(RawFactor, MinFactor, MaxFactor);
            const float EdgeWeight = GetRedistributionEdgeWeight(Idx);
            const float EffectiveFactor = FMath::Lerp(1.0f, ClampedFactor, EdgeWeight);
            SnowRedistributionFactors[Idx] = EffectiveFactor;

            const bool bEdgeCell = ModelParameters.Snow.bExcludeEdgeCellsFromMassConservation
                && (EdgeWeight < (1.0f - KINDA_SMALL_NUMBER));
            if (!bEdgeCell)
            {
                TotalBefore += 1.0;
                TotalAfter += EffectiveFactor;
            }
        }

        if (ModelParameters.Snow.bConserveMassDuringRedistribution && TotalAfter > KINDA_SMALL_NUMBER)
        {
            SnowRedistributionConservationFactor = static_cast<float>(TotalBefore / TotalAfter);
        }

        static int32 RedistributionLogCounter = 0;
        if ((RedistributionLogCounter++ % 100) == 0)
        {
            UE_LOG(LogTemp, Display, TEXT("[FSM2] Snow redistribution: Enabled=%d, MinSnowfall=%.2f mm, ConserveMass=%d, Normalization=%.4f"),
                ModelParameters.Snow.bApplySlopeCurvatureRedistribution ? 1 : 0,
                ModelParameters.Snow.MinSnowfallForRedistribution_mm,
                ModelParameters.Snow.bConserveMassDuringRedistribution ? 1 : 0,
                SnowRedistributionConservationFactor);
        }
    }

    const bool bCollectDiagnostics = ModelParameters.Diagnostics.bEnableDiagnostics && ModelParameters.Diagnostics.DiagnosticsEveryNSteps > 0;
    TArray<FFSM2CellDiagnostics> DiagnosticsBuffer;
    TArray<int32> ValidTrackedIndices;
    TArray<bool> TrackedFlags;

    static bool bLoggedDiagnosticsStatus = false;
    if (!bLoggedDiagnosticsStatus)
    {
        const ASnowSimulationActor* ActorPtrForDiag = OwningSimulationActor.Get();
        UE_LOG(LogTemp, Display, TEXT("[FSM2] Diagnostics config: Enable=%d EveryNSteps=%d TrackedIndices=%d TrackAllWhenEmpty=%d RadiationDiag=%d"),
            ModelParameters.Diagnostics.bEnableDiagnostics ? 1 : 0,
            ModelParameters.Diagnostics.DiagnosticsEveryNSteps,
            ModelParameters.Diagnostics.DiagnosticsTrackedCellIndices.Num(),
            ModelParameters.Diagnostics.bTrackAllCellsWhenEmpty ? 1 : 0,
            (ActorPtrForDiag && ActorPtrForDiag->bEnableRadiationDiagnostics) ? 1 : 0);
        bLoggedDiagnosticsStatus = true;
    }

    if (bCollectDiagnostics)
    {
        TrackedFlags.Init(false, CellCount);

        if (ModelParameters.Diagnostics.DiagnosticsTrackedCellIndices.Num() == 0)
        {
            if (ModelParameters.Diagnostics.bTrackAllCellsWhenEmpty)
            {
                ValidTrackedIndices.Reserve(CellCount);
                for (int32 Index = 0; Index < CellCount; ++Index)
                {
                    if (!TrackedFlags[Index])
                    {
                        TrackedFlags[Index] = true;
                        ValidTrackedIndices.Add(Index);
                    }
                }
            }
            else if (CellCount > 0)
            {
                // Track first, middle, and last cells
                const int32 FirstIndex = 0;
                const int32 CenterY = (GridY > 0) ? (GridY / 2) : 0;
                const int32 CenterX = (GridX > 0) ? (GridX / 2) : 0;
                const int32 MiddleIndex = FMath::Clamp(CenterY * GridX + CenterX, 0, CellCount - 1);
                const int32 LastIndex = CellCount - 1;

                TArray<int32> DefaultIndices = {FirstIndex, MiddleIndex, LastIndex};
                for (int32 Index : DefaultIndices)
                {
                    if (Index >= 0 && Index < CellCount && !TrackedFlags[Index])
                    {
                        TrackedFlags[Index] = true;
                        ValidTrackedIndices.Add(Index);
                    }
                }
            }
        }
        else
        {
            for (int32 RawIndex : ModelParameters.Diagnostics.DiagnosticsTrackedCellIndices)
            {
                if (RawIndex >= 0 && RawIndex < CellCount && !TrackedFlags[RawIndex])
                {
                    TrackedFlags[RawIndex] = true;
                    ValidTrackedIndices.Add(RawIndex);
                }
            }

            if (ValidTrackedIndices.Num() == 0 && CellCount > 0)
            {
                TrackedFlags[0] = true;
                ValidTrackedIndices.Add(0);
            }
        }

        DiagnosticsBuffer.Reserve(FMath::Max(ValidTrackedIndices.Num(), 1));
    }

	// Parallelized across cells. Forced single-thread when collecting diagnostics because
	// DiagnosticsBuffer.Emplace_GetRef() grows a shared TArray (not thread-safe).
	const EParallelForFlags FSM2MainLoopFlags = bCollectDiagnostics
		? EParallelForFlags::ForceSingleThread
		: EParallelForFlags::None;
	ParallelFor(OutDepthMeters.Num(), [&](int32 Index)
	{
        FFSM2ColumnState& Column = CellStates[Index];
        Column.Runoff_kgm2 = 0.0f;

        const float DtHours = DtSeconds / 3600.0f;
        AgeSnowpack(Column.Snow, DtHours);

        Column.SnowCoverFraction = ComputeSnowCoverFraction(Column.Snow);
        const float SnowDepthForReference_m = ComputeSnowDepthMeters(Column.Snow);

        float NetSurfaceFlux = 0.0f;
        float SurfaceTemperatureK = Column.SurfaceTemperature_K;
        if (SurfaceTemperatureK <= 0.0f)
        {
             SurfaceTemperatureK = Column.Snow.LayerCount > 0
                ? FMath::Clamp(Column.Snow.Temperature_K[0], ModelParameters.Snow.MinSnowTemperature_K, FreezePoint_K)
                : FMath::Max(ModelParameters.Soil.GroundTemperature_K, ModelParameters.Snow.MinSnowTemperature_K);
        }
        float ThroughfallSnowOut = 0.0f;
        float ThroughfallRainOut = 0.0f;
        FFSM2EnergyFluxes SurfaceFluxes;
        FFSM2SurfaceUpdate SurfaceUpdate;
        FFSM2CellDiagnostics* DiagnosticsPtr = nullptr;

        const bool bTrackCell = bCollectDiagnostics && TrackedFlags.IsValidIndex(Index) && TrackedFlags[Index];
        if (bTrackCell)
        {
            FFSM2CellDiagnostics& NewDiag = DiagnosticsBuffer.Emplace_GetRef();
            NewDiag.Reset();
            NewDiag.StepIndex = SimulationStepCounter;
            NewDiag.CellIndex = Index;
            NewDiag.SimulationTimeSeconds = ElapsedSimulationSeconds;
            DiagnosticsPtr = &NewDiag;
        }

        float LocalAirTempK = BaseAirTempK;
        float LocalPrecipitation_kgm2 = BasePrecipitation_kgm2;
        ApplyLapseRateAdjustments(Index, BaseAirTempK, BasePrecipitation_kgm2, LocalAirTempK, LocalPrecipitation_kgm2);

        float LocalRH = BaseRelativeHumidity;
        if (LocalAirTempK < FreezePoint_K)
        {
            const float TempC = LocalAirTempK - FreezePoint_K;
            const float RHminIce = FMath::Exp(22.4422f * TempC / (272.186f + TempC)) /
                FMath::Exp(17.5043f * TempC / (241.3f + TempC));
            LocalRH = FMath::Max(LocalRH, RHminIce);
        }
        LocalRH = FMath::Clamp(LocalRH, 0.0f, 1.5f);

        const float Qa = CalcSpecificHumidity(LocalAirTempK, LocalRH, Pressure);
        const float AirDensity = CalcAirDensity(LocalAirTempK, Pressure);

        float LocalSnowFracEffective = 0.0f;
        if (LocalPrecipitation_kgm2 > KINDA_SMALL_NUMBER)
        {
            constexpr float Twarm = 0.5f;
            constexpr float Tcold = -0.5f;

            if (Forcing.bHasExplicitPrecipitation && BaseExplicitTotal_kgm2 > KINDA_SMALL_NUMBER)
            {
                LocalSnowFracEffective = FMath::Clamp(BaseExplicitSnow_kgm2 / BaseExplicitTotal_kgm2, 0.0f, 1.0f);
            }
            else
            {
                LocalSnowFracEffective = FMath::Clamp(Forcing.SnowFrac_01, 0.0f, 1.0f);
            }

            const float TempC = LocalAirTempK - FreezePoint_K;
            if (TempC >= Twarm)
            {
                LocalSnowFracEffective = 0.0f;
            }
            else if (TempC <= Tcold)
            {
                LocalSnowFracEffective = 1.0f;
            }
            else if (!Forcing.bHasExplicitPrecipitation || BaseExplicitTotal_kgm2 <= KINDA_SMALL_NUMBER)
            {
                const float Alpha = (Twarm - TempC) / (Twarm - Tcold);
                LocalSnowFracEffective = FMath::Clamp(Alpha, 0.0f, 1.0f);
            }
        }

        // Apply UE radiation modifiers to incoming shortwave (direct + diffuse + terrain residual).
        const float IncomingSW = FMath::Max(0.0f, Forcing.SWdown_Wm2);
        float RadiationIndex = 1.0f;
        float RadiationIndex_Direct = 0.0f;
        float RadiationIndex_Diffuse = 0.0f;
        ASnowSimulationActor* ActorPtr = OwningSimulationActor.Get();
        const bool bDirectOnlyRadiationIndex = ActorPtr && ActorPtr->bUseDirectRadiationIndexOnly && ModelParameters.Radiation.bUseUERadiationIndex;
        const bool bTotalOnlyRadiationIndex = ActorPtr && ActorPtr->bUseTotalRadiationIndexOnly && ModelParameters.Radiation.bUseUERadiationIndex;
        const float RadiationIndexClampMax = ActorPtr ? ActorPtr->GetMaxRadiationIndexClamp() : 5.0f;

        float BaseDiffuseSW = 0.0f;
        float BaseDirectSW = 0.0f;
        const bool bUseUEExplicitSplit = ModelParameters.Radiation.bUseUERadiationIndex
            && ModelParameters.Radiation.bUseTerrainInterreflection;
        if (bUseUEExplicitSplit && Forcing.DirectSWdown_Wm2 >= 0.0f && Forcing.DiffuseSWdown_Wm2 >= 0.0f)
        {
            // RI + interreflection mode: explicit forcing split can be used directly.
            BaseDirectSW = FMath::Max(0.0f, Forcing.DirectSWdown_Wm2);
            BaseDiffuseSW = FMath::Max(0.0f, Forcing.DiffuseSWdown_Wm2);
        }
        else
        {
            // Baseline (no RI or no interreflection): follow Fortran SWPART semantics for GHI partitioning.
            PartitionShortwave(Forcing, IncomingSW, BaseDiffuseSW, BaseDirectSW);
        }

        if (bDirectOnlyRadiationIndex)
        {
            BaseDirectSW = IncomingSW;
            BaseDiffuseSW = 0.0f;
        }

        float DiffuseSW = BaseDiffuseSW;
        float DirectSW = BaseDirectSW;
        float TerrainSW = 0.0f;
        float ReferenceScaleTotal = 0.0f;
        float ReferenceScaleDirect = 0.0f;
        float ReferenceScaleDiffuse = 0.0f;
        float ReferenceScaleTotalNoGI = 0.0f;
        bool bSkyOnlyReferenceUsable = false;
        bool bSkyOnlyReferenceMeetsMinLuminance = false;
        bool bUseNoGIReferenceForScaling = false;
        float SkyOnlyReferenceRatio = 0.0f;
        float SkyOnlyRTYRatio = 0.0f;
        float ReferenceLuminanceTotalUsed = 0.0f;
        float ReferenceLuminanceDirectUsed = 0.0f;
        float ReferenceLuminanceDiffuseUsed = 0.0f;
        float ReferenceLuminanceDiffuseNoGIUsed = 0.0f;
        float ReferenceLuminanceTotalNoGIUsed = 0.0f;
        float RTY_Total = 0.0f;
        float RTY_Direct = 0.0f;
        float RTY_Diffuse = 0.0f;
        float RTY_DiffuseNoGI = 0.0f;
        float RTY_TotalNoGI = 0.0f;
        float RTY_SurfaceState = 0.0f;
        float RTY_Terrain = 0.0f;
        bool bHasTotalCellRTY = false;
        bool bHasDirectCellRTY = false;
        bool bHasDiffuseCellRTY = false;
        bool bHasDiffuseNoGICellRTY = false;
        bool bHasSurfaceStateCellRTY = false;
        bool bReferenceValidForCell = false;
        uint8 ReferenceInvalidReasonCode = ReferenceInvalidReason_None;
        bool bReferenceActorValidityFlagAtSelection = ActorPtr ? ActorPtr->bReferenceLuminanceValid : false;
        bool bReferenceActorValidityOverridden = false;
        bool bUsedSplitComponentMode = false;
        bool bDualReferenceStripEnabledAtSelection = ActorPtr ? ActorPtr->bUseDualReferenceStrip : false;
        bool bReferenceSelectedGroundHalf = !bDualReferenceStripEnabledAtSelection;
        bool bReferenceSelectionUsedRenderSurfaceState = false;
        bool bReferenceSelectionPlausibilityOverride = false;
        float ReferenceSelectionSnowDepth_m = SnowDepthForReference_m;
        float ReferenceSelectionSnowCoverFraction = Column.SnowCoverFraction;
        float ReferenceSnowBlendWeight = 0.0f;
        float ReferenceRenderSurfaceSnowBlendWeight = 0.0f;
        int32 ReferenceSelectionSnowLayerCount = Column.Snow.LayerCount;
        float ReferenceLuminanceTotalFullStripSnapshot = ActorPtr ? ActorPtr->ReferenceLuminance_Total : 0.0f;
        float ReferenceLuminanceDirectFullStripSnapshot = ActorPtr ? ActorPtr->ReferenceLuminance_Direct : 0.0f;
        float ReferenceLuminanceDiffuseFullStripSnapshot = ActorPtr ? ActorPtr->ReferenceLuminance_Diffuse : 0.0f;
        float ReferenceLuminanceDiffuseNoGIFullStripSnapshot = ActorPtr ? ActorPtr->ReferenceLuminance_DiffuseNoGI : 0.0f;
        float ReferenceLuminanceSurfaceStateFullStripSnapshot = ActorPtr ? ActorPtr->ReferenceLuminance_SurfaceState : 0.0f;
        float ReferenceLuminanceDirectGroundSnapshot = ActorPtr ? ActorPtr->ReferenceLuminance_Direct_Ground : 0.0f;
        float ReferenceLuminanceDirectSnowSnapshot = ActorPtr ? ActorPtr->ReferenceLuminance_Direct_Snow : 0.0f;
        float ReferenceLuminanceSurfaceStateGroundSnapshot = ActorPtr ? ActorPtr->ReferenceLuminance_SurfaceState_Ground : 0.0f;
        float ReferenceLuminanceSurfaceStateSnowSnapshot = ActorPtr ? ActorPtr->ReferenceLuminance_SurfaceState_Snow : 0.0f;

        // Enhanced diagnostic logging for FSM2 radiation
        static bool bLoggedFSM2RadiationStatus = false;
        if (!bLoggedFSM2RadiationStatus)
        {
            UE_LOG(LogTemp, Warning, TEXT("[FSM2] ===== UE RADIATION DIAGNOSTIC ====="));
            UE_LOG(LogTemp, Warning, TEXT("[FSM2]   bUseUERadiationIndex: %s"),
                ModelParameters.Radiation.bUseUERadiationIndex ? TEXT("TRUE") : TEXT("FALSE"));
            UE_LOG(LogTemp, Warning, TEXT("[FSM2]   bUseFluxCalibratedUERadiation: %s"),
                ModelParameters.Radiation.bUseFluxCalibratedUERadiation ? TEXT("TRUE") : TEXT("FALSE"));
            UE_LOG(LogTemp, Warning, TEXT("[FSM2]   bUseTerrainInterreflection: %s"),
                ModelParameters.Radiation.bUseTerrainInterreflection ? TEXT("TRUE") : TEXT("FALSE"));
            UE_LOG(LogTemp, Warning, TEXT("[FSM2]   bNeutralizeCaptureAlbedo: %s"),
                ModelParameters.Radiation.bNeutralizeCaptureAlbedo ? TEXT("TRUE") : TEXT("FALSE"));
            UE_LOG(LogTemp, Warning, TEXT("[FSM2]   RefAlb(single/ground/snow)=%.3f/%.3f/%.3f | MinAlb=%.3f | MaxFactor=%.3f"),
                ModelParameters.Radiation.ReferenceStripAssumedAlbedo,
                ModelParameters.Radiation.ReferenceStripAssumedGroundAlbedo,
                ModelParameters.Radiation.ReferenceStripAssumedSnowAlbedo,
                ModelParameters.Radiation.MinSurfaceAlbedoForNeutralization,
                ModelParameters.Radiation.MaxAlbedoNeutralizationFactor);
            UE_LOG(LogTemp, Warning, TEXT("[FSM2]   DirectOnlyIndex: %s"), bDirectOnlyRadiationIndex ? TEXT("TRUE") : TEXT("FALSE"));
            UE_LOG(LogTemp, Warning, TEXT("[FSM2]   TotalOnlyIndex: %s"), bTotalOnlyRadiationIndex ? TEXT("TRUE") : TEXT("FALSE"));

            if (!ModelParameters.Radiation.bUseUERadiationIndex)
            {
                UE_LOG(LogTemp, Warning, TEXT("[FSM2]   UE radiation DISABLED"));
                UE_LOG(LogTemp, Warning, TEXT("[FSM2]   Using geometric slope adjustment: %s"),
                    ModelParameters.Radiation.bUseSlopeAdjustedShortwave ? TEXT("YES") : TEXT("NO"));
            }
            else if (!ActorPtr)
            {
                UE_LOG(LogTemp, Error, TEXT("[FSM2]   CRITICAL: ActorPtr is NULL"));
                UE_LOG(LogTemp, Error, TEXT("[FSM2]   Cannot access UE radiation indices"));
            }
            else
            {
                const bool bHasValid = ActorPtr->HasValidRadiationIndices();
                const TArray<float>& RadIndices = ActorPtr->GetCachedRadiationIndices();

                UE_LOG(LogTemp, Warning, TEXT("[FSM2]   HasValidRadiationIndices(): %s"), bHasValid ? TEXT("TRUE") : TEXT("FALSE"));
                UE_LOG(LogTemp, Warning, TEXT("[FSM2]   CachedRadiationIndices.Num(): %d"), RadIndices.Num());
                UE_LOG(LogTemp, Warning, TEXT("[FSM2]   Expected cell count: %d"), OutDepthMeters.Num());
            }
            UE_LOG(LogTemp, Warning, TEXT("[FSM2] ==================================="));
            bLoggedFSM2RadiationStatus = true;
        }

        if (ModelParameters.Radiation.bUseUERadiationIndex)
        {
            if (ActorPtr)
            {
                const TArray<float>& CachedRTYTotal = ActorPtr->GetCachedTotalRTY();
                const TArray<float>& CachedRTYDirect = ActorPtr->GetCachedDirectRTY();
                const TArray<float>& CachedRTYDiffuse = ActorPtr->GetCachedDiffuseRTY();
                const TArray<float>& CachedRTYDiffuseNoGI = ActorPtr->GetCachedDiffuseNoGIRTY();
                const TArray<float>& CachedRTYTotalNoGI = ActorPtr->GetCachedTotalNoGIRTY();
                const TArray<float>& CachedRTYSurfaceState = ActorPtr->GetCachedSurfaceStateRTY();
                const TArray<float>& CachedRTYTerrain = ActorPtr->GetCachedTerrainResidualRTY();

                bHasTotalCellRTY = CachedRTYTotal.IsValidIndex(Index);
                bHasDirectCellRTY = CachedRTYDirect.IsValidIndex(Index);
                bHasDiffuseCellRTY = CachedRTYDiffuse.IsValidIndex(Index);
                RTY_Total = bHasTotalCellRTY ? CachedRTYTotal[Index] : 0.0f;
                RTY_Direct = bHasDirectCellRTY ? CachedRTYDirect[Index] : 0.0f;
                RTY_Diffuse = bHasDiffuseCellRTY ? CachedRTYDiffuse[Index] : 0.0f;
                bHasDiffuseNoGICellRTY = CachedRTYDiffuseNoGI.IsValidIndex(Index);
                RTY_DiffuseNoGI = bHasDiffuseNoGICellRTY ? CachedRTYDiffuseNoGI[Index] : 0.0f;
                RTY_TotalNoGI = CachedRTYTotalNoGI.IsValidIndex(Index)
                    ? CachedRTYTotalNoGI[Index]
                    : (RTY_Direct + RTY_DiffuseNoGI);
                bHasSurfaceStateCellRTY = CachedRTYSurfaceState.IsValidIndex(Index);
                RTY_SurfaceState = bHasSurfaceStateCellRTY ? CachedRTYSurfaceState[Index] : 0.0f;
                RTY_Terrain = CachedRTYTerrain.IsValidIndex(Index) ? CachedRTYTerrain[Index] : 0.0f;
            }

            if (ActorPtr)
            {
                const bool bFluxCalibratedUERadiation = ModelParameters.Radiation.bUseFluxCalibratedUERadiation;
                const bool bHasFluxCalibratedTotalRTY = bHasTotalCellRTY
                    && FMath::IsFinite(RTY_Total);
                const bool bHasFluxCalibratedSplitRTY = bHasFluxCalibratedTotalRTY
                    && bHasDirectCellRTY
                    && bHasDiffuseCellRTY
                    && FMath::IsFinite(RTY_Direct)
                    && FMath::IsFinite(RTY_Diffuse);
                const bool bHasFluxCalibratedRTY = bTotalOnlyRadiationIndex
                    ? bHasFluxCalibratedTotalRTY
                    : bHasFluxCalibratedSplitRTY;
                const bool bHasLegacyIndices = ActorPtr->HasValidRadiationIndices();
                const TArray<float>* DirectIndicesPtr = bHasLegacyIndices ? &ActorPtr->GetCachedDirectIndices() : nullptr;
                const TArray<float>* DiffuseIndicesPtr = bHasLegacyIndices ? &ActorPtr->GetCachedDiffuseIndices() : nullptr;
                const TArray<float>* TotalIndicesPtr = bHasLegacyIndices ? &ActorPtr->GetCachedRadiationIndices() : nullptr;

                const bool bHasDirectIndex = DirectIndicesPtr && DirectIndicesPtr->IsValidIndex(Index);
                const bool bHasDiffuseIndex = DiffuseIndicesPtr && DiffuseIndicesPtr->IsValidIndex(Index);
                const bool bHasTotalIndex = TotalIndicesPtr && TotalIndicesPtr->IsValidIndex(Index);
                const float DirectIdx = bHasDirectIndex
                    ? FMath::Clamp((*DirectIndicesPtr)[Index], 0.0f, RadiationIndexClampMax)
                    : 0.0f;
                const float DiffuseIdx = bHasDiffuseIndex
                    ? FMath::Clamp((*DiffuseIndicesPtr)[Index], 0.0f, RadiationIndexClampMax)
                    : 0.0f;
                const float TotalIdx = bHasTotalIndex
                    ? FMath::Clamp((*TotalIndicesPtr)[Index], 0.0f, RadiationIndexClampMax)
                    : 0.0f;

                if (bFluxCalibratedUERadiation && bHasFluxCalibratedRTY)
                {
                    // Flux-calibrated FSM2 works from the luminance captures directly.
                    // Total-only mode intentionally depends only on total luminance; missing diffuse capture must not
                    // invalidate the melt-model radiation index.
                    bUsedSplitComponentMode = true;

                    const bool bUseDualReferenceStrip = ActorPtr->bUseDualReferenceStrip;
                    const float SnowBlendWeightForReference = bUseDualReferenceStrip
                        ? ComputeDualReferenceSnowBlendWeight(
                            ActorPtr,
                            Column.Snow.LayerCount,
                            Column.SnowCoverFraction,
                            SnowDepthForReference_m)
                        : 0.0f;
                    float EffectiveSnowBlendWeightForReference = SnowBlendWeightForReference;
                    bDualReferenceStripEnabledAtSelection = bUseDualReferenceStrip;
                    ReferenceSelectionSnowDepth_m = SnowDepthForReference_m;
                    ReferenceSelectionSnowCoverFraction = Column.SnowCoverFraction;
                    ReferenceSelectionSnowLayerCount = Column.Snow.LayerCount;
                    ReferenceLuminanceTotalFullStripSnapshot = ActorPtr->ReferenceLuminance_Total;
                    ReferenceLuminanceDirectFullStripSnapshot = ActorPtr->ReferenceLuminance_Direct;
                    ReferenceLuminanceDiffuseFullStripSnapshot = ActorPtr->ReferenceLuminance_Diffuse;
                    ReferenceLuminanceDiffuseNoGIFullStripSnapshot = ActorPtr->ReferenceLuminance_DiffuseNoGI;
                    ReferenceLuminanceSurfaceStateFullStripSnapshot = ActorPtr->ReferenceLuminance_SurfaceState;
                    ReferenceLuminanceDirectGroundSnapshot = ActorPtr->ReferenceLuminance_Direct_Ground;
                    ReferenceLuminanceDirectSnowSnapshot = ActorPtr->ReferenceLuminance_Direct_Snow;
                    ReferenceLuminanceSurfaceStateGroundSnapshot = ActorPtr->ReferenceLuminance_SurfaceState_Ground;
                    ReferenceLuminanceSurfaceStateSnowSnapshot = ActorPtr->ReferenceLuminance_SurfaceState_Snow;

                    if (bUseDualReferenceStrip && bHasSurfaceStateCellRTY)
                    {
                        float RenderSurfaceSnowBlendWeight = 0.0f;
                        if (TryComputeRenderSurfaceSnowBlendWeight(
                            RTY_SurfaceState,
                            ReferenceLuminanceSurfaceStateGroundSnapshot,
                            ReferenceLuminanceSurfaceStateSnowSnapshot,
                            RenderSurfaceSnowBlendWeight))
                        {
                            EffectiveSnowBlendWeightForReference = RenderSurfaceSnowBlendWeight;
                            ReferenceRenderSurfaceSnowBlendWeight = RenderSurfaceSnowBlendWeight;
                            bReferenceSelectionUsedRenderSurfaceState = true;
                        }
                    }

                    if (bUseDualReferenceStrip
                        && ModelParameters.Radiation.bUseDualReferencePlausibilityGuard
                        && BaseDirectSW > KINDA_SMALL_NUMBER)
                    {
                        const bool bInitiallySelectedGroundHalf = EffectiveSnowBlendWeightForReference < 0.5f;
                        float GroundDirectIndex = 0.0f;
                        float SnowDirectIndex = 0.0f;
                        const bool bGroundDirectIndexValid = TryComputeDirectReferenceIndex(
                            RTY_Direct,
                            ReferenceLuminanceDirectGroundSnapshot,
                            GroundDirectIndex);
                        const bool bSnowDirectIndexValid = TryComputeDirectReferenceIndex(
                            RTY_Direct,
                            ReferenceLuminanceDirectSnowSnapshot,
                            SnowDirectIndex);
                        const float MaxPlausibleDirectIndex = FMath::Max(
                            ModelParameters.Radiation.DualReferenceMaxPlausibleDirectIndex,
                            0.1f);
                        const bool bGroundDirectPlausible = bGroundDirectIndexValid && GroundDirectIndex <= MaxPlausibleDirectIndex;
                        const bool bSnowDirectPlausible = bSnowDirectIndexValid && SnowDirectIndex <= MaxPlausibleDirectIndex;

                        if (bInitiallySelectedGroundHalf && !bGroundDirectPlausible && bSnowDirectPlausible)
                        {
                            EffectiveSnowBlendWeightForReference = 1.0f;
                            bReferenceSelectionPlausibilityOverride = true;
                        }
                        else if (!bInitiallySelectedGroundHalf && !bSnowDirectPlausible && bGroundDirectPlausible)
                        {
                            EffectiveSnowBlendWeightForReference = 0.0f;
                            bReferenceSelectionPlausibilityOverride = true;
                        }

                        if (bReferenceSelectionPlausibilityOverride)
                        {
                            static int32 DualReferenceOverrideLogCount = 0;
                            if (DualReferenceOverrideLogCount++ < 32)
                            {
                                UE_LOG(
                                    LogTemp,
                                    Warning,
                                    TEXT("[FSM2] Dual-reference plausibility override at cell %d step %d: preselected %s (depth=%.4fm, cover=%.4f, renderWeight=%.3f, renderOverride=%d) but direct candidate indices were ground=%.3f snow=%.3f. Switching to %s."),
                                    Index,
                                    SimulationStepCounter,
                                    bInitiallySelectedGroundHalf ? TEXT("GroundHalf") : TEXT("SnowHalf"),
                                    SnowDepthForReference_m,
                                    Column.SnowCoverFraction,
                                    ReferenceRenderSurfaceSnowBlendWeight,
                                    bReferenceSelectionUsedRenderSurfaceState ? 1 : 0,
                                    GroundDirectIndex,
                                    SnowDirectIndex,
                                    EffectiveSnowBlendWeightForReference < 0.5f ? TEXT("GroundHalf") : TEXT("SnowHalf"));
                            }
                        }
                    }

                    bReferenceSelectedGroundHalf = !bUseDualReferenceStrip || EffectiveSnowBlendWeightForReference < 0.5f;
                    ReferenceSnowBlendWeight = EffectiveSnowBlendWeightForReference;

                    ReferenceLuminanceDirectUsed = bUseDualReferenceStrip
                        ? BlendDualReferenceValue(
                            ActorPtr->ReferenceLuminance_Direct_Ground,
                            ActorPtr->ReferenceLuminance_Direct_Snow,
                            EffectiveSnowBlendWeightForReference,
                            ActorPtr->ReferenceLuminance_Direct)
                        : ActorPtr->ReferenceLuminance_Direct;
                    ReferenceLuminanceTotalUsed = bUseDualReferenceStrip
                        ? BlendDualReferenceValue(
                            ActorPtr->ReferenceLuminance_Total_Ground,
                            ActorPtr->ReferenceLuminance_Total_Snow,
                            EffectiveSnowBlendWeightForReference,
                            ActorPtr->ReferenceLuminance_Total)
                        : ActorPtr->ReferenceLuminance_Total;
                    ReferenceLuminanceDiffuseUsed = bUseDualReferenceStrip
                        ? BlendDualReferenceValue(
                            ActorPtr->ReferenceLuminance_Diffuse_Ground,
                            ActorPtr->ReferenceLuminance_Diffuse_Snow,
                            EffectiveSnowBlendWeightForReference,
                            ActorPtr->ReferenceLuminance_Diffuse)
                        : ActorPtr->ReferenceLuminance_Diffuse;
                    ReferenceLuminanceDiffuseNoGIUsed = bUseDualReferenceStrip
                        ? BlendDualReferenceValue(
                            ActorPtr->ReferenceLuminance_DiffuseNoGI_Ground,
                            ActorPtr->ReferenceLuminance_DiffuseNoGI_Snow,
                            EffectiveSnowBlendWeightForReference,
                            ActorPtr->ReferenceLuminance_DiffuseNoGI)
                        : ActorPtr->ReferenceLuminance_DiffuseNoGI;
                    ReferenceLuminanceTotalNoGIUsed = bUseDualReferenceStrip
                        ? BlendDualReferenceValue(
                            ActorPtr->ReferenceLuminance_TotalNoGI_Ground,
                            ActorPtr->ReferenceLuminance_TotalNoGI_Snow,
                            EffectiveSnowBlendWeightForReference,
                            ActorPtr->ReferenceLuminance_TotalNoGI)
                        : ActorPtr->ReferenceLuminance_TotalNoGI;

                    // Sanitize reference channels before scaling/diagnostics.
                    // Guard against stale impossible states (e.g., Total < Direct, or DiffuseNoGI >> Total).
                    ReferenceLuminanceDirectUsed = FMath::Max(ReferenceLuminanceDirectUsed, 0.0f);
                    ReferenceLuminanceTotalUsed = FMath::Max(ReferenceLuminanceTotalUsed, 0.0f);
                    ReferenceLuminanceDiffuseUsed = FMath::Max(ReferenceLuminanceDiffuseUsed, 0.0f);
                    if (ReferenceLuminanceTotalUsed < ReferenceLuminanceDirectUsed)
                    {
                        ReferenceLuminanceTotalUsed = ReferenceLuminanceDirectUsed;
                    }
                    ReferenceLuminanceDiffuseUsed = FMath::Max(
                        ReferenceLuminanceDiffuseUsed,
                        ReferenceLuminanceTotalUsed - ReferenceLuminanceDirectUsed);
                    if (!FMath::IsFinite(ReferenceLuminanceDiffuseNoGIUsed)
                        || ReferenceLuminanceDiffuseNoGIUsed <= 0.0f
                        || ReferenceLuminanceDiffuseNoGIUsed > (ReferenceLuminanceTotalUsed + 1e-4f))
                    {
                        ReferenceLuminanceDiffuseNoGIUsed = 0.0f;
                    }
                    ReferenceLuminanceTotalNoGIUsed = FMath::Clamp(
                        FMath::IsFinite(ReferenceLuminanceTotalNoGIUsed)
                            ? ReferenceLuminanceTotalNoGIUsed
                            : (ReferenceLuminanceDirectUsed + ReferenceLuminanceDiffuseNoGIUsed),
                        ReferenceLuminanceDirectUsed,
                        ReferenceLuminanceTotalUsed);

                    bSkyOnlyReferenceUsable = FSM2_HasUsableSkyOnlyReference(
                        ReferenceLuminanceDiffuseNoGIUsed,
                        ReferenceLuminanceDiffuseUsed,
                        ReferenceLuminanceTotalUsed);
                    bSkyOnlyReferenceMeetsMinLuminance =
                        ReferenceLuminanceDiffuseNoGIUsed >= ModelParameters.Radiation.ReferenceStripMinLuminance;
                    bUseNoGIReferenceForScaling = (bSkyOnlyReferenceUsable
                        && bSkyOnlyReferenceMeetsMinLuminance
                        && ReferenceLuminanceTotalNoGIUsed > 1e-6f);
                    SkyOnlyReferenceRatio =
                        (FMath::IsFinite(ReferenceLuminanceDiffuseNoGIUsed)
                            && FMath::IsFinite(ReferenceLuminanceDiffuseUsed)
                            && ReferenceLuminanceDiffuseUsed > 1e-6f)
                        ? (ReferenceLuminanceDiffuseNoGIUsed / ReferenceLuminanceDiffuseUsed)
                        : 0.0f;
                    SkyOnlyRTYRatio =
                        (FMath::IsFinite(RTY_DiffuseNoGI)
                            && FMath::IsFinite(RTY_Diffuse)
                            && RTY_Diffuse > 1e-6f)
                        ? (RTY_DiffuseNoGI / RTY_Diffuse)
                        : 0.0f;
                    const float ReferenceDiffuseForScaling = bUseNoGIReferenceForScaling
                        ? ReferenceLuminanceTotalNoGIUsed
                        : ReferenceLuminanceDiffuseUsed;

                    const bool bSelectedTotalReferenceUsable =
                        FMath::IsFinite(ReferenceLuminanceTotalUsed) && (ReferenceLuminanceTotalUsed > 0.0f);
                    const bool bSelectedSplitReferenceValuesUsable =
                        bSelectedTotalReferenceUsable &&
                        FMath::IsFinite(ReferenceLuminanceDirectUsed) && (ReferenceLuminanceDirectUsed >= 0.0f) &&
                        FMath::IsFinite(ReferenceDiffuseForScaling) && (ReferenceDiffuseForScaling >= 0.0f);
                    const bool bSelectedReferenceValuesUsable = bTotalOnlyRadiationIndex
                        ? bSelectedTotalReferenceUsable
                        : bSelectedSplitReferenceValuesUsable;

                    ReferenceInvalidReasonCode = bSelectedReferenceValuesUsable
                        ? ReferenceInvalidReason_None
                        : ReferenceInvalidReason_UnusableSelectedValues;
                    bReferenceValidForCell = bSelectedReferenceValuesUsable;
                    if (!ActorPtr->bReferenceLuminanceValid && bReferenceValidForCell)
                    {
                        bReferenceActorValidityOverridden = true;
                        static int32 ReferenceActorOverrideLogCount = 0;
                        if (ReferenceActorOverrideLogCount++ < 24)
                        {
                            UE_LOG(LogTemp, Warning, TEXT("[FSM2] Reference override: actor validity flag is false but selected reference values are usable. Using selected strip values for flux calibration."));
                        }
                    }

                    if (bReferenceValidForCell && ModelParameters.Radiation.bUseReferenceStripGuard)
                    {
                        const float CaptureCosZenith = FMath::Clamp(ActorPtr->GetCachedCaptureCosSolarZenith(), -1.0f, 1.0f);
                        const float SunElevationDeg = FMath::RadiansToDegrees(FMath::Asin(CaptureCosZenith));
                        const bool bSunHighEnough = SunElevationDeg >= ModelParameters.Radiation.ReferenceStripMinSunElevation_deg;
                        const bool bTotalRefValid = ReferenceLuminanceTotalUsed >= ModelParameters.Radiation.ReferenceStripMinLuminance;
                        const bool bDirectRefValid = bTotalOnlyRadiationIndex || (BaseDirectSW <= KINDA_SMALL_NUMBER) || (ReferenceLuminanceDirectUsed >= ModelParameters.Radiation.ReferenceStripMinLuminance);
                        const bool bDiffuseRefValid = bTotalOnlyRadiationIndex || (BaseDiffuseSW <= KINDA_SMALL_NUMBER) || (ReferenceDiffuseForScaling >= ModelParameters.Radiation.ReferenceStripMinLuminance);
                        bReferenceValidForCell = bSunHighEnough && bTotalRefValid && bDirectRefValid && bDiffuseRefValid;
                        if (!bReferenceValidForCell)
                        {
                            if (!bSunHighEnough)
                            {
                                ReferenceInvalidReasonCode = ReferenceInvalidReason_SunBelowMinElevation;
                            }
                            else if (!bTotalRefValid)
                            {
                                ReferenceInvalidReasonCode = ReferenceInvalidReason_TotalBelowMinLuminance;
                            }
                            else if (!bDirectRefValid)
                            {
                                ReferenceInvalidReasonCode = ReferenceInvalidReason_DirectBelowMinLuminance;
                            }
                            else if (!bDiffuseRefValid)
                            {
                                ReferenceInvalidReasonCode = ReferenceInvalidReason_DiffuseBelowMinLuminance;
                            }
                        }
                    }

                    if (!bReferenceValidForCell && ReferenceInvalidReasonCode == ReferenceInvalidReason_None)
                    {
                        const bool bNoIncomingShortwave =
                            IncomingSW <= KINDA_SMALL_NUMBER &&
                            BaseDirectSW <= KINDA_SMALL_NUMBER &&
                            BaseDiffuseSW <= KINDA_SMALL_NUMBER;

                        if (bNoIncomingShortwave)
                        {
                            ReferenceInvalidReasonCode = ReferenceInvalidReason_NoIncomingShortwave;
                        }
                        else if (!ActorPtr->bReferenceLuminanceValid)
                        {
                            ReferenceInvalidReasonCode = ReferenceInvalidReason_ActorReferenceUnavailable;
                        }
                    }

                    if (bReferenceValidForCell)
                    {
                        const float BaseTotal = BaseDirectSW + BaseDiffuseSW;
                        if (bTotalOnlyRadiationIndex)
                        {
                            ReferenceScaleTotal = (BaseTotal > 0.0f && ReferenceLuminanceTotalUsed > 1e-6f)
                                ? (BaseTotal / ReferenceLuminanceTotalUsed)
                                : 0.0f;
                            const float TotalScaled = (ReferenceScaleTotal > 0.0f && FMath::IsFinite(RTY_Total))
                                ? FMath::Max(0.0f, RTY_Total * ReferenceScaleTotal)
                                : BaseTotal;
                            const float FluxCalibratedTotalIdx = (BaseTotal > KINDA_SMALL_NUMBER)
                                ? FMath::Clamp(TotalScaled / BaseTotal, 0.0f, RadiationIndexClampMax)
                                : 1.0f;
                            DirectSW = BaseDirectSW * FluxCalibratedTotalIdx;
                            DiffuseSW = BaseDiffuseSW * FluxCalibratedTotalIdx;
                            TerrainSW = 0.0f;
                            RadiationIndex = FluxCalibratedTotalIdx;
                        }
                        else
                        {
                            ReferenceScaleDirect = (BaseDirectSW > 0.0f && ReferenceLuminanceDirectUsed > 1e-6f)
                                ? (BaseDirectSW / ReferenceLuminanceDirectUsed)
                                : 0.0f;
                            ReferenceScaleDiffuse = (BaseDiffuseSW > 0.0f && ReferenceDiffuseForScaling > 1e-6f)
                                ? (BaseDiffuseSW / ReferenceDiffuseForScaling)
                                : 0.0f;
                            ReferenceScaleTotalNoGI = (IncomingSW > 0.0f && ReferenceLuminanceTotalNoGIUsed > 1e-6f)
                                ? (IncomingSW / ReferenceLuminanceTotalNoGIUsed)
                                : 0.0f;
                            ReferenceScaleTotal = (IncomingSW > 0.0f && ReferenceLuminanceTotalUsed > 1e-6f)
                                ? (IncomingSW / ReferenceLuminanceTotalUsed)
                                : 0.0f;

                            const float DirectRTYForScaling = FMath::Max(0.0f, RTY_Direct);
                            const float TotalNoGIRTYForScaling = bUseNoGIReferenceForScaling
                                ? FMath::Max(0.0f, RTY_TotalNoGI)
                                : 0.0f;
                            const float SkyOnlyRTYForScaling = (bUseNoGIReferenceForScaling && bHasDiffuseNoGICellRTY && FMath::IsFinite(RTY_DiffuseNoGI))
                                ? FMath::Max(0.0f, RTY_DiffuseNoGI)
                                : FMath::Max(0.0f, RTY_Diffuse);

                            DirectSW = (ReferenceScaleDirect > 0.0f)
                                ? FMath::Max(0.0f, DirectRTYForScaling * ReferenceScaleDirect)
                                : 0.0f;
                            if (bUseNoGIReferenceForScaling && ReferenceScaleDiffuse > 0.0f && FMath::IsFinite(TotalNoGIRTYForScaling))
                            {
                                const float TotalNoGIScaled = FMath::Max(0.0f, TotalNoGIRTYForScaling * ReferenceScaleDiffuse);
                                DiffuseSW = FMath::Max(0.0f, TotalNoGIScaled - DirectSW);
                            }
                            else
                            {
                                DiffuseSW = (ReferenceScaleDiffuse > 0.0f && FMath::IsFinite(SkyOnlyRTYForScaling))
                                    ? FMath::Max(0.0f, SkyOnlyRTYForScaling * ReferenceScaleDiffuse)
                                    : 0.0f;
                            }

                            if (ModelParameters.Radiation.bUseTerrainInterreflection)
                            {
                                const bool bTotalScaleValid = ReferenceScaleTotal > 0.0f
                                    && ReferenceScaleTotal <= ModelParameters.Radiation.ReferenceStripMaxTotalScale
                                    && FMath::IsFinite(RTY_Total);
                                if (bTotalScaleValid)
                                {
                                    const float TotalScaled = FMath::Max(0.0f, RTY_Total * ReferenceScaleTotal);
                                    if (bUseNoGIReferenceForScaling && ReferenceScaleDiffuse > 0.0f && FMath::IsFinite(TotalNoGIRTYForScaling))
                                    {
                                        const float TotalNoGIScaled = FMath::Max(0.0f, TotalNoGIRTYForScaling * ReferenceScaleDiffuse);
                                        TerrainSW = FMath::Max(0.0f, TotalScaled - TotalNoGIScaled);
                                    }
                                    else
                                    {
                                        TerrainSW = FMath::Max(0.0f, TotalScaled - DirectSW - DiffuseSW);
                                    }
                                }
                                else
                                {
                                    TerrainSW = 0.0f;
                                }
                            }
                            else
                            {
                                TerrainSW = 0.0f;
                            }
                        }
                    }
                    else
                    {
                        DirectSW = BaseDirectSW;
                        DiffuseSW = BaseDiffuseSW;
                        TerrainSW = 0.0f;

                        static int32 ReferenceFallbackLogCount = 0;
                        if (ReferenceFallbackLogCount++ < 24)
                        {
                            UE_LOG(LogTemp, Warning, TEXT("[FSM2] Reference invalid for flux-calibrated UE radiation; falling back to default shortwave for this frame."));
                        }
                    }

                    const float BaseTotal = BaseDirectSW + BaseDiffuseSW;
                    const float SplitTotal = DirectSW + DiffuseSW + TerrainSW;
                    RadiationIndex = (BaseTotal > KINDA_SMALL_NUMBER)
                        ? FMath::Clamp(SplitTotal / BaseTotal, 0.0f, RadiationIndexClampMax)
                        : 1.0f;
                }
                else if (bDirectOnlyRadiationIndex && bHasDirectIndex)
                {
                    DirectSW = BaseDirectSW * DirectIdx;
                    DiffuseSW = 0.0f;
                    TerrainSW = 0.0f;
                    RadiationIndex = DirectIdx;
                }
                else if (bTotalOnlyRadiationIndex && bHasTotalIndex)
                {
                    DirectSW = BaseDirectSW * TotalIdx;
                    DiffuseSW = BaseDiffuseSW * TotalIdx;
                    TerrainSW = 0.0f;
                    RadiationIndex = TotalIdx;
                }
                else if (!bFluxCalibratedUERadiation && bHasDirectIndex && bHasDiffuseIndex)
                {
                    RadiationIndex_Direct = DirectIdx;
                    RadiationIndex_Diffuse = DiffuseIdx;
                    bUsedSplitComponentMode = true;
                    DirectSW = BaseDirectSW * DirectIdx;
                    DiffuseSW = BaseDiffuseSW * DiffuseIdx;
                    const float BaseTotal = BaseDirectSW + BaseDiffuseSW;
                    const float SplitTotal = DirectSW + DiffuseSW;
                    RadiationIndex = (BaseTotal > KINDA_SMALL_NUMBER)
                        ? FMath::Clamp(SplitTotal / BaseTotal, 0.0f, RadiationIndexClampMax)
                        : 1.0f;
                }
                else if (!bFluxCalibratedUERadiation && bHasTotalIndex)
                {
                    // Fallback to total index if split indices are missing.
                    DirectSW = BaseDirectSW * TotalIdx;
                    DiffuseSW = BaseDiffuseSW * TotalIdx;
                    TerrainSW = 0.0f;
                    RadiationIndex = TotalIdx;
                }
                else if (bFluxCalibratedUERadiation && ModelParameters.Radiation.bUseUERadiationIndex)
                {
                    static int32 MissingFluxCalibratedRTYLogCount = 0;
                    if (MissingFluxCalibratedRTYLogCount++ < 24)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("[FSM2] Flux-calibrated UE radiation missing RTY inputs for cell %d; keeping forcing-consistent shortwave for this frame."), Index);
                    }
                }
                else if (bHasLegacyIndices && bEnableHotPathLogs)
                {
                    UE_LOG(LogTemp, Error, TEXT("[FSM2] CRITICAL: Invalid cell index %d"), Index);
                }
            }
        }
        // Note: Slope attenuation is NOT applied when using UE radiation index.
        else if (ModelParameters.Radiation.bUseSlopeAdjustedShortwave)
        {
            const float SlopeAttenuation = ComputeSlopeAttenuation(Index);
            DirectSW *= SlopeAttenuation;
            DiffuseSW *= SlopeAttenuation;
        }

        const float BaseTotalSW = BaseDirectSW + BaseDiffuseSW;

        float WindHeight = ModelParameters.Atmosphere.WindMeasurementHeight_m;
        float TempHeight = ModelParameters.Atmosphere.TemperatureMeasurementHeight_m;
        AdjustMeasurementHeights(WindHeight, TempHeight);
        const float BulkTransferCoefficient = ResolveBulkTransferCoefficient(WindHeight, TempHeight);

        float LocalSnowInput_kgm2 = LocalPrecipitation_kgm2 * LocalSnowFracEffective;
        if (bApplySnowRedistribution && SnowRedistributionFactors.IsValidIndex(Index))
        {
            LocalSnowInput_kgm2 *= SnowRedistributionFactors[Index] * SnowRedistributionConservationFactor;
        }
        float LocalRainInput_kgm2 = FMath::Max(0.0f, LocalPrecipitation_kgm2 - LocalSnowInput_kgm2);

        const float SnowInputRate = (DtSeconds > KINDA_SMALL_NUMBER) ? LocalSnowInput_kgm2 / DtSeconds : 0.0f;
        // Filter "micro-snow" so tiny daily dustings do not persist and pin Tsrf near 0 C.
        // Threshold: Configurable via MinimumSnowfallRate_mmph (default 0.0 = disabled).
        // Conversion: mm/hr -> kg/m2/s (divide by 3600)
        const float ThresholdRate = FMath::Max(0.0f, ModelParameters.Snow.MinimumSnowfallRate_mmph) / 3600.0f;
        const float EffectiveSnowRate = (SnowInputRate > ThresholdRate) ? SnowInputRate : 0.0f;
        if (SnowInputRate > EffectiveSnowRate)
        {
            const float FilteredSnow_kgm2 = EffectiveSnowRate * DtSeconds;
            const float MicroSnow_kgm2 = LocalSnowInput_kgm2 - FilteredSnow_kgm2;
            LocalSnowInput_kgm2 = FilteredSnow_kgm2;
            // Preserve total precip energy by routing filtered micro-snow to rain
            LocalRainInput_kgm2 += MicroSnow_kgm2;
        }

        Column.SnowAlbedo = UpdateSnowAlbedo(Column, DtSeconds, SurfaceTemperatureK, EffectiveSnowRate);
        Column.SurfaceAlbedo = ComputeSurfaceAlbedo(Column, Column.SnowAlbedo, Column.SnowCoverFraction);

        const float MinSurfaceAlbedoForNeutralization = FMath::Clamp(ModelParameters.Radiation.MinSurfaceAlbedoForNeutralization, 0.01f, 1.0f);
        float NeutralizationSnowBlendWeight = FMath::Clamp(Column.SnowCoverFraction, 0.0f, 1.0f);
        float NeutralizationSurfaceAlbedo = FMath::Clamp(Column.SurfaceAlbedo, MinSurfaceAlbedoForNeutralization, 1.0f);
        float NeutralizationReferenceAlbedo = FMath::Clamp(ModelParameters.Radiation.ReferenceStripAssumedAlbedo, MinSurfaceAlbedoForNeutralization, 1.0f);
        float AlbedoNeutralizationRawFactor = 1.0f;
        float AlbedoNeutralizationFactor = 1.0f;
        bool bNeutralizationUsedRenderSurfaceState = false;

        if (ModelParameters.Radiation.bUseUERadiationIndex
            && ModelParameters.Radiation.bUseFluxCalibratedUERadiation
            && ModelParameters.Radiation.bNeutralizeCaptureAlbedo)
        {
            float ReferenceCaptureAlbedo = ModelParameters.Radiation.ReferenceStripAssumedAlbedo;
            float SurfaceSnowBlendWeightForNeutralization = FMath::Clamp(Column.SnowCoverFraction, 0.0f, 1.0f);
            if (ActorPtr && ActorPtr->bUseDualReferenceStrip)
            {
                const float SnowBlendWeightForReference = bUsedSplitComponentMode
                    ? ReferenceSnowBlendWeight
                    : ComputeDualReferenceSnowBlendWeight(
                        ActorPtr,
                        Column.Snow.LayerCount,
                        Column.SnowCoverFraction,
                        SnowDepthForReference_m);
                ReferenceCaptureAlbedo = FMath::Lerp(
                    ModelParameters.Radiation.ReferenceStripAssumedGroundAlbedo,
                    ModelParameters.Radiation.ReferenceStripAssumedSnowAlbedo,
                    SnowBlendWeightForReference);

                if (bReferenceSelectionUsedRenderSurfaceState)
                {
                    SurfaceSnowBlendWeightForNeutralization = ReferenceRenderSurfaceSnowBlendWeight;
                    bNeutralizationUsedRenderSurfaceState = true;
                }
                else if (bUsedSplitComponentMode)
                {
                    SurfaceSnowBlendWeightForNeutralization = ReferenceSnowBlendWeight;
                }
            }

            NeutralizationSnowBlendWeight = FMath::Clamp(SurfaceSnowBlendWeightForNeutralization, 0.0f, 1.0f);
            if (ActorPtr && ActorPtr->bUseDualReferenceStrip)
            {
                if (bNeutralizationUsedRenderSurfaceState)
                {
                    // Keep the cell-side neutralization in the same albedo space as the render-selected reference.
                    NeutralizationSurfaceAlbedo = FMath::Lerp(
                        ModelParameters.Radiation.ReferenceStripAssumedGroundAlbedo,
                        ModelParameters.Radiation.ReferenceStripAssumedSnowAlbedo,
                        NeutralizationSnowBlendWeight);
                }
                else
                {
                    NeutralizationSurfaceAlbedo = ComputeSurfaceAlbedo(Column, Column.SnowAlbedo, NeutralizationSnowBlendWeight);
                }
            }
            else
            {
                NeutralizationSurfaceAlbedo = Column.SurfaceAlbedo;
            }

            const float SurfaceAlbedoForNeutralization = FMath::Clamp(NeutralizationSurfaceAlbedo, MinSurfaceAlbedoForNeutralization, 1.0f);
            const float ReferenceAlbedoForNeutralization = FMath::Clamp(ReferenceCaptureAlbedo, MinSurfaceAlbedoForNeutralization, 1.0f);

            NeutralizationSurfaceAlbedo = SurfaceAlbedoForNeutralization;
            NeutralizationReferenceAlbedo = ReferenceAlbedoForNeutralization;
            AlbedoNeutralizationRawFactor = ReferenceAlbedoForNeutralization / SurfaceAlbedoForNeutralization;
            const float MaxNeutralizationFactor = FMath::Max(1.0f, ModelParameters.Radiation.MaxAlbedoNeutralizationFactor);
            const float MinNeutralizationFactor = 1.0f / MaxNeutralizationFactor;
            AlbedoNeutralizationFactor = FMath::Clamp(AlbedoNeutralizationRawFactor, MinNeutralizationFactor, MaxNeutralizationFactor);

            DirectSW *= AlbedoNeutralizationFactor;
            DiffuseSW *= AlbedoNeutralizationFactor;
            TerrainSW *= AlbedoNeutralizationFactor;
        }

        if (bUsedSplitComponentMode)
        {
            RadiationIndex_Direct = (BaseDirectSW > KINDA_SMALL_NUMBER)
                ? FMath::Clamp(DirectSW / BaseDirectSW, 0.0f, RadiationIndexClampMax)
                : 0.0f;
            RadiationIndex_Diffuse = (BaseDiffuseSW > KINDA_SMALL_NUMBER)
                ? FMath::Clamp(DiffuseSW / BaseDiffuseSW, 0.0f, RadiationIndexClampMax)
                : 0.0f;
        }

        const float TotalSW = DirectSW + DiffuseSW + TerrainSW;
        if (BaseTotalSW > KINDA_SMALL_NUMBER)
        {
            RadiationIndex = FMath::Clamp(TotalSW / BaseTotalSW, 0.0f, RadiationIndexClampMax);
        }
        else if (!FMath::IsFinite(RadiationIndex))
        {
            RadiationIndex = 1.0f;
        }

        Column.DiffuseShortwave_Wm2 = DiffuseSW;
        Column.DirectShortwave_Wm2 = DirectSW;
        Column.TerrainShortwave_Wm2 = TerrainSW;

        EnergyBalanceStage(Index, Column, DtSeconds, Forcing, LocalSnowInput_kgm2, LocalRainInput_kgm2, AirDensity, LocalAirTempK, Qa, Pressure, Wind, FreshDensity, FreshGrain, BulkTransferCoefficient, DiffuseSW, DirectSW, TerrainSW, Column.SurfaceAlbedo, NetSurfaceFlux, SurfaceTemperatureK, ThroughfallSnowOut, ThroughfallRainOut, SurfaceFluxes, SurfaceUpdate, DiagnosticsPtr);

        const float RadiationImpact_Wm2 = TotalSW - BaseTotalSW;
        const float RadiationImpactRatio = (BaseTotalSW > KINDA_SMALL_NUMBER) ? (TotalSW / BaseTotalSW) : 1.0f;
        const float CalibratedSWROut_Wm2 = TotalSW * Column.SurfaceAlbedo;

        if (DiagnosticsPtr)
        {
            const float ForcingDirectInputSW = (Forcing.DirectSWdown_Wm2 >= 0.0f)
                ? FMath::Max(0.0f, Forcing.DirectSWdown_Wm2)
                : BaseDirectSW;
            const float ForcingDiffuseInputSW = (Forcing.DiffuseSWdown_Wm2 >= 0.0f)
                ? FMath::Max(0.0f, Forcing.DiffuseSWdown_Wm2)
                : BaseDiffuseSW;

            DiagnosticsPtr->ForcingIncomingShortwave_Wm2 = IncomingSW;
            DiagnosticsPtr->ForcingDirectShortwave_Wm2 = ForcingDirectInputSW;
            DiagnosticsPtr->ForcingDiffuseShortwave_Wm2 = ForcingDiffuseInputSW;
            DiagnosticsPtr->ForcingDirectShortwaveBase_Wm2 = BaseDirectSW;
            DiagnosticsPtr->ForcingDiffuseShortwaveBase_Wm2 = BaseDiffuseSW;
            DiagnosticsPtr->DiffuseShortwave_Wm2 = DiffuseSW;
            DiagnosticsPtr->DirectShortwave_Wm2 = DirectSW;
            DiagnosticsPtr->TerrainShortwave_Wm2 = TerrainSW;
            DiagnosticsPtr->TotalShortwave_Wm2 = TotalSW;
            DiagnosticsPtr->RadiationIndex = RadiationIndex;
            DiagnosticsPtr->RadiationIndex_Direct = RadiationIndex_Direct;
            DiagnosticsPtr->RadiationIndex_Diffuse = RadiationIndex_Diffuse;
            DiagnosticsPtr->RadiationComponent = TotalSW;
            DiagnosticsPtr->RadiationIndexImpact_Wm2 = RadiationImpact_Wm2;
            DiagnosticsPtr->RadiationIndexImpactRatio = RadiationImpactRatio;
            DiagnosticsPtr->CalibratedSWROut_Wm2 = CalibratedSWROut_Wm2;
            DiagnosticsPtr->NeutralizationSurfaceAlbedo = NeutralizationSurfaceAlbedo;
            DiagnosticsPtr->NeutralizationReferenceAlbedo = NeutralizationReferenceAlbedo;
            DiagnosticsPtr->NeutralizationSnowBlendWeight = NeutralizationSnowBlendWeight;
            DiagnosticsPtr->AlbedoNeutralizationRawFactor = AlbedoNeutralizationRawFactor;
            DiagnosticsPtr->AlbedoNeutralizationFactor = AlbedoNeutralizationFactor;
            DiagnosticsPtr->ReferenceScale_Total = ReferenceScaleTotal;
            DiagnosticsPtr->ReferenceScale_Direct = ReferenceScaleDirect;
            DiagnosticsPtr->ReferenceScale_Diffuse = ReferenceScaleDiffuse;
            DiagnosticsPtr->ReferenceScale_TotalNoGI = ReferenceScaleTotalNoGI;
            DiagnosticsPtr->bSkyOnlyReferenceUsable = bSkyOnlyReferenceUsable ? 1 : 0;
            DiagnosticsPtr->bSkyOnlyReferenceMeetsMinLuminance = bSkyOnlyReferenceMeetsMinLuminance ? 1 : 0;
            DiagnosticsPtr->bSkyOnlyDiffuseScalingUsed = bUseNoGIReferenceForScaling ? 1 : 0;
            DiagnosticsPtr->SkyOnlyReferenceRatio = SkyOnlyReferenceRatio;
            DiagnosticsPtr->SkyOnlyRTYRatio = SkyOnlyRTYRatio;
            DiagnosticsPtr->ReferenceLuminance_Total = ReferenceLuminanceTotalUsed;
            DiagnosticsPtr->ReferenceLuminance_Direct = ReferenceLuminanceDirectUsed;
            DiagnosticsPtr->ReferenceLuminance_Diffuse = ReferenceLuminanceDiffuseUsed;
            DiagnosticsPtr->ReferenceLuminance_DiffuseNoGI = ReferenceLuminanceDiffuseNoGIUsed;
            DiagnosticsPtr->ReferenceLuminance_TotalNoGI = ReferenceLuminanceTotalNoGIUsed;
            DiagnosticsPtr->ReferenceLuminance_SurfaceState_FullStrip_Snapshot = ReferenceLuminanceSurfaceStateFullStripSnapshot;
            DiagnosticsPtr->ReferenceLuminance_Total_FullStrip_Snapshot = ReferenceLuminanceTotalFullStripSnapshot;
            DiagnosticsPtr->ReferenceLuminance_Direct_FullStrip_Snapshot = ReferenceLuminanceDirectFullStripSnapshot;
            DiagnosticsPtr->ReferenceLuminance_Diffuse_FullStrip_Snapshot = ReferenceLuminanceDiffuseFullStripSnapshot;
            DiagnosticsPtr->ReferenceLuminance_DiffuseNoGI_FullStrip_Snapshot = ReferenceLuminanceDiffuseNoGIFullStripSnapshot;
            DiagnosticsPtr->ReferenceLuminance_Direct_Ground_Snapshot = ReferenceLuminanceDirectGroundSnapshot;
            DiagnosticsPtr->ReferenceLuminance_Direct_Snow_Snapshot = ReferenceLuminanceDirectSnowSnapshot;
            DiagnosticsPtr->ReferenceLuminance_SurfaceState_Ground_Snapshot = ReferenceLuminanceSurfaceStateGroundSnapshot;
            DiagnosticsPtr->ReferenceLuminance_SurfaceState_Snow_Snapshot = ReferenceLuminanceSurfaceStateSnowSnapshot;
            DiagnosticsPtr->ReferenceSelectionSnowDepth_m = ReferenceSelectionSnowDepth_m;
            DiagnosticsPtr->ReferenceSelectionSnowCoverFraction = ReferenceSelectionSnowCoverFraction;
            DiagnosticsPtr->ReferenceSnowBlendWeight = ReferenceSnowBlendWeight;
            DiagnosticsPtr->ReferenceRenderSurfaceSnowBlendWeight = ReferenceRenderSurfaceSnowBlendWeight;
            DiagnosticsPtr->ReferenceSelectionSnowLayerCount = ReferenceSelectionSnowLayerCount;
            DiagnosticsPtr->bDualReferenceStripEnabled = bDualReferenceStripEnabledAtSelection ? 1 : 0;
            DiagnosticsPtr->bReferenceSelectedGroundHalf = bReferenceSelectedGroundHalf ? 1 : 0;
            DiagnosticsPtr->bReferenceSelectionUsedRenderSurfaceState = bReferenceSelectionUsedRenderSurfaceState ? 1 : 0;
            DiagnosticsPtr->bReferenceSelectionPlausibilityOverride = bReferenceSelectionPlausibilityOverride ? 1 : 0;
            DiagnosticsPtr->bNeutralizationUsedRenderSurfaceState = bNeutralizationUsedRenderSurfaceState ? 1 : 0;
            DiagnosticsPtr->RTY_Total = RTY_Total;
            DiagnosticsPtr->RTY_Direct = RTY_Direct;
            DiagnosticsPtr->RTY_Diffuse = RTY_Diffuse;
            DiagnosticsPtr->RTY_DiffuseNoGI = RTY_DiffuseNoGI;
            DiagnosticsPtr->RTY_TotalNoGI = RTY_TotalNoGI;
            DiagnosticsPtr->RTY_SurfaceState = RTY_SurfaceState;
            DiagnosticsPtr->RTY_Terrain = RTY_Terrain;
            DiagnosticsPtr->bReferenceValid = bReferenceValidForCell ? 1 : 0;
            DiagnosticsPtr->ReferenceInvalidReasonCode = ReferenceInvalidReasonCode;
            DiagnosticsPtr->bReferenceActorValidityFlag = bReferenceActorValidityFlagAtSelection ? 1 : 0;
            DiagnosticsPtr->bReferenceActorValidityOverridden = bReferenceActorValidityOverridden ? 1 : 0;
        }

        // Persist the calculated skin temperature for the next time step's initial guess
        Column.SurfaceTemperature_K = SurfaceTemperatureK;

        const bool bHadSnowBeforeMassUpdate = (Column.Snow.LayerCount > 0);
        const bool bUseDarcyHydrology = (ModelParameters.Snow.HydrologyScheme == EFSM2HydrologyScheme::Darcy);
        float SublimationMass = 0.0f;

        if (bHadSnowBeforeMassUpdate)
        {
            const float RainIntoTopLayer_kgm2 = bUseDarcyHydrology ? 0.0f : ThroughfallRainOut;
            MeltRefreezeStage(Column.Snow, SurfaceUpdate, RainIntoTopLayer_kgm2, DtSeconds, SublimationMass);

            // Keep surface at melt point when melt occurred this step to mirror Fortran behavior and enable melt-albedo decay
            if (SurfaceUpdate.MeltMass_kgm2 > KINDA_SMALL_NUMBER && Column.Snow.LayerCount > 0)
            {
                Column.Snow.Temperature_K[0] = FreezePoint_K;
                SurfaceUpdate.TemperatureK = FreezePoint_K;
                SurfaceTemperatureK = FreezePoint_K;
            }

            ApplyDensityEvolution(Column.Snow, DtSeconds);
            UpdateGrainGrowth(Column.Snow, Column.Soil, SurfaceTemperatureK, DtSeconds);
        }

        // Fortran SNOW order: apply melt/sublimation and compaction first, then add snowfall/frost.
        float FrostFormation_kgm2 = 0.0f;
        if (SurfaceUpdate.SurfaceMassFlux_kgm2s < 0.0f && SurfaceTemperatureK < FreezePoint_K)
        {
            FrostFormation_kgm2 = -SurfaceUpdate.SurfaceMassFlux_kgm2s * DtSeconds;
        }

        if (ThroughfallSnowOut > KINDA_SMALL_NUMBER)
        {
            DepositionStage(Column.Snow, ThroughfallSnowOut, LocalAirTempK, FreshDensity, FreshGrain);
        }

        if (FrostFormation_kgm2 > KINDA_SMALL_NUMBER)
        {
            if (Column.Snow.LayerCount > 0)
            {
                const float CurrentDensity = Column.Snow.Density_kgm3[0];
                InsertSurfaceLayer(Column.Snow, FrostFormation_kgm2, 0.0f, CurrentDensity, SurfaceTemperatureK, FreshGrain);
            }
            else
            {
                InsertSurfaceLayer(Column.Snow, FrostFormation_kgm2, 0.0f, FreshDensity, SurfaceTemperatureK, FreshGrain);
            }
        }

        // If this step created a new snowpack, route rainfall into top-layer liquid storage.
        if (!bUseDarcyHydrology && !bHadSnowBeforeMassUpdate && Column.Snow.LayerCount > 0 && ThroughfallRainOut > KINDA_SMALL_NUMBER)
        {
            Column.Snow.LiquidMass_kgm2[0] += ThroughfallRainOut;
            const float CurrentThickness = FMath::Max(Column.Snow.Thickness_m[0], MinimumLayerThickness_m);
            const float NewMass = Column.Snow.IceMass_kgm2[0] + Column.Snow.LiquidMass_kgm2[0];
            const float NewDensity = (CurrentThickness > KINDA_SMALL_NUMBER)
                ? FMath::Clamp(NewMass / CurrentThickness, ModelParameters.Snow.MinSnowDensity_kgm3, ModelParameters.PhysicalConstants.DensityIce_kgm3)
                : Column.Snow.Density_kgm3[0];
            Column.Snow.Density_kgm3[0] = NewDensity;
            UpdateLayerDiagnostics(Column.Snow, 0);
        }

        if (Column.Snow.LayerCount == 0)
        {
            if (ThroughfallRainOut > KINDA_SMALL_NUMBER)
            {
                Column.Runoff_kgm2 += ThroughfallRainOut;
            }

            ConductiveHeatExchange(Column.Snow, Column.Soil, DtSeconds, NetSurfaceFlux);

            OutDepthMeters[Index] = 0.0f;
            CachedAlbedo[Index] = Column.SurfaceAlbedo;
            const float BareSurfaceTemperatureK = (Column.Soil.LayerCount > 0)
                ? Column.Soil.Temperature_K[0]
                : ModelParameters.Soil.GroundTemperature_K;
            SurfaceUpdate.TemperatureK = BareSurfaceTemperatureK;
            SurfaceTemperatureK = BareSurfaceTemperatureK;

            if (DiagnosticsPtr)
            {
                DiagnosticsPtr->SnowDepth_m = 0.0f;
                DiagnosticsPtr->SnowMass_kgm2 = 0.0f;
                DiagnosticsPtr->Runoff_kgm2 = Column.Runoff_kgm2;
                DiagnosticsPtr->Timestamp = Forcing.Timestamp;
                DiagnosticsPtr->CanopySnow_kgm2 = Column.Canopy.SnowStorage_kgm2;
                DiagnosticsPtr->CanopyLiquid_kgm2 = Column.Canopy.LiquidStorage_kgm2;
                DiagnosticsPtr->SurfaceTemperatureK = BareSurfaceTemperatureK;
                DiagnosticsPtr->NetSurfaceFlux_Wm2 = SurfaceFluxes.Total();
                // Store fluxes using Fortran sign convention: upward positive (cooling)
                DiagnosticsPtr->EnergyFluxes = SurfaceFluxes;
                DiagnosticsPtr->MeltMass_kgm2 = SurfaceUpdate.MeltMass_kgm2;
                DiagnosticsPtr->RefreezeMass_kgm2 = SurfaceUpdate.RefreezeMass_kgm2;
                DiagnosticsPtr->SublimationMass_kgm2 = SublimationMass;
                DiagnosticsPtr->FrostMass_kgm2 = FrostFormation_kgm2;
                DiagnosticsPtr->ThroughfallSnow_kgm2 = ThroughfallSnowOut;
                DiagnosticsPtr->ThroughfallRain_kgm2 = ThroughfallRainOut;
                DiagnosticsPtr->RadiationIndex = RadiationIndex;
                DiagnosticsPtr->RadiationComponent = TotalSW;
                DiagnosticsPtr->SnowLayerCount = 0;
                DiagnosticsPtr->SoilLayerCount = Column.Soil.LayerCount;
                for (int32 SoilIdx = 0; SoilIdx < Column.Soil.LayerCount && SoilIdx < GFSM2MaxSoilLayers; ++SoilIdx)
                {
                    DiagnosticsPtr->SoilThickness_m[SoilIdx] = Column.Soil.Thickness_m[SoilIdx];
                    DiagnosticsPtr->SoilTemperature_K[SoilIdx] = Column.Soil.Temperature_K[SoilIdx];
                    DiagnosticsPtr->SoilMoisture_Vol[SoilIdx] = Column.Soil.Moisture_VolumeFraction[SoilIdx];
                }
                DiagnosticsPtr->SimulationTimeSeconds = ElapsedSimulationSeconds + DtSeconds;
            }

            // Reset runoff after recording it for this step
            Column.Runoff_kgm2 = 0.0f;

            return;
        }

        RebuildLayers(Column.Snow);
        ConductiveHeatExchange(Column.Snow, Column.Soil, DtSeconds, NetSurfaceFlux);

        const float IceMassBeforeHydrology = ComputeSnowIceMass(Column.Snow);
        float HydrologyRunoff = 0.0f;
        HydrologyStage(Column.Snow, DtSeconds, ThroughfallRainOut, HydrologyRunoff, DiagnosticsPtr);
        Column.Runoff_kgm2 += HydrologyRunoff;
        const float IceMassAfterHydrology = ComputeSnowIceMass(Column.Snow);
        SurfaceUpdate.RefreezeMass_kgm2 = FMath::Max(0.0f, IceMassAfterHydrology - IceMassBeforeHydrology);

        // Apply surface temperature from energy balance to the top snow layer
        // This ensures the snowpack warms up correctly (breaking the cold-layer feedback loop)
        // and that albedo decay uses the correct skin temperature.
        if (Column.Snow.LayerCount > 0)
        {
            Column.Snow.Temperature_K[0] = SurfaceUpdate.TemperatureK;
        }

        const float ResolvedSurfaceTemperatureK = SurfaceUpdate.TemperatureK;
        SurfaceTemperatureK = ResolvedSurfaceTemperatureK;



        OutDepthMeters[Index] = ComputeSnowDepthMeters(Column.Snow);
        CachedAlbedo[Index] = Column.SurfaceAlbedo;

        if (DiagnosticsPtr)
        {
            DiagnosticsPtr->SnowDepth_m = OutDepthMeters[Index];
            const float SnowMass = ComputeSnowMass(Column.Snow);
            DiagnosticsPtr->SnowMass_kgm2 = SnowMass;
            DiagnosticsPtr->Runoff_kgm2 = Column.Runoff_kgm2;
            DiagnosticsPtr->Timestamp = Forcing.Timestamp;
            DiagnosticsPtr->CanopySnow_kgm2 = Column.Canopy.SnowStorage_kgm2;
            DiagnosticsPtr->CanopyLiquid_kgm2 = Column.Canopy.LiquidStorage_kgm2;
            DiagnosticsPtr->SurfaceTemperatureK = ResolvedSurfaceTemperatureK;
            DiagnosticsPtr->NetSurfaceFlux_Wm2 = SurfaceFluxes.Total();
            // Store fluxes using Fortran sign convention: upward positive (cooling)
            DiagnosticsPtr->EnergyFluxes = SurfaceFluxes;
            DiagnosticsPtr->MeltMass_kgm2 = SurfaceUpdate.MeltMass_kgm2;
            DiagnosticsPtr->RefreezeMass_kgm2 = SurfaceUpdate.RefreezeMass_kgm2;
            DiagnosticsPtr->SublimationMass_kgm2 = SublimationMass;
            DiagnosticsPtr->FrostMass_kgm2 = FrostFormation_kgm2;
            DiagnosticsPtr->RadiationIndex = RadiationIndex;
            DiagnosticsPtr->RadiationComponent = TotalSW;
            const int32 SnowLayerCount = FMath::Clamp(Column.Snow.LayerCount, 0, GFSM2MaxLayers);
            const int32 SoilLayerCount = FMath::Clamp(Column.Soil.LayerCount, 0, GFSM2MaxSoilLayers);
            DiagnosticsPtr->SnowLayerCount = SnowLayerCount;
            DiagnosticsPtr->SoilLayerCount = SoilLayerCount;
            DiagnosticsPtr->ThroughfallSnow_kgm2 = ThroughfallSnowOut;
            DiagnosticsPtr->ThroughfallRain_kgm2 = ThroughfallRainOut;

            for (int32 LayerIdx = 0; LayerIdx < SnowLayerCount; ++LayerIdx)
            {
                DiagnosticsPtr->SnowThickness_m[LayerIdx] = Column.Snow.Thickness_m[LayerIdx];
                DiagnosticsPtr->SnowTemperature_K[LayerIdx] = Column.Snow.Temperature_K[LayerIdx];
                DiagnosticsPtr->SnowIceMass_kgm2[LayerIdx] = Column.Snow.IceMass_kgm2[LayerIdx];
                DiagnosticsPtr->SnowLiquidMass_kgm2[LayerIdx] = Column.Snow.LiquidMass_kgm2[LayerIdx];
                DiagnosticsPtr->SnowDensity_kgm3[LayerIdx] = Column.Snow.Density_kgm3[LayerIdx];
                DiagnosticsPtr->SnowGrainRadius_m[LayerIdx] = Column.Snow.GrainRadius_m[LayerIdx];
            }
            for (int32 LayerIdx = SnowLayerCount; LayerIdx < GFSM2MaxLayers; ++LayerIdx)
            {
                DiagnosticsPtr->SnowThickness_m[LayerIdx] = 0.0f;
                DiagnosticsPtr->SnowTemperature_K[LayerIdx] = 0.0f;
                DiagnosticsPtr->SnowIceMass_kgm2[LayerIdx] = 0.0f;
                DiagnosticsPtr->SnowLiquidMass_kgm2[LayerIdx] = 0.0f;
                DiagnosticsPtr->SnowDensity_kgm3[LayerIdx] = 0.0f;
                DiagnosticsPtr->SnowGrainRadius_m[LayerIdx] = 0.0f;
            }

            for (int32 SoilIdx = 0; SoilIdx < SoilLayerCount; ++SoilIdx)
            {
                DiagnosticsPtr->SoilThickness_m[SoilIdx] = Column.Soil.Thickness_m[SoilIdx];
                DiagnosticsPtr->SoilTemperature_K[SoilIdx] = Column.Soil.Temperature_K[SoilIdx];
                DiagnosticsPtr->SoilMoisture_Vol[SoilIdx] = Column.Soil.Moisture_VolumeFraction[SoilIdx];
            }
            for (int32 SoilIdx = SoilLayerCount; SoilIdx < GFSM2MaxSoilLayers; ++SoilIdx)
            {
                DiagnosticsPtr->SoilThickness_m[SoilIdx] = 0.0f;
                DiagnosticsPtr->SoilTemperature_K[SoilIdx] = 0.0f;
                DiagnosticsPtr->SoilMoisture_Vol[SoilIdx] = 0.0f;
            }

            DiagnosticsPtr->SimulationTimeSeconds = ElapsedSimulationSeconds + DtSeconds;
        }

        // Refresh albedo using the resolved surface state for the next timestep
    const bool bMeltingSurface = (SurfaceUpdate.MeltMass_kgm2 > KINDA_SMALL_NUMBER);
        const float AlbedoTempK = bMeltingSurface ? FreezePoint_K : SurfaceTemperatureK;
        Column.SnowAlbedo = UpdateSnowAlbedo(Column, DtSeconds, AlbedoTempK, EffectiveSnowRate);
        Column.SurfaceAlbedo = ComputeSurfaceAlbedo(Column, Column.SnowAlbedo, Column.SnowCoverFraction);
        CachedAlbedo[Index] = Column.SurfaceAlbedo;

        // Reset runoff after recording it for this step
        Column.Runoff_kgm2 = 0.0f;
    }, FSM2MainLoopFlags);

    if (bCollectDiagnostics && DiagnosticsBuffer.Num() > 0)
    {
        // Write diagnostics to file first (before moving data)
        EnsureDiagnosticsFileInitialized();
        const bool bShouldWriteThisStep =
            (SimulationStepCounter % FMath::Max(1, ModelParameters.Diagnostics.DiagnosticsEveryNSteps) == 0);
        if (bShouldWriteThisStep)
        {
            WriteDiagnostics(DiagnosticsBuffer);

            if (ASnowSimulationActor* Actor = OwningSimulationActor.Get())
            {
                if (Actor->bEnableRadiationDiagnostics)
                {
                    EnsureRadiationDiagnosticsFileInitialized();
                    WriteRadiationDiagnostics(DiagnosticsBuffer);
                }
            }
        }

        // Cache diagnostics for debug visualization (move after writing)
        LastStepDiagnostics = MoveTemp(DiagnosticsBuffer);
    }
    else
    {
        // Clear cache if diagnostics are disabled
        LastStepDiagnostics.Empty();
    }

    if (bUseDynamicSurfaceGeometry)
    {
        UpdateDynamicSurfaceGeometry();
    }

    ++SimulationStepCounter;
    ElapsedSimulationSeconds += DtSeconds;
}

float UFSM2SnowSimulation::GetMaxSnow()
{
	float LocalMax = 0.0f;
    for (const FFSM2ColumnState& Column : CellStates)
	{
        LocalMax = FMath::Max(LocalMax, ComputeSnowDepthMeters(Column.Snow));
	}
	return LocalMax * 1000.0f;
}

void UFSM2SnowSimulation::ResetState()
{
    for (FFSM2ColumnState& Column : CellStates)
	{
        InitializeColumnDefaults(Column);
	}
	CachedAlbedo.Reset();
}

void UFSM2SnowSimulation::EnsureStateSize(int32 CellCount)
{
	if (CellCount <= 0)
	{
		CellStates.Reset();
		CachedAlbedo.Reset();
		return;
	}

    const int32 PrevNum = CellStates.Num();
    if (PrevNum != CellCount)
	{
		CellStates.SetNum(CellCount, EAllowShrinking::No);
        for (int32 Idx = PrevNum; Idx < CellCount; ++Idx)
		{
            InitializeColumnDefaults(CellStates[Idx]);
		}
		CachedAlbedo.Reset();
	}
}

void UFSM2SnowSimulation::SanitizeLayerConfiguration()
{
    ModelParameters.Layers.MaxSnowLayers = FMath::Clamp(ModelParameters.Layers.MaxSnowLayers, 1, GFSM2MaxLayers);
    ModelParameters.Layers.MaxActiveLayers = FMath::Clamp(ModelParameters.Layers.MaxActiveLayers, 1, ModelParameters.Layers.MaxSnowLayers);
    ModelParameters.Layers.SoilLayerCount = FMath::Clamp(ModelParameters.Layers.SoilLayerCount, 0, GFSM2MaxSoilLayers);

    if (ModelParameters.Layers.MinimumSnowLayerThicknesses_m.Num() == 0)
    {
        // Default to FSM2 standard profile (0.1m top layer)
        ModelParameters.Layers.MinimumSnowLayerThicknesses_m = {0.1f, 0.2f, 0.4f};
    }

    ModelParameters.Layers.MaximumLayerThickness_m = FMath::Max(ModelParameters.Layers.MaximumLayerThickness_m, GetMinimumSnowLayerThickness(0));

    if (ModelParameters.Layers.SoilLayerThicknesses_m.Num() < ModelParameters.Layers.SoilLayerCount)
    {
        ModelParameters.Layers.SoilLayerThicknesses_m.SetNum(ModelParameters.Layers.SoilLayerCount);
    }
}

float UFSM2SnowSimulation::ComputeSlopeAttenuation(int32 CellIndex) const
{
	if (!ModelParameters.Radiation.bUseSlopeAdjustedShortwave || !bHasTerrainMetadata)
	{
		return 1.0f;
	}

	if (!TerrainSlopeDegrees.IsValidIndex(CellIndex))
	{
		return 1.0f;
	}

	const float SlopeDeg = TerrainSlopeDegrees[CellIndex];
	const float CosTheta = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(SlopeDeg, 0.0f, 80.0f)));
	return FMath::Clamp(CosTheta, 0.2f, 1.0f);
}

float UFSM2SnowSimulation::CalcSpecificHumidity(float TemperatureK, float RelativeHumidity, float PressurePa) const
{
    const float Ratio = ModelParameters.PhysicalConstants.WaterVapourMolecularRatio;
	const float es = CalcSaturationVapourPressure(TemperatureK);
	const float ea = FMath::Clamp(RelativeHumidity, 0.0f, 1.0f) * es;
    const float Den = PressurePa - (1.0f - Ratio) * ea;
    return (Den > 0.0f) ? Ratio * ea / Den : 0.0f;
}

float UFSM2SnowSimulation::CalcSaturationVapourPressure(float TemperatureK) const
{
    const float TempC = TemperatureK - ModelParameters.PhysicalConstants.MeltingPoint_K;
    const float e0 = ModelParameters.PhysicalConstants.SaturationVapourPressure0_Pa;
    
    if (TempC > 0.0f)
    {
        // Over Water (Fortran: 17.5043, 241.3)
        return e0 * FMath::Exp((17.5043f * TempC) / (TempC + 241.3f));
    }
    else
    {
        // Over Ice (Fortran: 22.4422, 272.186)
        return e0 * FMath::Exp((22.4422f * TempC) / (TempC + 272.186f));
    }
}

float UFSM2SnowSimulation::CalcAirDensity(float TemperatureK, float PressurePa) const
{
    return PressurePa / (ModelParameters.PhysicalConstants.GasConstantDryAir_JkgK * TemperatureK);
}

float UFSM2SnowSimulation::ClampStateDensity(const FFSM2SnowColumn& Snow, int32 LayerIdx, float ProposedDensity) const
{
    // Mirror Fortran FSM2 behavior: cold layers tighten to rcld, melting/wet layers to rmlt.
    const bool bMeltingLayer = Snow.Temperature_K[LayerIdx] >= FreezePoint_K - KelvinEpsilon;
    const float StateMax = bMeltingLayer
        ? FMath::Min(ModelParameters.Snow.MaxMeltSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3)
        : FMath::Min(ModelParameters.Snow.MaxColdSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3);

    const float PhysicalMax = ModelParameters.PhysicalConstants.DensityIce_kgm3;
    const float UpperBound = FMath::Min(StateMax, PhysicalMax);

    return FMath::Clamp(ProposedDensity, ModelParameters.Snow.MinSnowDensity_kgm3, UpperBound);
}

float UFSM2SnowSimulation::GetLayerMass(const FFSM2SnowColumn& Snow, int32 LayerIdx) const
{
    if (LayerIdx < 0 || LayerIdx >= Snow.LayerCount)
    {
        return 0.0f;
    }
    return FMath::Max(0.0f, Snow.IceMass_kgm2[LayerIdx] + Snow.LiquidMass_kgm2[LayerIdx]);
}

float UFSM2SnowSimulation::GetLayerHeatCapacity(const FFSM2SnowColumn& Snow, int32 LayerIdx) const
{
    if (LayerIdx < 0 || LayerIdx >= Snow.LayerCount)
    {
        return 0.0f;
    }
    return Snow.IceMass_kgm2[LayerIdx] * ModelParameters.PhysicalConstants.SpecificHeatIce_JkgK + Snow.LiquidMass_kgm2[LayerIdx] * ModelParameters.PhysicalConstants.SpecificHeatWater_JkgK;
}

float UFSM2SnowSimulation::GetLayerThermalConductivity(const FFSM2SnowColumn& Snow, int32 LayerIdx) const
{
    if (LayerIdx < 0 || LayerIdx >= Snow.LayerCount)
    {
        return ModelParameters.Snow.FixedSnowConductivity_WmK;
    }

    switch (ModelParameters.Snow.ConductivityScheme)
    {
    case EFSM2SnowConductivityScheme::Fixed:
        return ModelParameters.Snow.FixedSnowConductivity_WmK;

    case EFSM2SnowConductivityScheme::DensityDependent:
    default:
    {
        const float Density = FMath::Clamp<float>(Snow.Density_kgm3[LayerIdx], ModelParameters.Snow.MinSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3);
        const float DensityRatio = Density / ModelParameters.PhysicalConstants.DensityWater_kgm3;
        // Use empirical power law relationship from original FSM2: k = 2.224 * (ρ/ρ_water)^1.885
        const float ConductivityIce = ModelParameters.PhysicalConstants.ThermalConductivityIce_WmK;
        return FMath::Clamp<float>(ConductivityIce * FMath::Pow(DensityRatio, 1.885f), ModelParameters.PhysicalConstants.ThermalConductivityAir_WmK, ConductivityIce);
    }
    }
}

void UFSM2SnowSimulation::UpdateLayerDiagnostics(FFSM2SnowColumn& Snow, int32 LayerIdx) const
{
    if (LayerIdx < 0 || LayerIdx >= GFSM2MaxLayers)
    {
        return;
    }

    // Fortran does not re-clamp density here; it simply uses the current density to diagnose thickness.
    const float Density = FMath::Clamp(Snow.Density_kgm3[LayerIdx], ModelParameters.Snow.MinSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3);
    Snow.Density_kgm3[LayerIdx] = Density;
    const float Mass = Snow.IceMass_kgm2[LayerIdx] + Snow.LiquidMass_kgm2[LayerIdx];
    Snow.Thickness_m[LayerIdx] = (Mass > KINDA_SMALL_NUMBER && Density > 1.0f) ? Mass / Density : 0.0f;
}

void UFSM2SnowSimulation::InsertSurfaceLayer(FFSM2SnowColumn& Snow, float IceMass, float LiquidMass, float Density, float TemperatureK, float GrainRadius)
{
    if (Snow.LayerCount == 0)
    {
        // Initialize new snowpack
        Snow.LayerCount = 1;
        Snow.Thickness_m[0] = (Density > 1.0f) ? (IceMass + LiquidMass) / Density : 0.0f;
        Snow.Temperature_K[0] = FMath::Clamp(TemperatureK, ModelParameters.Snow.MinSnowTemperature_K, FreezePoint_K);
        Snow.IceMass_kgm2[0] = FMath::Max(0.0f, IceMass);
        Snow.LiquidMass_kgm2[0] = FMath::Max(0.0f, LiquidMass);
        Snow.Density_kgm3[0] = FMath::Clamp(Density, ModelParameters.Snow.MinSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3);
        Snow.GrainRadius_m[0] = GrainRadius;
        Snow.AgeHours[0] = 0.0f;
    }
    else
    {
        // Mix into existing top layer (Fortran style)
        const float OldMass = Snow.IceMass_kgm2[0] + Snow.LiquidMass_kgm2[0];
        const float NewMass = IceMass + LiquidMass;
        const float TotalMass = OldMass + NewMass;

        if (TotalMass > KINDA_SMALL_NUMBER)
        {
            // Mass-weighted grain radius
            Snow.GrainRadius_m[0] = (Snow.GrainRadius_m[0] * OldMass + GrainRadius * NewMass) / TotalMass;
            
            // Enthalpy mixing for temperature
            const float OldHeatCap = GetLayerHeatCapacity(Snow, 0);
            const float NewHeatCap = IceMass * ModelParameters.PhysicalConstants.SpecificHeatIce_JkgK + LiquidMass * ModelParameters.PhysicalConstants.SpecificHeatWater_JkgK;
            const float TotalHeatCap = OldHeatCap + NewHeatCap;
            
            if (TotalHeatCap > 0.0f)
            {
                const float OldEnthalpy = OldHeatCap * (Snow.Temperature_K[0] - FreezePoint_K);
                const float NewEnthalpy = NewHeatCap * (TemperatureK - FreezePoint_K);
                Snow.Temperature_K[0] = FreezePoint_K + (OldEnthalpy + NewEnthalpy) / TotalHeatCap;
            }

            // Volume-weighted density (Conservation of volume: V_new = V_old + V_added)
            const float AddedVolume = (Density > 1.0f) ? NewMass / Density : 0.0f;
            const float NewThickness = Snow.Thickness_m[0] + AddedVolume;
            
            float NewDensity = Snow.Density_kgm3[0];
            if (NewThickness > KINDA_SMALL_NUMBER)
            {
                NewDensity = TotalMass / NewThickness;
            }
            Snow.Density_kgm3[0] = FMath::Clamp(NewDensity, ModelParameters.Snow.MinSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3);
        }

        Snow.IceMass_kgm2[0] += IceMass;
        Snow.LiquidMass_kgm2[0] += LiquidMass;
    }

    UpdateLayerDiagnostics(Snow, 0);
}

void UFSM2SnowSimulation::MergeLayerDown(FFSM2SnowColumn& Snow, int32 UpperIndex)
{
    if (UpperIndex < 0 || UpperIndex >= Snow.LayerCount - 1)
    {
        return;
    }

    const int32 LowerIndex = UpperIndex + 1;

    const float UpperHeatCap = FMath::Max(GetLayerHeatCapacity(Snow, UpperIndex), MinHeatCapacity_Jm2K);
    const float LowerHeatCap = FMath::Max(GetLayerHeatCapacity(Snow, LowerIndex), MinHeatCapacity_Jm2K);

    Snow.IceMass_kgm2[LowerIndex] += Snow.IceMass_kgm2[UpperIndex];
    Snow.LiquidMass_kgm2[LowerIndex] += Snow.LiquidMass_kgm2[UpperIndex];
    Snow.IceMass_kgm2[LowerIndex] = FMath::Max(0.0f, Snow.IceMass_kgm2[LowerIndex]);
    Snow.LiquidMass_kgm2[LowerIndex] = FMath::Max(0.0f, Snow.LiquidMass_kgm2[LowerIndex]);

    const float UpperMass = GetLayerMass(Snow, UpperIndex);
    const float LowerMass = GetLayerMass(Snow, LowerIndex);
    const float TotalMass = UpperMass + LowerMass;
    if (TotalMass > KINDA_SMALL_NUMBER)
    {
        Snow.GrainRadius_m[LowerIndex] = FMath::Clamp((Snow.GrainRadius_m[LowerIndex] * LowerMass + Snow.GrainRadius_m[UpperIndex] * UpperMass) / TotalMass, 1.0e-6f, 1.0e-2f);
    }

    const float TotalHeatCap = UpperHeatCap + LowerHeatCap;
    if (TotalHeatCap > 0.0f)
    {
        Snow.Temperature_K[LowerIndex] = (Snow.Temperature_K[UpperIndex] * UpperHeatCap + Snow.Temperature_K[LowerIndex] * LowerHeatCap) / TotalHeatCap;
    }

    Snow.AgeHours[LowerIndex] = (Snow.AgeHours[LowerIndex] * LowerHeatCap + Snow.AgeHours[UpperIndex] * UpperHeatCap) / FMath::Max(TotalHeatCap, MinHeatCapacity_Jm2K);
    const float TotalThickness = Snow.Thickness_m[LowerIndex] + Snow.Thickness_m[UpperIndex];
    float MergedDensity = ModelParameters.Snow.MinSnowDensity_kgm3;
    if (TotalThickness > KINDA_SMALL_NUMBER)
    {
        MergedDensity = TotalMass / TotalThickness;
    }
    Snow.Density_kgm3[LowerIndex] = FMath::Clamp(MergedDensity, ModelParameters.Snow.MinSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3);
    UpdateLayerDiagnostics(Snow, LowerIndex);

    for (int32 ShiftIdx = UpperIndex; ShiftIdx < Snow.LayerCount - 1; ++ShiftIdx)
    {
        Snow.Thickness_m[ShiftIdx] = Snow.Thickness_m[ShiftIdx + 1];
        Snow.Temperature_K[ShiftIdx] = Snow.Temperature_K[ShiftIdx + 1];
        Snow.IceMass_kgm2[ShiftIdx] = Snow.IceMass_kgm2[ShiftIdx + 1];
        Snow.LiquidMass_kgm2[ShiftIdx] = Snow.LiquidMass_kgm2[ShiftIdx + 1];
        Snow.Density_kgm3[ShiftIdx] = Snow.Density_kgm3[ShiftIdx + 1];
        Snow.GrainRadius_m[ShiftIdx] = Snow.GrainRadius_m[ShiftIdx + 1];
        Snow.AgeHours[ShiftIdx] = Snow.AgeHours[ShiftIdx + 1];
    }

    Snow.LayerCount = FMath::Max(0, Snow.LayerCount - 1);
    if (Snow.LayerCount >= 0 && Snow.LayerCount < GFSM2MaxLayers)
    {
        const int32 ClearIdx = Snow.LayerCount;
        Snow.Thickness_m[ClearIdx] = 0.0f;
        Snow.Temperature_K[ClearIdx] = FreezePoint_K;
        Snow.IceMass_kgm2[ClearIdx] = 0.0f;
        Snow.LiquidMass_kgm2[ClearIdx] = 0.0f;
        Snow.Density_kgm3[ClearIdx] = ModelParameters.Snow.MinSnowDensity_kgm3;
        Snow.GrainRadius_m[ClearIdx] = FMath::Clamp(ModelParameters.Snow.FreshSnowGrainRadius_m, 1.0e-6f, 1.0e-2f);
        Snow.AgeHours[ClearIdx] = 0.0f;
    }
}

void UFSM2SnowSimulation::CullAndMergeLayers(FFSM2SnowColumn& Snow)
{
    for (int32 LayerIdx = Snow.LayerCount - 1; LayerIdx >= 0; --LayerIdx)
    {
        if (GetLayerMass(Snow, LayerIdx) < 1e-4f)
        {
            for (int32 ShiftIdx = LayerIdx; ShiftIdx < Snow.LayerCount - 1; ++ShiftIdx)
            {
                Snow.Thickness_m[ShiftIdx] = Snow.Thickness_m[ShiftIdx + 1];
                Snow.Temperature_K[ShiftIdx] = Snow.Temperature_K[ShiftIdx + 1];
                Snow.IceMass_kgm2[ShiftIdx] = Snow.IceMass_kgm2[ShiftIdx + 1];
                Snow.LiquidMass_kgm2[ShiftIdx] = Snow.LiquidMass_kgm2[ShiftIdx + 1];
                Snow.Density_kgm3[ShiftIdx] = Snow.Density_kgm3[ShiftIdx + 1];
                Snow.GrainRadius_m[ShiftIdx] = Snow.GrainRadius_m[ShiftIdx + 1];
                Snow.AgeHours[ShiftIdx] = Snow.AgeHours[ShiftIdx + 1];
            }
            Snow.LayerCount = FMath::Max(0, Snow.LayerCount - 1);
        }
    }

    if (Snow.LayerCount <= 1)
    {
        return;
    }

    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount - 1; ++LayerIdx)
    {
        const float MinThickness = GetMinimumSnowLayerThickness(LayerIdx);
        const float MaxThickness = FMath::Max(ModelParameters.Layers.MaximumLayerThickness_m, MinThickness);

        if (Snow.Thickness_m[LayerIdx] < MinThickness)
        {
            MergeLayerDown(Snow, LayerIdx);
            --LayerIdx;
			continue;
		}

        if (Snow.Thickness_m[LayerIdx] > MaxThickness)
        {
            MergeLayerDown(Snow, LayerIdx);
            --LayerIdx;
        }
    }

    const int32 MaxLayersAllowed = FMath::Clamp(ModelParameters.Layers.MaxActiveLayers, 1, GFSM2MaxLayers);
    while (Snow.LayerCount > MaxLayersAllowed)
    {
        MergeLayerDown(Snow, Snow.LayerCount - 2);
    }
}

void UFSM2SnowSimulation::ApplyDensityEvolution(FFSM2SnowColumn& Snow, float DtSeconds)
{
    if (Snow.LayerCount == 0)
    {
        return;
    }

    switch (ModelParameters.Snow.DensityScheme)
    {
    case EFSM2DensityScheme::FixedDensity:
    {
        const float Target = FMath::Clamp(ModelParameters.Snow.FixedSnowDensity_kgm3, ModelParameters.Snow.MinSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3);
        for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
        {
            if (GetLayerMass(Snow, LayerIdx) < KINDA_SMALL_NUMBER)
            {
                continue;
            }

            Snow.Density_kgm3[LayerIdx] = Target;
            UpdateLayerDiagnostics(Snow, LayerIdx);
        }
        break;
    }

    case EFSM2DensityScheme::AgeCompaction:
    {
        for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
        {
            if (GetLayerMass(Snow, LayerIdx) < KINDA_SMALL_NUMBER)
            {
                continue;
            }

            const bool bMelting = Snow.Temperature_K[LayerIdx] >= FreezePoint_K - KelvinEpsilon;
            
            // Fortran DENSTY=1 logic:
            // if (Tsnow(n) >= Tm) then
            //     if (rhos < rmlt) rhos = rmlt + (rhos - rmlt)*exp(-dt/trho)
            // else
            //     if (rhos < rcld) rhos = rcld + (rhos - rcld)*exp(-dt/trho)
            // end if

            const float TargetDensityRaw = bMelting ? ModelParameters.Snow.MaxMeltSnowDensity_kgm3 : ModelParameters.Snow.MaxColdSnowDensity_kgm3;
            const float TargetDensity = FMath::Min(TargetDensityRaw, ModelParameters.Snow.MaxSnowDensity_kgm3);
            const float Timescale = ModelParameters.Snow.CompactionTimescale_s;
            const float CurrentDensity = Snow.Density_kgm3[LayerIdx];

            float NewDensity = CurrentDensity;
            // Only apply compaction if current density is LESS than target
            // If density is already higher (e.g. due to melt/rain), do not artificially lower it
            if (CurrentDensity < TargetDensity)
            {
                NewDensity = TargetDensity + (CurrentDensity - TargetDensity) * FMath::Exp(-DtSeconds / FMath::Max(Timescale, 1.0f));
                Snow.Density_kgm3[LayerIdx] = FMath::Clamp(NewDensity, ModelParameters.Snow.MinSnowDensity_kgm3, TargetDensity);
            }
            // Else: maintain current high density (don't clamp down to TargetDensity)
            
            UpdateLayerDiagnostics(Snow, LayerIdx);
        }
        break;
    }

    case EFSM2DensityScheme::OverburdenCompaction:
    {
        // Fortran SNOW.F90 lines 243-254 (DENSTY=2). No state-density cap is applied:
        // density self-limits because the viscosity exp(rho/55.6) and metamorphism
        // exp(-(rho-150)/21.7) terms grow rapidly with rho, asymptotically halting compaction.
        // Imposing rcld/rmlt as a hard ceiling truncates this asymptote and produces a
        // visible plateau in the density curve (Bug B).
        float OverburdenMass = 0.0f;
        const float Gravity = ModelParameters.PhysicalConstants.Gravity_mps2;
        const float MinDensity = ModelParameters.Snow.MinSnowDensity_kgm3;
        const float PhysicalMax = ModelParameters.PhysicalConstants.DensityIce_kgm3;

        for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
        {
            const float Mass = GetLayerMass(Snow, LayerIdx);
            if (Mass < KINDA_SMALL_NUMBER)
            {
                continue;
            }

            OverburdenMass += 0.5f * Mass;

            const float Thickness = FMath::Max(Snow.Thickness_m[LayerIdx], MinimumLayerThickness_m);
            float Density = FMath::Clamp(Mass / Thickness, MinDensity, PhysicalMax);

            const float TemperatureOffset = Snow.Temperature_K[LayerIdx] - FreezePoint_K;
            const float ViscosityTerm = FMath::Max(ModelParameters.Snow.ReferenceViscosity_PaS * FMath::Exp(-TemperatureOffset / 12.4f + Density / 55.6f), 1.0f);
            const float CompactionIncrement = (Density * Gravity * OverburdenMass * DtSeconds) / ViscosityTerm;
            const float MetamorphismIncrement = DtSeconds * Density * ModelParameters.Snow.ThermalMetamorphismRate_s * FMath::Exp(TemperatureOffset / 23.8f - FMath::Max(Density - 150.0f, 0.0f) / 21.7f);
            Density = FMath::Clamp(Density + CompactionIncrement + MetamorphismIncrement, MinDensity, PhysicalMax);

            Snow.Density_kgm3[LayerIdx] = Density;
            UpdateLayerDiagnostics(Snow, LayerIdx);

            OverburdenMass += 0.5f * Mass;
        }
        break;
    }

    default:
        break;
    }
}

void UFSM2SnowSimulation::MeltRefreezeStage(FFSM2SnowColumn& Snow, const FFSM2SurfaceUpdate& SurfaceUpdate, float ThroughfallRain_kgm2, float DtSeconds, float& OutSublimationMass_kgm2)
{
    OutSublimationMass_kgm2 = 0.0f;

    if (Snow.LayerCount == 0)
    {
        return;
    }

    // Add rainfall to top layer liquid water content (do not change thickness here;
    // Fortran keeps Dsnw fixed and lets density adjust via compaction later)
    if (ThroughfallRain_kgm2 > KINDA_SMALL_NUMBER)
    {
        Snow.LiquidMass_kgm2[0] += ThroughfallRain_kgm2;
        // Keep thickness unchanged by setting density to conserve volume at current thickness
        const float CurrentThickness = FMath::Max(Snow.Thickness_m[0], MinimumLayerThickness_m);
        const float NewMass = Snow.IceMass_kgm2[0] + Snow.LiquidMass_kgm2[0];
        const float PhysicalMaxDensity = ModelParameters.PhysicalConstants.DensityIce_kgm3;
        const float NewDensity = (CurrentThickness > KINDA_SMALL_NUMBER)
            ? FMath::Clamp(NewMass / CurrentThickness, ModelParameters.Snow.MinSnowDensity_kgm3, PhysicalMaxDensity)
            : Snow.Density_kgm3[0];
        Snow.Density_kgm3[0] = NewDensity;
    }

    // Calculate sublimation mass from surface moisture flux.
    // SurfaceMassFlux_kgm2s is positive when moisture leaves the surface (sublimation/evaporation).
    // Fortran SRFEBAL.F90 lines 610-616 caps sublimation by remaining ice after melt:
    //     Ssub = sum(Sice(:)) - Melt*dt
    //     if (Ssub > 0 .or. Tsrf<Tm) Esrf = min(Esrf, Ssub/dt)
    // i.e., only ICE counts toward the cap, and pending melt has first claim on it. (Bug C)
    if (SurfaceUpdate.SurfaceMassFlux_kgm2s > 0.0f)
    {
        const float PotentialSublimationMass = SurfaceUpdate.SurfaceMassFlux_kgm2s * DtSeconds;
        const float PendingMeltMass = FMath::Max(0.0f, SurfaceUpdate.MeltMass_kgm2);
        const float AvailableIceMass = FMath::Max(0.0f, ComputeSnowIceMass(Snow) - PendingMeltMass);

        if (AvailableIceMass > KINDA_SMALL_NUMBER)
        {
            const float MaxSublimationRate = AvailableIceMass / DtSeconds;
            const float ActualSublimationRate = FMath::Min(SurfaceUpdate.SurfaceMassFlux_kgm2s, MaxSublimationRate);
            OutSublimationMass_kgm2 = ActualSublimationRate * DtSeconds;
        }
        else if (SurfaceUpdate.TemperatureK < FreezePoint_K)
        {
            // No ice left but surface is sub-freezing: keep parity with Fortran's Ssub<=0 .and. Tsrf<Tm
            // branch which still allows the flux through (frost handling lives elsewhere).
            OutSublimationMass_kgm2 = PotentialSublimationMass;
        }
        // Else: no ice and surface above freezing — sublimation is zero.
    }

    // Apply melt and sublimation using Fortran-style approach
    // Extract rates from surface energy balance
    const float MeltMass_kgm2 = FMath::Max(0.0f, SurfaceUpdate.MeltMass_kgm2);

    ApplySurfaceMeltFortran(Snow, MeltMass_kgm2 / DtSeconds, DtSeconds);
    ApplySurfaceSublimationFortran(Snow, OutSublimationMass_kgm2 / DtSeconds, DtSeconds);

    UpdateLayerDiagnostics(Snow, 0);
}

void UFSM2SnowSimulation::ApplySurfaceMelt(FFSM2SnowColumn& Snow, float MeltMass_kgm2)
{
    if (Snow.LayerCount <= 0 || MeltMass_kgm2 <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    float RemainingMelt = MeltMass_kgm2;
    int32 LayerIdx = 0;

    while (LayerIdx < Snow.LayerCount && RemainingMelt > KINDA_SMALL_NUMBER)
    {
        const float LayerMass = GetLayerMass(Snow, LayerIdx);

        if (LayerMass <= KINDA_SMALL_NUMBER)
        {
            MergeLayerDown(Snow, LayerIdx);
            continue;
        }

        const float MeltHere = FMath::Min(LayerMass, RemainingMelt);
        const float MeltFraction = MeltHere / LayerMass;

        Snow.IceMass_kgm2[LayerIdx] = FMath::Max(0.0f, Snow.IceMass_kgm2[LayerIdx] - MeltHere);
        const float OldIceMass = LayerMass; // Actually LayerMass is TotalMass? No, GetLayerMass returns Ice+Liq
        // Wait, ApplySurfaceMelt logic is different. 
        // It iterates.
        // Let's stick to fixing density update.
        
        Snow.LiquidMass_kgm2[LayerIdx] += MeltHere;
        Snow.AgeHours[LayerIdx] = Snow.AgeHours[LayerIdx] * (1.0f - MeltFraction);
        RemainingMelt -= MeltHere;
        RemainingMelt = FMath::Max(RemainingMelt, 0.0f);

        // FIX: Apply thickness collapse and density update for this non-Fortran version too
        // Assuming this version should also follow FSM2 physics
        // Thickness should reduce by MeltFraction of the ICE portion?
        // This function doesn't explicitly reduce Thickness. 
        // It calls UpdateLayerDiagnostics which sets Thickness = Mass / Density.
        // Since Density is constant, Thickness is constant. 
        // If we want collapse, we must increase Density.
        // NewDensity = OldDensity * (IceOld / IceNew)
        
        const float IceNew = Snow.IceMass_kgm2[LayerIdx];
        const float IceOld = IceNew + MeltHere;
        if (IceNew > KINDA_SMALL_NUMBER && IceOld > KINDA_SMALL_NUMBER)
        {
            const float NewDensity = Snow.Density_kgm3[LayerIdx] * (IceOld / IceNew);
            const float PhysicalMaxDensity = ModelParameters.PhysicalConstants.DensityIce_kgm3;
            Snow.Density_kgm3[LayerIdx] = FMath::Clamp(NewDensity, ModelParameters.Snow.MinSnowDensity_kgm3, PhysicalMaxDensity);
        }

        UpdateLayerDiagnostics(Snow, LayerIdx);

        if (GetLayerMass(Snow, LayerIdx) <= KINDA_SMALL_NUMBER)
        {
            MergeLayerDown(Snow, LayerIdx);
            continue;
        }

        ++LayerIdx;
    }
}

// Fortran-style surface melt implementation (SNOW.F90 lines 169-190)
void UFSM2SnowSimulation::ApplySurfaceMeltFortran(FFSM2SnowColumn& Snow, float MeltRate_kgm2s, float DtSeconds)
{
    if (Snow.LayerCount <= 0 || DtSeconds <= 0.0f)
    {
        return;
    }

    const float LatentHeatFusion = FMath::Max(ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg, 1.0f);

    // dSice = Melt*dt (Fortran SNOW.F90 line 170)
    float RemainingMelt = FMath::Max(MeltRate_kgm2s, 0.0f) * DtSeconds;

    // Walk down layers. Per Fortran, each iteration: (1) donate warm-layer cold content into
    // dSice and pin warm layer to Tm; (2) consume dSice from this layer's ice if any remains.
    // Cold layers (coldcont >= 0) contribute nothing here and are NOT thermally modified.
    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        const float LayerIceMass = Snow.IceMass_kgm2[LayerIdx];
        if (LayerIceMass <= KINDA_SMALL_NUMBER)
        {
            continue;
        }

        // Fortran SNOW.F90 lines 172-176: warm-layer cold-content donation.
        // coldcont = csnow * (Tm - Tsnow). If coldcont < 0 the layer is above Tm; release
        // the excess thermal energy as additional melt and snap the layer to Tm.
        const float HeatCapacity = GetLayerHeatCapacity(Snow, LayerIdx);
        const float ColdContent = HeatCapacity * (FreezePoint_K - Snow.Temperature_K[LayerIdx]);
        if (ColdContent < 0.0f)
        {
            RemainingMelt -= ColdContent / LatentHeatFusion;  // -coldcont/Lf > 0
            Snow.Temperature_K[LayerIdx] = FreezePoint_K;
        }

        if (RemainingMelt <= KINDA_SMALL_NUMBER)
        {
            // Still need to commit any temperature change made above.
            UpdateLayerDiagnostics(Snow, LayerIdx);
            continue;
        }

        if (RemainingMelt > LayerIceMass)
        {
            // Fortran SNOW.F90 lines 178-182: layer melts completely.
            RemainingMelt -= LayerIceMass;
            Snow.LiquidMass_kgm2[LayerIdx] += LayerIceMass;
            Snow.IceMass_kgm2[LayerIdx] = 0.0f;
            Snow.Thickness_m[LayerIdx] = 0.0f;
            // Cold layers below keep their original Tsnow; only warm-layer branch above pins to Tm.
        }
        else
        {
            // Fortran SNOW.F90 lines 183-187: layer melts partially.
            const float MeltHere = RemainingMelt;
            const float MeltFraction = MeltHere / LayerIceMass;
            Snow.IceMass_kgm2[LayerIdx] -= MeltHere;
            Snow.LiquidMass_kgm2[LayerIdx] += MeltHere;
            Snow.Thickness_m[LayerIdx] *= FMath::Max(1.0f - MeltFraction, 0.0f);
            RemainingMelt = 0.0f;

            // Reflect Dsnw collapse in density. Fortran does not clamp here; let the
            // overburden viscosity term self-limit via exp(rhos/55.6). Use physical bounds only.
            if (Snow.Thickness_m[LayerIdx] > KINDA_SMALL_NUMBER)
            {
                const float NewMass = Snow.IceMass_kgm2[LayerIdx] + Snow.LiquidMass_kgm2[LayerIdx];
                const float NewDensity = NewMass / Snow.Thickness_m[LayerIdx];
                const float PhysicalMax = ModelParameters.PhysicalConstants.DensityIce_kgm3;
                Snow.Density_kgm3[LayerIdx] = FMath::Clamp(NewDensity, ModelParameters.Snow.MinSnowDensity_kgm3, PhysicalMax);
            }
        }

        UpdateLayerDiagnostics(Snow, LayerIdx);
    }
}

// Fortran-style surface sublimation implementation
void UFSM2SnowSimulation::ApplySurfaceSublimationFortran(FFSM2SnowColumn& Snow, float SublimationRate_kgm2s, float DtSeconds)
{
    if (Snow.LayerCount <= 0 || DtSeconds <= 0.0f)
    {
        return;
    }

    const float SublimationMass = SublimationRate_kgm2s * DtSeconds;
    if (SublimationMass <= 1.0e-6f)
    {
        return;
    }

    float RemainingSublimation = SublimationMass; // δI = Eδt

    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount && RemainingSublimation > KINDA_SMALL_NUMBER; ++LayerIdx)
    {
        const float LayerIceMass = Snow.IceMass_kgm2[LayerIdx];
        if (LayerIceMass <= KINDA_SMALL_NUMBER)
        {
            continue;
        }

        if (RemainingSublimation > LayerIceMass)
        {
            // Layer sublimates completely - remove layer
            RemainingSublimation -= LayerIceMass;
            Snow.Thickness_m[LayerIdx] = 0.0f;
            Snow.IceMass_kgm2[LayerIdx] = 0.0f;
        }
        else
        {
            // Layer sublimates partially - reduce thickness proportionally
            Snow.Thickness_m[LayerIdx] *= (1.0f - RemainingSublimation / LayerIceMass);
            Snow.IceMass_kgm2[LayerIdx] -= RemainingSublimation;
            RemainingSublimation = 0.0f;
        }

        UpdateLayerDiagnostics(Snow, LayerIdx);
    }
}

float UFSM2SnowSimulation::GetMinimumSnowLayerThickness(int32 LayerIdx) const
{
    const float GlobalMinimum = FMath::Max(ModelParameters.Layers.MinimumLayerThickness_m, MinimumLayerThickness_m);

    const TArray<float>& Configured = ModelParameters.Layers.MinimumSnowLayerThicknesses_m;
    if (Configured.Num() > 0)
    {
        if (LayerIdx < Configured.Num())
        {
            return FMath::Max(Configured[LayerIdx], GlobalMinimum);
        }

        return FMath::Max(Configured.Last(), GlobalMinimum);
    }

    return GlobalMinimum;
}

void UFSM2SnowSimulation::ConductiveHeatExchange(FFSM2SnowColumn& Snow, FFSM2SoilColumn& Soil, float DtSeconds, float SurfaceFlux_Wm2)
{
    const int32 SnowCount = FMath::Clamp(Snow.LayerCount, 0, GFSM2MaxLayers);
    const int32 SoilCount = FMath::Clamp(Soil.LayerCount, 0, GFSM2MaxSoilLayers);

    if ((SnowCount <= 0 && SoilCount <= 0) || DtSeconds <= 0.0f)
    {
        return;
    }

    double Gsoil_Wm2 = SurfaceFlux_Wm2; // Default to surface flux when no snow (matches Fortran: Gsoil = Gsrf)
    if (SnowCount > 0)
    {
        ComputeSnowConduction(Snow, Soil, DtSeconds, SurfaceFlux_Wm2, Gsoil_Wm2);
    }

    if (SoilCount > 0)
    {
        AdvanceSoilColumn(Soil, DtSeconds, Gsoil_Wm2);
    }
}

void UFSM2SnowSimulation::HydrologyStage(FFSM2SnowColumn& Snow, float DtSeconds, float SurfaceRainInput_kgm2, float& OutRunoff_kgm2, FFSM2CellDiagnostics* OptionalDiagnostics)
{
    OutRunoff_kgm2 = 0.0f;
    const float SurfaceRainRate = (DtSeconds > 0.0f) ? SurfaceRainInput_kgm2 / DtSeconds : 0.0f;

    switch (ModelParameters.Snow.HydrologyScheme)
    {
    case EFSM2HydrologyScheme::FreeDrain:
        HydrologyFreeDrain(Snow, OutRunoff_kgm2, OptionalDiagnostics);
        break;

    case EFSM2HydrologyScheme::Bucket:
        HydrologyBucket(Snow, DtSeconds, SurfaceRainRate, OutRunoff_kgm2, OptionalDiagnostics);
        break;

    case EFSM2HydrologyScheme::Darcy:
        HydrologyGravitational(Snow, DtSeconds, SurfaceRainRate, OutRunoff_kgm2, OptionalDiagnostics);
        break;

    default:
        HydrologyBucket(Snow, DtSeconds, SurfaceRainRate, OutRunoff_kgm2, OptionalDiagnostics);
        break;
    }
}

void UFSM2SnowSimulation::HydrologyFreeDrain(FFSM2SnowColumn& Snow, float& OutRunoff_kgm2, FFSM2CellDiagnostics* OptionalDiagnostics)
{
    float RunoffRate = 0.0f; // kg/m²/s accumulator

    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        if (Snow.LiquidMass_kgm2[LayerIdx] <= KINDA_SMALL_NUMBER)
        {
            if (OptionalDiagnostics && LayerIdx < GFSM2MaxLayers)
            {
                OptionalDiagnostics->SnowWaterFlux_kgm2s[LayerIdx] = 0.0f;
            }
            continue;
        }

        OutRunoff_kgm2 += Snow.LiquidMass_kgm2[LayerIdx];
        RunoffRate += Snow.LiquidMass_kgm2[LayerIdx]; // All liquid becomes runoff
        
        // FIX: Update density to reflect mass loss at constant volume (pore emptying)
        // If we don't do this, UpdateLayerDiagnostics will use the old high density 
        // and shrink the thickness to match the new lower mass, causing artificial compaction.
        if (Snow.Thickness_m[LayerIdx] > KINDA_SMALL_NUMBER)
        {
            const float NewMass = Snow.IceMass_kgm2[LayerIdx]; // Liquid becomes 0
            Snow.Density_kgm3[LayerIdx] = NewMass / Snow.Thickness_m[LayerIdx];
        }
        
        Snow.LiquidMass_kgm2[LayerIdx] = 0.0f;
        UpdateLayerDiagnostics(Snow, LayerIdx);

        // Record water flux (instantaneous drainage in free-drain mode)
        if (OptionalDiagnostics && LayerIdx < GFSM2MaxLayers)
        {
            OptionalDiagnostics->SnowWaterFlux_kgm2s[LayerIdx] = RunoffRate;
        }
    }
}

void UFSM2SnowSimulation::HydrologyBucket(FFSM2SnowColumn& Snow, float DtSeconds, float SurfaceRainRate, float& OutRunoff_kgm2, FFSM2CellDiagnostics* OptionalDiagnostics)
{
    if (Snow.LayerCount == 0 || DtSeconds <= 0.0f)
    {
        return;
    }

    // Check if there's any liquid water or rainfall to process
    bool HasLiquid = false;
    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        if (Snow.LiquidMass_kgm2[LayerIdx] > KINDA_SMALL_NUMBER)
        {
            HasLiquid = true;
            break;
        }
    }

    if (!HasLiquid)
    {
        // Zero out water flux in diagnostics if no liquid
        if (OptionalDiagnostics)
        {
            for (int32 i = 0; i < GFSM2MaxLayers; ++i)
            {
                OptionalDiagnostics->SnowWaterFlux_kgm2s[i] = 0.0f;
            }
        }
        return;
    }

    const float DensityWater = ModelParameters.PhysicalConstants.DensityWater_kgm3;
    const float DensityIce = ModelParameters.PhysicalConstants.DensityIce_kgm3;
    const float LatentHeatFusion = ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg;
    const float IrreducibleWaterFraction = FMath::Clamp(ModelParameters.Snow.IrreducibleWaterFraction, 0.0f, 0.2f);

    float CurrentRunoff = 0.0f; // Runoff to pass to next layer (kg/m²/s)

    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        // Use raw layer thickness — Fortran SNOW.F90 line 425 has no minimum floor, only
        // a `Dsnw(n) > epsilon(Dsnw)` guard. Forcing thickness up to MinimumLayerThickness_m
        // here inflated thin-layer liquid capacity (Bug F), so residual layers near the end
        // of the melt season retained meltwater that should have drained.
        const float Thickness = Snow.Thickness_m[LayerIdx];
        if (Thickness <= KINDA_SMALL_NUMBER)
        {
            // Layer has no real volume — drain any liquid straight through.
            const float Inflow = CurrentRunoff + (LayerIdx == 0 ? SurfaceRainRate : 0.0f);
            CurrentRunoff += Snow.LiquidMass_kgm2[LayerIdx] / DtSeconds;
            Snow.LiquidMass_kgm2[LayerIdx] = 0.0f;
            if (OptionalDiagnostics && LayerIdx < GFSM2MaxLayers)
            {
                OptionalDiagnostics->SnowWaterFlux_kgm2s[LayerIdx] = Inflow;
            }
            continue;
        }

        // Porosity (Fortran SNOW.F90 line 425): phi = 1 - Sice / (rho_ice * Dsnw)
        float Porosity = 1.0f - Snow.IceMass_kgm2[LayerIdx] / (DensityIce * Thickness);
        Porosity = FMath::Max(0.0f, Porosity);

        // Max irreducible liquid (Fortran line 427): SliqMax = rho_wat * Dsnw * phi * Wirr
        const float MaxLiquidMass = DensityWater * Thickness * Porosity * IrreducibleWaterFraction;

        // Capture inflow to this layer (kg/m^2/s) to mirror Fortran Wflx meaning
        const float InboundFlux = CurrentRunoff;

        // Add incoming runoff to this layer
        Snow.LiquidMass_kgm2[LayerIdx] += CurrentRunoff * DtSeconds;
        // Reset runoff accumulator; we will compute the outbound flux below
        CurrentRunoff = 0.0f;

        // Record inbound flux for this layer (Fortran Wflx is "into snow layer")
        if (OptionalDiagnostics && LayerIdx < GFSM2MaxLayers)
        {
            float Inflow = InboundFlux;
            if (LayerIdx == 0)
            {
                // Include surface rain rate so diagnostics match Fortran Wflx(1)=Roff
                Inflow += SurfaceRainRate;
            }
            OptionalDiagnostics->SnowWaterFlux_kgm2s[LayerIdx] = Inflow;
        }

        // Check if liquid capacity is exceeded
        if (Snow.LiquidMass_kgm2[LayerIdx] > MaxLiquidMass)
        {
            // Calculate drainage rate (kg/m²/s)
            CurrentRunoff = (Snow.LiquidMass_kgm2[LayerIdx] - MaxLiquidMass) / DtSeconds;

            // Preserve layer thickness through the inverted (Density-primary) state by
            // updating Density to the current Mass/Thickness. UpdateLayerDiagnostics will
            // then re-derive Thickness as Mass/Density ≈ original Thickness.
            const float NewMass = Snow.IceMass_kgm2[LayerIdx] + MaxLiquidMass;
            Snow.Density_kgm3[LayerIdx] = NewMass / Thickness;

            // Set liquid mass to maximum capacity
            Snow.LiquidMass_kgm2[LayerIdx] = MaxLiquidMass;
        }

        // Ensure liquid mass doesn't go negative
        Snow.LiquidMass_kgm2[LayerIdx] = FMath::Max(0.0f, Snow.LiquidMass_kgm2[LayerIdx]);

        // Calculate heat capacity
        const float HeatCapacity = GetLayerHeatCapacity(Snow, LayerIdx);

        // Check for freezing
        const float ColdContent = HeatCapacity * FMath::Max(FreezePoint_K - Snow.Temperature_K[LayerIdx], 0.0f);
        if (ColdContent > 0.0f && Snow.LiquidMass_kgm2[LayerIdx] > KINDA_SMALL_NUMBER)
        {
            // Calculate how much liquid can freeze
            const float FreezeMass = FMath::Min(Snow.LiquidMass_kgm2[LayerIdx],
                ColdContent / FMath::Max(LatentHeatFusion, 1.0f));

            // Update masses and temperature
            Snow.LiquidMass_kgm2[LayerIdx] -= FreezeMass;
            Snow.IceMass_kgm2[LayerIdx] += FreezeMass;
            Snow.Temperature_K[LayerIdx] += (LatentHeatFusion * FreezeMass) / FMath::Max(HeatCapacity, MinHeatCapacity_Jm2K);
            Snow.Temperature_K[LayerIdx] = FMath::Clamp(Snow.Temperature_K[LayerIdx],
                ModelParameters.Snow.MinSnowTemperature_K, FreezePoint_K);
        }

        UpdateLayerDiagnostics(Snow, LayerIdx);
    }

    // For Fortran parity: bottom layer flux in NetCDF is overwritten with runoff.
    if (OptionalDiagnostics && Snow.LayerCount > 0)
    {
        const int32 BottomIdx = FMath::Clamp(Snow.LayerCount - 1, 0, GFSM2MaxLayers - 1);
        OptionalDiagnostics->SnowWaterFlux_kgm2s[BottomIdx] = CurrentRunoff;
    }

    // Any remaining runoff becomes total runoff
    OutRunoff_kgm2 += CurrentRunoff * DtSeconds;
}

void UFSM2SnowSimulation::HydrologyGravitational(FFSM2SnowColumn& Snow, float DtSeconds, float SurfaceRainRate_kgm2s, float& OutRunoff_kgm2, FFSM2CellDiagnostics* OptionalDiagnostics)
{
    if (Snow.LayerCount == 0 || DtSeconds <= 0.0f)
    {
        return;
    }

    const int32 LayerCount = FMath::Clamp(Snow.LayerCount, 0, GFSM2MaxLayers);
    const int32 SubSteps = FMath::Max(1, ModelParameters.Snow.HydrologySubsteps);
    const int32 Iterations = FMath::Max(1, ModelParameters.Snow.DarcyIterations);
    const float SubDt = DtSeconds / static_cast<float>(SubSteps);
    const float NewtonAveraging = 1.0f / static_cast<float>(Iterations);
    const float ResidualFrac = FMath::Clamp(ModelParameters.Snow.IrreducibleWaterFraction, 0.0f, 0.2f);
    const float DensityWater = ModelParameters.PhysicalConstants.DensityWater_kgm3;
    const float DensityIce = ModelParameters.PhysicalConstants.DensityIce_kgm3;
    const float Gravity = ModelParameters.PhysicalConstants.Gravity_mps2;
    const float Viscosity = ModelParameters.PhysicalConstants.WaterDynamicViscosity_Pas;
    const float TopRainRate = FMath::Max(SurfaceRainRate_kgm2s, 0.0f);

    TStaticArray<float, GFSM2MaxLayers + 1> Qw;
    TStaticArray<float, GFSM2MaxLayers> Phi;
    TStaticArray<float, GFSM2MaxLayers> Theta;
    TStaticArray<float, GFSM2MaxLayers> Theta0;
    TStaticArray<float, GFSM2MaxLayers> ThetaR;
    TStaticArray<float, GFSM2MaxLayers> Thickness;
    TStaticArray<float, GFSM2MaxLayers> Ksat;
    TStaticArray<float, GFSM2MaxLayers> a;
    TStaticArray<float, GFSM2MaxLayers> b;
    TStaticArray<float, GFSM2MaxLayers> rhs;
    TStaticArray<float, GFSM2MaxLayers> dTheta;

    if (OptionalDiagnostics)
    {
        for (int32 LayerIdx = 0; LayerIdx < GFSM2MaxLayers; ++LayerIdx)
        {
            OptionalDiagnostics->SnowWaterFlux_kgm2s[LayerIdx] = 0.0f;
        }
    }

    for (int32 StepIdx = 0; StepIdx < SubSteps; ++StepIdx)
    {
        Qw[0] = TopRainRate / FMath::Max(DensityWater, 1.0f); // m/s
        for (int32 i = 1; i < GFSM2MaxLayers + 1; ++i)
        {
            Qw[i] = 0.0f;
        }

        for (int32 LayerIdx = 0; LayerIdx < LayerCount; ++LayerIdx)
        {
            Thickness[LayerIdx] = FMath::Max(Snow.Thickness_m[LayerIdx], MinimumLayerThickness_m);
            Phi[LayerIdx] = FMath::Clamp(1.0f - Snow.IceMass_kgm2[LayerIdx] / (DensityIce * Thickness[LayerIdx]), 0.0f, 0.95f);
            ThetaR[LayerIdx] = ResidualFrac * Phi[LayerIdx];
            Theta[LayerIdx] = Snow.LiquidMass_kgm2[LayerIdx] / (DensityWater * Thickness[LayerIdx]);
            Ksat[LayerIdx] = 0.31f * (DensityWater * Gravity / Viscosity)
                * FMath::Square(FMath::Max(Snow.GrainRadius_m[LayerIdx], 1.0e-6f))
                * FMath::Exp(-7.8f * Snow.IceMass_kgm2[LayerIdx] / (DensityWater * Thickness[LayerIdx]));

            if (Theta[LayerIdx] > Phi[LayerIdx])
            {
                const float ExcessMass = (Theta[LayerIdx] - Phi[LayerIdx]) * DensityWater * Thickness[LayerIdx];
                Snow.LiquidMass_kgm2[LayerIdx] = FMath::Max(0.0f, Snow.LiquidMass_kgm2[LayerIdx] - ExcessMass);
                OutRunoff_kgm2 += ExcessMass;
                Theta[LayerIdx] = Phi[LayerIdx];
            }

            Theta0[LayerIdx] = Theta[LayerIdx];
        }

        for (int32 Iter = 0; Iter < Iterations; ++Iter)
        {
            for (int32 LayerIdx = 0; LayerIdx < LayerCount; ++LayerIdx)
            {
                a[LayerIdx] = 0.0f;
                b[LayerIdx] = 1.0f / FMath::Max(SubDt, KINDA_SMALL_NUMBER);
                rhs[LayerIdx] = 0.0f;
                dTheta[LayerIdx] = 0.0f;
            }

            if (LayerCount > 0)
            {
                const float LayerDenom = FMath::Max(Phi[0] - ThetaR[0], SaturationTolerance);
                if (Theta[0] > ThetaR[0])
                {
                    b[0] += 3.0f * Ksat[0] * FMath::Square(Theta[0] - ThetaR[0]) /
                        (FMath::Pow(LayerDenom, 3.0f) * FMath::Max(Thickness[0], MinimumLayerThickness_m));
                    Qw[1] = Ksat[0] * FMath::Pow(FMath::Clamp((Theta[0] - ThetaR[0]) / LayerDenom, 0.0f, 1.0f), 3.0f);
                }
                else
                {
                    Qw[1] = 0.0f;
                }

                rhs[0] = (Theta[0] - Theta0[0]) / FMath::Max(SubDt, KINDA_SMALL_NUMBER)
                    + (Qw[1] - Qw[0]) / FMath::Max(Thickness[0], MinimumLayerThickness_m);
            }

            for (int32 LayerIdx = 1; LayerIdx < LayerCount; ++LayerIdx)
            {
                const float PrevDenom = FMath::Max(Phi[LayerIdx - 1] - ThetaR[LayerIdx - 1], SaturationTolerance);
                if (Theta[LayerIdx - 1] > ThetaR[LayerIdx - 1])
                {
                    a[LayerIdx] = -3.0f * Ksat[LayerIdx - 1] * FMath::Square(Theta[LayerIdx - 1] - ThetaR[LayerIdx - 1]) /
                        (FMath::Pow(PrevDenom, 3.0f) * FMath::Max(Thickness[LayerIdx - 1], MinimumLayerThickness_m));
                }

                const float LayerDenom = FMath::Max(Phi[LayerIdx] - ThetaR[LayerIdx], SaturationTolerance);
                if (Theta[LayerIdx] > ThetaR[LayerIdx])
                {
                    b[LayerIdx] += 3.0f * Ksat[LayerIdx] * FMath::Square(Theta[LayerIdx] - ThetaR[LayerIdx]) /
                        (FMath::Pow(LayerDenom, 3.0f) * FMath::Max(Thickness[LayerIdx], MinimumLayerThickness_m));
                    Qw[LayerIdx + 1] = Ksat[LayerIdx] * FMath::Pow(FMath::Clamp((Theta[LayerIdx] - ThetaR[LayerIdx]) / LayerDenom, 0.0f, 1.0f), 3.0f);
                }
                else
                {
                    Qw[LayerIdx + 1] = 0.0f;
                }

                rhs[LayerIdx] = (Theta[LayerIdx] - Theta0[LayerIdx]) / FMath::Max(SubDt, KINDA_SMALL_NUMBER)
                    + (Qw[LayerIdx + 1] - Qw[LayerIdx]) / FMath::Max(Thickness[LayerIdx], MinimumLayerThickness_m);
            }

            constexpr float PivotEps = 1.0e-12f;
            if (LayerCount > 0)
            {
                float Denom0 = b[0];
                if (FMath::Abs(Denom0) < PivotEps)
                {
                    Denom0 = (Denom0 >= 0.0f) ? PivotEps : -PivotEps;
                }
                dTheta[0] = -rhs[0] / Denom0;
            }

            for (int32 LayerIdx = 1; LayerIdx < LayerCount; ++LayerIdx)
            {
                float Denom = b[LayerIdx];
                if (FMath::Abs(Denom) < PivotEps)
                {
                    Denom = (Denom >= 0.0f) ? PivotEps : -PivotEps;
                }
                dTheta[LayerIdx] = -(a[LayerIdx] * dTheta[LayerIdx - 1] + rhs[LayerIdx]) / Denom;
            }

            for (int32 LayerIdx = 0; LayerIdx < LayerCount; ++LayerIdx)
            {
                Theta[LayerIdx] = FMath::Max(Theta[LayerIdx] + dTheta[LayerIdx], 0.0f);

                if (Theta[LayerIdx] > Phi[LayerIdx])
                {
                    Qw[LayerIdx + 1] += (Theta[LayerIdx] - Phi[LayerIdx]) * Thickness[LayerIdx] / FMath::Max(SubDt, KINDA_SMALL_NUMBER);
                    Theta[LayerIdx] = Phi[LayerIdx];
                }
            }
        }

        for (int32 LayerIdx = 0; LayerIdx < LayerCount; ++LayerIdx)
        {
            const float NewLiquidMass = FMath::Max(0.0f, Theta[LayerIdx] * DensityWater * Thickness[LayerIdx]);
            Snow.LiquidMass_kgm2[LayerIdx] = NewLiquidMass;
            if (Thickness[LayerIdx] > KINDA_SMALL_NUMBER)
            {
                const float NewMass = Snow.IceMass_kgm2[LayerIdx] + NewLiquidMass;
                Snow.Density_kgm3[LayerIdx] = NewMass / Thickness[LayerIdx];
            }
            UpdateLayerDiagnostics(Snow, LayerIdx);
        }

        if (OptionalDiagnostics)
        {
            for (int32 LayerIdx = 0; LayerIdx < LayerCount && LayerIdx < GFSM2MaxLayers; ++LayerIdx)
            {
                OptionalDiagnostics->SnowWaterFlux_kgm2s[LayerIdx] += DensityWater * Qw[LayerIdx] * NewtonAveraging;
            }
        }

        const float RunoffRate_kgm2s = DensityWater * Qw[LayerCount] * NewtonAveraging;
        OutRunoff_kgm2 += RunoffRate_kgm2s * SubDt;
    }

    if (OptionalDiagnostics && SubSteps > 1)
    {
        const float InvSubSteps = 1.0f / static_cast<float>(SubSteps);
        for (int32 LayerIdx = 0; LayerIdx < LayerCount && LayerIdx < GFSM2MaxLayers; ++LayerIdx)
        {
            OptionalDiagnostics->SnowWaterFlux_kgm2s[LayerIdx] *= InvSubSteps;
        }
    }

    for (int32 LayerIdx = 0; LayerIdx < LayerCount; ++LayerIdx)
    {
        const float HeatCapacity = FMath::Max(GetLayerHeatCapacity(Snow, LayerIdx), MinHeatCapacity_Jm2K);
        if (HeatCapacity <= MinHeatCapacity_Jm2K)
        {
            continue;
        }

        const float ColdContent = HeatCapacity * FMath::Max(FreezePoint_K - Snow.Temperature_K[LayerIdx], 0.0f);
        if (ColdContent <= 0.0f || Snow.LiquidMass_kgm2[LayerIdx] <= KINDA_SMALL_NUMBER)
        {
            continue;
        }

        const float FreezeMass = FMath::Min(Snow.LiquidMass_kgm2[LayerIdx], ColdContent / FMath::Max(ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg, 1.0f));
        Snow.LiquidMass_kgm2[LayerIdx] -= FreezeMass;
        Snow.IceMass_kgm2[LayerIdx] += FreezeMass;
        Snow.Temperature_K[LayerIdx] = FMath::Clamp(
            Snow.Temperature_K[LayerIdx] + (FreezeMass * ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg) / HeatCapacity,
            ModelParameters.Snow.MinSnowTemperature_K,
            FreezePoint_K);
        UpdateLayerDiagnostics(Snow, LayerIdx);
    }
}

void UFSM2SnowSimulation::UpdateGrainGrowth(FFSM2SnowColumn& Snow, const FFSM2SoilColumn& Soil, float SurfaceTemperatureK, float DtSeconds)
{
    if (Snow.LayerCount == 0 || DtSeconds <= 0.0f)
    {
        return;
    }

    const float FreshGrain = FMath::Clamp<float>(ModelParameters.Snow.FreshSnowGrainRadius_m, 1.0e-6f, 1.0e-2f);
    const float LatentHeatSublimation_Jkg = ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg + ModelParameters.PhysicalConstants.LatentHeatVapor_Jkg;
    const float DensityWater = ModelParameters.PhysicalConstants.DensityWater_kgm3;
    const float e0 = ModelParameters.PhysicalConstants.SaturationVapourPressure0_Pa;
    const float Rv = ModelParameters.PhysicalConstants.GasConstantWaterVapour_JkgK;

    switch (ModelParameters.Snow.GrainGrowthScheme)
    {
    case EFSM2GrainGrowthScheme::None:
        for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
        {
            if (GetLayerMass(Snow, LayerIdx) < KINDA_SMALL_NUMBER)
            {
                continue;
            }
            Snow.GrainRadius_m[LayerIdx] = FreshGrain;
        }
        break;

    case EFSM2GrainGrowthScheme::TemperatureDependent:
        for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
        {
            if (GetLayerMass(Snow, LayerIdx) < KINDA_SMALL_NUMBER)
            {
                continue;
            }

            float Grain = FMath::Clamp<float>(Snow.GrainRadius_m[LayerIdx], 1.0e-6f, 1.0e-2f);
            float GrowthRate = 2.0e-13f;
            const float TemperatureK = Snow.Temperature_K[LayerIdx];
            if (TemperatureK < FreezePoint_K)
            {
                if (Grain < 1.5e-4f)
                {
                    GrowthRate = 2.0e-14f;
                }
                else
                {
                    GrowthRate = 7.3e-8f * FMath::Exp(-4600.0f / FMath::Max<float>(TemperatureK, 150.0f));
                }
            }

            Grain += DtSeconds * GrowthRate / FMath::Max<float>(Grain, 1.0e-6f);
            Snow.GrainRadius_m[LayerIdx] = FMath::Clamp<float>(Grain, 1.0e-6f, 1.0e-2f);
        }
        break;

    case EFSM2GrainGrowthScheme::TemperatureGradient:
        for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
        {
            if (GetLayerMass(Snow, LayerIdx) < KINDA_SMALL_NUMBER)
            {
                continue;
            }

            float Grain = FMath::Clamp<float>(Snow.GrainRadius_m[LayerIdx], 1.0e-6f, 1.0e-2f);
            const float Thickness = FMath::Max<float>(Snow.Thickness_m[LayerIdx], MinimumLayerThickness_m);

            float UpperTempK = SurfaceTemperatureK; // Start with surface temperature
            if (LayerIdx > 0)
            {
                const float Weight = FMath::Max<float>(Snow.Thickness_m[LayerIdx - 1] + Thickness, MinimumLayerThickness_m);
                UpperTempK = (Snow.Thickness_m[LayerIdx - 1] * Snow.Temperature_K[LayerIdx] + Thickness * Snow.Temperature_K[LayerIdx - 1]) / FMath::Max<float>(Weight, MinimumLayerThickness_m);
            }

            float LowerTempK = Soil.Temperature_K[0]; // Start with top soil temperature
            if (LayerIdx + 1 < Snow.LayerCount)
            {
                const float Weight = FMath::Max<float>(Snow.Thickness_m[LayerIdx + 1] + Thickness, MinimumLayerThickness_m);
                LowerTempK = (Snow.Thickness_m[LayerIdx + 1] * Snow.Temperature_K[LayerIdx] + Thickness * Snow.Temperature_K[LayerIdx + 1]) / FMath::Max<float>(Weight, MinimumLayerThickness_m);
            }

            const float TempGradient = FMath::Abs(UpperTempK - LowerTempK) / Thickness;
            const float LiquidVolumeFraction = Snow.LiquidMass_kgm2[LayerIdx] / (DensityWater * Thickness);

            float GrowthRate = 0.0f;
            if (LiquidVolumeFraction < 1.0e-4f)
            {
                const float TempK = FMath::Clamp<float>(Snow.Temperature_K[LayerIdx], 150.0f, 320.0f);
                const float DpdT = (e0 / (Rv * TempK * TempK)) * ((LatentHeatSublimation_Jkg / (Rv * TempK)) - 1.0f) * FMath::Exp((LatentHeatSublimation_Jkg / Rv) * (1.0f / FreezePoint_K - 1.0f / TempK));
                const float Qv = 9.2e-5f * FMath::Pow(TempK / FreezePoint_K, 6.0f) * DpdT * TempGradient;
                GrowthRate = 1.25e-7f * FMath::Min<float>(Qv, 1.0e-6f);
            }
            else
            {
                GrowthRate = 1.0e-12f * FMath::Min<float>(LiquidVolumeFraction + 0.05f, 0.14f);
            }

            Grain += DtSeconds * GrowthRate / FMath::Max<float>(Grain, 1.0e-6f);
            Snow.GrainRadius_m[LayerIdx] = FMath::Clamp<float>(Grain, 1.0e-6f, 1.0e-2f);
        }
        break;

    default:
        break;
    }
}

void UFSM2SnowSimulation::RebuildLayers(FFSM2SnowColumn& Snow)
{
    // CullAndMergeLayers(Snow); // Removed to match Fortran re-meshing logic

    const int32 OldCount = Snow.LayerCount;
    if (OldCount <= 0)
    {
        Snow.LayerCount = 0;
        for (int32 Idx = 0; Idx < GFSM2MaxLayers; ++Idx)
        {
            Snow.Thickness_m[Idx] = 0.0f;
            Snow.Temperature_K[Idx] = FreezePoint_K;
            Snow.IceMass_kgm2[Idx] = 0.0f;
            Snow.LiquidMass_kgm2[Idx] = 0.0f;
            Snow.Density_kgm3[Idx] = ModelParameters.Snow.MinSnowDensity_kgm3;
            Snow.GrainRadius_m[Idx] = FMath::Clamp(ModelParameters.Snow.FreshSnowGrainRadius_m, 1.0e-6f, 1.0e-2f);
            Snow.AgeHours[Idx] = 0.0f;
        }
        return;
    }

    TStaticArray<float, GFSM2MaxLayers> OldThickness;
    TStaticArray<float, GFSM2MaxLayers> OldIce;
    TStaticArray<float, GFSM2MaxLayers> OldLiquid;
    TStaticArray<float, GFSM2MaxLayers> OldEnergy;
    TStaticArray<float, GFSM2MaxLayers> OldGrain;
    TStaticArray<float, GFSM2MaxLayers> OldAge;

    for (int32 Idx = 0; Idx < GFSM2MaxLayers; ++Idx)
    {
        OldThickness[Idx] = 0.0f;
        OldIce[Idx] = 0.0f;
        OldLiquid[Idx] = 0.0f;
        OldEnergy[Idx] = 0.0f;
        OldGrain[Idx] = ModelParameters.Snow.FreshSnowGrainRadius_m;
        OldAge[Idx] = 0.0f;
    }

    float TotalMass = 0.0f;
    float TotalThickness = 0.0f;

    for (int32 Idx = 0; Idx < OldCount; ++Idx)
    {
        const float IceMass = FMath::Max(Snow.IceMass_kgm2[Idx], 0.0f);
        const float LiquidMass = FMath::Max(Snow.LiquidMass_kgm2[Idx], 0.0f);
        const float StoredDensity = FMath::Max(Snow.Density_kgm3[Idx], ModelParameters.Snow.MinSnowDensity_kgm3);
        const float LayerMass = IceMass + LiquidMass;

        OldIce[Idx] = IceMass;
        OldLiquid[Idx] = LiquidMass;
        OldGrain[Idx] = FMath::Clamp(Snow.GrainRadius_m[Idx], 1.0e-6f, 1.0e-2f);
        OldAge[Idx] = FMath::Max(Snow.AgeHours[Idx], 0.0f);

        const float HeatCapacity = IceMass * ModelParameters.PhysicalConstants.SpecificHeatIce_JkgK + LiquidMass * ModelParameters.PhysicalConstants.SpecificHeatWater_JkgK;
        OldEnergy[Idx] = HeatCapacity * (Snow.Temperature_K[Idx] - FreezePoint_K);

        float LayerThickness = 0.0f;
        if (LayerMass > KINDA_SMALL_NUMBER && StoredDensity > 1.0f)
        {
            LayerThickness = LayerMass / StoredDensity;
        }

        OldThickness[Idx] = FMath::Max(LayerThickness, 0.0f);
        TotalMass += LayerMass;
        TotalThickness += OldThickness[Idx];
    }

    if (TotalMass <= KINDA_SMALL_NUMBER)
    {
        Snow.LayerCount = 0;
        for (int32 Idx = 0; Idx < GFSM2MaxLayers; ++Idx)
        {
            Snow.Thickness_m[Idx] = 0.0f;
            Snow.Temperature_K[Idx] = FreezePoint_K;
            Snow.IceMass_kgm2[Idx] = 0.0f;
            Snow.LiquidMass_kgm2[Idx] = 0.0f;
            Snow.Density_kgm3[Idx] = ModelParameters.Snow.MinSnowDensity_kgm3;
        Snow.GrainRadius_m[Idx] = FMath::Clamp(ModelParameters.Snow.FreshSnowGrainRadius_m, 1.0e-6f, 1.0e-2f);
            Snow.AgeHours[Idx] = 0.0f;
        }
        return;
    }

    if (TotalThickness <= KINDA_SMALL_NUMBER)
    {
        const float PhysicalMaxDensity = ModelParameters.PhysicalConstants.DensityIce_kgm3;
        TotalThickness = TotalMass / FMath::Max(PhysicalMaxDensity, 1.0f);
    }

    const float MinLayerThickness = GetMinimumSnowLayerThickness(0);
    const int32 MaxLayersAllowed = FMath::Clamp(ModelParameters.Layers.MaxActiveLayers, 1, GFSM2MaxLayers);

    auto GetConfiguredLayerThickness = [&](int32 LayerIdx) -> float
    {
        return GetMinimumSnowLayerThickness(LayerIdx);
    };

    TStaticArray<float, GFSM2MaxLayers> TargetThickness;
    for (int32 Idx = 0; Idx < GFSM2MaxLayers; ++Idx)
    {
        TargetThickness[Idx] = 0.0f;
    }

    int32 TargetCount = 1;
    TargetThickness[0] = TotalThickness;

    if (TargetThickness[0] > GetConfiguredLayerThickness(0))
    {
        float Remaining = TotalThickness;
        for (int32 LayerIdx = 0; LayerIdx < MaxLayersAllowed; ++LayerIdx)
        {
            const float BaseThickness = GetMinimumSnowLayerThickness(LayerIdx);
            TargetThickness[LayerIdx] = FMath::Min(Remaining, BaseThickness);
            Remaining -= TargetThickness[LayerIdx];
            TargetCount = LayerIdx + 1;

            if (Remaining <= GetConfiguredLayerThickness(LayerIdx) || LayerIdx == MaxLayersAllowed - 1)
            {
                TargetThickness[LayerIdx] += Remaining;
                Remaining = 0.0f;
                break;
            }
        }
    }
    else
    {
        TargetThickness[0] = TotalThickness;
        TargetCount = 1;
    }

    const int32 DesiredLayerCount = TargetCount;
    TargetCount = FMath::Clamp(TargetCount, 1, MaxLayersAllowed);

    if (DesiredLayerCount > TargetCount)
    {
        float SpilloverThickness = 0.0f;
        for (int32 Idx = TargetCount; Idx < DesiredLayerCount; ++Idx)
        {
            SpilloverThickness += TargetThickness[Idx];
            TargetThickness[Idx] = 0.0f;
        }

        if (TargetCount > 0)
        {
            TargetThickness[TargetCount - 1] += SpilloverThickness;
        }
    }

    // Reset current snow column prior to redistribution
    for (int32 Idx = 0; Idx < GFSM2MaxLayers; ++Idx)
    {
        Snow.Thickness_m[Idx] = 0.0f;
        Snow.Temperature_K[Idx] = FreezePoint_K;
        Snow.IceMass_kgm2[Idx] = 0.0f;
        Snow.LiquidMass_kgm2[Idx] = 0.0f;
        Snow.Density_kgm3[Idx] = ModelParameters.Snow.MinSnowDensity_kgm3;
        Snow.GrainRadius_m[Idx] = FMath::Clamp(ModelParameters.Snow.FreshSnowGrainRadius_m, 1.0e-6f, 1.0e-2f);
        Snow.AgeHours[Idx] = 0.0f;
    }

    TStaticArray<float, GFSM2MaxLayers> LayerEnergy;
    TStaticArray<float, GFSM2MaxLayers> GrainNumerator;
    TStaticArray<float, GFSM2MaxLayers> AgeNumerator;

    for (int32 Idx = 0; Idx < GFSM2MaxLayers; ++Idx)
    {
        LayerEnergy[Idx] = 0.0f;
        GrainNumerator[Idx] = 0.0f;
        AgeNumerator[Idx] = 0.0f;
    }

    int32 NewLayerIdx = 0;
    float RemainingTarget = (TargetCount > 0) ? TargetThickness[0] : 0.0f;

    for (int32 OldIdx = 0; OldIdx < OldCount; ++OldIdx)
    {
        float ThicknessRemaining = OldThickness[OldIdx];
        float IceRemaining = OldIce[OldIdx];
        float LiquidRemaining = OldLiquid[OldIdx];
        float EnergyRemaining = OldEnergy[OldIdx];
        const float GrainValue = OldGrain[OldIdx];
        const float AgeValue = OldAge[OldIdx];

        while (ThicknessRemaining > KINDA_SMALL_NUMBER && TargetCount > 0)
        {
            if (NewLayerIdx >= TargetCount)
            {
                NewLayerIdx = TargetCount - 1;
            }

            if (NewLayerIdx == TargetCount - 1)
            {
                RemainingTarget = FMath::Max(RemainingTarget, ThicknessRemaining);
            }

            if (RemainingTarget <= KINDA_SMALL_NUMBER)
            {
                if (NewLayerIdx < TargetCount - 1)
                {
                    ++NewLayerIdx;
                    RemainingTarget = TargetThickness[NewLayerIdx];
                    continue;
                }
                RemainingTarget = ThicknessRemaining;
            }

            const float TransferThickness = FMath::Min(ThicknessRemaining, RemainingTarget);
            if (TransferThickness <= KINDA_SMALL_NUMBER)
            {
                break;
            }

            const float Fraction = (ThicknessRemaining > KINDA_SMALL_NUMBER) ? (TransferThickness / ThicknessRemaining) : 1.0f;

            const float IceTransfer = IceRemaining * Fraction;
            const float LiquidTransfer = LiquidRemaining * Fraction;
            const float EnergyTransfer = EnergyRemaining * Fraction;
            const float MassTransfer = IceTransfer + LiquidTransfer;

            Snow.IceMass_kgm2[NewLayerIdx] += IceTransfer;
            Snow.LiquidMass_kgm2[NewLayerIdx] += LiquidTransfer;
            LayerEnergy[NewLayerIdx] += EnergyTransfer;
            GrainNumerator[NewLayerIdx] += IceTransfer * GrainValue;
            AgeNumerator[NewLayerIdx] += MassTransfer * AgeValue;

            ThicknessRemaining -= TransferThickness;
            IceRemaining -= IceTransfer;
            LiquidRemaining -= LiquidTransfer;
            EnergyRemaining -= EnergyTransfer;

            RemainingTarget = FMath::Max(RemainingTarget - TransferThickness, 0.0f);

            if (RemainingTarget <= KINDA_SMALL_NUMBER && NewLayerIdx < TargetCount - 1)
            {
                ++NewLayerIdx;
                if (NewLayerIdx < TargetCount)
                {
                    RemainingTarget = TargetThickness[NewLayerIdx];
                }
            }
        }
    }

    Snow.LayerCount = TargetCount;

    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        const float Mass = Snow.IceMass_kgm2[LayerIdx] + Snow.LiquidMass_kgm2[LayerIdx];
        const float HeatCapacity = Snow.IceMass_kgm2[LayerIdx] * ModelParameters.PhysicalConstants.SpecificHeatIce_JkgK + Snow.LiquidMass_kgm2[LayerIdx] * ModelParameters.PhysicalConstants.SpecificHeatWater_JkgK;

        Snow.Thickness_m[LayerIdx] = TargetThickness[LayerIdx];

        if (HeatCapacity > MinHeatCapacity_Jm2K)
        {
            const float DiagnosedTempK = FreezePoint_K + LayerEnergy[LayerIdx] / HeatCapacity;
            Snow.Temperature_K[LayerIdx] = FMath::Clamp(DiagnosedTempK, ModelParameters.Snow.MinSnowTemperature_K, FreezePoint_K);
        }
        else
        {
            Snow.Temperature_K[LayerIdx] = FreezePoint_K;
        }

        if (Snow.IceMass_kgm2[LayerIdx] > KINDA_SMALL_NUMBER)
        {
            Snow.GrainRadius_m[LayerIdx] = FMath::Clamp(GrainNumerator[LayerIdx] / Snow.IceMass_kgm2[LayerIdx], 1.0e-6f, 1.0e-2f);
        }
        else
        {
            Snow.GrainRadius_m[LayerIdx] = FMath::Clamp(ModelParameters.Snow.FreshSnowGrainRadius_m, 1.0e-6f, 1.0e-2f);
        }

        const float MassForAge = FMath::Max(Mass, KINDA_SMALL_NUMBER);
        Snow.AgeHours[LayerIdx] = AgeNumerator[LayerIdx] / MassForAge;

        float Density = 0.0f;
        // Mirror Fortran redistribution: thickness stays as assigned above, density is diagnosed from mass/thickness
        if (ModelParameters.Snow.DensityScheme == EFSM2DensityScheme::FixedDensity)
        {
            Density = FMath::Clamp(ModelParameters.Snow.FixedSnowDensity_kgm3, ModelParameters.Snow.MinSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3);
        }
        else if (Snow.Thickness_m[LayerIdx] > KINDA_SMALL_NUMBER)
        {
            Density = Mass / Snow.Thickness_m[LayerIdx];
        }
        Density = FMath::Clamp(Density, ModelParameters.Snow.MinSnowDensity_kgm3, ModelParameters.Snow.MaxSnowDensity_kgm3);
        Snow.Density_kgm3[LayerIdx] = Density;
    }

    for (int32 LayerIdx = Snow.LayerCount; LayerIdx < GFSM2MaxLayers; ++LayerIdx)
    {
        Snow.Thickness_m[LayerIdx] = 0.0f;
        Snow.Temperature_K[LayerIdx] = FreezePoint_K;
        Snow.IceMass_kgm2[LayerIdx] = 0.0f;
        Snow.LiquidMass_kgm2[LayerIdx] = 0.0f;
        Snow.Density_kgm3[LayerIdx] = ModelParameters.Snow.MinSnowDensity_kgm3;
        Snow.GrainRadius_m[LayerIdx] = FMath::Clamp(ModelParameters.Snow.FreshSnowGrainRadius_m, 1.0e-6f, 1.0e-2f);
        Snow.AgeHours[LayerIdx] = 0.0f;
    }
}

void UFSM2SnowSimulation::InitializeColumnDefaults(FFSM2ColumnState& Column) const
{
    Column.Reset();
    InitializeSoilProfile(Column.Soil);
    if (Column.Soil.LayerCount > 0)
    {
        Column.SurfaceTemperature_K = Column.Soil.Temperature_K[0];
    }
}

void UFSM2SnowSimulation::CalculateSurfaceLayerThermalProperties(const FFSM2SnowColumn& Snow, const FFSM2SoilColumn& Soil, float& OutSurfaceLayerThickness, float& OutSurfaceLayerTemperature, float& OutSurfaceLayerConductivity) const
{
    const int32 SnowCount = FMath::Clamp(Snow.LayerCount, 0, GFSM2MaxLayers);
    const int32 SoilCount = FMath::Clamp(Soil.LayerCount, 0, GFSM2MaxSoilLayers);

    // Default values
    OutSurfaceLayerTemperature = ModelParameters.Soil.GroundTemperature_K;
    OutSurfaceLayerConductivity = ModelParameters.Soil.GroundConductivity_WmK;
    OutSurfaceLayerThickness = FMath::Max(ModelParameters.Layers.SoilLayerThicknesses_m.Num() > 0 ? ModelParameters.Layers.SoilLayerThicknesses_m[0] : MinimumLayerThickness_m, MinimumLayerThickness_m);

    if (SoilCount == 0)
    {
        return;
    }

    // Get top soil layer properties
    const float SoilThickness = FMath::Max(Soil.Thickness_m[0], MinimumLayerThickness_m);
    const float SoilTemperature = Soil.Temperature_K[0];

    // Calculate soil thermal conductivity for surface layer
    TArray<double> SoilThicknessArray, SoilConductivityArray, SoilHeatCapacityArray;
    EvaluateSoilThermalProfile(Soil, SoilThicknessArray, SoilConductivityArray, SoilHeatCapacityArray);
    const float SoilConductivity = (SoilConductivityArray.Num() > 0) ? SoilConductivityArray[0] : ModelParameters.Soil.ThermalConductivity_WmK;

    OutSurfaceLayerThickness = SoilThickness;

    if (SnowCount == 0)
    {
        // No snow - surface layer is just the top soil layer
        OutSurfaceLayerTemperature = SoilTemperature;
        OutSurfaceLayerConductivity = SoilConductivity;
        return;
    }

    // Snow is present - calculate combined surface layer properties
    const float SnowThickness = FMath::Max(Snow.Thickness_m[0], MinimumLayerThickness_m);
    const float SnowTemperature = Snow.Temperature_K[0];
    const float SnowConductivity = GetLayerThermalConductivity(Snow, 0);
    const float TotalSnowDepth = ComputeSnowDepthMeters(Snow);

    // Surface layer thickness is maximum of soil and snow layer thicknesses
    OutSurfaceLayerThickness = FMath::Max(SoilThickness, SnowThickness);

    // Surface layer temperature calculation (following FSM2 Fortran)
    if (TotalSnowDepth <= SoilThickness)
    {
        // Shallow snow - weighted average temperature
        OutSurfaceLayerTemperature = SoilTemperature + (SnowTemperature - SoilTemperature) * SnowThickness / SoilThickness;
    }
    else
    {
        // Deep snow - surface temperature is snow temperature
        OutSurfaceLayerTemperature = SnowTemperature;
    }

    // Surface layer thermal conductivity calculation (harmonic mean for thermal resistance)
    if (TotalSnowDepth <= 0.5f * SoilThickness)
    {
        // Shallow snow - harmonic mean of thermal conductivities weighted by thicknesses
        // λ₁ = Δz_sl,1 / (2*D_sn,1/λ_sn,1 + (Δz_sl,1 - 2*D_sn,1)/λ_sl,1)
        const float Term1 = 2.0f * SnowThickness / SnowConductivity;
        const float Term2 = (SoilThickness - 2.0f * SnowThickness) / SoilConductivity;
        const float Denominator = Term1 + Term2;

        if (Denominator > KINDA_SMALL_NUMBER)
        {
            OutSurfaceLayerConductivity = SoilThickness / Denominator;
        }
        else
        {
            OutSurfaceLayerConductivity = SnowConductivity;
        }
    }
    else
    {
        // Deep snow - surface conductivity is snow conductivity
        OutSurfaceLayerConductivity = SnowConductivity;
    }

    // Clamp to reasonable values
    OutSurfaceLayerConductivity = FMath::Clamp(OutSurfaceLayerConductivity, 0.01f, 10.0f);
}

void UFSM2SnowSimulation::CalculateSoilProperties(float& OutClappHornbergerExponent, float& OutVolumetricHeatCapacityDry, float& OutThermalConductivityDry, float& OutSaturatedMatricPotential, float& OutCriticalVolumetricWater, float& OutSaturatedVolumetricWater) const
{
    // Calculate soil properties from clay and sand fractions (following FSM2 Fortran implementation)
    const float Fcly = FMath::Clamp(ModelParameters.Soil.ClayFraction, 0.0f, 1.0f);
    const float Fsnd = FMath::Clamp(ModelParameters.Soil.SandFraction, 0.0f, 1.0f);

    // Clapp-Hornberger exponent
    OutClappHornbergerExponent = 3.1f + 15.7f * Fcly - 0.3f * Fsnd;

    // Volumetric heat capacity of dry soil (J/m³/K)
    const float ClayHcap = 2.128e6f; // J/m³/K
    const float SandHcap = 2.385e6f; // J/m³/K
    OutVolumetricHeatCapacityDry = (ClayHcap * Fcly + SandHcap * Fsnd) / FMath::Max(Fcly + Fsnd, 1e-6f);

    // Saturated volumetric water content
    OutSaturatedVolumetricWater = 0.505f - 0.037f * Fcly - 0.142f * Fsnd;

    // Saturated matric potential (m)
    OutSaturatedMatricPotential = FMath::Pow(10.0f, 0.17f - 0.63f * Fcly - 1.58f * Fsnd);

    // Critical volumetric water content
    const float ReferencePotential = 3.364f; // m
    OutCriticalVolumetricWater = OutSaturatedVolumetricWater * FMath::Pow(OutSaturatedMatricPotential / ReferencePotential, 1.0f / OutClappHornbergerExponent);

    // Dry soil thermal conductivity (W/m/K)
    const float HconAir = ModelParameters.PhysicalConstants.ThermalConductivityAir_WmK;
    const float HconClay = ModelParameters.PhysicalConstants.ThermalConductivityClay_WmK;
    const float HconSand = ModelParameters.PhysicalConstants.ThermalConductivitySand_WmK;
    const float Vsat = OutSaturatedVolumetricWater;

    OutThermalConductivityDry = FMath::Pow(HconAir, Vsat) *
        FMath::Pow(FMath::Pow(HconClay, Fcly) * FMath::Pow(HconSand, 1.0f - Fcly), 1.0f - Vsat);
}

void UFSM2SnowSimulation::EvaluateSoilThermalProfile(const FFSM2SoilColumn& Soil, TArray<double>& OutThickness, TArray<double>& OutConductivity, TArray<double>& OutHeatCapacity) const
{
    const int32 SoilCount = FMath::Clamp(Soil.LayerCount, 0, GFSM2MaxSoilLayers);
    OutThickness.Reset();
    OutConductivity.Reset();
    OutHeatCapacity.Reset();

    if (SoilCount == 0)
    {
        return;
    }

    // Calculate soil properties from composition
    float B, HcapSoil, HconSoil, Sathh, Vcrit, Vsat;
    CalculateSoilProperties(B, HcapSoil, HconSoil, Sathh, Vcrit, Vsat);

    OutThickness.Reserve(SoilCount);
    OutConductivity.Reserve(SoilCount);
    OutHeatCapacity.Reserve(SoilCount);

    // Calculate dPsidT (temperature derivative of ice potential)
    const float RhoIce = ModelParameters.PhysicalConstants.DensityIce_kgm3;
    const float Lf = ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg;
    const float G = ModelParameters.PhysicalConstants.Gravity_mps2;
    const float Tm = ModelParameters.PhysicalConstants.MeltingPoint_K;
    const float DPsidT = -RhoIce * Lf / (ModelParameters.PhysicalConstants.DensityWater_kgm3 * G * Tm);

    for (int32 SoilIdx = 0; SoilIdx < SoilCount; ++SoilIdx)
    {
        const float Thickness = FMath::Max<double>(Soil.Thickness_m[SoilIdx], MinimumLayerThickness_m);
        const float Temperature = Soil.Temperature_K[SoilIdx];
        const float Moisture = FMath::Clamp(Soil.Moisture_VolumeFraction[SoilIdx], 0.0f, 1.0f);

        OutThickness.Add(Thickness);

        // Start with dry soil heat capacity
        double HeatCapacity = HcapSoil * Thickness;

        // Start with dry soil thermal conductivity
        double Conductivity = HconSoil;

        if (Moisture > KINDA_SMALL_NUMBER)
        {
            // Calculate unfrozen and frozen moisture fractions based on temperature
            float DthudT = 0.0f; // Derivative of unfrozen water content w.r.t. temperature
            float Sthu = Moisture; // Unfrozen moisture content
            float Sthf = 0.0f; // Frozen moisture content
            const float Tc = Temperature - Tm; // Temperature in °C

            const float Tmax = Tm + (Sathh / DPsidT) * FMath::Pow(Vsat / Moisture, B);
            if (Temperature < Tmax)
            {
                DthudT = (-DPsidT * Vsat / (B * Sathh)) * FMath::Pow(DPsidT * Tc / Sathh, -1.0f / B - 1.0f);
                Sthu = Vsat * FMath::Pow(DPsidT * Tc / Sathh, -1.0f / B);
                Sthu = FMath::Min(Sthu, Moisture);
                Sthf = (Moisture - Sthu) * ModelParameters.PhysicalConstants.DensityWater_kgm3 / RhoIce;
            }

            // Calculate masses
            const float Mf = RhoIce * Thickness * Sthf; // Frozen moisture mass (kg/m²)
            const float Mu = ModelParameters.PhysicalConstants.DensityWater_kgm3 * Thickness * Sthu; // Unfrozen moisture mass (kg/m²)

            // Update heat capacity including phase change effects
            HeatCapacity = HcapSoil * Thickness +
                          ModelParameters.PhysicalConstants.SpecificHeatIce_JkgK * Mf +
                          ModelParameters.PhysicalConstants.SpecificHeatWater_JkgK * Mu +
                          ModelParameters.PhysicalConstants.DensityWater_kgm3 * Thickness *
                          ((ModelParameters.PhysicalConstants.SpecificHeatWater_JkgK - ModelParameters.PhysicalConstants.SpecificHeatIce_JkgK) * Tc + Lf) * DthudT;

            // Calculate volumetric fractions for thermal conductivity
            const float Smf = RhoIce * Sthf / (ModelParameters.PhysicalConstants.DensityWater_kgm3 * Vsat); // Fractional frozen moisture
            const float Smu = Sthu / Vsat; // Fractional unfrozen moisture

            float Thice = 0.0f;
            if (Smf > 0.0f)
            {
                Thice = Vsat * Smf / (Smu + Smf);
            }

            float Thwat = 0.0f;
            if (Smu > 0.0f)
            {
                Thwat = Vsat * Smu / (Smu + Smf);
            }

            // Calculate saturated thermal conductivity
            const float HconWat = ModelParameters.PhysicalConstants.ThermalConductivityWater_WmK;
            const float HconIce = ModelParameters.PhysicalConstants.ThermalConductivityIce_WmK;
            const float HconAir = ModelParameters.PhysicalConstants.ThermalConductivityAir_WmK;

            const float HconSat = HconSoil * FMath::Pow(HconWat, Thwat) * FMath::Pow(HconIce, Thice) / FMath::Pow(HconAir, Vsat);

            // Final thermal conductivity
            Conductivity = (HconSat - HconSoil) * (Smf + Smu) + HconSoil;
        }

        OutHeatCapacity.Add(FMath::Max(HeatCapacity, MinHeatCapacity_Jm2K));
        OutConductivity.Add(FMath::Max(Conductivity, 1e-3));
    }
}

float UFSM2SnowSimulation::ComputeSurfaceMoistureConductance(const FFSM2SoilColumn& Soil) const
{
    const int32 SoilCount = FMath::Clamp(Soil.LayerCount, 0, GFSM2MaxSoilLayers);
    if (SoilCount <= 0)
    {
        return ModelParameters.Soil.SurfaceMoistureConductance_ms;
    }

    float B, HcapSoil, HconSoil, Sathh, Vcrit, Vsat;
    CalculateSoilProperties(B, HcapSoil, HconSoil, Sathh, Vcrit, Vsat);

    const float Moisture = FMath::Clamp(Soil.Moisture_VolumeFraction[0], 0.0f, 1.0f);
    if (Moisture <= KINDA_SMALL_NUMBER || Vsat <= KINDA_SMALL_NUMBER)
    {
        return FMath::Max(ModelParameters.Soil.SaturatedSurfaceConductance_ms, 1.0e-5f);
    }

    const float SaturatedConductance = FMath::Max(ModelParameters.Soil.SaturatedSurfaceConductance_ms, 0.0f);
    const float Tm = ModelParameters.PhysicalConstants.MeltingPoint_K;
    const float Temperature = Soil.Temperature_K[0];

    const float RhoIce = ModelParameters.PhysicalConstants.DensityIce_kgm3;
    const float RhoWater = ModelParameters.PhysicalConstants.DensityWater_kgm3;
    const float Lf = ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg;
    const float G = ModelParameters.PhysicalConstants.Gravity_mps2;

    const float DPsidT = -RhoIce * Lf / (RhoWater * G * Tm);

    float Sthu = Moisture;
    if (Sathh > KINDA_SMALL_NUMBER)
    {
        const float Tc = Temperature - Tm;
        const float MoistureClamp = FMath::Max(Moisture, 1.0e-6f);
        const float Tmax = Tm + (Sathh / DPsidT) * FMath::Pow(Vsat / MoistureClamp, B);

        if (Temperature < Tmax)
        {
            const float Argument = DPsidT * Tc / Sathh;
            if (Argument > 0.0f)
            {
                Sthu = FMath::Min(Vsat * FMath::Pow(Argument, -1.0f / B), Moisture);
            }
        }
    }

    float Conductance = FMath::Max(ModelParameters.Soil.SurfaceMoistureConductance_ms, SaturatedConductance);
    if (Vcrit > KINDA_SMALL_NUMBER && SaturatedConductance > 0.0f)
    {
        const float Ratio = FMath::Max(Sthu / Vcrit, 0.0f);
        // Match Fortran THERMAL.F90: gs1 = gsat*max((Smu*Vsat/Vcrit)^2, 1.)
        Conductance = SaturatedConductance * FMath::Max(Ratio * Ratio, 1.0f);
    }

    return Conductance;
}

void UFSM2SnowSimulation::InitializeSoilProfile(FFSM2SoilColumn& Soil) const
{
    Soil.Reset();
    Soil.LayerCount = FMath::Clamp(ModelParameters.Layers.SoilLayerCount, 0, GFSM2MaxSoilLayers);

    for (int32 SoilIdx = 0; SoilIdx < Soil.LayerCount; ++SoilIdx)
    {
        const float Thickness = ModelParameters.Layers.SoilLayerThicknesses_m.IsValidIndex(SoilIdx) ? FMath::Max(ModelParameters.Layers.SoilLayerThicknesses_m[SoilIdx], MinimumLayerThickness_m) : FMath::Pow(2.0f, static_cast<float>(SoilIdx)) * 0.1f;
        Soil.Thickness_m[SoilIdx] = Thickness;
        Soil.Temperature_K[SoilIdx] = FMath::Clamp(ModelParameters.Soil.InitialTemperature_K, 200.0f, 330.0f);
        Soil.Moisture_VolumeFraction[SoilIdx] = FMath::Clamp(ModelParameters.Soil.InitialMoisture_Vol, 0.0f, 1.0f);
    }
}

void UFSM2SnowSimulation::AgeSnowpack(FFSM2SnowColumn& Snow, float DtHours)
{
    if (DtHours <= 0.0f)
    {
        return;
    }

    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        Snow.AgeHours[LayerIdx] += DtHours;
    }
}

void UFSM2SnowSimulation::ApplyCanopyExchange(float& ThroughfallSnow, float& ThroughfallRain, float& IncomingShortwave, float& IncomingLongwave) const
{
    if (!ModelParameters.Modules.bEnableCanopy)
    {
        return;
    }

    const float InterceptedSnow = ThroughfallSnow * FMath::Clamp(ModelParameters.Vegetation.SnowInterceptionFraction, 0.0f, 1.0f);
    ThroughfallSnow -= InterceptedSnow;
    ThroughfallRain += InterceptedSnow;
    IncomingShortwave *= FMath::Clamp(ModelParameters.Vegetation.ShortwaveTransmittance, 0.0f, 1.0f);
    IncomingLongwave += ModelParameters.Vegetation.LongwaveAddition_Wm2;
}

void UFSM2SnowSimulation::EnergyBalanceStage(int32 /*CellIndex*/, FFSM2ColumnState& Column, float DtSeconds, const FWeatherForcingData& Forcing, float SnowInput_kgm2, float RainInput_kgm2, float AirDensity, float AirTempK, float Qa, float PressurePa, float WindSpeed, float FreshDensity, float FreshGrainRadius, float BulkTransferCoefficient, float DiffuseSW, float DirectSW, float TerrainSW, float SurfaceAlbedo, float& OutNetSurfaceFlux, float& OutSurfaceTemperatureK, float& OutThroughfallSnow, float& OutThroughfallRain, FFSM2EnergyFluxes& OutFluxes, FFSM2SurfaceUpdate& OutSurfaceUpdate, FFSM2CellDiagnostics* OutDiagnostics)
{
    (void)FreshDensity;
    (void)FreshGrainRadius;

    OutNetSurfaceFlux = 0.0f;
    OutSurfaceTemperatureK = Column.Snow.LayerCount > 0
        ? FMath::Clamp(Column.Snow.Temperature_K[0], ModelParameters.Snow.MinSnowTemperature_K, FreezePoint_K)
        : FMath::Max(ModelParameters.Soil.GroundTemperature_K, ModelParameters.Snow.MinSnowTemperature_K);
    OutThroughfallSnow = 0.0f;
    OutThroughfallRain = 0.0f;
    OutFluxes = FFSM2EnergyFluxes();
    OutSurfaceUpdate = FFSM2SurfaceUpdate();

    float ThroughfallSnow = SnowInput_kgm2;
    float ThroughfallRain = RainInput_kgm2;
    float IncomingSW = DiffuseSW + DirectSW + TerrainSW;
    float IncomingLW = Forcing.LWdown_Wm2;
    if (IncomingLW <= 0.0f)
    {
        static bool bWarnedLW = false;
        if (!bWarnedLW)
        {
            UE_LOG(LogTemp, Warning, TEXT("[FSM2] Incoming Longwave Radiation is <= 0.0 (%.2f). This will cause extreme cooling! Check your weather data input."), IncomingLW);
            bWarnedLW = true;
        }
    }

    ApplyCanopyExchange(ThroughfallSnow, ThroughfallRain, IncomingSW, IncomingLW);

    const float SurfaceSnow = FMath::Max(0.0f, ThroughfallSnow);
    const float SurfaceRain = FMath::Max(0.0f, ThroughfallRain);
    OutThroughfallSnow = SurfaceSnow;
    OutThroughfallRain = SurfaceRain;

    const float NetShortwave = IncomingSW * (1.0f - SurfaceAlbedo);

    float SurfaceConductance_ms = ModelParameters.Soil.SurfaceMoistureConductance_ms;
    float SurfaceWaterAvailability = 1.0f;

    // Fortran SRFEBAL uses air temperature directly (no potential temperature) for sensible heat
    OutNetSurfaceFlux = ComputeSurfaceEnergyBalance(Column.Snow, Column.Soil, NetShortwave, IncomingSW, SurfaceAlbedo, IncomingLW, AirDensity, AirTempK, Qa, PressurePa, WindSpeed, SurfaceRain, DtSeconds, BulkTransferCoefficient, OutSurfaceTemperatureK, OutFluxes, OutSurfaceUpdate, SurfaceConductance_ms, SurfaceWaterAvailability);

    if (OutDiagnostics)
    {
        OutDiagnostics->GroundHeatFlux_Wm2 = OutNetSurfaceFlux;
        OutDiagnostics->ForcingAirTemperatureK = AirTempK;
        OutDiagnostics->ForcingIncomingLongwave_Wm2 = IncomingLW;
        OutDiagnostics->TerrainShortwave_Wm2 = TerrainSW;
        OutDiagnostics->TotalShortwave_Wm2 = IncomingSW;
        OutDiagnostics->ForcingWindSpeed_mps = WindSpeed;
        OutDiagnostics->SurfaceMoistureConductance_ms = SurfaceConductance_ms;
        OutDiagnostics->SurfaceMoistureAvailability = SurfaceWaterAvailability;
        OutDiagnostics->EnergyFluxes = OutFluxes;
        OutDiagnostics->SurfaceAlbedo = SurfaceAlbedo;
    }

    // Diagnose frost formation here; mass updates are applied later in SNOW step order.
    float FrostFormation_kgm2 = 0.0f;
    if (OutSurfaceUpdate.SurfaceMassFlux_kgm2s < 0.0f && OutSurfaceTemperatureK < FreezePoint_K)
    {
        FrostFormation_kgm2 = -OutSurfaceUpdate.SurfaceMassFlux_kgm2s * DtSeconds;
    }

    if (OutDiagnostics)
    {
        OutDiagnostics->ThroughfallSnow_kgm2 = OutThroughfallSnow;
        OutDiagnostics->ThroughfallRain_kgm2 = OutThroughfallRain;
        OutDiagnostics->FrostMass_kgm2 = FrostFormation_kgm2;
        OutDiagnostics->EnergyFluxes = OutFluxes;
        OutDiagnostics->MeltMass_kgm2 = OutSurfaceUpdate.MeltMass_kgm2;
        OutDiagnostics->RefreezeMass_kgm2 = OutSurfaceUpdate.RefreezeMass_kgm2;
    }
}

void UFSM2SnowSimulation::DepositionStage(FFSM2SnowColumn& Snow, float ThroughfallSnow, float AirTempK, float FreshDensity, float FreshGrainRadius)
{
    if (ThroughfallSnow <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    InsertSurfaceLayer(Snow, ThroughfallSnow, 0.0f, FreshDensity, FMath::Clamp(AirTempK, ModelParameters.Snow.MinSnowTemperature_K, FreezePoint_K), FreshGrainRadius);
}

float UFSM2SnowSimulation::ComputeSurfaceEnergyBalance(const FFSM2SnowColumn& Snow, const FFSM2SoilColumn& Soil, float NetShortwave, float IncomingShortwave, float SurfaceAlbedo, float IncomingLongwave, float AirDensity, float AirTempK, float Qa, float PressurePa, float WindSpeed, float ThroughfallRain, float DtSeconds, float BulkTransferCoefficient, float& OutSurfaceTemperatureK, FFSM2EnergyFluxes& OutFluxes, FFSM2SurfaceUpdate& OutSurfaceUpdate, float& OutSurfaceMoistureConductance_ms, float& OutSurfaceMoistureAvailability) const
{
    OutFluxes = FFSM2EnergyFluxes();
    OutSurfaceUpdate = FFSM2SurfaceUpdate();
    OutSurfaceMoistureConductance_ms = ModelParameters.Soil.SurfaceMoistureConductance_ms;
    OutSurfaceMoistureAvailability = 1.0f;

    if (DtSeconds <= 0.0f)
    {
        const float GroundTemperature = (Soil.LayerCount > 0) ? Soil.Temperature_K[0] : ModelParameters.Soil.GroundTemperature_K;
        OutSurfaceTemperatureK = (Snow.LayerCount > 0)
            ? FMath::Max(Snow.Temperature_K[0], ModelParameters.Snow.MinSnowTemperature_K)
            : GroundTemperature;
        if (Soil.LayerCount > 0)
        {
            OutSurfaceMoistureConductance_ms = ComputeSurfaceMoistureConductance(Soil);
        }
        return 0.0f;
    }

    const bool bHasSnow = Snow.LayerCount > 0;
    const float MinSnowTempK = ModelParameters.Snow.MinSnowTemperature_K;
    const float GroundTemperature = (Soil.LayerCount > 0) ? Soil.Temperature_K[0] : ModelParameters.Soil.GroundTemperature_K;
    float InitialTemp = bHasSnow
        ? FMath::Max(Snow.Temperature_K[0], MinSnowTempK)
        : GroundTemperature;
    const float InitialIce = bHasSnow ? FMath::Max(0.0f, Snow.IceMass_kgm2[0]) : 0.0f;
    const float InitialLiquid = bHasSnow ? FMath::Max(0.0f, Snow.LiquidMass_kgm2[0]) : 0.0f;
    float HeatCapacity = bHasSnow ? FMath::Max(GetLayerHeatCapacity(Snow, 0), MinHeatCapacity_Jm2K) : MinHeatCapacity_Jm2K;

    float TotalColumnIceMass = 0.0f;
    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        TotalColumnIceMass += FMath::Max(0.0f, Snow.IceMass_kgm2[LayerIdx]);
    }

    const float SnowCoverFraction = ComputeSnowCoverFraction(Snow);
    const bool bHasCanopyExchange = ModelParameters.Modules.bEnableCanopy && ModelParameters.Layers.CanopyLayerCount > 0;
    const float TotalSnowDepth = ComputeSnowDepthMeters(Snow);
    float MeasurementWindHeight = ModelParameters.Atmosphere.WindMeasurementHeight_m;
    float MeasurementTempHeight = ModelParameters.Atmosphere.TemperatureMeasurementHeight_m;
    AdjustMeasurementHeights(MeasurementWindHeight, MeasurementTempHeight);

    // Use measurement heights directly (match Fortran SRFEBAL; no snow-depth offset)
    MeasurementWindHeight = FMath::Max(MeasurementWindHeight, 0.1f);
    MeasurementTempHeight = FMath::Max(MeasurementTempHeight, 0.1f);
    const float VonKarman = ModelParameters.PhysicalConstants.VonKarman;
    const float Gravity = ModelParameters.PhysicalConstants.Gravity_mps2;
    const float EffectiveWindSpeed = FMath::Max(WindSpeed, ModelParameters.Atmosphere.MinimumWindSpeed_mps);
    const float LegacyConductance = FMath::Max(BulkTransferCoefficient * EffectiveWindSpeed, 0.0f);
    float LastAerodynamicConductance = LegacyConductance;

    // Calculate surface layer thermal properties for shallow snow conditions
    float SurfaceLayerThickness, SurfaceLayerTemperature, SurfaceLayerConductivity;
    CalculateSurfaceLayerThermalProperties(Snow, Soil, SurfaceLayerThickness, SurfaceLayerTemperature, SurfaceLayerConductivity);

    TArray<double> SoilThicknessProfile;
    TArray<double> SoilConductivityProfile;
    TArray<double> SoilHeatCapacityProfile;
    if (Soil.LayerCount > 0)
    {
        EvaluateSoilThermalProfile(Soil, SoilThicknessProfile, SoilConductivityProfile, SoilHeatCapacityProfile);
    }

    // Fortran FSM2 does not lump soil heat capacity into the surface state.
    // Keep conduction via (ks1, Ts1, Ds1) only; no discrete heat-capacity jump in shallow snow.

    if (!bHasCanopyExchange)
    {
        const float SurfaceMoistureConductance = (Soil.LayerCount > 0)
            ? ComputeSurfaceMoistureConductance(Soil)
            : ModelParameters.Soil.SurfaceMoistureConductance_ms;
        // Fortran SRFEBAL does not include rain enthalpy; keep rain mass but zero energy term
        OutFluxes.Rain = 0.0f;

        FOpenSurfaceEnergyResult OpenResult = SolveOpenSurfaceEnergyBalance(
            DtSeconds,
            NetShortwave,
            IncomingLongwave,
            AirDensity,
            AirTempK,
            Qa,
            PressurePa,
            EffectiveWindSpeed,
            SnowCoverFraction,
            SurfaceMoistureConductance,
            SurfaceLayerThickness,
            SurfaceLayerTemperature,
            SurfaceLayerConductivity,
            Snow,
            TotalColumnIceMass,
            InitialTemp,
            MeasurementWindHeight,
            MeasurementTempHeight);

        OutFluxes.NetShortwave = NetShortwave;
        OutFluxes.NetLongwave = OpenResult.NetLongwave_Wm2;
        // OpenResult returns turbulent fluxes with Fortran sign (upward positive)
        OutFluxes.Sensible = OpenResult.Sensible_Wm2;
        OutFluxes.Latent = OpenResult.Latent_Wm2;
        OutFluxes.ShortwaveUpwelling = IncomingShortwave * SurfaceAlbedo;
        const float Emissivity = ModelParameters.Radiation.SnowEmissivity;
        const float StefanBoltzmann = ModelParameters.PhysicalConstants.StefanBoltzmann_Wm2K4;
        OutFluxes.LongwaveUpwelling = Emissivity * StefanBoltzmann * FMath::Pow(FMath::Max(OpenResult.SurfaceTempK, MinSnowTempK), 4.0f);
        FFSM2SurfaceUpdate Update;
        Update.SurfaceMassFlux_kgm2s = OpenResult.SurfaceMassFlux_kgm2s;
        Update.MeltMass_kgm2 = FMath::Max(0.0f, OpenResult.MeltRate_kgm2s * DtSeconds);
        Update.RefreezeMass_kgm2 = 0.0f;

        const float SurfaceIceBefore = (Snow.LayerCount > 0) ? FMath::Max(0.0f, Snow.IceMass_kgm2[0]) : 0.0f;
        const float SurfaceLiquidBefore = (Snow.LayerCount > 0) ? FMath::Max(0.0f, Snow.LiquidMass_kgm2[0]) : 0.0f;
        const float SurfaceMeltApplied = FMath::Min(SurfaceIceBefore, Update.MeltMass_kgm2);
        Update.IceMass_kgm2 = FMath::Max(0.0f, SurfaceIceBefore - SurfaceMeltApplied);
        Update.LiquidMass_kgm2 = SurfaceLiquidBefore + SurfaceMeltApplied;
        // BUGFIX: Allow snow surface to reach melting point without artificial upper clamp during energy balance
        // The melt logic in SolveOpenSurfaceEnergyBalance already handles phase change correctly
        Update.TemperatureK = FMath::Max(OpenResult.SurfaceTempK, MinSnowTempK);

        OutSurfaceMoistureConductance_ms = SurfaceMoistureConductance;
        OutSurfaceMoistureAvailability = OpenResult.WaterAvailability;

        OutFluxes = OutFluxes; // This line is redundant, OutFluxes is already updated
        OutSurfaceUpdate = Update;
        OutSurfaceTemperatureK = Update.TemperatureK;

        const float FinalGroundHeatFlux_Wm2 = OpenResult.GroundHeatFlux_Wm2;
        return FinalGroundHeatFlux_Wm2;
    }

    if (bHasSnow && SurfaceLayerThickness > KINDA_SMALL_NUMBER)
    {
        float CombinedHeatCapacity = 0.0f;
        double WeightedEnergy = 0.0;
        float RemainingDepth = SurfaceLayerThickness;

        for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount && RemainingDepth > KINDA_SMALL_NUMBER; ++LayerIdx)
        {
            const float LayerThickness = FMath::Max(Snow.Thickness_m[LayerIdx], MinimumLayerThickness_m);
            const float Portion = FMath::Min(LayerThickness, RemainingDepth);
            if (Portion <= KINDA_SMALL_NUMBER)
            {
                continue;
            }

            const float Fraction = Portion / LayerThickness;
            const float LayerHeatCapacity = GetLayerHeatCapacity(Snow, LayerIdx) * Fraction;
            if (LayerHeatCapacity <= 0.0f)
            {
                continue;
            }

            CombinedHeatCapacity += LayerHeatCapacity;
            WeightedEnergy += static_cast<double>(LayerHeatCapacity) * static_cast<double>(Snow.Temperature_K[LayerIdx]);
            RemainingDepth -= Portion;
        }

        if (RemainingDepth > KINDA_SMALL_NUMBER && SoilHeatCapacityProfile.Num() > 0 && SoilThicknessProfile.Num() > 0)
        {
            const float SoilThickness = FMath::Max(static_cast<float>(SoilThicknessProfile[0]), MinimumLayerThickness_m);
            const float Portion = FMath::Min(SoilThickness, RemainingDepth);
            if (Portion > KINDA_SMALL_NUMBER)
            {
                const float Fraction = (SoilThickness > KINDA_SMALL_NUMBER) ? Portion / SoilThickness : 0.0f;
                const float SoilHeatCapacity = static_cast<float>(SoilHeatCapacityProfile[0]) * Fraction;
                if (SoilHeatCapacity > 0.0f)
                {
                    const float SoilTemp = Soil.Temperature_K[0];
                    CombinedHeatCapacity += SoilHeatCapacity;
                    WeightedEnergy += static_cast<double>(SoilHeatCapacity) * static_cast<double>(SoilTemp);
                }
            }
        }

        if (CombinedHeatCapacity > 0.0f)
        {
            HeatCapacity = FMath::Max(HeatCapacity, CombinedHeatCapacity);
            if (Snow.Thickness_m[0] + KINDA_SMALL_NUMBER < SurfaceLayerThickness)
            {
                const float BlendedTemperature = static_cast<float>(WeightedEnergy / static_cast<double>(CombinedHeatCapacity));
                InitialTemp = BlendedTemperature;
            }
        }
    }

    HeatCapacity = FMath::Max(HeatCapacity, MinHeatCapacity_Jm2K);
    if (!bHasSnow && SurfaceLayerThickness > KINDA_SMALL_NUMBER)
    {
        InitialTemp = SurfaceLayerTemperature;
    }

    const float SpecificHeatAir = ModelParameters.PhysicalConstants.SpecificHeatAir_JkgK;
    const float LatentVapour = ModelParameters.PhysicalConstants.LatentHeatVapor_Jkg;
    constexpr float StefanBoltzmann = 5.670374419e-8f;
    const float Emissivity = ModelParameters.Radiation.SnowEmissivity;
    const float LatentFusion = ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg;
    const float SurfaceMoistureConductance = (Soil.LayerCount > 0)
        ? ComputeSurfaceMoistureConductance(Soil)
        : ModelParameters.Soil.SurfaceMoistureConductance_ms;

    auto EvaluateFluxesInternal = [&](float SurfaceTemp, bool bLimitLatentHeat, float* OutWaterAvailability, float* OutSurfaceMoistureConductance) -> FFSM2EnergyFluxes
    {
        // BUGFIX: Don't clamp temperature here - let it evolve naturally to enable proper melt
        const float EffectiveTemp = FMath::Max(SurfaceTemp, MinSnowTempK);
        FFSM2EnergyFluxes Fluxes;
        Fluxes.NetShortwave = NetShortwave;
        const float SWUpwelling = IncomingShortwave * SurfaceAlbedo;
        Fluxes.ShortwaveUpwelling = SWUpwelling;
        const float LWUpwelling = Emissivity * StefanBoltzmann * FMath::Pow(EffectiveTemp, 4.0f);
        Fluxes.NetLongwave = IncomingLongwave - LWUpwelling;
        Fluxes.LongwaveUpwelling = LWUpwelling;
        const float Qs = CalcSpecificHumidity(EffectiveTemp, 1.0f, PressurePa);
        const bool bSurfaceAtOrAboveFreezing = (EffectiveTemp >= FreezePoint_K - KelvinEpsilon);
        const float SurfaceLatentHeat = bSurfaceAtOrAboveFreezing ? LatentVapour : (LatentVapour + LatentFusion);
        float AerodynamicConductance = LegacyConductance;
        if (!bHasCanopyExchange)
        {
            const float SnowFrac = FMath::Clamp(SnowCoverFraction, 0.0f, 1.0f);
            const float RoughSnow = FMath::Max(ModelParameters.Snow.SnowRoughnessLength_m, MinRoughnessLength_m);
            const float RoughBare = FMath::Max(ModelParameters.Soil.SnowFreeRoughnessLength_m, MinRoughnessLength_m);
            const float WeightedLog = SnowFrac * FMath::Loge(RoughSnow) + (1.0f - SnowFrac) * FMath::Loge(RoughBare);
            const float MomentumRoughness = FMath::Exp(WeightedLog);
            const float ThermalRoughness = FMath::Max(0.1f * MomentumRoughness, MinRoughnessLength_m);
            const float zU = FMath::Max(MeasurementWindHeight, MomentumRoughness + 1.0e-3f);
            const float zT = FMath::Max(MeasurementTempHeight, ThermalRoughness + 1.0e-3f);

            float UStar = VonKarman * EffectiveWindSpeed / FMath::Max(FMath::Loge(zU / MomentumRoughness), 1.0e-3f);
            float Ga = VonKarman * UStar / FMath::Max(FMath::Loge(zT / ThermalRoughness), 1.0e-3f);
            float Stability = 0.0f;

            for (int32 Iter = 0; Iter < 10; ++Iter)
            {
                if (Iter < 8)
                {
                    const float Den = FMath::Max(UStar * UStar * UStar, 1.0e-6f);
                    Stability = -VonKarman * Gravity * Ga * (EffectiveTemp - AirTempK) / (FMath::Max(AirTempK, 200.0f) * Den);
                }

                const float PsiMTop = StabilityPsiM(zU, Stability);
                const float PsiMBot = StabilityPsiM(MomentumRoughness, Stability);
                const float PsiHTop = StabilityPsiH(zT, Stability);
                const float PsiHBot = StabilityPsiH(ThermalRoughness, Stability);

                const float DenomM = FMath::Max(FMath::Loge(zU / MomentumRoughness) - (PsiMTop - PsiMBot), 1.0e-3f);
                const float DenomH = FMath::Max(FMath::Loge(zT / ThermalRoughness) - (PsiHTop - PsiHBot), 1.0e-3f);

                const float NewUStar = VonKarman * EffectiveWindSpeed / DenomM;
                const float NewGa = VonKarman * NewUStar / DenomH;

                if (!FMath::IsFinite(NewUStar) || !FMath::IsFinite(NewGa))
                {
                    break;
                }

                const float DeltaGa = FMath::Abs(NewGa - Ga);
                UStar = FMath::Clamp(NewUStar, 1.0e-4f, 2.0f);
                Ga = FMath::Clamp(NewGa, 0.0f, 2.0f);

                if (DeltaGa < 1.0e-5f)
                {
                    break;
                }
            }

            if (FMath::IsFinite(Ga) && Ga > 0.0f)
            {
                AerodynamicConductance = FMath::Clamp(Ga, 0.0f, 2.0f);
            }
            else
            {
                AerodynamicConductance = LegacyConductance;
            }
        }
        else
        {
            AerodynamicConductance = LegacyConductance;
        }

        // Fortran/CF convention: turbulent fluxes stored as upward-positive (cooling the surface).
        // Sensible: H = rho * cp * Ga * (Ts - Ta)
        Fluxes.Sensible = AirDensity * SpecificHeatAir * AerodynamicConductance * (EffectiveTemp - AirTempK);
        // Latent: L*E with upward-positive when surface is wetter/warmer (Qs > Qa)
        const float LatentUnlim = AirDensity * SurfaceLatentHeat * AerodynamicConductance * (Qs - Qa);

        // Apply moisture availability limitation (Fortran-style)
        float WaterAvailability = 1.0f;
        float SurfaceConductance = SurfaceMoistureConductance;

        if (bLimitLatentHeat)
        {
            if (Qa > Qs + SaturationTolerance)
            {
                // Condensation (Qa > Qs): Water available from air - use full conductance
                WaterAvailability = 1.0f;
            }
            else
            {
                // Evaporation (Qs > Qa): Limited by surface moisture conductance
                SurfaceConductance = SurfaceMoistureConductance;
                const float Denom = SurfaceConductance + AerodynamicConductance;
                WaterAvailability = (Denom > 0.0f)
                    ? SnowCoverFraction + (1.0f - SnowCoverFraction) * (SurfaceConductance / Denom)
                    : SnowCoverFraction;
                WaterAvailability = FMath::Clamp(WaterAvailability, 0.0f, 1.0f);
            }
        }

        Fluxes.Latent = LatentUnlim * WaterAvailability;

        // SRFEBAL does not include a rain enthalpy term; report zero energy from rain
        Fluxes.Rain = 0.0f;
        LastAerodynamicConductance = AerodynamicConductance;
        if (OutWaterAvailability)
        {
            *OutWaterAvailability = WaterAvailability;
        }
        if (OutSurfaceMoistureConductance)
        {
            *OutSurfaceMoistureConductance = SurfaceConductance;
        }
        return Fluxes;
    };

    auto EvaluateFluxes = [&](float SurfaceTemp, bool bLimitLatentHeat = true, float* OutWaterAvailability = nullptr, float* OutSurfaceConductance = nullptr) -> FFSM2EnergyFluxes
    {
        return EvaluateFluxesInternal(SurfaceTemp, bLimitLatentHeat, OutWaterAvailability, OutSurfaceConductance);
    };

    auto ApplyEnergy = [&](const FFSM2EnergyFluxes& Fluxes)
    {
        FFSM2SurfaceUpdate Update;
        float Temperature = InitialTemp;
        float Ice = InitialIce;
        float Liquid = InitialLiquid;
        float Energy = Fluxes.Total() * DtSeconds;
        float MeltMass = 0.0f;
        float ColumnIceAvailable = TotalColumnIceMass;

        if (HeatCapacity <= MinHeatCapacity_Jm2K)
        {
            // BUGFIX: No clamp here either - let temperature follow air temperature
            Temperature = FMath::Max(AirTempK, MinSnowTempK);
            Energy = 0.0f;
        }
        else
        {
            // BUGFIX: Fortran-style energy application without artificial temperature clamps
            // Allow temperature to evolve naturally, handling phase change via energy consumption
            if (Temperature < FreezePoint_K - KelvinEpsilon)
            {
                // Warming toward freeze point
                const float EnergyToWarm = (FreezePoint_K - Temperature) * HeatCapacity;
                if (Energy >= EnergyToWarm)
                {
                    Temperature = FreezePoint_K;
                    Energy -= EnergyToWarm;
                }
                else
                {
                    Temperature = FMath::Max(Temperature + Energy / HeatCapacity, MinSnowTempK);
                    Energy = 0.0f;
                }
            }

            ColumnIceAvailable = FMath::Max(ColumnIceAvailable, 0.0f);

            // At or above freeze point with available ice: melt consumes energy
            if (Energy > 0.0f && ColumnIceAvailable > KINDA_SMALL_NUMBER)
            {
                const float PotentialMelt = Energy / LatentFusion;
                const float ActualMelt = FMath::Min(PotentialMelt, ColumnIceAvailable);
                if (ActualMelt > KINDA_SMALL_NUMBER)
                {
                    const float SurfaceMelt = FMath::Min(Ice, ActualMelt);
                    if (SurfaceMelt > 0.0f)
                    {
                        Ice -= SurfaceMelt;
                        Liquid += SurfaceMelt;
                    }

                    MeltMass += ActualMelt;
                    Energy -= ActualMelt * LatentFusion;
                    ColumnIceAvailable = FMath::Max(ColumnIceAvailable - ActualMelt, 0.0f);
                }
            }

            // Apply remaining energy as temperature change (no upper clamp for proper physics)
            if (FMath::Abs(Energy) > KINDA_SMALL_NUMBER)
            {
                const float DeltaT = Energy / HeatCapacity;
                Temperature = FMath::Max(Temperature + DeltaT, MinSnowTempK);
                // Note: No upper clamp - temperature can exceed freeze point if all ice melted
                // This matches Fortran FSM2 behavior where bare ground can warm significantly
            }
        }

        if (ThroughfallRain > KINDA_SMALL_NUMBER)
        {
            Liquid += ThroughfallRain;
        }

        // BUGFIX: Store temperature without upper clamp - natural physics handles this
        Update.TemperatureK = FMath::Max(Temperature, MinSnowTempK);
        Update.IceMass_kgm2 = FMath::Max(0.0f, Ice);
        Update.LiquidMass_kgm2 = FMath::Max(0.0f, Liquid);
        Update.MeltMass_kgm2 = MeltMass;
        Update.RefreezeMass_kgm2 = 0.0f;

        // Calculate surface mass flux from latent heat flux
        // Latent flux is energy flux, convert to mass flux using appropriate latent heat
        if (FMath::Abs(Fluxes.Latent) > KINDA_SMALL_NUMBER)
        {
            const float LatentHeat = (Update.TemperatureK > FreezePoint_K) ?
                LatentVapour : // Evaporation
                LatentVapour + LatentFusion; // Sublimation
            Update.SurfaceMassFlux_kgm2s = Fluxes.Latent / LatentHeat;
        }
        else
        {
            Update.SurfaceMassFlux_kgm2s = 0.0f;
        }

        return Update;
    };

    // Fortran-style iterative solution for surface energy balance
    float SurfaceTemp = InitialTemp;
    const int32 MaxIterations = 50;
    const float ResidualTolerance = 0.01f;
    const float Rwat = ModelParameters.PhysicalConstants.GasConstantWaterVapour_JkgK;

    // Log first few timesteps to diagnose 330K issue
    static int32 StepCount = 0;
    const bool bLogThisStep = (StepCount < 5);
    if (bLogThisStep)
    {
        UE_LOG(LogTemp, Warning, TEXT("=== Energy Balance Step %d ==="), StepCount);
        UE_LOG(LogTemp, Warning, TEXT("  InitialTemp: %.2f K, bHasSnow: %d, GroundTemp: %.2f K"),
            InitialTemp, bHasSnow, GroundTemperature);
        UE_LOG(LogTemp, Warning, TEXT("  AirTemp: %.2f K, NetSW: %.2f W/m2, InLW: %.2f W/m2"),
            AirTempK, NetShortwave, IncomingLongwave);
        UE_LOG(LogTemp, Warning, TEXT("  HeatCapacity: %.2f J/m2K, DtSeconds: %.2f"),
            HeatCapacity, DtSeconds);
    }

    FFSM2EnergyFluxes Fluxes = FFSM2EnergyFluxes();

    bool bConverged = false;
    for (int32 Iter = 0; Iter < MaxIterations; ++Iter)
    {
        float WaterAvailability = 1.0f;
        float SurfaceConductance = SurfaceMoistureConductance;
        Fluxes = EvaluateFluxesInternal(SurfaceTemp, true, &WaterAvailability, &SurfaceConductance);

        // BUGFIX: Don't clamp iteration temperature - allow natural evolution
        const float IterTemp = FMath::Max(SurfaceTemp, MinSnowTempK);
        const bool bWithinBounds = (SurfaceTemp > MinSnowTempK + KelvinEpsilon);
        const float ClampDerivative = bWithinBounds ? 1.0f : 0.0f;

        float GroundHeatFlux = 0.0f;
        float GroundDerivative = 0.0f;
        if (ModelParameters.Modules.bEnableGroundHeatFlux && SurfaceLayerThickness > KINDA_SMALL_NUMBER)
        {
            const float SurfaceConductivity = SurfaceLayerConductivity;
            GroundHeatFlux = 2.0f * SurfaceConductivity * (IterTemp - SurfaceLayerTemperature) / SurfaceLayerThickness;
            GroundDerivative = 2.0f * SurfaceConductivity / SurfaceLayerThickness * ClampDerivative;
        }

        const float QsIter = CalcSpecificHumidity(IterTemp, 1.0f, PressurePa);
        const float EffectiveLatentHeat = (IterTemp >= FreezePoint_K - KelvinEpsilon)
            ? LatentVapour
            : LatentVapour + LatentFusion;
        const float Dqs_dT = (IterTemp > 0.0f)
            ? EffectiveLatentHeat * QsIter / (Rwat * IterTemp * IterTemp)
            : 0.0f;

        // BUGFIX: Don't compute melt inside iteration - just check energy balance
        // Melt will be handled after convergence, Fortran-style
        const float EnergyResidual = Fluxes.Total() - GroundHeatFlux;
        const float DEnergy_dT =
            -4.0f * Emissivity * StefanBoltzmann * FMath::Pow(IterTemp, 3.0f) * ClampDerivative
            - AirDensity * SpecificHeatAir * LastAerodynamicConductance * ClampDerivative
            - AirDensity * EffectiveLatentHeat * LastAerodynamicConductance * WaterAvailability * Dqs_dT * ClampDerivative
            - GroundDerivative;

        if (bLogThisStep)
        {
            UE_LOG(LogTemp, Warning, TEXT("    Iter %d: SurfTemp=%.2f K, Residual=%.2f W/m2, dE/dT=%.2f, H=%.2f, LE=%.2f, GFlux=%.2f"),
                Iter, IterTemp, EnergyResidual, DEnergy_dT, Fluxes.Sensible, Fluxes.Latent, GroundHeatFlux);
        }

        const bool bResidualWithinTolerance = FMath::Abs(EnergyResidual) < ResidualTolerance;
        const bool bSlopeTooSmall = FMath::Abs(DEnergy_dT) < KINDA_SMALL_NUMBER;
        if (bResidualWithinTolerance || bSlopeTooSmall)
        {
            SurfaceTemp = IterTemp;
            if (bLogThisStep)
            {
                UE_LOG(LogTemp, Warning, TEXT("    Converged at iter %d"), Iter);
            }
            bConverged = bResidualWithinTolerance;
            break;
        }

        float TempIncrement = -EnergyResidual / DEnergy_dT;
        TempIncrement = FMath::Clamp(TempIncrement, -40.0f, 40.0f);

        // Only enforce minimum temperature for numerical stability
        SurfaceTemp = FMath::Max(SurfaceTemp + TempIncrement, MinSnowTempK);
    }

    if (bLogThisStep)
    {
        UE_LOG(LogTemp, Warning, TEXT("  Final SurfaceTemp: %.2f K"), SurfaceTemp);
        StepCount++;
    }

    // BUGFIX: Final temperature only needs lower bound, not upper clamp
    SurfaceTemp = FMath::Max(SurfaceTemp, MinSnowTempK);

    // Final fluxes at converged surface temperature
    float FinalWaterAvailability = 1.0f;
    float FinalSurfaceConductance = SurfaceMoistureConductance;
    Fluxes = EvaluateFluxes(SurfaceTemp, true, &FinalWaterAvailability, &FinalSurfaceConductance);

    // BUGFIX: Apply Fortran-style melt check AFTER convergence (SRFEBAL.F90 lines 224-244)
    FFSM2SurfaceUpdate Update;
    Update.TemperatureK = SurfaceTemp;
    Update.IceMass_kgm2 = InitialIce;
    Update.LiquidMass_kgm2 = InitialLiquid;
    Update.MeltMass_kgm2 = 0.0f;
    Update.RefreezeMass_kgm2 = 0.0f;
    Update.SurfaceMassFlux_kgm2s = 0.0f;

    // Check if surface would exceed melting point
    if (bHasSnow && SurfaceTemp > FreezePoint_K && TotalColumnIceMass > KINDA_SMALL_NUMBER)
    {
        // Fortran: Temperature exceeds melt point, so force to Tm and solve for melt rate
        SurfaceTemp = FreezePoint_K;
        Update.TemperatureK = FreezePoint_K;

        // Recalculate fluxes at freeze point
        Fluxes = EvaluateFluxes(FreezePoint_K, true, &FinalWaterAvailability, &FinalSurfaceConductance);

        // Calculate ground heat flux at freeze point
        float GroundHeatFlux = 0.0f;
        if (ModelParameters.Modules.bEnableGroundHeatFlux && SurfaceLayerThickness > KINDA_SMALL_NUMBER)
        {
            GroundHeatFlux = 2.0f * SurfaceLayerConductivity * (FreezePoint_K - SurfaceLayerTemperature) / SurfaceLayerThickness;
        }

        // Energy-balanced melt rate (upward-positive diagnostic fluxes): Melt = (Rsrf - Gsrf - H - LE) / Lf
        const float MeltEnergy_Wm2 = Fluxes.Total() - GroundHeatFlux;
        const float MeltRate_kgm2s = FMath::Max(MeltEnergy_Wm2 / LatentFusion, 0.0f);
        Update.MeltMass_kgm2 = MeltRate_kgm2s * DtSeconds;

        // Apply melt to surface layer
        const float SurfaceMelt = FMath::Min(InitialIce, Update.MeltMass_kgm2);
        Update.IceMass_kgm2 = FMath::Max(0.0f, InitialIce - SurfaceMelt);
        Update.LiquidMass_kgm2 = InitialLiquid + SurfaceMelt;
    }

    // Calculate surface mass flux from latent heat flux
    if (FMath::Abs(Fluxes.Latent) > KINDA_SMALL_NUMBER)
    {
        const float LatentHeat = (Update.TemperatureK >= FreezePoint_K - KelvinEpsilon) ?
            LatentVapour : // Evaporation
            LatentVapour + LatentFusion; // Sublimation
        Update.SurfaceMassFlux_kgm2s = Fluxes.Latent / LatentHeat;
    }

    // Add rain to liquid
    if (ThroughfallRain > KINDA_SMALL_NUMBER)
    {
        Update.LiquidMass_kgm2 += ThroughfallRain;
    }

    auto ResolveEffectiveSurfaceTemp = [&](const FFSM2SurfaceUpdate& SurfaceUpdate) -> float
    {
        // BUGFIX: No upper clamp - allow natural temperature evolution
        return FMath::Max(SurfaceUpdate.TemperatureK, MinSnowTempK);
    };

    // BUGFIX: Update is already calculated above with proper Fortran-style melt logic
    // No need for additional temperature adjustment iteration
    SurfaceTemp = Update.TemperatureK;

    float FinalGroundHeatFlux_Wm2 = 0.0f;
    if (SurfaceLayerThickness > KINDA_SMALL_NUMBER)
    {
        FinalGroundHeatFlux_Wm2 = 2.0f * SurfaceLayerConductivity * (SurfaceTemp - SurfaceLayerTemperature) / SurfaceLayerThickness;
    }

    float MeltEnergy_Wm2 = (DtSeconds > KINDA_SMALL_NUMBER)
        ? Update.MeltMass_kgm2 * LatentFusion / DtSeconds
        : 0.0f;
    float EnergyResidual_Wm2 = Fluxes.Total() - FinalGroundHeatFlux_Wm2 - MeltEnergy_Wm2;

    // Mirror FSM2's "all snow melts" trial step: if residual energy remains while snow is present,
    // spend it on additional melt before giving up on the imbalance.
    if (DtSeconds > KINDA_SMALL_NUMBER && EnergyResidual_Wm2 > ResidualTolerance)
    {
        const float IceRemaining = FMath::Max(0.0f, TotalColumnIceMass - Update.MeltMass_kgm2);
        if (IceRemaining > KINDA_SMALL_NUMBER)
        {
            const float ResidualEnergy_Jm2 = EnergyResidual_Wm2 * DtSeconds;
            const float AdditionalMelt = FMath::Min(ResidualEnergy_Jm2 / LatentFusion, IceRemaining);
            if (AdditionalMelt > KINDA_SMALL_NUMBER)
            {
                Update.MeltMass_kgm2 += AdditionalMelt;

                const float SurfaceMelt = FMath::Min(Update.IceMass_kgm2, AdditionalMelt);
                if (SurfaceMelt > KINDA_SMALL_NUMBER)
                {
                    Update.IceMass_kgm2 = FMath::Max(0.0f, Update.IceMass_kgm2 - SurfaceMelt);
                }
                Update.LiquidMass_kgm2 += AdditionalMelt;

                MeltEnergy_Wm2 += AdditionalMelt * LatentFusion / DtSeconds;
                EnergyResidual_Wm2 = Fluxes.Total() - FinalGroundHeatFlux_Wm2 - MeltEnergy_Wm2;
            }
        }
    }

    if (!bConverged && FMath::Abs(EnergyResidual_Wm2) > ResidualTolerance)
    {
        FinalGroundHeatFlux_Wm2 += EnergyResidual_Wm2;
    }

    OutFluxes = Fluxes;
    OutSurfaceUpdate = Update;
    OutSurfaceTemperatureK = Update.TemperatureK;

    OutSurfaceMoistureConductance_ms = FinalSurfaceConductance;
    OutSurfaceMoistureAvailability = FinalWaterAvailability;
    return FinalGroundHeatFlux_Wm2;
}

UFSM2SnowSimulation::FOpenSurfaceEnergyResult UFSM2SnowSimulation::SolveOpenSurfaceEnergyBalance(
    float DtSeconds,
    float NetShortwave,
    float IncomingLongwave,
    float AirDensity,
    float AirTempK,
    float Qa,
    float PressurePa,
    float EffectiveWindSpeed,
    float SnowCoverFraction,
    float SurfaceMoistureConductance,
    float SurfaceLayerThickness,
    float SurfaceLayerTemperature,
    float SurfaceLayerConductivity,
    const FFSM2SnowColumn& Snow,
    float TotalColumnIceMass,
    float InitialSurfaceTemp,
    float MeasurementWindHeight,
    float MeasurementTempHeight) const
{
    FOpenSurfaceEnergyResult Result;
    Result.bConverged = false;
    Result.NetShortwave_Wm2 = NetShortwave;

    const float Dt = FMath::Max(DtSeconds, KINDA_SMALL_NUMBER);
    const float SpecificHeatAir = ModelParameters.PhysicalConstants.SpecificHeatAir_JkgK;
    const float LatentHeatVapour = ModelParameters.PhysicalConstants.LatentHeatVapor_Jkg;
    const float LatentHeatFusion = ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg;
    const float LatentHeatSublimation = LatentHeatVapour + LatentHeatFusion;
    const float GasConstAir = ModelParameters.PhysicalConstants.GasConstantDryAir_JkgK;
    const float GasConstVapour = ModelParameters.PhysicalConstants.GasConstantWaterVapour_JkgK;
    const float VonKarman = ModelParameters.PhysicalConstants.VonKarman;
    const float Gravity = ModelParameters.PhysicalConstants.Gravity_mps2;
    constexpr float StefanBoltzmann = 5.670374419e-8f;

    const float Emissivity = ModelParameters.Radiation.SnowEmissivity;
    const float MinTempK = ModelParameters.Snow.MinSnowTemperature_K;
    const float SurfaceThickness = FMath::Max(SurfaceLayerThickness, MinimumLayerThickness_m);
    const float SurfaceConductivity = FMath::Max(SurfaceLayerConductivity, 1.0e-4f);
    const float SurfaceGs = FMath::Max(SurfaceMoistureConductance, 0.0f);

    const float RhoAir = PressurePa / (GasConstAir * FMath::Max(AirTempK, 200.0f));

    // BUGFIX: Initialize without upper clamp - let energy balance determine temperature
    float Tsrf = FMath::Max(InitialSurfaceTemp, MinTempK);

    const float SnowRough = FMath::Max(ModelParameters.Snow.SnowRoughnessLength_m, MinRoughnessLength_m);
    const float BareRough = FMath::Max(ModelParameters.Soil.SnowFreeRoughnessLength_m, MinRoughnessLength_m);

    float z0g = SnowRough;
    if (SnowCoverFraction < 1.0f)
    {
        const float LogSnow = FMath::Loge(SnowRough);
        const float LogBare = FMath::Loge(BareRough);
        z0g = FMath::Exp(SnowCoverFraction * LogSnow + (1.0f - SnowCoverFraction) * LogBare);
    }
    const float z0h = FMath::Max(0.1f * z0g, MinRoughnessLength_m);

    const float zU = FMath::Max(MeasurementWindHeight, z0g + 1.0e-3f);
    const float zT = FMath::Max(MeasurementTempHeight, z0h + 1.0e-3f);

    float ustar = VonKarman * EffectiveWindSpeed / FMath::Max(FMath::Loge(zU / z0g), 1.0e-6f);
    float ga = VonKarman * ustar / FMath::Max(FMath::Loge(zT / z0h), 1.0e-6f);
    float rL = 0.0f;

    float FinalGsrf = 0.0f;
    float FinalHsrf = 0.0f;
    float FinalEsrf = 0.0f;
    float FinalNetLongwave = IncomingLongwave - Emissivity * StefanBoltzmann * FMath::Pow(Tsrf, 4.0f);
    float FinalMeltRate = 0.0f;
    float FinalLsrf = LatentHeatSublimation;
    float FinalWsrf = 1.0f;

    for (int32 Iter = 0; Iter < 10; ++Iter)
    {
        if (Iter < 8)
        {
            const float Den = FMath::Max(ustar * ustar * ustar, 1.0e-6f);
            rL = -VonKarman * Gravity * ga * (Tsrf - AirTempK) / (FMath::Max(AirTempK, 200.0f) * Den);
        }

        const float PsiMTop = StabilityPsiM(zU, rL);
        const float PsiMBot = StabilityPsiM(z0g, rL);
        const float PsiHTop = StabilityPsiH(zT, rL);
        const float PsiHBot = StabilityPsiH(z0h, rL);

        const float DenomM = FMath::Max(FMath::Loge(zU / z0g) - (PsiMTop - PsiMBot), 1.0e-6f);
        const float DenomH = FMath::Max(FMath::Loge(zT / z0h) - (PsiHTop - PsiHBot), 1.0e-6f);

        ustar = VonKarman * EffectiveWindSpeed / DenomM;
        ga = VonKarman * ustar / DenomH;
        ga = FMath::Clamp(ga, 0.0f, 2.0f);

        float LatentHeat = (Tsrf > FreezePoint_K) ? LatentHeatVapour : LatentHeatSublimation;
        float Lsrf = LatentHeat;  // Store latent heat for later use
        float Qsrf = CalcSpecificHumidity(Tsrf, 1.0f, PressurePa);
        float DqsDT = (Tsrf > 0.0f) ? LatentHeat * Qsrf / (GasConstVapour * Tsrf * Tsrf) : 0.0f;
        float Dsrf = DqsDT;  // Store specific humidity derivative for later use
        float wsrf = (Qa > Qsrf)
            ? 1.0f
            : FMath::Clamp(SnowCoverFraction + (1.0f - SnowCoverFraction) * SurfaceGs / FMath::Max(SurfaceGs + ga, 1.0e-6f), 0.0f, 1.0f);

        float Esrf = RhoAir * wsrf * ga * (Qa - Qsrf);
        float Gsrf = 2.0f * SurfaceConductivity * (Tsrf - SurfaceLayerTemperature) / SurfaceThickness;
        float Hsrf = RhoAir * SpecificHeatAir * ga * (AirTempK - Tsrf);
        float NetLongwave = IncomingLongwave - Emissivity * StefanBoltzmann * FMath::Pow(Tsrf, 4.0f);
        float Rsrf = NetShortwave + NetLongwave;

        // Energy balance residual (f = Rn + H + LE - G); rain enthalpy excluded to match SRFEBAL
        // Turbulent fluxes (H, LE) are Inputs (Positive Downwards)
        float Residual = Rsrf + Hsrf + LatentHeat * Esrf - Gsrf;

        float Denominator = 4.0f * Emissivity * StefanBoltzmann * FMath::Pow(Tsrf, 3.0f)
            + RhoAir * SpecificHeatAir * ga
            + RhoAir * LatentHeat * wsrf * ga * DqsDT
            + 2.0f * SurfaceConductivity / SurfaceThickness;
        Denominator = FMath::Max(Denominator, 1.0e-6f);

        float dTs = Residual / Denominator;
        
        float MeltRate = 0.0f;
        float dEs = 0.0f;
        float dGs = 0.0f;
        float dHs = 0.0f;

        // Step 2: Check if temperature would exceed melting point
        if (Tsrf + dTs > FreezePoint_K && TotalColumnIceMass > KINDA_SMALL_NUMBER)
        {
            // Step 3a: Try maximum possible melt rate (all ice melts in this timestep)
            const float MaxMeltRate = TotalColumnIceMass / Dt;

            // Recalculate dTs with melt energy sink included
            float MeltDenom = 4.0f * Emissivity * StefanBoltzmann * FMath::Pow(Tsrf, 3.0f)
                + 2.0f * SurfaceConductivity / SurfaceThickness
                + RhoAir * (SpecificHeatAir + LatentHeatSublimation * Dsrf * wsrf) * ga;
            MeltDenom = FMath::Max(MeltDenom, 1.0e-6f);

            dTs = (Rsrf - Gsrf + Hsrf + Lsrf * Esrf - LatentHeatFusion * MaxMeltRate) / MeltDenom;

            // Step 3b: Check if melt rate brings temperature back below freeze point
            if (Tsrf + dTs < FreezePoint_K)
            {
                // Maximum melt was too much - solve for exact melt rate at Tsrf = FreezePoint_K
                // Fortran: "call QSAT(Ps,Tm,Qsrf)" etc. (lines 232-237)
                const float Tsrf_old = Tsrf;  // Save current Tsrf BEFORE overwriting

                Lsrf = LatentHeatSublimation;  // At freeze point, use sublimation
                Qsrf = CalcSpecificHumidity(FreezePoint_K, 1.0f, PressurePa);
                Dsrf = Lsrf * Qsrf / (GasConstVapour * FreezePoint_K * FreezePoint_K);

                // Recalculate fluxes at Tsrf = Tm.
                // Keep internal convention consistent: turbulent fluxes are downward-positive.
                Esrf = RhoAir * wsrf * ga * (Qa - Qsrf);
                Gsrf = 2.0f * SurfaceConductivity * (FreezePoint_K - SurfaceLayerTemperature) / SurfaceThickness;
                Hsrf = RhoAir * SpecificHeatAir * ga * (AirTempK - FreezePoint_K);
                NetLongwave = IncomingLongwave - Emissivity * StefanBoltzmann * FMath::Pow(FreezePoint_K, 4.0f);
                Rsrf = NetShortwave + NetLongwave;

                // Energy-balanced melt rate (downward-positive internal sign convention).
                MeltRate = FMath::Max((Rsrf + Hsrf + Lsrf * Esrf - Gsrf) / FMath::Max(LatentHeatFusion, 1.0e-6f), 0.0f);

                // Fortran line 242: dTs = Tm - Tsrf (where Tsrf is the OLD value)
                dTs = FreezePoint_K - Tsrf_old;

                // Fortran lines 239-241: Zero out increments (fluxes already recalculated above)
                dEs = 0.0f;
                dGs = 0.0f;
                dHs = 0.0f;
            }
            else
            {
                // Use maximum melt rate, temperature still exceeds freeze point
                MeltRate = MaxMeltRate;
                // dTs remains as calculated with melt
                // Derivatives are negative because Input Flux decreases as Surface Temp increases
                dEs = -RhoAir * wsrf * ga * Dsrf * dTs;
                dGs = 2.0f * SurfaceConductivity / SurfaceThickness * dTs;
                dHs = -RhoAir * SpecificHeatAir * ga * dTs;
            }
        }
        else
        {
            // No melt - apply normal increments
            // Derivatives are negative because Input Flux decreases as Surface Temp increases
            dEs = -RhoAir * wsrf * ga * Dsrf * dTs;
            dGs = 2.0f * SurfaceConductivity / SurfaceThickness * dTs;
            dHs = -RhoAir * SpecificHeatAir * ga * dTs;
        }

        Tsrf += dTs;
        Esrf += dEs;
        Gsrf += dGs;
        Hsrf += dHs;
        NetLongwave = IncomingLongwave - Emissivity * StefanBoltzmann * FMath::Pow(Tsrf, 4.0f);
        Rsrf = NetShortwave + NetLongwave;

        float EnergyResidual = Rsrf - Gsrf + Hsrf + Lsrf * Esrf - LatentHeatFusion * MeltRate;

        FinalGsrf = Gsrf;
        FinalHsrf = Hsrf;
        FinalEsrf = Esrf;
        FinalNetLongwave = NetLongwave;
        FinalMeltRate = MeltRate;
        FinalLsrf = Lsrf;
        FinalWsrf = wsrf;

        if (Iter >= 4 && FMath::Abs(EnergyResidual) < 0.01f)
        {
            Result.bConverged = true;
            break;
        }
    }

    Result.SurfaceTempK = Tsrf;
    Result.GroundHeatFlux_Wm2 = FinalGsrf;
    // Report turbulent fluxes using Fortran sign (upward positive)
    Result.Sensible_Wm2 = -FinalHsrf;
    Result.Latent_Wm2 = -FinalLsrf * FinalEsrf;
    Result.NetLongwave_Wm2 = FinalNetLongwave;
    Result.MeltRate_kgm2s = FMath::Max(0.0f, FinalMeltRate);
    Result.SurfaceMassFlux_kgm2s = -FinalEsrf;
    Result.WaterAvailability = FinalWsrf;
    Result.AerodynamicConductance_ms = ga;
    Result.LatentHeat_Jkg = FinalLsrf;

    return Result;
}

void UFSM2SnowSimulation::RefreshEnvironmentalMetadata(UWorld* World)
{
    bHasEnvironmentalOverride = false;

    if (!World)
    {
        return;
    }

    if (AGeoReferencingSystem* Geo = AGeoReferencingSystem::GetGeoReferencingSystem(World))
    {
        double Lat = Geo->OriginLatitude;
        if (Geo->bOriginLocationInProjectedCRS)
        {
            FGeographicCoordinates GeoCoords;
            Geo->ProjectedToGeographic(FVector(Geo->OriginProjectedCoordinatesEasting, Geo->OriginProjectedCoordinatesNorthing, Geo->OriginProjectedCoordinatesUp), GeoCoords);
            Lat = GeoCoords.Latitude;
        }

        CachedLatitudeDeg = static_cast<float>(Lat);
        CachedSolarNoon = ModelParameters.Atmosphere.SolarNoonLocalTime;
        bHasEnvironmentalOverride = true;
    }

    // Use model parameters for solar noon time
    // TODO: Optionally integrate with SunSky plugin if available in future
    if (bHasEnvironmentalOverride)
    {
        CachedSolarNoon = ModelParameters.Atmosphere.SolarNoonLocalTime;
    }

    if (bHasEnvironmentalOverride)
    {
        ModelParameters.Atmosphere.LatitudeDegrees = CachedLatitudeDeg;
        ModelParameters.Atmosphere.SolarNoonLocalTime = CachedSolarNoon;
    }
}

void UFSM2SnowSimulation::UpdateDynamicSurfaceGeometry()
{
    if (!bHasTerrainMetadata || GridX <= 0 || GridY <= 0)
    {
        return;
    }

    const int32 CellCount = GridX * GridY;
    if (DepthMeters.Num() != CellCount
        || TerrainAltitudeCm.Num() != CellCount
        || TerrainSlopeDegrees.Num() != CellCount
        || TerrainCurvature.Num() != CellCount)
    {
        return;
    }

    const ASnowSimulationActor* ActorPtr = OwningSimulationActor.Get();
    const bool bUseLegacyCurvatureCellSizeScaling = ActorPtr ? ActorPtr->bNormalizeCurvatureByCellSize : false;
    const float CurvatureReferenceMeters = ActorPtr ? FMath::Max(0.001f, ActorPtr->CurvatureReferenceMeters) : 10.0f;
    const float CurvatureClampAbs = ActorPtr ? ActorPtr->CurvatureClampAbs : 0.0f;
    const float CellSpacing = FMath::Max(CellSpacingMeters, KINDA_SMALL_NUMBER);
    const float TwoCellSpacing = 2.0f * CellSpacing;
    const float CellSpacingSq = CellSpacing * CellSpacing;

    const auto ClampIndex = [this](int32 X, int32 Y) -> int32
    {
        X = FMath::Clamp(X, 0, GridX - 1);
        Y = FMath::Clamp(Y, 0, GridY - 1);
        return X + Y * GridX;
    };

    const auto GetSurfaceHeightMeters = [this](int32 CellIndex) -> float
    {
        return (TerrainAltitudeCm[CellIndex] / 100.0f) + DepthMeters[CellIndex];
    };

    for (int32 Y = 0; Y < GridY; ++Y)
    {
        for (int32 X = 0; X < GridX; ++X)
        {
            const int32 Idx = X + Y * GridX;
            const int32 IdxN = ClampIndex(X, Y - 1);
            const int32 IdxS = ClampIndex(X, Y + 1);
            const int32 IdxE = ClampIndex(X + 1, Y);
            const int32 IdxW = ClampIndex(X - 1, Y);
            const float Z2 = GetSurfaceHeightMeters(IdxN);
            const float Z4 = GetSurfaceHeightMeters(IdxE);
            const float Z5 = GetSurfaceHeightMeters(Idx);
            const float Z6 = GetSurfaceHeightMeters(IdxW);
            const float Z8 = GetSurfaceHeightMeters(IdxS);

            const float dZdx = (Z4 - Z6) / TwoCellSpacing;
            const float dZdy = (Z8 - Z2) / TwoCellSpacing;
            const float SlopeRadians = FMath::Atan(FMath::Sqrt(dZdx * dZdx + dZdy * dZdy));
            TerrainSlopeDegrees[Idx] = FMath::RadiansToDegrees(SlopeRadians);

            const float D = ((Z4 + Z6) * 0.5f - Z5) / CellSpacingSq;
            const float E = ((Z2 + Z8) * 0.5f - Z5) / CellSpacingSq;
            float Curvature = 2.0f * (D + E);

            if (bUseLegacyCurvatureCellSizeScaling)
            {
                const float ScaleL2 = (CellSpacing * CellSpacing) / (CurvatureReferenceMeters * CurvatureReferenceMeters);
                Curvature *= ScaleL2;
            }

            if (CurvatureClampAbs > 0.0f)
            {
                Curvature = FMath::Clamp(Curvature, -CurvatureClampAbs, CurvatureClampAbs);
            }

            TerrainCurvature[Idx] = Curvature;
        }
    }

    static bool bLoggedDynamicGeometry = false;
    if (!bLoggedDynamicGeometry)
    {
        bLoggedDynamicGeometry = true;
        UE_LOG(LogTemp, Display, TEXT("[FSM2] Dynamic surface geometry enabled - slope/curvature updated from snow surface"));
    }
}

void UFSM2SnowSimulation::ApplyLapseRateAdjustments(int32 CellIndex, float BaseAirTempK, float BasePrecipitation_kgm2, float& OutAirTempK, float& OutPrecipitation_kgm2) const
{
    OutAirTempK = BaseAirTempK;
    OutPrecipitation_kgm2 = BasePrecipitation_kgm2;

    if (ModelParameters.Ensemble.bDisableLapseRateAdjustments || !TerrainAltitudeCm.IsValidIndex(CellIndex))
    {
        return;
    }

    const float AltitudeDeltaCm = TerrainAltitudeCm[CellIndex] - MeasurementAltitudeCm;
    constexpr float MaxReasonableAbsAltitudeDeltaCm = 20000.0f * 100.0f; // 20 km guardrail

    if (!FMath::IsFinite(AltitudeDeltaCm) || FMath::Abs(AltitudeDeltaCm) > MaxReasonableAbsAltitudeDeltaCm)
    {
        static bool bLoggedAltitudeDeltaOutlier = false;
        if (!bLoggedAltitudeDeltaOutlier)
        {
            bLoggedAltitudeDeltaOutlier = true;
            UE_LOG(LogTemp, Warning, TEXT("[FSM2] Ignoring unrealistic altitude delta in lapse correction (idx=%d, Delta=%.2f cm, CellAlt=%.2f cm, MeasurementAlt=%.2f cm)."),
                CellIndex,
                AltitudeDeltaCm,
                TerrainAltitudeCm[CellIndex],
                MeasurementAltitudeCm);
        }
        return;
    }

    const float DeltaHundredMeters = AltitudeDeltaCm / 10000.0f;
    OutAirTempK = FMath::Clamp(
        BaseAirTempK + ModelParameters.Ensemble.TemperatureLapseRate_CPer100m * DeltaHundredMeters,
        200.0f,
        320.0f);

    if (ModelParameters.Ensemble.bApplyPrecipLapseBelowStation || AltitudeDeltaCm > 0.0f)
    {
        const float DeltaKm = AltitudeDeltaCm / 100000.0f;
        const float PrecipScale = FMath::Max(
            0.0f,
            1.0f + ModelParameters.Ensemble.PrecipitationLapseRate_FractionPerKm * DeltaKm);
        OutPrecipitation_kgm2 = BasePrecipitation_kgm2 * PrecipScale;
    }
}

void UFSM2SnowSimulation::ComputeSnowConduction(FFSM2SnowColumn& Snow, FFSM2SoilColumn& Soil, float DtSeconds, float SurfaceFlux_Wm2, double& OutGsoil_Wm2)
{
    const int32 SnowCount = FMath::Clamp(Snow.LayerCount, 0, GFSM2MaxLayers);
    const int32 SoilCount = FMath::Clamp(Soil.LayerCount, 0, GFSM2MaxSoilLayers);

    OutGsoil_Wm2 = 0.0;

    if (SnowCount <= 0)
    {
        return;
    }

    TStaticArray<double, GFSM2MaxLayers> csnow;
    TStaticArray<double, GFSM2MaxLayers> ksnow;
    TStaticArray<double, GFSM2MaxLayers> thickness;

    for (int32 LayerIdx = 0; LayerIdx < SnowCount; ++LayerIdx)
    {
        const double HeatCapacity = Snow.IceMass_kgm2[LayerIdx] * ModelParameters.PhysicalConstants.SpecificHeatIce_JkgK +
            Snow.LiquidMass_kgm2[LayerIdx] * ModelParameters.PhysicalConstants.SpecificHeatWater_JkgK;
        csnow[LayerIdx] = FMath::Max<double>(HeatCapacity, MinHeatCapacity_Jm2K);
        ksnow[LayerIdx] = FMath::Max<double>(GetLayerThermalConductivity(Snow, LayerIdx), 1.0e-3);
        thickness[LayerIdx] = FMath::Max<double>(Snow.Thickness_m[LayerIdx], MinimumLayerThickness_m);
    }

    const double SoilTemperature = (SoilCount > 0) ? Soil.Temperature_K[0] : ModelParameters.Soil.GroundTemperature_K;
    const double SoilThickness = (SoilCount > 0)
        ? FMath::Max<double>(Soil.Thickness_m[0], MinimumLayerThickness_m)
        : ((ModelParameters.Layers.SoilLayerThicknesses_m.Num() > 0)
            ? FMath::Max<double>(ModelParameters.Layers.SoilLayerThicknesses_m[0], MinimumLayerThickness_m)
            : MinimumLayerThickness_m);

    // Get soil thermal conductivity using moisture-dependent calculation
    double SoilConductivity = ModelParameters.Soil.GroundConductivity_WmK;
    if (SoilCount > 0)
    {
        TArray<double> SoilThicknessArray, SoilConductivityArray, SoilHeatCapacityArray;
        EvaluateSoilThermalProfile(Soil, SoilThicknessArray, SoilConductivityArray, SoilHeatCapacityArray);
        if (SoilConductivityArray.Num() > 0)
        {
            SoilConductivity = FMath::Max<double>(SoilConductivityArray[0], 1.0e-3);
        }
    }

    const double Dt = FMath::Max<double>(DtSeconds, KINDA_SMALL_NUMBER);
    const double MinSnowTempK = static_cast<double>(ModelParameters.Snow.MinSnowTemperature_K);
    const double FreezePointSnowK = static_cast<double>(FreezePoint_K);

    if (SnowCount == 1)
    {
        const double SnowSoilResistance = (thickness[0] / ksnow[0]) + (SoilThickness / SoilConductivity);
        const double USoil = (SnowSoilResistance > KINDA_SMALL_NUMBER) ? (2.0 / SnowSoilResistance) : 0.0;

        const double Numerator = (SurfaceFlux_Wm2 + USoil * (SoilTemperature - Snow.Temperature_K[0])) * Dt;
        const double Denominator = csnow[0] + USoil * Dt;
        const double dT = (Denominator > KINDA_SMALL_NUMBER) ? Numerator / Denominator : 0.0;

        double NewTemp = Snow.Temperature_K[0] + dT;
        const bool bLayerHasIce = Snow.IceMass_kgm2[0] > KINDA_SMALL_NUMBER;

        // BUGFIX: Handle phase change instead of clamping
        if (bLayerHasIce && NewTemp > FreezePointSnowK)
        {
            // Excess energy would warm above freeze point - convert to melt
            const double ExcessEnergy = (NewTemp - FreezePointSnowK) * csnow[0];
            const double PotentialMelt = ExcessEnergy / static_cast<double>(ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg);
            const double ActualMelt = FMath::Min(PotentialMelt, static_cast<double>(Snow.IceMass_kgm2[0]));

            if (ActualMelt > KINDA_SMALL_NUMBER)
            {
                Snow.IceMass_kgm2[0] = FMath::Max(0.0f, Snow.IceMass_kgm2[0] - static_cast<float>(ActualMelt));
                Snow.LiquidMass_kgm2[0] += static_cast<float>(ActualMelt);
            }

            NewTemp = FreezePointSnowK;
        }

        // Only enforce minimum temperature for numerical stability
        NewTemp = FMath::Max(NewTemp, MinSnowTempK);
        Snow.Temperature_K[0] = static_cast<float>(NewTemp);
        OutGsoil_Wm2 = USoil * (Snow.Temperature_K[0] - SoilTemperature);

        // DEBUG: Log for shallow snow diagnosis
        if (SnowCount == 1)
        {
            static int32 LogCount = 0;
            if (LogCount++ < 100) // Limit logs
            {
                UE_LOG(LogTemp, Log, TEXT("[FSM2_DEBUG] N=1: Flux=%.2f, Tsnow=%.2f, Tsoil=%.2f, USoil=%.4f, dT=%.4f, Gsoil=%.2f"),
                    SurfaceFlux_Wm2, Snow.Temperature_K[0], SoilTemperature, USoil, dT, OutGsoil_Wm2);
            }
        }
        return;
    }

    TStaticArray<double, GFSM2MaxLayers> UBetween;
    for (int32 LayerIdx = 0; LayerIdx < SnowCount - 1; ++LayerIdx)
    {
        const double Resistance = (thickness[LayerIdx] / ksnow[LayerIdx]) + (thickness[LayerIdx + 1] / ksnow[LayerIdx + 1]);
        UBetween[LayerIdx] = (Resistance > KINDA_SMALL_NUMBER) ? (2.0 / Resistance) : 0.0;
    }

    const double BottomResistance = (thickness[SnowCount - 1] / ksnow[SnowCount - 1]) + (SoilThickness / SoilConductivity);
    const double UBottom = (BottomResistance > KINDA_SMALL_NUMBER) ? (2.0 / BottomResistance) : 0.0;

    TStaticArray<double, GFSM2MaxLayers> a;
    TStaticArray<double, GFSM2MaxLayers> b;
    TStaticArray<double, GFSM2MaxLayers> c;
    TStaticArray<double, GFSM2MaxLayers> rhs;
    TStaticArray<double, GFSM2MaxLayers> dT;

    for (int32 LayerIdx = 0; LayerIdx < SnowCount; ++LayerIdx)
    {
        if (LayerIdx == 0)
        {
            const double UBelow = UBetween[0];
            a[LayerIdx] = 0.0;
            b[LayerIdx] = csnow[LayerIdx] + UBelow * Dt;
            c[LayerIdx] = -UBelow * Dt;
            rhs[LayerIdx] = (SurfaceFlux_Wm2 - UBelow * (Snow.Temperature_K[LayerIdx] - Snow.Temperature_K[LayerIdx + 1])) * Dt;
        }
        else if (LayerIdx < SnowCount - 1)
        {
            const double UAbove = UBetween[LayerIdx - 1];
            const double UBelow = UBetween[LayerIdx];
            a[LayerIdx] = c[LayerIdx - 1];
            b[LayerIdx] = csnow[LayerIdx] + (UAbove + UBelow) * Dt;
            c[LayerIdx] = -UBelow * Dt;
            rhs[LayerIdx] = (UAbove * (Snow.Temperature_K[LayerIdx - 1] - Snow.Temperature_K[LayerIdx]) +
                UBelow * (Snow.Temperature_K[LayerIdx + 1] - Snow.Temperature_K[LayerIdx])) * Dt;
        }
        else
        {
            const double UAbove = UBetween[LayerIdx - 1];
            a[LayerIdx] = c[LayerIdx - 1];
            b[LayerIdx] = csnow[LayerIdx] + (UAbove + UBottom) * Dt;
            c[LayerIdx] = 0.0;
            rhs[LayerIdx] = (UAbove * (Snow.Temperature_K[LayerIdx - 1] - Snow.Temperature_K[LayerIdx]) +
                UBottom * (SoilTemperature - Snow.Temperature_K[LayerIdx])) * Dt;
        }
    }

    SolveTridiagonal(SnowCount, a.GetData(), b.GetData(), c.GetData(), rhs.GetData(), dT.GetData());

    for (int32 LayerIdx = 0; LayerIdx < SnowCount; ++LayerIdx)
    {
        double NewTemp = Snow.Temperature_K[LayerIdx] + dT[LayerIdx];
        const bool bLayerHasIce = Snow.IceMass_kgm2[LayerIdx] > KINDA_SMALL_NUMBER;

        // BUGFIX: Handle phase change in multi-layer snowpack
        if (bLayerHasIce && NewTemp > FreezePointSnowK)
        {
            // Excess energy would warm above freeze point - convert to melt
            const double ExcessEnergy = (NewTemp - FreezePointSnowK) * csnow[LayerIdx];
            const double PotentialMelt = ExcessEnergy / static_cast<double>(ModelParameters.PhysicalConstants.LatentHeatFusion_Jkg);
            const double ActualMelt = FMath::Min(PotentialMelt, static_cast<double>(Snow.IceMass_kgm2[LayerIdx]));

            if (ActualMelt > KINDA_SMALL_NUMBER)
            {
                Snow.IceMass_kgm2[LayerIdx] = FMath::Max(0.0f, Snow.IceMass_kgm2[LayerIdx] - static_cast<float>(ActualMelt));
                Snow.LiquidMass_kgm2[LayerIdx] += static_cast<float>(ActualMelt);
            }

            NewTemp = FreezePointSnowK;
        }

        // Only enforce minimum temperature for numerical stability
        NewTemp = FMath::Max(NewTemp, MinSnowTempK);
        Snow.Temperature_K[LayerIdx] = static_cast<float>(NewTemp);
    }

    OutGsoil_Wm2 = UBottom * (Snow.Temperature_K[SnowCount - 1] - SoilTemperature);

    // DEBUG: Log for multi-layer snow diagnosis
    if (SnowCount > 1 && SnowCount < 3)
    {
        static int32 LogCountMulti = 0;
        if (LogCountMulti++ < 100)
        {
            FString TempStr;
            for(int32 i=0; i<SnowCount; ++i) TempStr += FString::Printf(TEXT("%.2f, "), Snow.Temperature_K[i]);
            
            UE_LOG(LogTemp, Log, TEXT("[FSM2_DEBUG] N=%d: Flux=%.2f, Tsnow=[%s], Tsoil=%.2f, UBottom=%.4f, Gsoil=%.2f"),
                SnowCount, SurfaceFlux_Wm2, *TempStr, SoilTemperature, UBottom, OutGsoil_Wm2);
        }
    }
}

void UFSM2SnowSimulation::AdvanceSoilColumn(FFSM2SoilColumn& Soil, float DtSeconds, double Gsoil_Wm2)
{
    const int32 SoilCount = FMath::Clamp(Soil.LayerCount, 0, GFSM2MaxSoilLayers);
    if (SoilCount <= 0 || DtSeconds <= 0.0f)
    {
        return;
    }

    // Use the complex soil thermal profile calculation
    TArray<double> Thickness, Conductivity, HeatCapacity;
    EvaluateSoilThermalProfile(Soil, Thickness, Conductivity, HeatCapacity);

    const double Dt = FMath::Max<double>(DtSeconds, KINDA_SMALL_NUMBER);

    // Use zero flux boundary condition at the bottom (matches Fortran FSM2)
    // Fortran: U(Nsoil) = 0
    const double UBottom = 0.0;

    if (SoilCount == 1)
    {
        const double Denominator = HeatCapacity[0] + UBottom * Dt;
        const double dT = (Denominator > KINDA_SMALL_NUMBER) ? (Gsoil_Wm2 * Dt) / Denominator : 0.0;
        Soil.Temperature_K[0] = FMath::Clamp<double>(Soil.Temperature_K[0] + dT, 200.0, 330.0);
        return;
    }

    TStaticArray<double, GFSM2MaxSoilLayers> UBetween;
    for (int32 SoilIdx = 0; SoilIdx < SoilCount - 1; ++SoilIdx)
    {
        const double Resistance = (Thickness[SoilIdx] / Conductivity[SoilIdx]) + (Thickness[SoilIdx + 1] / Conductivity[SoilIdx + 1]);
        UBetween[SoilIdx] = (Resistance > KINDA_SMALL_NUMBER) ? (2.0 / Resistance) : 0.0;
    }

    TStaticArray<double, GFSM2MaxSoilLayers> a;
    TStaticArray<double, GFSM2MaxSoilLayers> b;
    TStaticArray<double, GFSM2MaxSoilLayers> c;
    TStaticArray<double, GFSM2MaxSoilLayers> rhs;
    TStaticArray<double, GFSM2MaxSoilLayers> dT;

    for (int32 SoilIdx = 0; SoilIdx < SoilCount; ++SoilIdx)
    {
        if (SoilIdx == 0)
        {
            const double UBelow = UBetween[0];
            a[SoilIdx] = 0.0;
            b[SoilIdx] = HeatCapacity[SoilIdx] + UBelow * Dt;
            c[SoilIdx] = -UBelow * Dt;
            rhs[SoilIdx] = (Gsoil_Wm2 - UBelow * (Soil.Temperature_K[SoilIdx] - Soil.Temperature_K[SoilIdx + 1])) * Dt;
        }
        else if (SoilIdx < SoilCount - 1)
        {
            const double UAbove = UBetween[SoilIdx - 1];
            const double UBelow = UBetween[SoilIdx];
            a[SoilIdx] = c[SoilIdx - 1];
            b[SoilIdx] = HeatCapacity[SoilIdx] + (UAbove + UBelow) * Dt;
            c[SoilIdx] = -UBelow * Dt;
            rhs[SoilIdx] = (UAbove * (Soil.Temperature_K[SoilIdx - 1] - Soil.Temperature_K[SoilIdx]) +
                UBelow * (Soil.Temperature_K[SoilIdx + 1] - Soil.Temperature_K[SoilIdx])) * Dt;
        }
        else
        {
            const double UAbove = UBetween[SoilIdx - 1];
            a[SoilIdx] = c[SoilIdx - 1];
            b[SoilIdx] = HeatCapacity[SoilIdx] + (UAbove + UBottom) * Dt;
            c[SoilIdx] = 0.0;
            rhs[SoilIdx] = (UAbove * (Soil.Temperature_K[SoilIdx - 1] - Soil.Temperature_K[SoilIdx])) * Dt;
        }
    }

    SolveTridiagonal(SoilCount, a.GetData(), b.GetData(), c.GetData(), rhs.GetData(), dT.GetData());

    for (int32 SoilIdx = 0; SoilIdx < SoilCount; ++SoilIdx)
    {
        Soil.Temperature_K[SoilIdx] = FMath::Clamp<double>(Soil.Temperature_K[SoilIdx] + dT[SoilIdx], 200.0, 330.0);
    }
}

TArray<float> UFSM2SnowSimulation::GetCellSnowAlbedoState() const
{
    TArray<float> AlbedoArray;
    AlbedoArray.Reserve(CellStates.Num());

    for (const FFSM2ColumnState& State : CellStates)
    {
        AlbedoArray.Add(State.SnowAlbedo);
    }

    return AlbedoArray;
}

TArray<float> UFSM2SnowSimulation::GetCellSurfaceAlbedoState() const
{
    TArray<float> AlbedoArray;
    AlbedoArray.Reserve(CellStates.Num());

    for (const FFSM2ColumnState& State : CellStates)
    {
        AlbedoArray.Add(State.SurfaceAlbedo);
    }

    return AlbedoArray;
}

float UFSM2SnowSimulation::ComputeSnowDepthMeters(const FFSM2SnowColumn& Snow) const
{
    float Depth = 0.0f;
    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        Depth += Snow.Thickness_m[LayerIdx];
    }
    return Depth;
}

float UFSM2SnowSimulation::ComputeSnowMass(const FFSM2SnowColumn& Snow) const
{
    float Mass = 0.0f;
    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        Mass += Snow.IceMass_kgm2[LayerIdx] + Snow.LiquidMass_kgm2[LayerIdx];
    }
    return Mass;
}

float UFSM2SnowSimulation::ComputeSnowIceMass(const FFSM2SnowColumn& Snow) const
{
    float Mass = 0.0f;
    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        Mass += FMath::Max(0.0f, Snow.IceMass_kgm2[LayerIdx]);
    }
    return Mass;
}

void UFSM2SnowSimulation::EnsureDiagnosticsFileInitialized()
{
    if (bDiagnosticsFileInitialized)
    {
        return;
    }

    ASnowSimulationActor* Actor = OwningSimulationActor.Get();

    // Create diagnostics directory if it doesn't exist
    FString DiagnosticsDir = Actor
        ? Actor->GetOutputCategoryDirectory(TEXT("Diagnostics"))
        : (FPaths::ProjectDir() / ModelParameters.Diagnostics.DiagnosticsDirectory);
    IFileManager::Get().MakeDirectory(*DiagnosticsDir, true);

    // Generate unique filename with timestamp
    FDateTime Now = FDateTime::Now();
    FString Timestamp = FString::Printf(TEXT("%04d%02d%02d_%02d%02d%02d"),
        Now.GetYear(), Now.GetMonth(), Now.GetDay(),
        Now.GetHour(), Now.GetMinute(), Now.GetSecond());
    const FString RunTag = Actor ? Actor->BuildRunTag(Timestamp) : Timestamp;

    DiagnosticsFilePath = DiagnosticsDir / FString::Printf(TEXT("FSM2Diagnostics_%s.csv"), *RunTag);

    bDiagnosticsFileInitialized = true;

    // Export parameters alongside diagnostics
    WriteParametersToFile(RunTag);
}

void UFSM2SnowSimulation::WriteDiagnostics(const TArray<FFSM2CellDiagnostics>& CellDiagnostics)
{
    if (!bDiagnosticsFileInitialized || CellDiagnostics.Num() == 0)
    {
        return;
    }

    FString Output;
    const bool bLeanPostRun = ModelParameters.Diagnostics.bWriteLeanPostRunBundleDiagnostics;

    // Write header if not already written
    if (!bDiagnosticsHeaderWritten)
    {
        FString Header;
        if (bLeanPostRun)
        {
            Header = TEXT("Step,CellIndex,TimeSeconds,DateTimeISO,SnowDepth_m,SnowMass_kgm2,ThroughfallSnow_kgm2,MeltMass_kgm2,SurfaceTemperatureK,Fluxes.Sensible,Fluxes.Latent,Fluxes.ShortwaveUpwelling,Fluxes.LongwaveUpwelling");
        }
        else
        {
            Header = TEXT("Step,CellIndex,TimeSeconds,DateTimeISO,SnowDepth_m,SnowMass_kgm2,Runoff_kgm2,ThroughfallSnow_kgm2,ThroughfallRain_kgm2,FrostMass_kgm2,MeltMass_kgm2,RefreezeMass_kgm2,SublimationMass_kgm2,CanopySnow_kgm2,CanopyLiquid_kgm2,SurfaceTemperatureK,NetSurfaceFlux_Wm2,GroundHeatFlux_Wm2,ForcingAirTemperatureK,ForcingIncomingLongwave_Wm2,ForcingIncomingShortwave_Wm2,ForcingDiffuseShortwave_Wm2,ForcingDirectShortwave_Wm2,ForcingWindSpeed_mps,RadiationIndex,RadiationIndex_Direct,RadiationIndex_Diffuse,RadiationComponent,SurfaceMoistureConductance_ms,SurfaceMoistureAvailability,ForcingDiffuseShortwaveBase_Wm2,ForcingDirectShortwaveBase_Wm2,TerrainShortwave_Wm2,TotalShortwave_Wm2,RadiationIndexImpact_Wm2,RadiationIndexImpactRatio,CalibratedSWROut_Wm2,ReferenceValid,ReferenceInvalidReasonCode,ReferenceInvalidReason,ReferenceActorValidityFlag,ReferenceActorValidityOverridden,ReferenceLuminance_Total,ReferenceLuminance_Direct,ReferenceLuminance_Diffuse,ReferenceLuminance_DiffuseNoGI,ReferenceLuminance_TotalNoGI,ReferenceScale_Total,ReferenceScale_Direct,ReferenceScale_Diffuse,ReferenceScale_TotalNoGI,DiffuseScalingMode,SkyOnlyReferenceUsable,SkyOnlyReferenceMeetsMinLuminance,SkyOnlyDiffuseScalingUsed,SkyOnlyReferenceRatio,SkyOnlyRTYRatio,");
            Header += TEXT("Fluxes.NetShortwave,Fluxes.NetLongwave,Fluxes.Sensible,Fluxes.Latent,Fluxes.Rain,Fluxes.ShortwaveUpwelling,Fluxes.LongwaveUpwelling,");
            Header += TEXT("SnowLayerCount,SoilLayerCount,");
        }

        if (!bLeanPostRun)
        {
            // Snow layer columns
            for (int32 i = 0; i < GFSM2MaxLayers; ++i)
            {
                Header += FString::Printf(TEXT("SnowThickness_m[%d],SnowTemperature_K[%d],SnowIceMass_kgm2[%d],SnowLiquidMass_kgm2[%d],SnowDensity_kgm3[%d],SnowGrainRadius_m[%d],SnowWaterFlux_kgm2s[%d],"), i, i, i, i, i, i, i);
            }

            // Soil layer columns
            for (int32 i = 0; i < GFSM2MaxSoilLayers; ++i)
            {
                Header += FString::Printf(TEXT("SoilThickness_m[%d],SoilTemperature_K[%d],SoilMoisture_Vol[%d],"), i, i, i);
            }

            Header.RemoveFromEnd(TEXT(","));
        }
        Output += Header + TEXT("\n");
        bDiagnosticsHeaderWritten = true;
    }

    // Write data rows
    for (const FFSM2CellDiagnostics& Diag : CellDiagnostics)
    {
        const FString TimestampString = Diag.Timestamp.ToIso8601();
        const int32 SnowCount = FMath::Clamp(Diag.SnowLayerCount, 0, GFSM2MaxLayers);
        const int32 SoilCount = FMath::Clamp(Diag.SoilLayerCount, 0, GFSM2MaxSoilLayers);

        auto SanitizeFloat = [](float Value) -> FString
        {
            return FString::SanitizeFloat(Value);
        };

        TArray<FString> Fields;
        if (bLeanPostRun)
        {
            Fields.Reserve(13);
            Fields.Add(FString::FromInt(Diag.StepIndex));
            Fields.Add(FString::FromInt(Diag.CellIndex));
            Fields.Add(FString::SanitizeFloat(Diag.SimulationTimeSeconds));
            Fields.Add(TimestampString);
            Fields.Add(SanitizeFloat(Diag.SnowDepth_m));
            Fields.Add(SanitizeFloat(Diag.SnowMass_kgm2));
            Fields.Add(SanitizeFloat(Diag.ThroughfallSnow_kgm2));
            Fields.Add(SanitizeFloat(Diag.MeltMass_kgm2));
            Fields.Add(SanitizeFloat(Diag.SurfaceTemperatureK));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.Sensible));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.Latent));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.ShortwaveUpwelling));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.LongwaveUpwelling));
        }
        else
        {
            Fields.Reserve(54 + GFSM2MaxLayers * 7 + GFSM2MaxSoilLayers * 3);

            Fields.Add(FString::FromInt(Diag.StepIndex));
            Fields.Add(FString::FromInt(Diag.CellIndex));
            Fields.Add(FString::SanitizeFloat(Diag.SimulationTimeSeconds));
            Fields.Add(TimestampString);
            Fields.Add(SanitizeFloat(Diag.SnowDepth_m));
            Fields.Add(SanitizeFloat(Diag.SnowMass_kgm2));
            Fields.Add(SanitizeFloat(Diag.Runoff_kgm2));
            Fields.Add(SanitizeFloat(Diag.ThroughfallSnow_kgm2));
            Fields.Add(SanitizeFloat(Diag.ThroughfallRain_kgm2));
            Fields.Add(SanitizeFloat(Diag.FrostMass_kgm2));
            Fields.Add(SanitizeFloat(Diag.MeltMass_kgm2));
            Fields.Add(SanitizeFloat(Diag.RefreezeMass_kgm2));
            Fields.Add(SanitizeFloat(Diag.SublimationMass_kgm2));
            Fields.Add(SanitizeFloat(Diag.CanopySnow_kgm2));
            Fields.Add(SanitizeFloat(Diag.CanopyLiquid_kgm2));
            Fields.Add(SanitizeFloat(Diag.SurfaceTemperatureK));
            Fields.Add(SanitizeFloat(Diag.NetSurfaceFlux_Wm2));
            Fields.Add(SanitizeFloat(Diag.GroundHeatFlux_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingAirTemperatureK));
            Fields.Add(SanitizeFloat(Diag.ForcingIncomingLongwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingIncomingShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDiffuseShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDirectShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingWindSpeed_mps));
            Fields.Add(SanitizeFloat(Diag.RadiationIndex));
            Fields.Add(SanitizeFloat(Diag.RadiationIndex_Direct));
            Fields.Add(SanitizeFloat(Diag.RadiationIndex_Diffuse));
            Fields.Add(SanitizeFloat(Diag.RadiationComponent));
            Fields.Add(SanitizeFloat(Diag.SurfaceMoistureConductance_ms));
            Fields.Add(SanitizeFloat(Diag.SurfaceMoistureAvailability));
            Fields.Add(SanitizeFloat(Diag.ForcingDiffuseShortwaveBase_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDirectShortwaveBase_Wm2));
            Fields.Add(SanitizeFloat(Diag.TerrainShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.TotalShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.RadiationIndexImpact_Wm2));
            Fields.Add(SanitizeFloat(Diag.RadiationIndexImpactRatio));
            Fields.Add(SanitizeFloat(Diag.CalibratedSWROut_Wm2));
            Fields.Add(FString::FromInt(Diag.bReferenceValid ? 1 : 0));
            Fields.Add(FString::FromInt(static_cast<int32>(Diag.ReferenceInvalidReasonCode)));
            Fields.Add(GetReferenceInvalidReasonText(Diag.ReferenceInvalidReasonCode));
            Fields.Add(FString::FromInt(Diag.bReferenceActorValidityFlag ? 1 : 0));
            Fields.Add(FString::FromInt(Diag.bReferenceActorValidityOverridden ? 1 : 0));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Total));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Direct));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Diffuse));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_DiffuseNoGI));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_TotalNoGI));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_Total));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_Direct));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_Diffuse));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_TotalNoGI));
            Fields.Add(Diag.bSkyOnlyDiffuseScalingUsed != 0 ? TEXT("total_nogi_closure") : TEXT("full_diffuse"));
            Fields.Add(FString::FromInt(Diag.bSkyOnlyReferenceUsable != 0 ? 1 : 0));
            Fields.Add(FString::FromInt(Diag.bSkyOnlyReferenceMeetsMinLuminance != 0 ? 1 : 0));
            Fields.Add(FString::FromInt(Diag.bSkyOnlyDiffuseScalingUsed != 0 ? 1 : 0));
            Fields.Add(SanitizeFloat(Diag.SkyOnlyReferenceRatio));
            Fields.Add(SanitizeFloat(Diag.SkyOnlyRTYRatio));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.NetShortwave));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.NetLongwave));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.Sensible));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.Latent));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.Rain));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.ShortwaveUpwelling));
            Fields.Add(SanitizeFloat(Diag.EnergyFluxes.LongwaveUpwelling));

            Fields.Add(FString::FromInt(SnowCount));
            Fields.Add(FString::FromInt(SoilCount));

            for (int32 i = 0; i < GFSM2MaxLayers; ++i)
            {
                if (i < SnowCount)
                {
                    Fields.Add(SanitizeFloat(Diag.SnowThickness_m[i]));
                    Fields.Add(SanitizeFloat(Diag.SnowTemperature_K[i]));
                    Fields.Add(SanitizeFloat(Diag.SnowIceMass_kgm2[i]));
                    Fields.Add(SanitizeFloat(Diag.SnowLiquidMass_kgm2[i]));
                    Fields.Add(SanitizeFloat(Diag.SnowDensity_kgm3[i]));
                    Fields.Add(SanitizeFloat(Diag.SnowGrainRadius_m[i]));
                    Fields.Add(SanitizeFloat(Diag.SnowWaterFlux_kgm2s[i]));
                }
                else
                {
                    Fields.Add(TEXT("nan"));
                    Fields.Add(TEXT("nan"));
                    Fields.Add(TEXT("nan"));
                    Fields.Add(TEXT("nan"));
                    Fields.Add(TEXT("nan"));
                    Fields.Add(TEXT("nan"));
                    Fields.Add(TEXT("nan"));
                }
            }

            for (int32 i = 0; i < GFSM2MaxSoilLayers; ++i)
            {
                if (i < SoilCount)
                {
                    Fields.Add(SanitizeFloat(Diag.SoilThickness_m[i]));
                    Fields.Add(SanitizeFloat(Diag.SoilTemperature_K[i]));
                    Fields.Add(SanitizeFloat(Diag.SoilMoisture_Vol[i]));
                }
                else
                {
                    Fields.Add(TEXT("nan"));
                    Fields.Add(TEXT("nan"));
                    Fields.Add(TEXT("nan"));
                }
            }
        }

        FString Line = FString::Join(Fields, TEXT(","));
        Output += Line;
        Output.AppendChar(TEXT('\n'));
    }

    const uint32 WriteFlags = ModelParameters.Diagnostics.bAppendDiagnostics ? FILEWRITE_Append : FILEWRITE_None;
    const bool bSuccess = FFileHelper::SaveStringToFile(
        Output,
        *DiagnosticsFilePath,
        FFileHelper::EEncodingOptions::AutoDetect,
        &IFileManager::Get(),
        WriteFlags);
    if (!bSuccess)
    {
        UE_LOG(LogTemp, Error, TEXT("[FSM2] Failed to write diagnostics file: %s"), *DiagnosticsFilePath);
    }
}

void UFSM2SnowSimulation::EnsureRadiationDiagnosticsFileInitialized()
{
    if (!RadiationDiagnosticsFilePath.IsEmpty())
    {
        return;
    }

    ASnowSimulationActor* Actor = OwningSimulationActor.Get();
    if (!Actor || !Actor->bEnableRadiationDiagnostics)
    {
        return;
    }

    const FString DiagnosticsDir = Actor->GetOutputCategoryDirectory(TEXT("Radiation"));
    IFileManager::Get().MakeDirectory(*DiagnosticsDir, true);

    const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    const FString RunTag = Actor->BuildRunTag(Timestamp);

    RadiationDiagnosticsFilePath = FPaths::Combine(DiagnosticsDir, FString::Printf(TEXT("FSM2_Radiation_%s.csv"), *RunTag));
    RadiationConfigFilePath = FPaths::Combine(DiagnosticsDir, FString::Printf(TEXT("FSM2_Radiation_%s_config.txt"), *RunTag));
    bRadiationDiagnosticsHeaderWritten = false;

    WriteRadiationConfigFile(RunTag);

    UE_LOG(LogTemp, Display, TEXT("[FSM2] Radiation diagnostics initialized: %s"), *RadiationDiagnosticsFilePath);
}

void UFSM2SnowSimulation::WriteRadiationDiagnostics(const TArray<FFSM2CellDiagnostics>& CellDiagnostics)
{
    ASnowSimulationActor* Actor = OwningSimulationActor.Get();
    if (!Actor || !Actor->bEnableRadiationDiagnostics || RadiationDiagnosticsFilePath.IsEmpty() || CellDiagnostics.Num() == 0)
    {
        return;
    }

    const bool bFileExists = FPaths::FileExists(RadiationDiagnosticsFilePath);
    const bool bIsFirstWrite = !bFileExists || !Actor->bAppendRadiationDiagnostics;
    const bool bLeanPostRun = ModelParameters.Diagnostics.bWriteLeanPostRunBundleDiagnostics;

    FString Output;
    if (bIsFirstWrite || !bRadiationDiagnosticsHeaderWritten)
    {
        if (bLeanPostRun)
        {
            Output += TEXT("StepIndex,CellIndex,SimulationTimeSeconds,Timestamp,");
            Output += TEXT("ForcingIncomingShortwave_Wm2,ForcingDirectShortwave_Wm2,ForcingDiffuseShortwave_Wm2,");
            Output += TEXT("ForcingDirectShortwaveBase_Wm2,ForcingDiffuseShortwaveBase_Wm2,");
            Output += TEXT("RadiationIndex,RadiationIndex_Direct,RadiationIndex_Diffuse,");
            Output += TEXT("DirectShortwave_Wm2,DiffuseShortwave_Wm2,TerrainShortwave_Wm2,TotalShortwave_Wm2,");
            Output += TEXT("ReferenceValid,ReferenceLuminance_Total_Selected,ReferenceLuminance_Direct_Selected,ReferenceLuminance_Diffuse_Selected,ReferenceLuminance_DiffuseNoGI_Selected,");
            Output += TEXT("ReferenceLuminance_Total_FullStrip,");
            Output += TEXT("ReferenceScale_Total,ReferenceScale_Direct,ReferenceScale_Diffuse,");
            Output += TEXT("DiffuseScalingMode,SkyOnlyReferenceUsable,SkyOnlyReferenceMeetsMinLuminance,SkyOnlyDiffuseScalingUsed,SkyOnlyReferenceRatio,SkyOnlyRTYRatio,");
            Output += TEXT("RTY_Total,RTY_Direct,RTY_Diffuse,RTY_DiffuseNoGI,RTY_Terrain");
        }
        else
        {
            Output += TEXT("StepIndex,CellIndex,SimulationTimeSeconds,Timestamp,");
            Output += TEXT("ForcingIncomingShortwave_Wm2,ForcingDirectShortwave_Wm2,ForcingDiffuseShortwave_Wm2,");
            Output += TEXT("ForcingDirectShortwaveBase_Wm2,ForcingDiffuseShortwaveBase_Wm2,");
            Output += TEXT("RadiationIndex,RadiationIndex_Direct,RadiationIndex_Diffuse,");
            Output += TEXT("DirectShortwave_Wm2,DiffuseShortwave_Wm2,TerrainShortwave_Wm2,TotalShortwave_Wm2,");
            Output += TEXT("RadiationIndexImpact_Wm2,RadiationIndexImpactRatio,CalibratedSWROut_Wm2,");
            Output += TEXT("SurfaceAlbedo_Model,NeutralizationSurfaceAlbedo,NeutralizationReferenceAlbedo,NeutralizationSnowBlendWeight,NeutralizationUsedRenderSurfaceState,AlbedoNeutralizationRawFactor,AlbedoNeutralizationFactor,");
            Output += TEXT("ReferenceValid,ReferenceInvalidReasonCode,ReferenceInvalidReason,ReferenceActorValidityFlag,ReferenceActorValidityOverridden,ReferenceLuminance_Total_Selected,ReferenceLuminance_Direct_Selected,ReferenceLuminance_Diffuse_Selected,ReferenceLuminance_DiffuseNoGI_Selected,ReferenceLuminance_TotalNoGI_Selected,");
            Output += TEXT("ReferenceLuminance_Total_FullStrip,ReferenceLuminance_Direct_FullStrip,ReferenceLuminance_Diffuse_FullStrip,ReferenceLuminance_DiffuseNoGI_FullStrip,");
            Output += TEXT("DualReferenceStripEnabled,ReferenceSurfaceClass,ReferenceSelectionSnowDepth_m,ReferenceSelectionThreshold_m,ReferenceSelectionSnowCoverFraction,ReferenceSnowBlendWeight,ReferenceRenderSurfaceSnowBlendWeight,ReferenceSelectionUsedRenderSurfaceState,ReferenceSelectionPlausibilityOverride,ReferenceSelectionSnowLayerCount,SnowLayerCount_Current,");
            Output += TEXT("ReferenceLuminance_Direct_Ground_AtSelection,ReferenceLuminance_Direct_Snow_AtSelection,ReferenceSurfaceState_FullStrip,ReferenceSurfaceState_Ground_AtSelection,ReferenceSurfaceState_Snow_AtSelection,");
            Output += TEXT("ReferenceScale_Total,ReferenceScale_Direct,ReferenceScale_Diffuse,ReferenceScale_TotalNoGI,");
            Output += TEXT("DiffuseScalingMode,SkyOnlyReferenceUsable,SkyOnlyReferenceMeetsMinLuminance,SkyOnlyDiffuseScalingUsed,SkyOnlyReferenceRatio,SkyOnlyRTYRatio,");
            Output += TEXT("RTY_Total,RTY_Direct,RTY_Diffuse,RTY_DiffuseNoGI,RTY_TotalNoGI,RTY_SurfaceState,RTY_Terrain,");
            Output += TEXT("ReferenceTotal_SelectedOverFullStrip,ReferenceDirect_SelectedOverFullStrip,ReferenceDirect_GroundOverSnow,ReferenceDirect_SnowOverGround,");
            Output += TEXT("RTYTotal_OverReferenceSelected,RTYTotal_OverReferenceFullStrip,");
            Output += TEXT("RTYDirect_OverReferenceSelected,RTYDirect_OverReferenceFullStrip,RTYDirect_OverReferenceGround,RTYDirect_OverReferenceSnow,");
            Output += TEXT("DirectShortwave_FullStripReference_Wm2,DirectShortwave_GroundReference_Wm2,DirectShortwave_SnowReference_Wm2,TotalShortwave_FullStripReference_Wm2");
        }
        Output.AppendChar(TEXT('\n'));
        bRadiationDiagnosticsHeaderWritten = true;
    }

    auto SanitizeFloat = [](float Value) -> FString
    {
        return FString::SanitizeFloat(Value);
    };
    auto FormatRatio = [&SanitizeFloat](float Numerator, float Denominator) -> FString
    {
        if (!FMath::IsFinite(Numerator) || !FMath::IsFinite(Denominator) || FMath::Abs(Denominator) <= KINDA_SMALL_NUMBER)
        {
            return TEXT("nan");
        }
        return SanitizeFloat(Numerator / Denominator);
    };
    auto FormatScaledFlux = [&SanitizeFloat](float BaseFlux, float SampleLuminance, float ReferenceLuminance) -> FString
    {
        if (!FMath::IsFinite(BaseFlux) || !FMath::IsFinite(SampleLuminance) || !FMath::IsFinite(ReferenceLuminance) || ReferenceLuminance <= KINDA_SMALL_NUMBER)
        {
            return TEXT("nan");
        }
        return SanitizeFloat(BaseFlux * SampleLuminance / ReferenceLuminance);
    };

    for (const FFSM2CellDiagnostics& Diag : CellDiagnostics)
    {
        const bool bDualReferenceStripEnabled = Diag.bDualReferenceStripEnabled != 0;
        const float ReferenceSelectionThreshold_m = Actor ? FMath::Max(0.0f, Actor->DualReferenceSnowDepthThreshold_m) : 0.0f;
        const FString ReferenceSurfaceClass = bDualReferenceStripEnabled
            ? (Diag.bReferenceSelectedGroundHalf != 0 ? TEXT("GroundHalf") : TEXT("SnowHalf"))
            : TEXT("SingleStrip");
        const float ReferenceLuminanceDirectGroundCurrent = Diag.ReferenceLuminance_Direct_Ground_Snapshot;
        const float ReferenceLuminanceDirectSnowCurrent = Diag.ReferenceLuminance_Direct_Snow_Snapshot;
        const float ReferenceSurfaceStateFullStrip = Diag.ReferenceLuminance_SurfaceState_FullStrip_Snapshot;
        const float ReferenceSurfaceStateGroundCurrent = Diag.ReferenceLuminance_SurfaceState_Ground_Snapshot;
        const float ReferenceSurfaceStateSnowCurrent = Diag.ReferenceLuminance_SurfaceState_Snow_Snapshot;
        const float ReferenceLuminanceTotalSelected = Diag.ReferenceLuminance_Total;
        const float ReferenceLuminanceDirectSelected = Diag.ReferenceLuminance_Direct;
        const float ReferenceLuminanceTotalFullStrip = Diag.ReferenceLuminance_Total_FullStrip_Snapshot;
        const float ReferenceLuminanceDirectFullStrip = Diag.ReferenceLuminance_Direct_FullStrip_Snapshot;

        TArray<FString> Fields;
        if (bLeanPostRun)
        {
            Fields.Reserve(36);
            Fields.Add(FString::FromInt(Diag.StepIndex));
            Fields.Add(FString::FromInt(Diag.CellIndex));
            Fields.Add(FString::SanitizeFloat(Diag.SimulationTimeSeconds));
            Fields.Add(Diag.Timestamp.ToIso8601());

            Fields.Add(SanitizeFloat(Diag.ForcingIncomingShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDirectShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDiffuseShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDirectShortwaveBase_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDiffuseShortwaveBase_Wm2));

            Fields.Add(SanitizeFloat(Diag.RadiationIndex));
            Fields.Add(SanitizeFloat(Diag.RadiationIndex_Direct));
            Fields.Add(SanitizeFloat(Diag.RadiationIndex_Diffuse));

            Fields.Add(SanitizeFloat(Diag.DirectShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.DiffuseShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.TerrainShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.TotalShortwave_Wm2));

            Fields.Add(FString::FromInt(Diag.bReferenceValid ? 1 : 0));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Total));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Direct));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Diffuse));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_DiffuseNoGI));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Total_FullStrip_Snapshot));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_Total));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_Direct));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_Diffuse));
            Fields.Add(Diag.bSkyOnlyDiffuseScalingUsed != 0 ? TEXT("total_nogi_closure") : TEXT("full_diffuse"));
            Fields.Add(FString::FromInt(Diag.bSkyOnlyReferenceUsable != 0 ? 1 : 0));
            Fields.Add(FString::FromInt(Diag.bSkyOnlyReferenceMeetsMinLuminance != 0 ? 1 : 0));
            Fields.Add(FString::FromInt(Diag.bSkyOnlyDiffuseScalingUsed != 0 ? 1 : 0));
            Fields.Add(SanitizeFloat(Diag.SkyOnlyReferenceRatio));
            Fields.Add(SanitizeFloat(Diag.SkyOnlyRTYRatio));
            Fields.Add(SanitizeFloat(Diag.RTY_Total));
            Fields.Add(SanitizeFloat(Diag.RTY_Direct));
            Fields.Add(SanitizeFloat(Diag.RTY_Diffuse));
            Fields.Add(SanitizeFloat(Diag.RTY_DiffuseNoGI));
            Fields.Add(SanitizeFloat(Diag.RTY_Terrain));
        }
        else
        {
            Fields.Reserve(96);

            Fields.Add(FString::FromInt(Diag.StepIndex));
            Fields.Add(FString::FromInt(Diag.CellIndex));
            Fields.Add(FString::SanitizeFloat(Diag.SimulationTimeSeconds));
            Fields.Add(Diag.Timestamp.ToIso8601());

            Fields.Add(SanitizeFloat(Diag.ForcingIncomingShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDirectShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDiffuseShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDirectShortwaveBase_Wm2));
            Fields.Add(SanitizeFloat(Diag.ForcingDiffuseShortwaveBase_Wm2));

            Fields.Add(SanitizeFloat(Diag.RadiationIndex));
            Fields.Add(SanitizeFloat(Diag.RadiationIndex_Direct));
            Fields.Add(SanitizeFloat(Diag.RadiationIndex_Diffuse));

            Fields.Add(SanitizeFloat(Diag.DirectShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.DiffuseShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.TerrainShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.TotalShortwave_Wm2));
            Fields.Add(SanitizeFloat(Diag.RadiationIndexImpact_Wm2));
            Fields.Add(SanitizeFloat(Diag.RadiationIndexImpactRatio));
            Fields.Add(SanitizeFloat(Diag.CalibratedSWROut_Wm2));
            Fields.Add(SanitizeFloat(Diag.SurfaceAlbedo));
            Fields.Add(SanitizeFloat(Diag.NeutralizationSurfaceAlbedo));
            Fields.Add(SanitizeFloat(Diag.NeutralizationReferenceAlbedo));
            Fields.Add(SanitizeFloat(Diag.NeutralizationSnowBlendWeight));
            Fields.Add(FString::FromInt(Diag.bNeutralizationUsedRenderSurfaceState != 0 ? 1 : 0));
            Fields.Add(SanitizeFloat(Diag.AlbedoNeutralizationRawFactor));
            Fields.Add(SanitizeFloat(Diag.AlbedoNeutralizationFactor));

            Fields.Add(FString::FromInt(Diag.bReferenceValid ? 1 : 0));
            Fields.Add(FString::FromInt(static_cast<int32>(Diag.ReferenceInvalidReasonCode)));
            Fields.Add(GetReferenceInvalidReasonText(Diag.ReferenceInvalidReasonCode));
            Fields.Add(FString::FromInt(Diag.bReferenceActorValidityFlag ? 1 : 0));
            Fields.Add(FString::FromInt(Diag.bReferenceActorValidityOverridden ? 1 : 0));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Total));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Direct));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Diffuse));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_DiffuseNoGI));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_TotalNoGI));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Total_FullStrip_Snapshot));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Direct_FullStrip_Snapshot));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_Diffuse_FullStrip_Snapshot));
            Fields.Add(SanitizeFloat(Diag.ReferenceLuminance_DiffuseNoGI_FullStrip_Snapshot));
            Fields.Add(FString::FromInt(bDualReferenceStripEnabled ? 1 : 0));
            Fields.Add(ReferenceSurfaceClass);
            Fields.Add(SanitizeFloat(Diag.ReferenceSelectionSnowDepth_m));
            Fields.Add(SanitizeFloat(ReferenceSelectionThreshold_m));
            Fields.Add(SanitizeFloat(Diag.ReferenceSelectionSnowCoverFraction));
            Fields.Add(SanitizeFloat(Diag.ReferenceSnowBlendWeight));
            Fields.Add(SanitizeFloat(Diag.ReferenceRenderSurfaceSnowBlendWeight));
            Fields.Add(FString::FromInt(Diag.bReferenceSelectionUsedRenderSurfaceState != 0 ? 1 : 0));
            Fields.Add(FString::FromInt(Diag.bReferenceSelectionPlausibilityOverride != 0 ? 1 : 0));
            Fields.Add(FString::FromInt(Diag.ReferenceSelectionSnowLayerCount));
            Fields.Add(FString::FromInt(Diag.SnowLayerCount));
            Fields.Add(SanitizeFloat(ReferenceLuminanceDirectGroundCurrent));
            Fields.Add(SanitizeFloat(ReferenceLuminanceDirectSnowCurrent));
            Fields.Add(SanitizeFloat(ReferenceSurfaceStateFullStrip));
            Fields.Add(SanitizeFloat(ReferenceSurfaceStateGroundCurrent));
            Fields.Add(SanitizeFloat(ReferenceSurfaceStateSnowCurrent));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_Total));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_Direct));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_Diffuse));
            Fields.Add(SanitizeFloat(Diag.ReferenceScale_TotalNoGI));
            Fields.Add(Diag.bSkyOnlyDiffuseScalingUsed != 0 ? TEXT("total_nogi_closure") : TEXT("full_diffuse"));
            Fields.Add(FString::FromInt(Diag.bSkyOnlyReferenceUsable != 0 ? 1 : 0));
            Fields.Add(FString::FromInt(Diag.bSkyOnlyReferenceMeetsMinLuminance != 0 ? 1 : 0));
            Fields.Add(FString::FromInt(Diag.bSkyOnlyDiffuseScalingUsed != 0 ? 1 : 0));
            Fields.Add(SanitizeFloat(Diag.SkyOnlyReferenceRatio));
            Fields.Add(SanitizeFloat(Diag.SkyOnlyRTYRatio));
            Fields.Add(SanitizeFloat(Diag.RTY_Total));
            Fields.Add(SanitizeFloat(Diag.RTY_Direct));
            Fields.Add(SanitizeFloat(Diag.RTY_Diffuse));
            Fields.Add(SanitizeFloat(Diag.RTY_DiffuseNoGI));
            Fields.Add(SanitizeFloat(Diag.RTY_TotalNoGI));
            Fields.Add(SanitizeFloat(Diag.RTY_SurfaceState));
            Fields.Add(SanitizeFloat(Diag.RTY_Terrain));
            Fields.Add(FormatRatio(ReferenceLuminanceTotalSelected, ReferenceLuminanceTotalFullStrip));
            Fields.Add(FormatRatio(ReferenceLuminanceDirectSelected, ReferenceLuminanceDirectFullStrip));
            Fields.Add(FormatRatio(ReferenceLuminanceDirectGroundCurrent, ReferenceLuminanceDirectSnowCurrent));
            Fields.Add(FormatRatio(ReferenceLuminanceDirectSnowCurrent, ReferenceLuminanceDirectGroundCurrent));
            Fields.Add(FormatRatio(Diag.RTY_Total, ReferenceLuminanceTotalSelected));
            Fields.Add(FormatRatio(Diag.RTY_Total, ReferenceLuminanceTotalFullStrip));
            Fields.Add(FormatRatio(Diag.RTY_Direct, ReferenceLuminanceDirectSelected));
            Fields.Add(FormatRatio(Diag.RTY_Direct, ReferenceLuminanceDirectFullStrip));
            Fields.Add(FormatRatio(Diag.RTY_Direct, ReferenceLuminanceDirectGroundCurrent));
            Fields.Add(FormatRatio(Diag.RTY_Direct, ReferenceLuminanceDirectSnowCurrent));
            Fields.Add(FormatScaledFlux(Diag.ForcingDirectShortwaveBase_Wm2, Diag.RTY_Direct, ReferenceLuminanceDirectFullStrip));
            Fields.Add(FormatScaledFlux(Diag.ForcingDirectShortwaveBase_Wm2, Diag.RTY_Direct, ReferenceLuminanceDirectGroundCurrent));
            Fields.Add(FormatScaledFlux(Diag.ForcingDirectShortwaveBase_Wm2, Diag.RTY_Direct, ReferenceLuminanceDirectSnowCurrent));
            Fields.Add(FormatScaledFlux(Diag.ForcingIncomingShortwave_Wm2, Diag.RTY_Total, ReferenceLuminanceTotalFullStrip));
        }

        Output += FString::Join(Fields, TEXT(","));
        Output.AppendChar(TEXT('\n'));
    }

    const uint32 WriteFlags = (bFileExists && Actor->bAppendRadiationDiagnostics) ? FILEWRITE_Append : FILEWRITE_None;
    const bool bSuccess = FFileHelper::SaveStringToFile(
        Output,
        *RadiationDiagnosticsFilePath,
        FFileHelper::EEncodingOptions::AutoDetect,
        &IFileManager::Get(),
        WriteFlags);

    if (!bSuccess)
    {
        UE_LOG(LogTemp, Error, TEXT("[FSM2] Failed to write radiation diagnostics: %s"), *RadiationDiagnosticsFilePath);
    }
}

void UFSM2SnowSimulation::WriteRadiationConfigFile(const FString& RunTag)
{
    ASnowSimulationActor* Actor = OwningSimulationActor.Get();
    if (!Actor || RadiationConfigFilePath.IsEmpty())
    {
        return;
    }

    FString Config;
    Config += TEXT("# FSM2 radiation diagnostics configuration\n");
    Config += FString::Printf(TEXT("# Generated: %s\n\n"), *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
    Config += FString::Printf(TEXT("RunTag=%s\n"), *RunTag);
    Config += FString::Printf(TEXT("bEnableRadiationCapture=%d\n"), Actor->bEnableRadiationCapture ? 1 : 0);
    Config += FString::Printf(TEXT("bEnableRadiationDiagnostics=%d\n"), Actor->bEnableRadiationDiagnostics ? 1 : 0);
    Config += FString::Printf(TEXT("bAppendRadiationDiagnostics=%d\n"), Actor->bAppendRadiationDiagnostics ? 1 : 0);
    Config += FString::Printf(TEXT("bWriteLeanPostRunBundleDiagnostics=%d\n"), ModelParameters.Diagnostics.bWriteLeanPostRunBundleDiagnostics ? 1 : 0);
    Config += FString::Printf(TEXT("bDirectCaptureIncludesAtmosphere=%d\n"), Actor->bDirectCaptureIncludesAtmosphere ? 1 : 0);
    Config += FString::Printf(TEXT("bUseDiffuseCapture=%d\n"), Actor->bUseDiffuseCapture ? 1 : 0);
    Config += FString::Printf(TEXT("bCaptureDiffuseNoGIForTerrain=%d\n"), Actor->bCaptureDiffuseNoGIForTerrain ? 1 : 0);
    Config += FString::Printf(TEXT("bUseDualReferenceStrip=%d\n"), Actor->bUseDualReferenceStrip ? 1 : 0);
    Config += FString::Printf(TEXT("ReferenceStripHeight=%d\n"), Actor->ReferenceStripHeight);
    Config += FString::Printf(TEXT("bEnableTimeIntegratedRadiation=%d\n"), Actor->bEnableTimeIntegratedRadiation ? 1 : 0);
    Config += FString::Printf(TEXT("bCenterTimeIntegrationWindow=%d\n"), Actor->bCenterTimeIntegrationWindow ? 1 : 0);
    Config += FString::Printf(TEXT("RadiationIntegrationSubstepMinutes=%d\n"), Actor->RadiationIntegrationSubstepMinutes);
    Config += FString::Printf(TEXT("bRecaptureSkyBeforeRadiationSample=%d\n"), Actor->bRecaptureSkyBeforeRadiationSample ? 1 : 0);
    Config += FString::Printf(TEXT("RadiationPrimingFrameCount=%d\n"), Actor->RadiationPrimingFrameCount);
    Config += FString::Printf(TEXT("RadiationCaptureEV100=%.6f\n"), Actor->RadiationCaptureEV100);
    Config += FString::Printf(TEXT("SkyLuminousEfficacy=%.6f\n"), Actor->SkyLuminousEfficacy);
    Config += FString::Printf(TEXT("SkyLightIntensityMultiplier=%.6f\n"), Actor->SkyLightIntensityMultiplier);
    Config += FString::Printf(TEXT("bLockCaptureLODOnce=%d\n"), Actor->bLockCaptureLODOnce ? 1 : 0);
    Config += FString::Printf(TEXT("CaptureLODDistanceFactor=%.6f\n"), Actor->CaptureLODDistanceFactor);
    Config += FString::Printf(TEXT("bForceLandscapeLOD0ForCapture=%d\n"), Actor->bForceLandscapeLOD0ForCapture ? 1 : 0);
    Config += FString::Printf(TEXT("bForceVHMLodForCapture=%d\n"), Actor->bForceVHMLodForCapture ? 1 : 0);
    Config += FString::Printf(TEXT("CaptureVHMLod0ScreenSize=%.6f\n"), Actor->CaptureVHMLod0ScreenSize);
    Config += FString::Printf(TEXT("CaptureVHMNumForceLoadLods=%d\n"), Actor->CaptureVHMNumForceLoadLods);
    Config += FString::Printf(TEXT("bEnableVHMViewLodFactorForCapture=%d\n"), Actor->bEnableVHMViewLodFactorForCapture ? 1 : 0);
    Config += FString::Printf(TEXT("bOverrideLumenCaptureCVars=%d\n"), Actor->bOverrideLumenCaptureCVars ? 1 : 0);
    Config += FString::Printf(TEXT("LumenCaptureTraceStepFactor=%.6f\n"), Actor->LumenCaptureTraceStepFactor);
    Config += FString::Printf(TEXT("LumenCaptureTraceDistanceScale=%.6f\n"), Actor->LumenCaptureTraceDistanceScale);
    Config += FString::Printf(TEXT("LumenCaptureDiffuseIndirectScale=%.6f\n"), Actor->LumenCaptureDiffuseIndirectScale);
    Config += FString::Printf(TEXT("bLumenCaptureForceMeshSDFs=%d\n"), Actor->bLumenCaptureForceMeshSDFs ? 1 : 0);
    Config += FString::Printf(TEXT("bCaptureUsePreExposure=%d\n"), Actor->bCaptureUsePreExposure ? 1 : 0);
    Config += FString::Printf(TEXT("bEnableLumenConvergence=%d\n"), Actor->bEnableLumenConvergence ? 1 : 0);
    Config += FString::Printf(TEXT("LumenConvergenceFrames=%d\n"), Actor->LumenConvergenceFrames);
    Config += FString::Printf(TEXT("LumenConvergenceHistoryWeight=%.6f\n"), Actor->LumenConvergenceHistoryWeight);
    Config += FString::Printf(TEXT("TerrainResidualMode=%d\n"), static_cast<int32>(Actor->TerrainResidualMode));
    Config += FString::Printf(TEXT("bBlurTerrainResidual=%d\n"), Actor->bBlurTerrainResidual ? 1 : 0);
    Config += FString::Printf(TEXT("TerrainRedistribution.bApplySlopeCurvatureRedistribution=%d\n"), Actor->TerrainRedistribution.bApplySlopeCurvatureRedistribution ? 1 : 0);
    Config += FString::Printf(TEXT("TerrainRedistribution.bUseDynamicSurfaceGeometry=%d\n"), Actor->TerrainRedistribution.bUseDynamicSurfaceGeometry ? 1 : 0);
    Config += FString::Printf(TEXT("TerrainRedistribution.MinSnowfallForRedistribution_mm=%.6f\n"), Actor->TerrainRedistribution.MinSnowfallForRedistribution_mm);
    Config += FString::Printf(TEXT("TerrainRedistribution.SlopeRedistributionStartDeg=%.6f\n"), Actor->TerrainRedistribution.SlopeRedistributionStartDeg);
    Config += FString::Printf(TEXT("TerrainRedistribution.SlopeRedistributionZeroDeg=%.6f\n"), Actor->TerrainRedistribution.SlopeRedistributionZeroDeg);
    Config += FString::Printf(TEXT("TerrainRedistribution.CurvatureRedistributionGain=%.6f\n"), Actor->TerrainRedistribution.CurvatureRedistributionGain);
    Config += FString::Printf(TEXT("TerrainRedistribution.RedistributionEdgeFadeCells=%d\n"), Actor->TerrainRedistribution.RedistributionEdgeFadeCells);
    Config += FString::Printf(TEXT("TerrainRedistribution.MinRedistributionFactor=%.6f\n"), Actor->TerrainRedistribution.MinRedistributionFactor);
    Config += FString::Printf(TEXT("TerrainRedistribution.MaxRedistributionFactor=%.6f\n"), Actor->TerrainRedistribution.MaxRedistributionFactor);
    Config += FString::Printf(TEXT("TerrainRedistribution.bConserveMassDuringRedistribution=%d\n"), Actor->TerrainRedistribution.bConserveMassDuringRedistribution ? 1 : 0);
    Config += FString::Printf(TEXT("TerrainRedistribution.bExcludeEdgeCellsFromMassConservation=%d\n"), Actor->TerrainRedistribution.bExcludeEdgeCellsFromMassConservation ? 1 : 0);
    Config += FString::Printf(TEXT("bNormalizeCurvatureByCellSize=%d\n"), Actor->bNormalizeCurvatureByCellSize ? 1 : 0);
    Config += FString::Printf(TEXT("CurvatureReferenceMeters=%.6f\n"), Actor->CurvatureReferenceMeters);
    Config += FString::Printf(TEXT("CurvatureClampAbs=%.6f\n"), Actor->CurvatureClampAbs);
    Config += FString::Printf(TEXT("CurvatureSmoothingRadiusMeters=%.6f\n"), Actor->CurvatureSmoothingRadiusMeters);
    Config += FString::Printf(TEXT("bUseUERadiationIndex=%d\n"), ModelParameters.Radiation.bUseUERadiationIndex ? 1 : 0);
    Config += FString::Printf(TEXT("bUseFluxCalibratedUERadiation=%d\n"), ModelParameters.Radiation.bUseFluxCalibratedUERadiation ? 1 : 0);
    Config += FString::Printf(TEXT("bUseTerrainInterreflection=%d\n"), ModelParameters.Radiation.bUseTerrainInterreflection ? 1 : 0);
    Config += FString::Printf(TEXT("bUseReferenceStripGuard=%d\n"), ModelParameters.Radiation.bUseReferenceStripGuard ? 1 : 0);
    Config += FString::Printf(TEXT("ReferenceStripMinSunElevation_deg=%.4f\n"), ModelParameters.Radiation.ReferenceStripMinSunElevation_deg);
    Config += FString::Printf(TEXT("ReferenceStripMinLuminance=%.6f\n"), ModelParameters.Radiation.ReferenceStripMinLuminance);
    Config += FString::Printf(TEXT("bUseDualReferencePlausibilityGuard=%d\n"), ModelParameters.Radiation.bUseDualReferencePlausibilityGuard ? 1 : 0);
    Config += FString::Printf(TEXT("DualReferenceMaxPlausibleDirectIndex=%.6f\n"), ModelParameters.Radiation.DualReferenceMaxPlausibleDirectIndex);
    Config += FString::Printf(TEXT("ReferenceStripMaxTotalScale=%.6f\n"), ModelParameters.Radiation.ReferenceStripMaxTotalScale);
    Config += FString::Printf(TEXT("bNeutralizeCaptureAlbedo=%d\n"), ModelParameters.Radiation.bNeutralizeCaptureAlbedo ? 1 : 0);
    Config += FString::Printf(TEXT("ReferenceStripAssumedAlbedo=%.6f\n"), ModelParameters.Radiation.ReferenceStripAssumedAlbedo);
    Config += FString::Printf(TEXT("ReferenceStripAssumedGroundAlbedo=%.6f\n"), ModelParameters.Radiation.ReferenceStripAssumedGroundAlbedo);
    Config += FString::Printf(TEXT("ReferenceStripAssumedSnowAlbedo=%.6f\n"), ModelParameters.Radiation.ReferenceStripAssumedSnowAlbedo);
    Config += FString::Printf(TEXT("MinSurfaceAlbedoForNeutralization=%.6f\n"), ModelParameters.Radiation.MinSurfaceAlbedoForNeutralization);
    Config += FString::Printf(TEXT("MaxAlbedoNeutralizationFactor=%.6f\n"), ModelParameters.Radiation.MaxAlbedoNeutralizationFactor);

    auto AppendRuntimeCVar = [&Config](const TCHAR* Key, const TCHAR* CVarName)
    {
        FString Value = TEXT("Unavailable");
        if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName))
        {
            Value = Var->GetString();
        }
        Config += FString::Printf(TEXT("%s=%s\n"), Key, *Value);
    };

    AppendRuntimeCVar(TEXT("RuntimeCVar_Lumen_DiffuseIndirect_TraceStepFactor"), TEXT("r.Lumen.DiffuseIndirect.TraceStepFactor"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_Lumen_TraceDistanceScale"), TEXT("r.Lumen.TraceDistanceScale"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_Lumen_TraceMeshSDFs"), TEXT("r.Lumen.TraceMeshSDFs"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_Lumen_ScreenProbeGather_TemporalFilter"), TEXT("r.Lumen.ScreenProbeGather.TemporalFilter"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_Lumen_ScreenProbeGather_HistoryWeight"), TEXT("r.Lumen.ScreenProbeGather.HistoryWeight"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_Lumen_DiffuseIndirect_TemporalFilter"), TEXT("r.Lumen.DiffuseIndirect.TemporalFilter"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_Lumen_DiffuseIndirect_AllowHistory"), TEXT("r.Lumen.DiffuseIndirect.AllowHistory"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_Lumen_Reflections_AllowHistory"), TEXT("r.Lumen.Reflections.AllowHistory"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_Lumen_Reflections_TemporalFilter"), TEXT("r.Lumen.Reflections.TemporalFilter"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_Lumen_ScreenProbeGather_FixedJitterIndex"), TEXT("r.Lumen.ScreenProbeGather.FixedJitterIndex"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_UsePreExposure"), TEXT("r.UsePreExposure"));
    AppendRuntimeCVar(TEXT("RuntimeCVar_VHM_EnableViewLodFactor"), TEXT("r.VHM.EnableViewLodFactor"));

    const bool bConfigSaved = FFileHelper::SaveStringToFile(Config, *RadiationConfigFilePath);
    if (!bConfigSaved)
    {
        UE_LOG(LogTemp, Error, TEXT("[FSM2] Failed to write radiation diagnostics config: %s"), *RadiationConfigFilePath);
    }
}

void UFSM2SnowSimulation::WriteParametersToFile(const FString& Timestamp)
{
    FString Output;
    Output += TEXT("=== FSM2 Model Parameters ===\n");
    Output += FString::Printf(TEXT("Timestamp: %s\n\n"), *Timestamp);
    ASnowSimulationActor* Actor = OwningSimulationActor.Get();

    Output += TEXT("[Schemes]\n");
    Output += FString::Printf(TEXT("GrainGrowthScheme=%d\n"), (int)ModelParameters.Snow.GrainGrowthScheme);
    Output += FString::Printf(TEXT("AlbedoScheme=%d\n"), (int)ModelParameters.Snow.AlbedoScheme);
    Output += FString::Printf(TEXT("DensityScheme=%d\n"), (int)ModelParameters.Snow.DensityScheme);
    Output += FString::Printf(TEXT("HydrologyScheme=%d\n"), (int)ModelParameters.Snow.HydrologyScheme);
    Output += FString::Printf(TEXT("ConductivityScheme=%d\n"), (int)ModelParameters.Snow.ConductivityScheme);
    Output += FString::Printf(TEXT("SnowCoverFractionScheme=%d\n"), (int)ModelParameters.Snow.SnowCoverFractionScheme);
    Output += TEXT("\n");

    Output += TEXT("[Ensemble]\n");
    Output += FString::Printf(TEXT("PrecipitationMultiplier=%.4f\n"), ModelParameters.Ensemble.PrecipitationMultiplier);
    Output += FString::Printf(TEXT("TemperatureOffset_K=%.4f\n"), ModelParameters.Ensemble.TemperatureOffset_K);
    Output += FString::Printf(TEXT("bDisableLapseRateAdjustments=%d\n"), ModelParameters.Ensemble.bDisableLapseRateAdjustments ? 1 : 0);
    Output += FString::Printf(TEXT("bApplyPrecipLapseBelowStation=%d\n"), ModelParameters.Ensemble.bApplyPrecipLapseBelowStation ? 1 : 0);
    Output += FString::Printf(TEXT("TemperatureLapseRate_CPer100m=%.4f\n"), ModelParameters.Ensemble.TemperatureLapseRate_CPer100m);
    Output += FString::Printf(TEXT("PrecipitationLapseRate_FractionPerKm=%.4f\n"), ModelParameters.Ensemble.PrecipitationLapseRate_FractionPerKm);
    Output += TEXT("\n");

    Output += TEXT("[Diagnostics]\n");
    Output += FString::Printf(TEXT("bEnableDiagnostics=%d\n"), ModelParameters.Diagnostics.bEnableDiagnostics ? 1 : 0);
    Output += FString::Printf(TEXT("DiagnosticsEveryNSteps=%d\n"), ModelParameters.Diagnostics.DiagnosticsEveryNSteps);
    Output += FString::Printf(TEXT("bAppendDiagnostics=%d\n"), ModelParameters.Diagnostics.bAppendDiagnostics ? 1 : 0);
    Output += FString::Printf(TEXT("bWriteLeanPostRunBundleDiagnostics=%d\n"), ModelParameters.Diagnostics.bWriteLeanPostRunBundleDiagnostics ? 1 : 0);
    Output += TEXT("\n");

    Output += TEXT("[Snow]\n");
    Output += FString::Printf(TEXT("FreshSnowDensity_kgm3=%.2f\n"), ModelParameters.Snow.FreshSnowDensity_kgm3);
    Output += FString::Printf(TEXT("FreshSnowGrainRadius_m=%.2e\n"), ModelParameters.Snow.FreshSnowGrainRadius_m);
    Output += FString::Printf(TEXT("MinimumSnowAlbedo=%.4f\n"), ModelParameters.Snow.MinimumSnowAlbedo);
    Output += FString::Printf(TEXT("MaximumSnowAlbedo=%.4f\n"), ModelParameters.Snow.MaximumSnowAlbedo);
    Output += FString::Printf(TEXT("ColdSnowAlbedoTimescale_s=%.2f\n"), ModelParameters.Snow.ColdSnowAlbedoTimescale_s);
    Output += FString::Printf(TEXT("MeltSnowAlbedoTimescale_s=%.2f\n"), ModelParameters.Snow.MeltSnowAlbedoTimescale_s);
    Output += FString::Printf(TEXT("AlbedoDecayTemperature_C=%.2f\n"), ModelParameters.Snow.AlbedoDecayTemperature_C);
    Output += FString::Printf(TEXT("MaxColdSnowDensity_kgm3=%.2f\n"), ModelParameters.Snow.MaxColdSnowDensity_kgm3);
    Output += FString::Printf(TEXT("MaxMeltSnowDensity_kgm3=%.2f\n"), ModelParameters.Snow.MaxMeltSnowDensity_kgm3);
    Output += FString::Printf(TEXT("CompactionTimescale_s=%.2f\n"), ModelParameters.Snow.CompactionTimescale_s);
    Output += FString::Printf(TEXT("IrreducibleWaterFraction=%.4f\n"), ModelParameters.Snow.IrreducibleWaterFraction);
    Output += FString::Printf(TEXT("ThermalMetamorphismRate_s=%.2e\n"), ModelParameters.Snow.ThermalMetamorphismRate_s);
    Output += FString::Printf(TEXT("SnowRoughnessLength_m=%.4f\n"), ModelParameters.Snow.SnowRoughnessLength_m);
    Output += FString::Printf(TEXT("MinimumSnowfallRate_mmph=%.4f\n"), ModelParameters.Snow.MinimumSnowfallRate_mmph);
    Output += FString::Printf(TEXT("bApplySlopeCurvatureRedistribution=%d\n"), ModelParameters.Snow.bApplySlopeCurvatureRedistribution ? 1 : 0);
    Output += FString::Printf(TEXT("bUseDynamicSurfaceGeometry=%d\n"), bUseDynamicSurfaceGeometry ? 1 : 0);
    Output += FString::Printf(TEXT("MinSnowfallForRedistribution_mm=%.4f\n"), ModelParameters.Snow.MinSnowfallForRedistribution_mm);
    Output += FString::Printf(TEXT("SlopeRedistributionStartDeg=%.2f\n"), ModelParameters.Snow.SlopeRedistributionStartDeg);
    Output += FString::Printf(TEXT("SlopeRedistributionZeroDeg=%.2f\n"), ModelParameters.Snow.SlopeRedistributionZeroDeg);
    Output += FString::Printf(TEXT("CurvatureRedistributionGain=%.4f\n"), ModelParameters.Snow.CurvatureRedistributionGain);
    Output += FString::Printf(TEXT("RedistributionEdgeFadeCells=%d\n"), ModelParameters.Snow.RedistributionEdgeFadeCells);
    Output += FString::Printf(TEXT("MinRedistributionFactor=%.4f\n"), ModelParameters.Snow.MinRedistributionFactor);
    Output += FString::Printf(TEXT("MaxRedistributionFactor=%.4f\n"), ModelParameters.Snow.MaxRedistributionFactor);
    Output += FString::Printf(TEXT("bConserveMassDuringRedistribution=%d\n"), ModelParameters.Snow.bConserveMassDuringRedistribution ? 1 : 0);
    Output += FString::Printf(TEXT("bExcludeEdgeCellsFromMassConservation=%d\n"), ModelParameters.Snow.bExcludeEdgeCellsFromMassConservation ? 1 : 0);
    Output += TEXT("\n");

    Output += TEXT("[Radiation]\n");
    Output += FString::Printf(TEXT("bUseSlopeAdjustedShortwave=%d\n"), ModelParameters.Radiation.bUseSlopeAdjustedShortwave);
    Output += FString::Printf(TEXT("bUseUERadiationIndex=%d\n"), ModelParameters.Radiation.bUseUERadiationIndex);
    Output += FString::Printf(TEXT("bUseFluxCalibratedUERadiation=%d\n"), ModelParameters.Radiation.bUseFluxCalibratedUERadiation);
    Output += FString::Printf(TEXT("bUseTerrainInterreflection=%d\n"), ModelParameters.Radiation.bUseTerrainInterreflection);
    Output += FString::Printf(TEXT("bUseReferenceStripGuard=%d\n"), ModelParameters.Radiation.bUseReferenceStripGuard);
    Output += FString::Printf(TEXT("ReferenceStripMinSunElevation_deg=%.2f\n"), ModelParameters.Radiation.ReferenceStripMinSunElevation_deg);
    Output += FString::Printf(TEXT("ReferenceStripMinLuminance=%.4f\n"), ModelParameters.Radiation.ReferenceStripMinLuminance);
    Output += FString::Printf(TEXT("bUseDualReferencePlausibilityGuard=%d\n"), ModelParameters.Radiation.bUseDualReferencePlausibilityGuard ? 1 : 0);
    Output += FString::Printf(TEXT("DualReferenceMaxPlausibleDirectIndex=%.4f\n"), ModelParameters.Radiation.DualReferenceMaxPlausibleDirectIndex);
    Output += FString::Printf(TEXT("ReferenceStripMaxTotalScale=%.4f\n"), ModelParameters.Radiation.ReferenceStripMaxTotalScale);
    Output += FString::Printf(TEXT("bNeutralizeCaptureAlbedo=%d\n"), ModelParameters.Radiation.bNeutralizeCaptureAlbedo ? 1 : 0);
    Output += FString::Printf(TEXT("ReferenceStripAssumedAlbedo=%.4f\n"), ModelParameters.Radiation.ReferenceStripAssumedAlbedo);
    Output += FString::Printf(TEXT("ReferenceStripAssumedGroundAlbedo=%.4f\n"), ModelParameters.Radiation.ReferenceStripAssumedGroundAlbedo);
    Output += FString::Printf(TEXT("ReferenceStripAssumedSnowAlbedo=%.4f\n"), ModelParameters.Radiation.ReferenceStripAssumedSnowAlbedo);
    Output += FString::Printf(TEXT("MinSurfaceAlbedoForNeutralization=%.4f\n"), ModelParameters.Radiation.MinSurfaceAlbedoForNeutralization);
    Output += FString::Printf(TEXT("MaxAlbedoNeutralizationFactor=%.4f\n"), ModelParameters.Radiation.MaxAlbedoNeutralizationFactor);
    Output += FString::Printf(TEXT("GroundAlbedo=%.4f\n"), ModelParameters.Radiation.GroundAlbedo);
    Output += TEXT("\n");

    if (Actor)
    {
        Output += TEXT("[ActorTerrainRedistribution]\n");
        Output += FString::Printf(TEXT("bApplySlopeCurvatureRedistribution=%d\n"), Actor->TerrainRedistribution.bApplySlopeCurvatureRedistribution ? 1 : 0);
        Output += FString::Printf(TEXT("bUseDynamicSurfaceGeometry=%d\n"), Actor->TerrainRedistribution.bUseDynamicSurfaceGeometry ? 1 : 0);
        Output += FString::Printf(TEXT("MinSnowfallForRedistribution_mm=%.4f\n"), Actor->TerrainRedistribution.MinSnowfallForRedistribution_mm);
        Output += FString::Printf(TEXT("SlopeRedistributionStartDeg=%.4f\n"), Actor->TerrainRedistribution.SlopeRedistributionStartDeg);
        Output += FString::Printf(TEXT("SlopeRedistributionZeroDeg=%.4f\n"), Actor->TerrainRedistribution.SlopeRedistributionZeroDeg);
        Output += FString::Printf(TEXT("CurvatureRedistributionGain=%.4f\n"), Actor->TerrainRedistribution.CurvatureRedistributionGain);
        Output += FString::Printf(TEXT("RedistributionEdgeFadeCells=%d\n"), Actor->TerrainRedistribution.RedistributionEdgeFadeCells);
        Output += FString::Printf(TEXT("MinRedistributionFactor=%.4f\n"), Actor->TerrainRedistribution.MinRedistributionFactor);
        Output += FString::Printf(TEXT("MaxRedistributionFactor=%.4f\n"), Actor->TerrainRedistribution.MaxRedistributionFactor);
        Output += FString::Printf(TEXT("bConserveMassDuringRedistribution=%d\n"), Actor->TerrainRedistribution.bConserveMassDuringRedistribution ? 1 : 0);
        Output += FString::Printf(TEXT("bExcludeEdgeCellsFromMassConservation=%d\n"), Actor->TerrainRedistribution.bExcludeEdgeCellsFromMassConservation ? 1 : 0);
        Output += FString::Printf(TEXT("bNormalizeCurvatureByCellSize=%d\n"), Actor->bNormalizeCurvatureByCellSize ? 1 : 0);
        Output += FString::Printf(TEXT("CurvatureReferenceMeters=%.4f\n"), Actor->CurvatureReferenceMeters);
        Output += FString::Printf(TEXT("CurvatureClampAbs=%.4f\n"), Actor->CurvatureClampAbs);
        Output += FString::Printf(TEXT("CurvatureSmoothingRadiusMeters=%.4f\n"), Actor->CurvatureSmoothingRadiusMeters);
        Output += TEXT("\n");

        Output += TEXT("[ActorRadiationCapture]\n");
        Output += FString::Printf(TEXT("bEnableRadiationCapture=%d\n"), Actor->bEnableRadiationCapture ? 1 : 0);
        Output += FString::Printf(TEXT("bDirectCaptureIncludesAtmosphere=%d\n"), Actor->bDirectCaptureIncludesAtmosphere ? 1 : 0);
        Output += FString::Printf(TEXT("bUseDiffuseCapture=%d\n"), Actor->bUseDiffuseCapture ? 1 : 0);
        Output += FString::Printf(TEXT("bCaptureDiffuseNoGIForTerrain=%d\n"), Actor->bCaptureDiffuseNoGIForTerrain ? 1 : 0);
        Output += FString::Printf(TEXT("bUseDualReferenceStrip=%d\n"), Actor->bUseDualReferenceStrip ? 1 : 0);
        Output += FString::Printf(TEXT("ReferenceStripHeight=%d\n"), Actor->ReferenceStripHeight);
        Output += FString::Printf(TEXT("RadiationPrimingFrameCount=%d\n"), Actor->RadiationPrimingFrameCount);
        Output += FString::Printf(TEXT("RadiationCaptureEV100=%.4f\n"), Actor->RadiationCaptureEV100);
        Output += FString::Printf(TEXT("SkyLuminousEfficacy=%.4f\n"), Actor->SkyLuminousEfficacy);
        Output += FString::Printf(TEXT("SkyLightIntensityMultiplier=%.4f\n"), Actor->SkyLightIntensityMultiplier);
        Output += FString::Printf(TEXT("bEnableTimeIntegratedRadiation=%d\n"), Actor->bEnableTimeIntegratedRadiation ? 1 : 0);
        Output += FString::Printf(TEXT("bCenterTimeIntegrationWindow=%d\n"), Actor->bCenterTimeIntegrationWindow ? 1 : 0);
        Output += FString::Printf(TEXT("RadiationIntegrationSubstepMinutes=%d\n"), Actor->RadiationIntegrationSubstepMinutes);
        Output += FString::Printf(TEXT("bRecaptureSkyBeforeRadiationSample=%d\n"), Actor->bRecaptureSkyBeforeRadiationSample ? 1 : 0);
        Output += FString::Printf(TEXT("bLockCaptureLODOnce=%d\n"), Actor->bLockCaptureLODOnce ? 1 : 0);
        Output += FString::Printf(TEXT("CaptureLODDistanceFactor=%.4f\n"), Actor->CaptureLODDistanceFactor);
        Output += FString::Printf(TEXT("bForceLandscapeLOD0ForCapture=%d\n"), Actor->bForceLandscapeLOD0ForCapture ? 1 : 0);
        Output += FString::Printf(TEXT("bForceVHMLodForCapture=%d\n"), Actor->bForceVHMLodForCapture ? 1 : 0);
        Output += FString::Printf(TEXT("CaptureVHMLod0ScreenSize=%.4f\n"), Actor->CaptureVHMLod0ScreenSize);
        Output += FString::Printf(TEXT("CaptureVHMNumForceLoadLods=%d\n"), Actor->CaptureVHMNumForceLoadLods);
        Output += FString::Printf(TEXT("bEnableVHMViewLodFactorForCapture=%d\n"), Actor->bEnableVHMViewLodFactorForCapture ? 1 : 0);
        Output += FString::Printf(TEXT("bOverrideLumenCaptureCVars=%d\n"), Actor->bOverrideLumenCaptureCVars ? 1 : 0);
        Output += FString::Printf(TEXT("LumenCaptureTraceStepFactor=%.4f\n"), Actor->LumenCaptureTraceStepFactor);
        Output += FString::Printf(TEXT("LumenCaptureTraceDistanceScale=%.4f\n"), Actor->LumenCaptureTraceDistanceScale);
        Output += FString::Printf(TEXT("LumenCaptureDiffuseIndirectScale=%.4f\n"), Actor->LumenCaptureDiffuseIndirectScale);
        Output += FString::Printf(TEXT("bLumenCaptureForceMeshSDFs=%d\n"), Actor->bLumenCaptureForceMeshSDFs ? 1 : 0);
        Output += FString::Printf(TEXT("bCaptureUsePreExposure=%d\n"), Actor->bCaptureUsePreExposure ? 1 : 0);
        Output += FString::Printf(TEXT("bEnableLumenConvergence=%d\n"), Actor->bEnableLumenConvergence ? 1 : 0);
        Output += FString::Printf(TEXT("LumenConvergenceFrames=%d\n"), Actor->LumenConvergenceFrames);
        Output += FString::Printf(TEXT("LumenConvergenceHistoryWeight=%.4f\n"), Actor->LumenConvergenceHistoryWeight);
        Output += FString::Printf(TEXT("TerrainResidualMode=%d\n"), static_cast<int32>(Actor->TerrainResidualMode));
        Output += FString::Printf(TEXT("bBlurTerrainResidual=%d\n"), Actor->bBlurTerrainResidual ? 1 : 0);
        Output += TEXT("\n");
    }

    Output += TEXT("[Atmosphere]\n");
    Output += FString::Printf(TEXT("SurfacePressure_Pa=%.2f\n"), ModelParameters.Atmosphere.SurfacePressure_Pa);
    Output += FString::Printf(TEXT("BulkTransferCoefficient=%.6f\n"), ModelParameters.Atmosphere.BulkTransferCoefficient);
    Output += FString::Printf(TEXT("WindMeasurementHeight_m=%.2f\n"), ModelParameters.Atmosphere.WindMeasurementHeight_m);
    Output += FString::Printf(TEXT("TemperatureMeasurementHeight_m=%.2f\n"), ModelParameters.Atmosphere.TemperatureMeasurementHeight_m);
    Output += TEXT("\n");

    Output += TEXT("[Soil]\n");
    Output += FString::Printf(TEXT("GroundConductivity_WmK=%.4f\n"), ModelParameters.Soil.GroundConductivity_WmK);
    Output += FString::Printf(TEXT("GroundTemperature_K=%.2f\n"), ModelParameters.Soil.GroundTemperature_K);
    Output += FString::Printf(TEXT("ClayFraction=%.4f\n"), ModelParameters.Soil.ClayFraction);
    Output += FString::Printf(TEXT("SandFraction=%.4f\n"), ModelParameters.Soil.SandFraction);
    Output += TEXT("\n");

    Output += TEXT("[Layers]\n");
    Output += FString::Printf(TEXT("MaxSnowLayers=%d\n"), ModelParameters.Layers.MaxSnowLayers);
    Output += FString::Printf(TEXT("SoilLayerCount=%d\n"), ModelParameters.Layers.SoilLayerCount);
    
    FString SoilThicknesses;
    for(float Thickness : ModelParameters.Layers.SoilLayerThicknesses_m)
    {
        SoilThicknesses += FString::Printf(TEXT("%.3f,"), Thickness);
    }
    SoilThicknesses.RemoveFromEnd(TEXT(","));
    Output += FString::Printf(TEXT("SoilLayerThicknesses_m=[%s]\n"), *SoilThicknesses);
    Output += TEXT("\n");

    // Save to file
    FString DiagnosticsDir = Actor
        ? Actor->GetOutputCategoryDirectory(TEXT("Diagnostics"))
        : (FPaths::ProjectDir() / ModelParameters.Diagnostics.DiagnosticsDirectory);
    IFileManager::Get().MakeDirectory(*DiagnosticsDir, true);
    FString ParamsFilePath = DiagnosticsDir / FString::Printf(TEXT("FSM2Parameters_%s.txt"), *Timestamp);
    
    const bool bSaved = FFileHelper::SaveStringToFile(Output, *ParamsFilePath);
    if (!bSaved)
    {
        UE_LOG(LogTemp, Error, TEXT("[FSM2] Failed to write parameters file: %s"), *ParamsFilePath);
    }
}

void UFSM2SnowSimulation::SolveTridiagonal(int32 Count, double* a, double* b, double* c, double* rhs, double* x) const
{
    if (Count <= 0 || !a || !b || !c || !rhs || !x)
    {
        return;
    }

    // Match Fortran TRIDIAG.F90 structure (beta + gamma workspace).
    TArray<double, TInlineAllocator<GFSM2MaxLayers + GFSM2MaxSoilLayers>> Gamma;
    Gamma.SetNumZeroed(Count);

    auto SafePivot = [](double Pivot) -> double
    {
        constexpr double PivotEps = 1.0e-12;
        if (FMath::Abs(Pivot) >= PivotEps)
        {
            return Pivot;
        }
        return (Pivot >= 0.0) ? PivotEps : -PivotEps;
    };

    double Beta = SafePivot(b[0]);
    x[0] = rhs[0] / Beta;

    for (int32 i = 1; i < Count; ++i)
    {
        Gamma[i] = c[i - 1] / Beta;
        Beta = SafePivot(b[i] - a[i] * Gamma[i]);
        x[i] = (rhs[i] - a[i] * x[i - 1]) / Beta;
    }

    for (int32 i = Count - 2; i >= 0; --i)
    {
        x[i] -= Gamma[i + 1] * x[i + 1];
    }
}

void UFSM2SnowSimulation::PartitionShortwave(const FWeatherForcingData& Forcing, float IncomingSW, float& OutDiffuse, float& OutDirect) const
{
    OutDiffuse = IncomingSW;
    OutDirect = 0.0f;

    if (ModelParameters.Atmosphere.ShortwavePartition != EFSM2ShortwavePartition::DiffuseAndDirect || IncomingSW <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    // Fortran-faithful SWPART=1 partitioning from FSM2_DRIVE.F90.
    const float LatitudeDeg = bHasEnvironmentalOverride ? CachedLatitudeDeg : ModelParameters.Atmosphere.LatitudeDegrees;
    const float LatitudeRad = FMath::DegreesToRadians(LatitudeDeg);
    const int32 DayOfYear = Forcing.Timestamp.GetDayOfYear();
    const float HourLocal = Forcing.Timestamp.GetHour()
        + Forcing.Timestamp.GetMinute() / 60.0f
        + Forcing.Timestamp.GetSecond() / 3600.0f;
    const float SolarNoon = bHasEnvironmentalOverride ? CachedSolarNoon : ModelParameters.Atmosphere.SolarNoonLocalTime;

    const float DayAngle = 2.0f * PI * static_cast<float>(DayOfYear - 1) / 365.0f;
    const float Declination = 0.006918f
        - 0.399912f * FMath::Cos(DayAngle)
        + 0.070257f * FMath::Sin(DayAngle)
        - 0.006758f * FMath::Cos(2.0f * DayAngle)
        + 0.000907f * FMath::Sin(2.0f * DayAngle)
        - 0.002697f * FMath::Cos(3.0f * DayAngle)
        + 0.001480f * FMath::Sin(3.0f * DayAngle);

    const float EquationOfTimeHours = (0.000075f
        + 0.001868f * FMath::Cos(DayAngle)
        - 0.032077f * FMath::Sin(DayAngle)
        - 0.014615f * FMath::Cos(2.0f * DayAngle)
        - 0.04089f * FMath::Sin(2.0f * DayAngle)) * (12.0f / PI);

    const float HourAngle = (PI / 12.0f) * (SolarNoon - HourLocal - EquationOfTimeHours);
    const float Elevation = FMath::Asin(
        FMath::Sin(Declination) * FMath::Sin(LatitudeRad)
        + FMath::Cos(Declination) * FMath::Cos(LatitudeRad) * FMath::Cos(HourAngle));

    const float SolarConstant = ModelParameters.PhysicalConstants.SolarConstant_Wm2;
    float Kt = 0.0f;
    if (Elevation > 0.0f)
    {
        const float SinElev = FMath::Sin(Elevation);
        if (SinElev > KINDA_SMALL_NUMBER)
        {
            Kt = IncomingSW / (SolarConstant * SinElev);
        }
    }

    float DiffuseFraction = 1.0f - 0.09f * Kt;
    if (Kt > 0.22f)
    {
        DiffuseFraction = 0.95f
            - 0.16f * Kt
            + 4.39f * FMath::Square(Kt)
            - 16.64f * FMath::Pow(Kt, 3.0f)
            + 12.34f * FMath::Pow(Kt, 4.0f);
    }
    if (Kt > 0.8f)
    {
        DiffuseFraction = 0.165f;
    }

    if (!FMath::IsFinite(DiffuseFraction))
    {
        DiffuseFraction = 1.0f;
    }
    DiffuseFraction = FMath::Clamp(DiffuseFraction, 0.0f, 1.0f);

    OutDiffuse = DiffuseFraction * IncomingSW;
    OutDirect = FMath::Max(0.0f, IncomingSW - OutDiffuse);
}

float UFSM2SnowSimulation::UpdateSnowAlbedo(FFSM2ColumnState& Column, float DtSeconds, float SurfaceTemperatureK, float SnowfallRate_kgm2s) const
{
    // Mirror FSM2 Fortran (SWRAD.F90, ALBEDO=2): albs = alim + (albs - alim)*exp(-(1/tdec + Sf/Salb)*dt)
    // where tdec = tcld or tmlt depending on Tsrf >= Tm, and Sf is snowfall rate (kg/m2/s).
    const float Fresh = FMath::Clamp(ModelParameters.Radiation.FreshSnowAlbedo, 0.0f, 0.98f);
    const float Old = FMath::Clamp(ModelParameters.Radiation.OldSnowAlbedo, 0.0f, Fresh);
    const float MinAlbedo = FMath::Clamp(ModelParameters.Snow.MinimumSnowAlbedo, 0.0f, Fresh);
    const float MaxAlbedo = FMath::Clamp(ModelParameters.Snow.MaximumSnowAlbedo, MinAlbedo, 0.98f);

    float& Albedo = Column.SnowAlbedo;

    if (Column.Snow.LayerCount == 0)
    {
        Albedo = Old;
        return Albedo;
    }

    switch (ModelParameters.Snow.AlbedoScheme)
    {
    case EFSM2AlbedoScheme::DiagnosticTemperature:
    {
        const float Talb_K = ModelParameters.PhysicalConstants.MeltingPoint_K + ModelParameters.Snow.AlbedoDecayTemperature_C;
        const float Ratio = FMath::Clamp((SurfaceTemperatureK - ModelParameters.PhysicalConstants.MeltingPoint_K) / FMath::Max(Talb_K - ModelParameters.PhysicalConstants.MeltingPoint_K, 1.0f), 0.0f, 1.0f);
        Albedo = FMath::Lerp(MaxAlbedo, MinAlbedo, Ratio);
        break;
    }

    case EFSM2AlbedoScheme::PrognosticAge:
    {
        const float Tm = ModelParameters.PhysicalConstants.MeltingPoint_K;
        const float tdec = FMath::Max((SurfaceTemperatureK >= Tm) ? ModelParameters.Snow.MeltSnowAlbedoTimescale_s
                                                                  : ModelParameters.Snow.ColdSnowAlbedoTimescale_s,
                                      1.0f);
        const float Salb = FMath::Max(ModelParameters.Snow.SnowfallRefreshAlbedo_kgm2, KINDA_SMALL_NUMBER);
        const float Sf = FMath::Max(SnowfallRate_kgm2s, 0.0f);
        const float DecayRate = (1.0f / tdec) + (Sf / Salb);
        const float alim = (MinAlbedo / tdec + MaxAlbedo * (Sf / Salb)) / DecayRate;
        const float Decay = (DtSeconds > 0.0f) ? FMath::Exp(-DecayRate * DtSeconds) : 1.0f;
        Albedo = alim + (Albedo - alim) * Decay;
        break;
    }

    default:
        Albedo = Old;
        break;
    }

    Albedo = FMath::Clamp(Albedo, MinAlbedo, MaxAlbedo);
    return Albedo;
}

float UFSM2SnowSimulation::ComputeSnowCoverFraction(const FFSM2SnowColumn& Snow) const
{
    float Depth = 0.0f;
    for (int32 LayerIdx = 0; LayerIdx < Snow.LayerCount; ++LayerIdx)
    {
        Depth += Snow.Thickness_m[LayerIdx];
    }

    const float Scale = FMath::Max(ModelParameters.Snow.SnowCoverFractionDepthScale_m, KINDA_SMALL_NUMBER);

    switch (ModelParameters.Snow.SnowCoverFractionScheme)
    {
    case EFSM2SnowFractionScheme::Linear:
        return FMath::Clamp(Depth / Scale, 0.0f, 1.0f);

    case EFSM2SnowFractionScheme::HyperbolicTangent:
        return FMath::Clamp(FMath::Tanh(Depth / Scale), 0.0f, 1.0f);

    case EFSM2SnowFractionScheme::Asymptotic:
        return Depth / (Depth + Scale);

    default:
        return FMath::Clamp(Depth / Scale, 0.0f, 1.0f);
    }
}

float UFSM2SnowSimulation::ComputeSurfaceAlbedo(const FFSM2ColumnState& Column, float SnowAlbedo, float SnowCoverFraction) const
{
    const float GroundAlbedo = FMath::Clamp(ModelParameters.Radiation.GroundAlbedo, 0.0f, 1.0f);
    const float Weighted = GroundAlbedo * (1.0f - SnowCoverFraction) + SnowAlbedo * SnowCoverFraction;
    return FMath::Clamp(Weighted, 0.0f, 1.0f);
}

void UFSM2SnowSimulation::AdjustMeasurementHeights(float& WindHeight, float& TempHeight) const
{
    if (ModelParameters.Atmosphere.MeasurementHeightReference == EFSM2HeightReference::AboveCanopyTop && ModelParameters.Vegetation.CanopyHeight_m > 0.0f)
    {
        const float Offset = ModelParameters.Vegetation.CanopyHeight_m;
        WindHeight += Offset;
        TempHeight += Offset;
    }
}

float UFSM2SnowSimulation::ResolveBulkTransferCoefficient(float WindHeight, float TempHeight) const
{
    const float Reference = FMath::Max(ModelParameters.Atmosphere.BulkTransferCoefficient, 1.0e-5f);
    const float RefWind = FMath::Max(ModelParameters.Atmosphere.WindMeasurementHeight_m, 0.1f);
    const float RefTemp = FMath::Max(ModelParameters.Atmosphere.TemperatureMeasurementHeight_m, 0.1f);
    const float EffectiveWind = FMath::Max(WindHeight, 0.1f);
    const float EffectiveTemp = FMath::Max(TempHeight, 0.1f);

    const float HeightFactor = FMath::Sqrt((RefWind * RefTemp) / FMath::Max(EffectiveWind * EffectiveTemp, 0.01f));
    return FMath::Clamp(Reference * HeightFactor, 1.0e-5f, 5.0e-3f);
}
