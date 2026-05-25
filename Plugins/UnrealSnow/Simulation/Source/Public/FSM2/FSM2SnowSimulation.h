#pragma once

#include "SnowSimulation.h"
#include "FSM2Parameters.h"
#include "Containers/StaticArray.h"
#include "Misc/DateTime.h"
#include "FSM2SnowSimulation.generated.h"

struct FDebugCell;

static constexpr int32 GFSM2MaxLayers = 3;
static constexpr int32 GFSM2MaxSoilLayers = 4;

struct FFSM2SnowColumn
{
    int32 LayerCount = 0;
    TStaticArray<float, GFSM2MaxLayers> Thickness_m;
    TStaticArray<float, GFSM2MaxLayers> Temperature_K;
    TStaticArray<float, GFSM2MaxLayers> IceMass_kgm2;
    TStaticArray<float, GFSM2MaxLayers> LiquidMass_kgm2;
    TStaticArray<float, GFSM2MaxLayers> Density_kgm3;
    TStaticArray<float, GFSM2MaxLayers> GrainRadius_m;
    TStaticArray<float, GFSM2MaxLayers> AgeHours;

    void Reset();
};

struct FFSM2SoilColumn
{
    int32 LayerCount = 0;
    TStaticArray<float, GFSM2MaxSoilLayers> Thickness_m;
    TStaticArray<float, GFSM2MaxSoilLayers> Temperature_K;
    TStaticArray<float, GFSM2MaxSoilLayers> Moisture_VolumeFraction;

    void Reset();
};

struct FFSM2CanopyState
{
    float SnowStorage_kgm2 = 0.0f;
    float LiquidStorage_kgm2 = 0.0f;
    float Temperature_K = 273.15f;
    float SpecificHumidity = 0.0f;

    void Reset();
};

struct FFSM2ColumnState
{
    FFSM2SnowColumn Snow;
    FFSM2SoilColumn Soil;
    FFSM2CanopyState Canopy;
    float Runoff_kgm2 = 0.0f;
    float SnowAlbedo = 0.0f;
    float SurfaceAlbedo = 0.0f;
    float SnowCoverFraction = 0.0f;
    float DiffuseShortwave_Wm2 = 0.0f;
    float DirectShortwave_Wm2 = 0.0f;
    float TerrainShortwave_Wm2 = 0.0f;
    float SurfaceTemperature_K = 285.0f;

    void Reset();
};

UCLASS(Blueprintable, BlueprintType, EditInlineNew)
class SIMULATION_API UFSM2SnowSimulation : public USnowSimulation
{
    GENERATED_BODY()

public:
    UFSM2SnowSimulation();

    virtual void PostLoad() override;

    virtual FString GetSimulationName() const override;

    virtual void Initialize(ASnowSimulationActor* SimulationActor, const TArray<FLandscapeCell>& Cells, float InitialMaxSnow, UWorld* World) override;

    virtual void Initialize_Implementation(int32 GX, int32 GY, float CellMeters) override;

    virtual void Simulate(ASnowSimulationActor* SimulationActor, int32 CurrentSimulationStep, int32 Timesteps, bool SaveSnowMap, bool CaptureDebugInformation, TArray<FDebugCell>& DebugCells) override;

    virtual void Step(float DtSeconds, const FWeatherForcingData& Forcing, TArray<float>& OutDepthMeters) override;

    virtual float GetMaxSnow() override;

    virtual void RenderDebug(UWorld* World, int /*CellDebugInfoDisplayDistance*/, EDebugVisualizationType /*VisualizationType*/) override {}

    /** Public accessor for snow albedo state array (for material texture binding) */
    virtual TArray<float> GetCellSnowAlbedoState() const override;

    /** Public accessor for surface albedo state array (includes land albedo where no snow) */
    TArray<float> GetCellSurfaceAlbedoState() const;

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Configuration")
    FFSM2ModelParameters ModelParameters;

    /** Internal mirror of the actor-level dynamic surface geometry toggle. */
    UPROPERTY()
    bool bUseDynamicSurfaceGeometry = false;

    /** Enable verbose per-step/per-cell UE_LOG inside hot simulation loops. Off by default for sim speed. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FSM2|Diagnostics", meta=(AdvancedDisplay))
    bool bEnableHotPathLogs = false;

    /** Public access to cell states for debug visualization and analysis */
    TArray<FFSM2ColumnState> CellStates;

    /** Energy flux data structure for diagnostics */
    struct FFSM2EnergyFluxes
    {
        float NetShortwave = 0.0f;
        float NetLongwave = 0.0f;
        float Sensible = 0.0f;
        float Latent = 0.0f;
        float Rain = 0.0f;
        float ShortwaveUpwelling = 0.0f;  // rsus: Surface upwelling shortwave radiation (W/m²)
        float LongwaveUpwelling = 0.0f;   // rlus: Surface upwelling longwave radiation (W/m²)

        float Total() const
        {
            // Energy balance (Fortran FSM2 convention):
            // - NetShortwave/NetLongwave are positive when directed toward the surface.
            // - Sensible/Latent are positive UPWARD (cooling the surface).
            // Net energy into the surface subtracts turbulent losses.
            return NetShortwave + NetLongwave - Sensible - Latent + Rain;
        }
    };

    struct FFSM2CellDiagnostics
    {
        int32 StepIndex = 0;
        int32 CellIndex = -1;
        double SimulationTimeSeconds = 0.0;
        FDateTime Timestamp;
        float SnowDepth_m = 0.0f;
        float SnowMass_kgm2 = 0.0f;
        float Runoff_kgm2 = 0.0f;
        float ThroughfallSnow_kgm2 = 0.0f;
        float ThroughfallRain_kgm2 = 0.0f;
        float FrostMass_kgm2 = 0.0f;
        float MeltMass_kgm2 = 0.0f;
        float RefreezeMass_kgm2 = 0.0f;
        float SublimationMass_kgm2 = 0.0f;
        float CanopySnow_kgm2 = 0.0f;
        float CanopyLiquid_kgm2 = 0.0f;
        float SurfaceTemperatureK = 0.0f;
        float NetSurfaceFlux_Wm2 = 0.0f;
        float GroundHeatFlux_Wm2 = 0.0f;
        float ForcingAirTemperatureK = 0.0f;
        float ForcingIncomingLongwave_Wm2 = 0.0f;
        float ForcingIncomingShortwave_Wm2 = 0.0f;
        float ForcingDiffuseShortwave_Wm2 = 0.0f;
        float ForcingDirectShortwave_Wm2 = 0.0f;
        float ForcingDiffuseShortwaveBase_Wm2 = 0.0f;
        float ForcingDirectShortwaveBase_Wm2 = 0.0f;
        float ForcingWindSpeed_mps = 0.0f;
        float SurfaceMoistureConductance_ms = 0.0f;
        float SurfaceMoistureAvailability = 0.0f;
        float SnowAlbedo = 0.0f;
        float SurfaceAlbedo = 0.0f;
        float SnowCoverFraction = 0.0f;
        float DiffuseShortwave_Wm2 = 0.0f;
        float DirectShortwave_Wm2 = 0.0f;
        float TerrainShortwave_Wm2 = 0.0f;
        float TotalShortwave_Wm2 = 0.0f;
        float RadiationIndex = 1.0f;  // UE radiation correction factor
        float RadiationIndex_Direct = 0.0f;
        float RadiationIndex_Diffuse = 0.0f;
        float RadiationComponent = 0.0f; // Diagnostic shortwave term used by FSM2 (W/m²)
        float RadiationIndexImpact_Wm2 = 0.0f;
        float RadiationIndexImpactRatio = 1.0f;
        float CalibratedSWROut_Wm2 = 0.0f;
        float NeutralizationSurfaceAlbedo = 0.0f;
        float NeutralizationReferenceAlbedo = 0.0f;
        float NeutralizationSnowBlendWeight = 0.0f;
        float AlbedoNeutralizationRawFactor = 1.0f;
        float AlbedoNeutralizationFactor = 1.0f;
        float ReferenceScale_Total = 0.0f;
        float ReferenceScale_Direct = 0.0f;
        float ReferenceScale_Diffuse = 0.0f;
        float ReferenceScale_TotalNoGI = 0.0f;
        uint8 bSkyOnlyReferenceUsable = 0;
        uint8 bSkyOnlyReferenceMeetsMinLuminance = 0;
        uint8 bSkyOnlyDiffuseScalingUsed = 0;
        float SkyOnlyReferenceRatio = 0.0f;
        float SkyOnlyRTYRatio = 0.0f;
        float ReferenceLuminance_Total = 0.0f;
        float ReferenceLuminance_Direct = 0.0f;
        float ReferenceLuminance_Diffuse = 0.0f;
        float ReferenceLuminance_DiffuseNoGI = 0.0f;
        float ReferenceLuminance_TotalNoGI = 0.0f;
        float ReferenceLuminance_SurfaceState_FullStrip_Snapshot = 0.0f;
        float ReferenceLuminance_Total_FullStrip_Snapshot = 0.0f;
        float ReferenceLuminance_Direct_FullStrip_Snapshot = 0.0f;
        float ReferenceLuminance_Diffuse_FullStrip_Snapshot = 0.0f;
        float ReferenceLuminance_DiffuseNoGI_FullStrip_Snapshot = 0.0f;
        float ReferenceLuminance_Direct_Ground_Snapshot = 0.0f;
        float ReferenceLuminance_Direct_Snow_Snapshot = 0.0f;
        float ReferenceLuminance_SurfaceState_Ground_Snapshot = 0.0f;
        float ReferenceLuminance_SurfaceState_Snow_Snapshot = 0.0f;
        float ReferenceSelectionSnowDepth_m = 0.0f;
        float ReferenceSelectionSnowCoverFraction = 0.0f;
        float ReferenceSnowBlendWeight = 0.0f;
        float ReferenceRenderSurfaceSnowBlendWeight = 0.0f;
        int32 ReferenceSelectionSnowLayerCount = 0;
        uint8 bDualReferenceStripEnabled = 0;
        uint8 bReferenceSelectedGroundHalf = 0;
        uint8 bReferenceSelectionUsedRenderSurfaceState = 0;
        uint8 bReferenceSelectionPlausibilityOverride = 0;
        uint8 bNeutralizationUsedRenderSurfaceState = 0;
        float RTY_Total = 0.0f;
        float RTY_Direct = 0.0f;
        float RTY_Diffuse = 0.0f;
        float RTY_DiffuseNoGI = 0.0f;
        float RTY_TotalNoGI = 0.0f;
        float RTY_SurfaceState = 0.0f;
        float RTY_Terrain = 0.0f;
        uint8 bReferenceValid = 0;
        uint8 ReferenceInvalidReasonCode = 0;
        uint8 bReferenceActorValidityFlag = 0;
        uint8 bReferenceActorValidityOverridden = 0;
        FFSM2EnergyFluxes EnergyFluxes;
        int32 SnowLayerCount = 0;
        int32 SoilLayerCount = 0;
        TStaticArray<float, GFSM2MaxLayers> SnowThickness_m;
        TStaticArray<float, GFSM2MaxLayers> SnowTemperature_K;
        TStaticArray<float, GFSM2MaxLayers> SnowIceMass_kgm2;
        TStaticArray<float, GFSM2MaxLayers> SnowLiquidMass_kgm2;
        TStaticArray<float, GFSM2MaxLayers> SnowDensity_kgm3;
        TStaticArray<float, GFSM2MaxLayers> SnowGrainRadius_m;
        TStaticArray<float, GFSM2MaxLayers> SnowWaterFlux_kgm2s;  // wflx: Water flux through each snow layer (kg/m²/s)
        TStaticArray<float, GFSM2MaxSoilLayers> SoilThickness_m;
        TStaticArray<float, GFSM2MaxSoilLayers> SoilTemperature_K;
        TStaticArray<float, GFSM2MaxSoilLayers> SoilMoisture_Vol;

        void Reset()
        {
            StepIndex = 0;
            CellIndex = -1;
            SimulationTimeSeconds = 0.0;
            Timestamp = FDateTime();
            SnowDepth_m = 0.0f;
            SnowMass_kgm2 = 0.0f;
            Runoff_kgm2 = 0.0f;
            ThroughfallSnow_kgm2 = 0.0f;
            ThroughfallRain_kgm2 = 0.0f;
            FrostMass_kgm2 = 0.0f;
            MeltMass_kgm2 = 0.0f;
            RefreezeMass_kgm2 = 0.0f;
            SublimationMass_kgm2 = 0.0f;
            CanopySnow_kgm2 = 0.0f;
            CanopyLiquid_kgm2 = 0.0f;
            SurfaceTemperatureK = 0.0f;
            NetSurfaceFlux_Wm2 = 0.0f;
            GroundHeatFlux_Wm2 = 0.0f;
            ForcingAirTemperatureK = 0.0f;
            ForcingIncomingLongwave_Wm2 = 0.0f;
            ForcingIncomingShortwave_Wm2 = 0.0f;
            ForcingDiffuseShortwave_Wm2 = 0.0f;
            ForcingDirectShortwave_Wm2 = 0.0f;
            ForcingDiffuseShortwaveBase_Wm2 = 0.0f;
            ForcingDirectShortwaveBase_Wm2 = 0.0f;
            ForcingWindSpeed_mps = 0.0f;
            SurfaceMoistureConductance_ms = 0.0f;
            SurfaceMoistureAvailability = 0.0f;
            SnowAlbedo = 0.0f;
            SurfaceAlbedo = 0.0f;
            SnowCoverFraction = 0.0f;
            DiffuseShortwave_Wm2 = 0.0f;
            DirectShortwave_Wm2 = 0.0f;
            TerrainShortwave_Wm2 = 0.0f;
            TotalShortwave_Wm2 = 0.0f;
            RadiationIndex = 1.0f;
            RadiationIndex_Direct = 0.0f;
            RadiationIndex_Diffuse = 0.0f;
            RadiationComponent = 0.0f;
            RadiationIndexImpact_Wm2 = 0.0f;
            RadiationIndexImpactRatio = 1.0f;
            CalibratedSWROut_Wm2 = 0.0f;
            NeutralizationSurfaceAlbedo = 0.0f;
            NeutralizationReferenceAlbedo = 0.0f;
            NeutralizationSnowBlendWeight = 0.0f;
            AlbedoNeutralizationRawFactor = 1.0f;
            AlbedoNeutralizationFactor = 1.0f;
            ReferenceScale_Total = 0.0f;
            ReferenceScale_Direct = 0.0f;
            ReferenceScale_Diffuse = 0.0f;
            ReferenceScale_TotalNoGI = 0.0f;
            bSkyOnlyReferenceUsable = 0;
            bSkyOnlyReferenceMeetsMinLuminance = 0;
            bSkyOnlyDiffuseScalingUsed = 0;
            SkyOnlyReferenceRatio = 0.0f;
            SkyOnlyRTYRatio = 0.0f;
            ReferenceLuminance_Total = 0.0f;
            ReferenceLuminance_Direct = 0.0f;
            ReferenceLuminance_Diffuse = 0.0f;
            ReferenceLuminance_DiffuseNoGI = 0.0f;
            ReferenceLuminance_TotalNoGI = 0.0f;
            ReferenceLuminance_SurfaceState_FullStrip_Snapshot = 0.0f;
            ReferenceLuminance_Total_FullStrip_Snapshot = 0.0f;
            ReferenceLuminance_Direct_FullStrip_Snapshot = 0.0f;
            ReferenceLuminance_Diffuse_FullStrip_Snapshot = 0.0f;
            ReferenceLuminance_DiffuseNoGI_FullStrip_Snapshot = 0.0f;
            ReferenceLuminance_Direct_Ground_Snapshot = 0.0f;
            ReferenceLuminance_Direct_Snow_Snapshot = 0.0f;
            ReferenceLuminance_SurfaceState_Ground_Snapshot = 0.0f;
            ReferenceLuminance_SurfaceState_Snow_Snapshot = 0.0f;
            ReferenceSelectionSnowDepth_m = 0.0f;
            ReferenceSelectionSnowCoverFraction = 0.0f;
            ReferenceSnowBlendWeight = 0.0f;
            ReferenceRenderSurfaceSnowBlendWeight = 0.0f;
            ReferenceSelectionSnowLayerCount = 0;
            bDualReferenceStripEnabled = 0;
            bReferenceSelectedGroundHalf = 0;
            bReferenceSelectionUsedRenderSurfaceState = 0;
            bReferenceSelectionPlausibilityOverride = 0;
            bNeutralizationUsedRenderSurfaceState = 0;
            RTY_Total = 0.0f;
            RTY_Direct = 0.0f;
            RTY_Diffuse = 0.0f;
            RTY_DiffuseNoGI = 0.0f;
            RTY_TotalNoGI = 0.0f;
            RTY_SurfaceState = 0.0f;
            RTY_Terrain = 0.0f;
            bReferenceValid = 0;
            ReferenceInvalidReasonCode = 0;
            bReferenceActorValidityFlag = 0;
            bReferenceActorValidityOverridden = 0;
            EnergyFluxes = FFSM2EnergyFluxes();
            SnowLayerCount = 0;
            SoilLayerCount = 0;
            for (int32 i = 0; i < GFSM2MaxLayers; ++i)
            {
                SnowThickness_m[i] = 0.0f;
                SnowTemperature_K[i] = 0.0f;
                SnowIceMass_kgm2[i] = 0.0f;
                SnowLiquidMass_kgm2[i] = 0.0f;
                SnowDensity_kgm3[i] = 0.0f;
                SnowGrainRadius_m[i] = 0.0f;
                SnowWaterFlux_kgm2s[i] = 0.0f;
            }
            for (int32 i = 0; i < GFSM2MaxSoilLayers; ++i)
            {
                SoilThickness_m[i] = 0.0f;
                SoilTemperature_K[i] = 0.0f;
                SoilMoisture_Vol[i] = 0.0f;
            }
        }
    };

    /** Cache of last step diagnostics for debug visualization (energy fluxes, mass fluxes, etc.) */
    TArray<FFSM2CellDiagnostics> LastStepDiagnostics;

protected:
    void ResetState();

    void EnsureStateSize(int32 CellCount);

    float ComputeSlopeAttenuation(int32 CellIndex) const;

    float CalcSpecificHumidity(float TemperatureK, float RelativeHumidity, float PressurePa) const;

    float CalcSaturationVapourPressure(float TemperatureK) const;

    float CalcAirDensity(float TemperatureK, float PressurePa) const;

    /** Clamp density to the appropriate state limit (cold vs melt), mirroring FSM2 rcld/rmlt logic. */
    float ClampStateDensity(const FFSM2SnowColumn& Snow, int32 LayerIdx, float ProposedDensity) const;

private:
    struct FOpenSurfaceEnergyResult
    {
        bool bConverged = false;
        float SurfaceTempK = 273.15f;  // Default to freezing point
        float GroundHeatFlux_Wm2 = 0.0f;
        float Sensible_Wm2 = 0.0f;
        float Latent_Wm2 = 0.0f;
        float NetShortwave_Wm2 = 0.0f;
        float NetLongwave_Wm2 = 0.0f;
        float MeltRate_kgm2s = 0.0f;
        float SurfaceMassFlux_kgm2s = 0.0f;
        float WaterAvailability = 1.0f;
        float AerodynamicConductance_ms = 0.0f;
        float LatentHeat_Jkg = 0.0f;
    };

    struct FFSM2SurfaceUpdate
    {
        float TemperatureK = 273.15f;
        float IceMass_kgm2 = 0.0f;
        float LiquidMass_kgm2 = 0.0f;
        float MeltMass_kgm2 = 0.0f;
        float RefreezeMass_kgm2 = 0.0f;
        float SurfaceMassFlux_kgm2s = 0.0f;
    };

    struct FSoilDerivedProperties
    {
        float ClappHornbergerB = 0.0f;
        float DryHeatCapacity_Jm3K = 2.2e6f;
        float DryConductivity_WmK = 1.5f;
        float SaturatedMatricPotential_m = 3.364f;
        float SaturatedVolumetricWater = 0.3f;
        float CriticalVolumetricWater = 0.2f;
    };


    float GetLayerMass(const FFSM2SnowColumn& Snow, int32 LayerIdx) const;
    float GetLayerHeatCapacity(const FFSM2SnowColumn& Snow, int32 LayerIdx) const;
    float GetLayerThermalConductivity(const FFSM2SnowColumn& Snow, int32 LayerIdx) const;
    float ComputeLayerDensity(const FFSM2SnowColumn& Snow, int32 LayerIdx) const;
    void UpdateLayerDiagnostics(FFSM2SnowColumn& Snow, int32 LayerIdx) const;
    void EvaluateSoilThermalProfile(const FFSM2SoilColumn& Soil, TArray<double>& OutThickness, TArray<double>& OutConductivity, TArray<double>& OutHeatCapacity) const;
    float ComputeSurfaceMoistureConductance(const FFSM2SoilColumn& Soil) const;
    void CalculateSoilProperties(float& OutClappHornbergerExponent, float& OutVolumetricHeatCapacityDry, float& OutThermalConductivityDry, float& OutSaturatedMatricPotential, float& OutCriticalVolumetricWater, float& OutSaturatedVolumetricWater) const;
    void CalculateSurfaceLayerThermalProperties(const FFSM2SnowColumn& Snow, const FFSM2SoilColumn& Soil, float& OutSurfaceLayerThickness, float& OutSurfaceLayerTemperature, float& OutSurfaceLayerConductivity) const;
    void InsertSurfaceLayer(FFSM2SnowColumn& Snow, float IceMass, float LiquidMass, float Density, float TemperatureK, float GrainRadius);
    void MergeLayerDown(FFSM2SnowColumn& Snow, int32 UpperIndex);
    void CullAndMergeLayers(FFSM2SnowColumn& Snow);

    void ApplyDensityEvolution(FFSM2SnowColumn& Snow, float DtSeconds);
    void MeltRefreezeStage(FFSM2SnowColumn& Snow, const FFSM2SurfaceUpdate& SurfaceUpdate, float ThroughfallRain_kgm2, float DtSeconds, float& OutSublimationMass_kgm2);
    void ApplySurfaceMelt(FFSM2SnowColumn& Snow, float MeltMass_kgm2);
    void ApplySurfaceMeltFortran(FFSM2SnowColumn& Snow, float MeltRate_kgm2s, float DtSeconds);
    void ApplySurfaceSublimationFortran(FFSM2SnowColumn& Snow, float SublimationRate_kgm2s, float DtSeconds);
    void ConductiveHeatExchange(FFSM2SnowColumn& Snow, FFSM2SoilColumn& Soil, float DtSeconds, float SurfaceFlux_Wm2);
    void HydrologyStage(FFSM2SnowColumn& Snow, float DtSeconds, float SurfaceRainInput_kgm2, float& OutRunoff_kgm2, FFSM2CellDiagnostics* OptionalDiagnostics = nullptr);
    void HydrologyFreeDrain(FFSM2SnowColumn& Snow, float& OutRunoff_kgm2, FFSM2CellDiagnostics* OptionalDiagnostics = nullptr);
    void HydrologyBucket(FFSM2SnowColumn& Snow, float DtSeconds, float SurfaceRainRate, float& OutRunoff_kgm2, FFSM2CellDiagnostics* OptionalDiagnostics = nullptr);
    void HydrologyGravitational(FFSM2SnowColumn& Snow, float DtSeconds, float SurfaceRainRate_kgm2s, float& OutRunoff_kgm2, FFSM2CellDiagnostics* OptionalDiagnostics = nullptr);
    void SolveTridiagonal(int32 Count, double* a, double* b, double* c, double* rhs, double* x) const;
    void ComputeSnowConduction(FFSM2SnowColumn& Snow, FFSM2SoilColumn& Soil, float DtSeconds, float SurfaceFlux_Wm2, double& OutGsoil_Wm2);
    void AdvanceSoilColumn(FFSM2SoilColumn& Soil, float DtSeconds, double Gsoil_Wm2);
    void RedistributeSnowpack(FFSM2SnowColumn& Snow);
    void UpdateGrainGrowth(FFSM2SnowColumn& Snow, const FFSM2SoilColumn& Soil, float SurfaceTemperatureK, float DtSeconds);
    void RebuildLayers(FFSM2SnowColumn& Snow);
    float GetMinimumSnowLayerThickness(int32 LayerIdx) const;

    void InitializeColumnDefaults(FFSM2ColumnState& Column) const;
    void InitializeSoilProfile(FFSM2SoilColumn& Soil) const;

    void AgeSnowpack(FFSM2SnowColumn& Snow, float DtHours);
    void ApplyCanopyExchange(float& ThroughfallSnow, float& ThroughfallRain, float& IncomingShortwave, float& IncomingLongwave) const;
    void RefreshEnvironmentalMetadata(UWorld* World);
    void ApplyLapseRateAdjustments(int32 CellIndex, float BaseAirTempK, float BasePrecipitation_kgm2, float& OutAirTempK, float& OutPrecipitation_kgm2) const;
    void PartitionShortwave(const FWeatherForcingData& Forcing, float IncomingSW, float& OutDiffuse, float& OutDirect) const;
    float UpdateSnowAlbedo(FFSM2ColumnState& Column, float DtSeconds, float SurfaceTemperatureK, float SnowfallRate_kgm2s) const;
    float ComputeSnowCoverFraction(const FFSM2SnowColumn& Snow) const;
    float ComputeSurfaceAlbedo(const FFSM2ColumnState& Column, float SnowAlbedo, float SnowCoverFraction) const;
    void AdjustMeasurementHeights(float& WindHeight, float& TempHeight) const;
    float ResolveBulkTransferCoefficient(float WindHeight, float TempHeight) const;
    FOpenSurfaceEnergyResult SolveOpenSurfaceEnergyBalance(
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
        float MeasurementTempHeight) const;
    void EnergyBalanceStage(int32 CellIndex, FFSM2ColumnState& Column, float DtSeconds, const FWeatherForcingData& Forcing, float SnowInput_kgm2, float RainInput_kgm2, float AirDensity, float AirTempK, float Qa, float PressurePa, float WindSpeed, float FreshDensity, float FreshGrainRadius, float BulkTransferCoefficient, float DiffuseSW, float DirectSW, float TerrainSW, float SurfaceAlbedo, float& OutNetSurfaceFlux, float& OutSurfaceTemperatureK, float& OutThroughfallSnow, float& OutThroughfallRain, FFSM2EnergyFluxes& OutFluxes, FFSM2SurfaceUpdate& OutSurfaceUpdate, FFSM2CellDiagnostics* OutDiagnostics);
    void DepositionStage(FFSM2SnowColumn& Snow, float ThroughfallSnow, float AirTempK, float FreshDensity, float FreshGrainRadius);

    float ComputeSurfaceEnergyBalance(const FFSM2SnowColumn& Snow, const FFSM2SoilColumn& Soil, float NetShortwave, float IncomingShortwave, float SurfaceAlbedo, float IncomingLongwave, float AirDensity, float AirTempK, float Qa, float PressurePa, float WindSpeed, float ThroughfallRain, float DtSeconds, float BulkTransferCoefficient, float& OutSurfaceTemperatureK, FFSM2EnergyFluxes& OutFluxes, FFSM2SurfaceUpdate& OutSurfaceUpdate, float& OutSurfaceMoistureConductance_ms, float& OutSurfaceMoistureAvailability) const;
    float ComputeSnowDepthMeters(const FFSM2SnowColumn& Snow) const;
    float ComputeSnowMass(const FFSM2SnowColumn& Snow) const;
    float ComputeSnowIceMass(const FFSM2SnowColumn& Snow) const;

    void EnsureDiagnosticsFileInitialized();
    void WriteDiagnostics(const TArray<FFSM2CellDiagnostics>& CellDiagnostics);
    void EnsureRadiationDiagnosticsFileInitialized();
    void WriteRadiationDiagnostics(const TArray<FFSM2CellDiagnostics>& CellDiagnostics);
    void WriteParametersToFile(const FString& Timestamp);
    void WriteRadiationConfigFile(const FString& RunTag);

    FSoilDerivedProperties DerivedSoilProperties;

    mutable TArray<float> CachedAlbedo;

    double ElapsedSimulationSeconds = 0.0;
    int32 SimulationStepCounter = 0;

    bool bDiagnosticsFileInitialized = false;
    bool bDiagnosticsHeaderWritten = false;
    FString DiagnosticsFilePath;
    bool bRadiationDiagnosticsHeaderWritten = false;
    FString RadiationDiagnosticsFilePath;
    FString RadiationConfigFilePath;
    float MeasurementAltitudeCm = 0.0f;
    float CellSpacingMeters = 0.0f;

    void UpdateDerivedParameters();
    void SanitizeLayerConfiguration();
    void UpdateDynamicSurfaceGeometry();

    mutable float CachedLatitudeDeg = 0.0f;
    mutable float CachedSolarNoon = 12.0f;
    mutable bool bHasEnvironmentalOverride = false;

    // Reference to owning simulation actor for accessing UE radiation indices
    TWeakObjectPtr<class ASnowSimulationActor> OwningSimulationActor;
};
