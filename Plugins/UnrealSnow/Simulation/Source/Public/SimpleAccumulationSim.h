#pragma once

#include "CoreMinimal.h"
#include "SnowSimulationActor.h"
#include "SnowSimulation.h"
#include "SimpleAccumulationSim.generated.h"

UCLASS(BlueprintType, Blueprintable, EditInlineNew)
class SIMULATION_API USimpleAccumulationSim : public USnowSimulation
{
	GENERATED_BODY()
public:
	virtual FString GetSimulationName() const override { return TEXT("SimpleAccumulation"); }

	virtual void Initialize(ASnowSimulationActor* SimulationActor, const TArray<FLandscapeCell>& Cells, float InitialMaxSnow, UWorld* World) override
	{
		const int32 DimX = SimulationActor ? SimulationActor->CellsDimensionX : 0;
		const int32 DimY = SimulationActor ? SimulationActor->CellsDimensionY : 0;
		InitializeGrid(DimX, DimY, 1.0f);
	}

	virtual void Step(float DtSeconds, const FWeatherForcingData& W, TArray<float>& OutDepthMeters) override
	{
		if (OutDepthMeters.Num() == 0 || DtSeconds <= 0.0f)
		{
			return;
		}

		// Convert precipitation mass flux to snow depth (meters) using configured density
		const float SnowFrac = FMath::Clamp(W.SnowFrac_01, 0.f, 1.f);
		const float rho_snow = (FreshSnowDensity_kgm3 > 1.0f) ? FreshSnowDensity_kgm3 : 100.0f; // kg/m^3
		const float dS_m = (rho_snow > 1e-6f) ? FMath::Max(0.0f, W.PrecipRate_kgm2s) * SnowFrac / rho_snow * DtSeconds : 0.0f;
		const float dS_mm = dS_m * 1000.0f; // for logging


		// Calculate current depth for logging
		const float CurrentDepth_mm = (OutDepthMeters.Num() > 0) ? OutDepthMeters[0] * 1000.0f : 0.0f;

		// Compute precipitation water equivalent over the step for logging (mm)
		const float PrecipWE_mm = FMath::Max(0.0f, W.PrecipRate_kgm2s) * DtSeconds; // 1 kg/m² = 1 mm

		// Verbose log each step using depth units
		UE_LOG(LogTemp, Verbose, TEXT("[Snow][Accum] dt=%.0fs precipWE=%.2f mm SnowFrac=%.2f -> dS=%.3f mm ; depth=%.3f mm"),
			DtSeconds, PrecipWE_mm, SnowFrac, dS_mm, CurrentDepth_mm);

		for (float& H : OutDepthMeters) { H += dS_m; }
	}

	virtual void Simulate(ASnowSimulationActor* SimulationActor, int32 /*CurrentSimulationStep*/, int32 /*Timesteps*/, bool /*SaveSnowMap*/, bool /*CaptureDebugInformation*/, TArray<FDebugCell>& /*DebugCells*/) override
	{
		if (!SimulationActor) return;
		
		// Pull weather forcing for current time
		FWeatherForcingData W;
			if (SimulationActor->WeatherProvider)
			{
				W = SimulationActor->WeatherProvider->GetWeatherForcing(SimulationActor->GetWeatherQueryTimeUtc());
			}

		// Convert precipitation mass flux to snow depth (meters) using configured density
		const float SnowFrac = FMath::Clamp(W.SnowFrac_01, 0.f, 1.f);
		const float rho_snow = (FreshSnowDensity_kgm3 > 1.0f) ? FreshSnowDensity_kgm3 : 100.0f; // kg/m^3
		const float dS_m = (rho_snow > 1e-6f) ? FMath::Max(0.0f, W.PrecipRate_kgm2s) * SnowFrac / rho_snow * SimulationActor->TimeStepSeconds : 0.0f;
		const float dS_mm = dS_m * 1000.0f; // for logging

		// Get probe cell depths before update
		const int32 DimX = SimulationActor->CellsDimensionX;
		const int32 DimY = SimulationActor->CellsDimensionY;
		const int32 CenterX = DimX / 2;
		const int32 CenterY = DimY / 2;
		const int32 CenterIndex = CenterY * DimX + CenterX;
		
		const float DepthBefore_0_0 = (DepthMeters.Num() > 0) ? DepthMeters[0] : 0.0f;
		const float DepthBefore_Center = (DepthMeters.Num() > CenterIndex) ? DepthMeters[CenterIndex] : 0.0f;

		// VERBOSE log before writing depth (depth units)
		const float PrecipWE_step_mm = FMath::Max(0.0f, W.PrecipRate_kgm2s) * SimulationActor->TimeStepSeconds;
		UE_LOG(LogTemp, Verbose, TEXT("[Accum] dt=%.0fs precipWE=%.2f mm snowfrac=%.2f -> dS_m=%.6f (%.3f mm) ; depth_before=(%.6f,%.6f) m ; depth_after=(%.6f,%.6f) m"),
			SimulationActor->TimeStepSeconds, PrecipWE_step_mm, SnowFrac, dS_m, dS_mm,
			DepthBefore_0_0, DepthBefore_Center,
			DepthBefore_0_0 + dS_m, DepthBefore_Center + dS_m);

		for (float& H : DepthMeters) { H += dS_m; }

		// Upload to PF_R16F texture
		UploadDepthToTexture();
	}

	// BN-event grid initializer; ensures base grid/texture setup is done
	virtual void Initialize_Implementation(int32 GX, int32 GY, float CellM) override
	{
		Super::Initialize_Implementation(GX, GY, CellM);
	}

	virtual void RenderDebug(UWorld* /*World*/, int /*CellDebugInfoDisplayDistance*/, EDebugVisualizationType /*VisualizationType*/) override {}

	virtual float GetMaxSnow() override
	{
		float LocalMax = 0.0f;
		for (const float V : DepthMeters) { LocalMax = FMath::Max(LocalMax, V); }
		return LocalMax * 1000.0f; // mm
	}
};
