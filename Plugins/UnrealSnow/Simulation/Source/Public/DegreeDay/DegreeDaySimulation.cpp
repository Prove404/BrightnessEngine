#include "DegreeDaySimulation.h"
#include "SnowSimulationActor.h"
#include "SimulationComputeShader.h"
#include "SimulationPixelShader.h"
#include "Async/ParallelFor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "ClimateData.h"
#include "Components/DirectionalLightComponent.h"
#include "DrawDebugHelpers.h"
#include "Util/MathUtil.h"
#include <cfloat>

namespace
{
	constexpr float DiffuseNoGIReferenceAbsoluteFloor = 1.0e-4f;
	constexpr float DiffuseNoGIReferenceRelativeFloor = 1.0e-2f;
	constexpr float DiffuseNoGIReferenceMinTotalFraction = 1.0e-2f;

	bool IsSnowFreeForDualReference(const ASnowSimulationActor* Actor, float SnowDepthMeters)
	{
		if (!Actor)
		{
			return true;
		}

		const float ThresholdMeters = FMath::Max(0.0f, Actor->DualReferenceSnowDepthThreshold_m);
		return SnowDepthMeters <= ThresholdMeters;
	}

	bool HasUsableSkyOnlyReference(float SkyOnlyReference, float DiffuseReference, float TotalReference)
	{
		if (!FMath::IsFinite(SkyOnlyReference) || SkyOnlyReference <= DiffuseNoGIReferenceAbsoluteFloor)
		{
			return false;
		}

		const float SafeDiffuseReference = FMath::Max(DiffuseReference, 0.0f);
		const float SafeTotalReference = FMath::Max(TotalReference, 0.0f);
		if (SafeDiffuseReference <= DiffuseNoGIReferenceAbsoluteFloor
			|| SafeTotalReference <= DiffuseNoGIReferenceAbsoluteFloor)
		{
			return false;
		}

		return SkyOnlyReference >= (SafeDiffuseReference * DiffuseNoGIReferenceRelativeFloor)
			&& SkyOnlyReference >= (SafeTotalReference * DiffuseNoGIReferenceMinTotalFraction);
	}

	float DD_BlendDualReferenceValue(float GroundValue, float SnowValue, float SnowWeight, float FullStripFallback)
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

	bool DD_TryComputeReferenceIndex(float SampleRTY, float ReferenceLuminance, float& OutIndex)
	{
		if (!FMath::IsFinite(SampleRTY) || SampleRTY < 0.0f
			|| !FMath::IsFinite(ReferenceLuminance) || ReferenceLuminance <= KINDA_SMALL_NUMBER)
		{
			OutIndex = 0.0f;
			return false;
		}

		OutIndex = SampleRTY / ReferenceLuminance;
		return FMath::IsFinite(OutIndex) && OutIndex >= 0.0f;
	}

	bool DD_TryComputeRenderSurfaceSnowBlendWeight(
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

	float DD_ResolveDualReferenceSnowBlendWeight(
		const ASnowSimulationActor* Actor,
		int32 CellIndex,
		float SnowDepthMeters,
		bool bPreferTotalReferencePlausibility,
		bool& bOutUsedRenderSurfaceState,
		bool& bOutUsedPlausibilityOverride)
	{
		bOutUsedRenderSurfaceState = false;
		bOutUsedPlausibilityOverride = false;

		if (!Actor || !Actor->bUseDualReferenceStrip)
		{
			return 0.0f;
		}

		float SnowBlendWeight = IsSnowFreeForDualReference(Actor, SnowDepthMeters) ? 0.0f : 1.0f;
		const TArray<float>& SurfaceStateRTY = Actor->GetCachedSurfaceStateRTY();
		if (SurfaceStateRTY.IsValidIndex(CellIndex))
		{
			float RenderSurfaceSnowBlendWeight = 0.0f;
			if (DD_TryComputeRenderSurfaceSnowBlendWeight(
				SurfaceStateRTY[CellIndex],
				Actor->ReferenceLuminance_SurfaceState_Ground,
				Actor->ReferenceLuminance_SurfaceState_Snow,
				RenderSurfaceSnowBlendWeight))
			{
				SnowBlendWeight = RenderSurfaceSnowBlendWeight;
				bOutUsedRenderSurfaceState = true;
			}
		}

		const float MaxPlausibleIndex = FMath::Max(Actor->GetMaxRadiationIndexClamp(), 0.1f);
		auto TryOverrideReferenceHalf = [&](float SampleRTY, float GroundReference, float SnowReference)
		{
			const bool bInitiallySelectedGroundHalf = SnowBlendWeight < 0.5f;
			float GroundIndex = 0.0f;
			float SnowIndex = 0.0f;
			const bool bGroundIndexValid = DD_TryComputeReferenceIndex(SampleRTY, GroundReference, GroundIndex);
			const bool bSnowIndexValid = DD_TryComputeReferenceIndex(SampleRTY, SnowReference, SnowIndex);
			const bool bGroundPlausible = bGroundIndexValid && GroundIndex <= MaxPlausibleIndex;
			const bool bSnowPlausible = bSnowIndexValid && SnowIndex <= MaxPlausibleIndex;

			if (bInitiallySelectedGroundHalf && !bGroundPlausible && bSnowPlausible)
			{
				SnowBlendWeight = 1.0f;
				bOutUsedPlausibilityOverride = true;
			}
			else if (!bInitiallySelectedGroundHalf && !bSnowPlausible && bGroundPlausible)
			{
				SnowBlendWeight = 0.0f;
				bOutUsedPlausibilityOverride = true;
			}
		};

		const TArray<float>& DirectRTY = Actor->GetCachedDirectRTY();
		if (DirectRTY.IsValidIndex(CellIndex))
		{
			TryOverrideReferenceHalf(
				DirectRTY[CellIndex],
				Actor->ReferenceLuminance_Direct_Ground,
				Actor->ReferenceLuminance_Direct_Snow);
		}

		if (bPreferTotalReferencePlausibility)
		{
			const TArray<float>& TotalRTY = Actor->GetCachedTotalRTY();
			if (TotalRTY.IsValidIndex(CellIndex))
			{
				TryOverrideReferenceHalf(
					TotalRTY[CellIndex],
					Actor->ReferenceLuminance_Total_Ground,
					Actor->ReferenceLuminance_Total_Snow);
			}
		}

		return FMath::Clamp(SnowBlendWeight, 0.0f, 1.0f);
	}
}

void UDegreeDaySimulation::Initialize_Implementation(int32 GX, int32 GY, float CellM)
{
	Super::Initialize_Implementation(GX, GY, CellM);
}

FString UDegreeDaySimulation::GetSimulationName() const
{
	return TEXT("DegreeDay");
}

void UDegreeDaySimulation::Initialize(ASnowSimulationActor* SimulationActor, const TArray<FLandscapeCell>& Cells, float InitialMaxSnow, UWorld* World)
{
	if (!SimulationActor) return;

	const int32 DimX = SimulationActor->CellsDimensionX;
	const int32 DimY = SimulationActor->CellsDimensionY;
	InitializeGrid(DimX, DimY, 1.0f);

	// Set terrain metadata for degree-day calculations
	SetTerrainMetadata(Cells, DimX, DimY);

	OwningSimulationActor = SimulationActor;
	USimulationWeatherDataProviderBase* ForcingProvider = SimulationActor->GetActiveWeatherProvider();
	MeasurementAltitudeCm = (ForcingProvider) ? ForcingProvider->GetMeasurementAltitude() : 0.0f;

	LegacyClimateData.Reset();
	if (ForcingProvider)
	{
		if (auto* RawClimate = ForcingProvider->CreateRawClimateDataResourceArray(
			SimulationActor->GetRunStartTime(), SimulationActor->GetRunEndTime()))
		{
			LegacyClimateData.Reserve(RawClimate->Num());
			for (const FClimateData& Entry : *RawClimate)
			{
				LegacyClimateData.Add(Entry);
			}
			delete RawClimate;
		}
	}

	InitializeCellState(Cells);
	UpdateDepthFromSnowWaterEquivalent();
}

UTexture* UDegreeDaySimulation::GetSnowMapTexture()
{
	return Super::GetSnowMapTexture();
}

float UDegreeDaySimulation::GetMaxSnow()
{
	if (CurrentMaxSnowMM <= 0.0f)
	{
		float LocalMax = 0.0f;
		for (const float V : DepthMeters)
		{
			LocalMax = FMath::Max(LocalMax, V);
		}
		CurrentMaxSnowMM = LocalMax * 1000.0f;
	}
	return CurrentMaxSnowMM;
}

void UDegreeDaySimulation::Step(float DtSeconds, const FWeatherForcingData& W, TArray<float>& OutDepthMeters)
{
	if (DepthMeters.Num() == 0 || DtSeconds <= 0.0f)
	{
		return;
	}

	ASnowSimulationActor* Actor = OwningSimulationActor.Get();
	float LocalMeasurementAltitude = MeasurementAltitudeCm;
	int32 DayOfYear = 172; // reasonable default (June solstice) if actor not available

	if (Actor)
	{
		if (USimulationWeatherDataProviderBase* ForcingProvider = Actor->GetActiveWeatherProvider())
		{
			LocalMeasurementAltitude = ForcingProvider->GetMeasurementAltitude();

			// Log measurement altitude on first step
			static bool bLoggedMeasurementAlt = false;
			if (!bLoggedMeasurementAlt)
			{
				bLoggedMeasurementAlt = true;
				UE_LOG(LogTemp, Display, TEXT("[DegreeDay] MeasurementAltitude from ClimateData: %.0f cm (%.2f m)"),
					LocalMeasurementAltitude, LocalMeasurementAltitude / 100.0f);
			}
		}
		DayOfYear = Actor->CurrentSimulationTime.GetDayOfYear();
	}
	else
	{
		DayOfYear = FDateTime::Now().GetDayOfYear();
	}

	PerformDegreeDayStep(DtSeconds, W, DayOfYear, LocalMeasurementAltitude);

	// Ensure the caller sees the updated depth buffer even if they did not pass DepthMeters
	if (&OutDepthMeters != &DepthMeters)
	{
		OutDepthMeters = DepthMeters;
	}
}

void UDegreeDaySimulation::UpdateRTYLuminance(const TArray<float>& InTotalRTY, const TArray<float>& InDirectRTY, const TArray<float>& InDiffuseRTY, const TArray<float>& InDiffuseNoGIRTY, const TArray<float>& InTotalNoGIRTY, const TArray<float>& InTerrainRTY)
{
	const int32 NumCellsLocal = CellLastRTY_Direct.Num();
	static int32 RTYLogCounter = 0;

	if (bEnableHotPathLogs)
	{
		UE_LOG(LogTemp, Error, TEXT("[DegreeDay][RTY] UpdateRTYLuminance called on instance %p: InTotal=%d InDirect=%d InDiffuse=%d InDiffuseNoGI=%d InTotalNoGI=%d InTerrain=%d NumCells=%d"),
			this, InTotalRTY.Num(), InDirectRTY.Num(), InDiffuseRTY.Num(), InDiffuseNoGIRTY.Num(), InTotalNoGIRTY.Num(), InTerrainRTY.Num(), NumCellsLocal);
	}

	if (NumCellsLocal <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[DegreeDay][RTY] Update skipped: NumCellsLocal=%d"), NumCellsLocal);
		return;
	}

	// Ensure sizes match expected grid; skip update if mismatched to avoid stale pointers.
	if (InTotalRTY.Num() != NumCellsLocal || InDirectRTY.Num() != NumCellsLocal || InDiffuseRTY.Num() != NumCellsLocal)
	{
		UE_LOG(LogTemp, Error, TEXT("[DegreeDay][RTY] SIZE MISMATCH! InTotal=%d InDir=%d InDiff=%d Expected=%d - UPDATE SKIPPED!"),
			InTotalRTY.Num(), InDirectRTY.Num(), InDiffuseRTY.Num(), NumCellsLocal);
		return;
	}
	bHasDiffuseNoGI = (InDiffuseNoGIRTY.Num() == NumCellsLocal);
	bHasTerrainRTY = (InTerrainRTY.Num() == NumCellsLocal);

	if (RTYLogCounter < 3)
	{
		float MinTot = FLT_MAX, MaxTot = -FLT_MAX;
		float MinDir = FLT_MAX, MaxDir = -FLT_MAX;
		float MinDiff = FLT_MAX, MaxDiff = -FLT_MAX;
		double SumTot = 0.0, SumDir = 0.0, SumDiff = 0.0;

		for (int32 Idx = 0; Idx < NumCellsLocal; ++Idx)
		{
			const float Tot = InTotalRTY[Idx];
			const float Dir = InDirectRTY[Idx];
			const float Dif = InDiffuseRTY[Idx];
			MinTot = FMath::Min(MinTot, Tot);
			MaxTot = FMath::Max(MaxTot, Tot);
			SumTot += Tot;
			MinDir = FMath::Min(MinDir, Dir);
			MaxDir = FMath::Max(MaxDir, Dir);
			SumDir += Dir;
			MinDiff = FMath::Min(MinDiff, Dif);
			MaxDiff = FMath::Max(MaxDiff, Dif);
			SumDiff += Dif;
		}

		const float AvgTot = NumCellsLocal > 0 ? static_cast<float>(SumTot / NumCellsLocal) : 0.0f;
		const float AvgDir = NumCellsLocal > 0 ? static_cast<float>(SumDir / NumCellsLocal) : 0.0f;
		const float AvgDiff = NumCellsLocal > 0 ? static_cast<float>(SumDiff / NumCellsLocal) : 0.0f;

		UE_LOG(LogTemp, Warning, TEXT("[DegreeDay][RTY] Update #%d: Cells=%d Tot[min=%.3f max=%.3f avg=%.3f] Dir[min=%.3f max=%.3f avg=%.3f] Diff[min=%.3f max=%.3f avg=%.3f]"),
			RTYLogCounter + 1, NumCellsLocal, MinTot, MaxTot, AvgTot, MinDir, MaxDir, AvgDir, MinDiff, MaxDiff, AvgDiff);
		++RTYLogCounter;
	}

	FMemory::Memcpy(CellLastRTY_Total.GetData(), InTotalRTY.GetData(), NumCellsLocal * sizeof(float));
	FMemory::Memcpy(CellLastRTY_Direct.GetData(), InDirectRTY.GetData(), NumCellsLocal * sizeof(float));
	FMemory::Memcpy(CellLastRTY_Diffuse.GetData(), InDiffuseRTY.GetData(), NumCellsLocal * sizeof(float));
	if (bHasDiffuseNoGI && CellLastRTY_DiffuseNoGI.Num() == NumCellsLocal)
	{
		FMemory::Memcpy(CellLastRTY_DiffuseNoGI.GetData(), InDiffuseNoGIRTY.GetData(), NumCellsLocal * sizeof(float));
	}
	else if (CellLastRTY_DiffuseNoGI.Num() == NumCellsLocal)
	{
		FMemory::Memset(CellLastRTY_DiffuseNoGI.GetData(), 0, NumCellsLocal * sizeof(float));
	}
	if (CellLastRTY_TotalNoGI.Num() == NumCellsLocal)
	{
		if (InTotalNoGIRTY.Num() == NumCellsLocal)
		{
			FMemory::Memcpy(CellLastRTY_TotalNoGI.GetData(), InTotalNoGIRTY.GetData(), NumCellsLocal * sizeof(float));
		}
		else if (bHasDiffuseNoGI)
		{
			for (int32 Idx = 0; Idx < NumCellsLocal; ++Idx)
			{
				CellLastRTY_TotalNoGI[Idx] = FMath::Max(0.0f, InDirectRTY[Idx]) + FMath::Max(0.0f, InDiffuseNoGIRTY[Idx]);
			}
		}
		else
		{
			FMemory::Memset(CellLastRTY_TotalNoGI.GetData(), 0, NumCellsLocal * sizeof(float));
		}
	}
	if (bHasTerrainRTY && CellLastRTY_Terrain.Num() == NumCellsLocal)
	{
		FMemory::Memcpy(CellLastRTY_Terrain.GetData(), InTerrainRTY.GetData(), NumCellsLocal * sizeof(float));
	}
	else if (CellLastRTY_Terrain.Num() == NumCellsLocal)
	{
		FMemory::Memset(CellLastRTY_Terrain.GetData(), 0, NumCellsLocal * sizeof(float));
	}

	if (bEnableHotPathLogs)
	{
		UE_LOG(LogTemp, Error, TEXT("[DegreeDay][RTY] Memcpy completed successfully. CellLastRTY_Total[0]=%.4f, CellLastRTY_Direct[0]=%.4f, CellLastRTY_Diffuse[0]=%.4f, CellLastRTY_DiffuseNoGI[0]=%.4f, CellLastRTY_TotalNoGI[0]=%.4f, CellLastRTY_Terrain[0]=%.4f"),
			CellLastRTY_Total[0], CellLastRTY_Direct[0], CellLastRTY_Diffuse[0],
			CellLastRTY_DiffuseNoGI.IsValidIndex(0) ? CellLastRTY_DiffuseNoGI[0] : 0.0f,
			CellLastRTY_TotalNoGI.IsValidIndex(0) ? CellLastRTY_TotalNoGI[0] : 0.0f,
			CellLastRTY_Terrain.IsValidIndex(0) ? CellLastRTY_Terrain[0] : 0.0f);
	}
}

void UDegreeDaySimulation::InitializeCellState(const TArray<FLandscapeCell>& Cells)
{
	const int32 CellCount = Cells.Num();
	UE_LOG(LogTemp, Error, TEXT("[DegreeDay] InitializeCellState called with %d cells on instance %p"), CellCount, this);

	if (CellCount <= 0)
	{
		CellAreaSqMeters.Reset();
		UE_LOG(LogTemp, Error, TEXT("[DegreeDay] No cells to initialize!"));
		return;
	}

	// Use member variables for albedo (now exposed as UPROPERTY in header)

	CellAreaSqMeters.SetNum(CellCount, EAllowShrinking::No);
	CellAreaXYSqMeters.SetNum(CellCount, EAllowShrinking::No);
	CellAltitudeCm.SetNum(CellCount, EAllowShrinking::No);
	CellAspectRad.SetNum(CellCount, EAllowShrinking::No);
	CellInclinationRad.SetNum(CellCount, EAllowShrinking::No);
	CellLatitudeRad.SetNum(CellCount, EAllowShrinking::No);

	// Store base terrain cell corners for dynamic surface geometry
	CellBaseP0.SetNum(CellCount, EAllowShrinking::No);
	CellBaseP1.SetNum(CellCount, EAllowShrinking::No);
	CellBaseP2.SetNum(CellCount, EAllowShrinking::No);
	CellBaseP3.SetNum(CellCount, EAllowShrinking::No);
	CellSnowWaterEquivalentLiters.SetNum(CellCount, EAllowShrinking::No);
	CellInterpolatedSWE_Liters.SetNum(CellCount, EAllowShrinking::No);
	CellSnowAlbedoState.SetNum(CellCount, EAllowShrinking::No);
	CellDaysSinceSnowfall.SetNum(CellCount, EAllowShrinking::No);
	CellAlbedoTempSum.SetNum(CellCount, EAllowShrinking::No);
	CellDailyMaxTemp.SetNum(CellCount, EAllowShrinking::No);
	CellLastRadiationIndex.SetNum(CellCount, EAllowShrinking::No);
	CellLastRadiationIndex_Swift.SetNum(CellCount, EAllowShrinking::No);
	CellLastRadiationIndex_UE.SetNum(CellCount, EAllowShrinking::No);
	CellLastRadiationIndex_UE_Raw.SetNum(CellCount, EAllowShrinking::No);
	CellLastCloudinessRatio.SetNum(CellCount, EAllowShrinking::No);
	CellLastNetShortwaveAbsorbed_Wm2.SetNum(CellCount, EAllowShrinking::No);
	CellLastPotentialDirect_Wm2.SetNum(CellCount, EAllowShrinking::No);
	CellLastPotentialHorizontal_Wm2.SetNum(CellCount, EAllowShrinking::No);
	CellLastRTY_Total.SetNum(CellCount, EAllowShrinking::No);
	CellLastRTY_Direct.SetNum(CellCount, EAllowShrinking::No);
	CellLastRTY_Diffuse.SetNum(CellCount, EAllowShrinking::No);
	CellLastRTY_DiffuseNoGI.SetNum(CellCount, EAllowShrinking::No);
	CellLastRTY_TotalNoGI.SetNum(CellCount, EAllowShrinking::No);
	CellLastRTY_Terrain.SetNum(CellCount, EAllowShrinking::No);
	UE_LOG(LogTemp, Error, TEXT("[DegreeDay] RTY arrays initialized: Direct=%d Diffuse=%d DiffuseNoGI=%d TotalNoGI=%d Terrain=%d"),
		CellLastRTY_Direct.Num(), CellLastRTY_Diffuse.Num(), CellLastRTY_DiffuseNoGI.Num(), CellLastRTY_TotalNoGI.Num(), CellLastRTY_Terrain.Num());
	CellLastAccumulationDepth_m.SetNum(CellCount, EAllowShrinking::No);
	CellLastMeltDepth_m.SetNum(CellCount, EAllowShrinking::No);
	CellLastMeltFactor.SetNum(CellCount, EAllowShrinking::No);
	CellLastLocalAirTempC.SetNum(CellCount, EAllowShrinking::No);
	CellLastSlopeFiltered.SetNum(CellCount, EAllowShrinking::No);
	CellLastComputedSnowRate.SetNum(CellCount, EAllowShrinking::No);
	CellLastWeatherSnowFrac.SetNum(CellCount, EAllowShrinking::No);
	CellLastEffectiveSnowFrac.SetNum(CellCount, EAllowShrinking::No);
	CellLastRedistributionFactor.SetNum(CellCount, EAllowShrinking::No);

	GeoReferencingOriginUpCm = 0.0f;
	LastAlbedoUpdateDayOfYear = -1;

	// Compute cell spacing from first two cells (assuming regular grid)
	if (CellCount >= 2)
	{
		const FVector& C0 = Cells[0].Centroid;
		const FVector& C1 = Cells[1].Centroid;
		CellSpacingMeters = (C1 - C0).Size() / 100.0f;  // Convert cm to meters
	}

	for (int32 Idx = 0; Idx < CellCount; ++Idx)
	{
		const FLandscapeCell& Cell = Cells[Idx];

		const float AreaSqMeters = Cell.Area / (100.0f * 100.0f);
		const float AreaXYSqMeters = Cell.AreaXY / (100.0f * 100.0f);

		CellAreaSqMeters[Idx] = AreaSqMeters;
		CellAreaXYSqMeters[Idx] = AreaXYSqMeters;
		CellAltitudeCm[Idx] = Cell.Altitude;

		const float DerivedGeoOriginCm = Cell.Altitude - Cell.Centroid.Z;
		if (Idx == 0 || FMath::IsNearlyZero(GeoReferencingOriginUpCm))
		{
			GeoReferencingOriginUpCm = DerivedGeoOriginCm;
		}
		else if (!FMath::IsNearlyEqual(GeoReferencingOriginUpCm, DerivedGeoOriginCm, 1.0f))
		{
			UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] GeoReferencing origin mismatch detected for cell %d: expected %.2f cm, got %.2f cm"),
				Idx, GeoReferencingOriginUpCm, DerivedGeoOriginCm);
		}

		CellAspectRad[Idx] = Cell.Aspect;
		CellInclinationRad[Idx] = Cell.Inclination;
		CellLatitudeRad[Idx] = Cell.Latitude;

		// Store base terrain corners for dynamic surface geometry updates
		CellBaseP0[Idx] = Cell.P1;
		CellBaseP1[Idx] = Cell.P2;
		CellBaseP2[Idx] = Cell.P3;
		CellBaseP3[Idx] = Cell.P4;

		const float InitialSWE = FMath::Max(0.0f, Cell.InitialWaterEquivalent);
		CellSnowWaterEquivalentLiters[Idx] = InitialSWE;
		CellInterpolatedSWE_Liters[Idx] = InitialSWE;
		CellSnowAlbedoState[Idx] = (InitialSWE > 0.0f) ? FreshSnowAlbedo : OldSnowAlbedo;
		CellDaysSinceSnowfall[Idx] = 0.0f;
		CellAlbedoTempSum[Idx] = 0.0f;
		CellDailyMaxTemp[Idx] = -FLT_MAX;
		CellLastRadiationIndex[Idx] = 0.0f;
		CellLastAccumulationDepth_m[Idx] = 0.0f;
		CellLastMeltDepth_m[Idx] = 0.0f;
		CellLastMeltFactor[Idx] = 0.0f;
		CellLastLocalAirTempC[Idx] = 0.0f;
		CellLastSlopeFiltered[Idx] = 0;
		CellLastComputedSnowRate[Idx] = 0.0f;
		CellLastWeatherSnowFrac[Idx] = 0.0f;
		CellLastEffectiveSnowFrac[Idx] = 0.0f;
		CellLastNetShortwaveAbsorbed_Wm2[Idx] = 0.0f;
		CellLastPotentialDirect_Wm2[Idx] = 0.0f;
		CellLastPotentialHorizontal_Wm2[Idx] = 0.0f;
		CellLastRTY_Total[Idx] = 0.0f;
		CellLastRTY_Direct[Idx] = 0.0f;
		CellLastRTY_Diffuse[Idx] = 0.0f;
		CellLastRTY_DiffuseNoGI[Idx] = 0.0f;
		CellLastRTY_TotalNoGI[Idx] = 0.0f;
		CellLastRTY_Terrain[Idx] = 0.0f;
		CellLastRedistributionFactor[Idx] = 1.0f;
	}

	LastMassConservationFactor = 1.0f;

	if (GeoReferencingOriginUpCm != 0.0f)
	{
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Derived GeoReferencing origin offset: %.2f m"), GeoReferencingOriginUpCm / 100.0f);
	}

	// Log first cell to verify initialization
	if (CellCount > 0)
	{
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] First cell initialized: Area=%.2f mÂ², Altitude=%.0f cm, Aspect=%.2f rad, Inclination=%.2f rad, Latitude=%.4f rad"),
			CellAreaSqMeters[0], CellAltitudeCm[0], CellAspectRad[0], CellInclinationRad[0], CellLatitudeRad[0]);
	}

	CurrentMaxSnowMM = 0.0f;
}

void UDegreeDaySimulation::PerformDegreeDayStep(float DtSeconds, const FWeatherForcingData& W, int32 InDayOfYear, float MeasurementAltitude)
{
	const int32 CellCount = CellSnowWaterEquivalentLiters.Num();

	// Log array sizes on first step
	static bool bLoggedArraySizes = false;
	if (!bLoggedArraySizes && CellCount > 0)
	{
		bLoggedArraySizes = true;
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] PerformDegreeDayStep: CellCount=%d, CellAreaSqMeters.Num()=%d, CellAltitudeCm.Num()=%d"),
			CellCount, CellAreaSqMeters.Num(), CellAltitudeCm.Num());
		if (CellAreaSqMeters.Num() > 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[DegreeDay] First cell in PerformDegreeDayStep: Area=%.2f mÂ², Altitude=%.0f cm"),
				CellAreaSqMeters[0], CellAltitudeCm[0]);
		}
	}

	if (CellCount <= 0 || DtSeconds <= 0.0f)
	{
		return;
	}

	MeasurementAltitudeCm = MeasurementAltitude;
	const float MaxReasonableAbsAltitudeDeltaCm = 20000.0f * 100.0f; // 20 km guardrail
	static bool bLoggedAltitudeDeltaOutlier = false;

	const float DtDays = DtSeconds / 86400.0f;
	const float BaseTempC = W.Temperature_K - 273.15f;
	const float BaseExplicitSnow_mm = FMath::Max(0.0f, W.SnowRate_kgm2s) * DtSeconds;
	const float BaseExplicitRain_mm = FMath::Max(0.0f, W.RainRate_kgm2s) * DtSeconds;
	const float BaseExplicitTotal_mm = BaseExplicitSnow_mm + BaseExplicitRain_mm;
	const bool bHasUsableExplicitPrecipitation = W.bHasExplicitPrecipitation && BaseExplicitTotal_mm > KINDA_SMALL_NUMBER;
	const float PrecipBase_mm = bHasUsableExplicitPrecipitation
		? BaseExplicitTotal_mm
		: (FMath::Max(0.0f, W.PrecipRate_kgm2s) * DtSeconds);
	// Use member variables for albedo (now exposed as UPROPERTY in header)
	auto ResolveSnowFraction = [&](float LocalTemperatureC) -> float
	{
		if (PrecipBase_mm <= 0.0f)
		{
			return 0.0f;
		}

		if (bHasUsableExplicitPrecipitation)
		{
			return FMath::Clamp(BaseExplicitSnow_mm / BaseExplicitTotal_mm, 0.0f, 1.0f);
		}

		if (bUseWeatherSnowFraction)
		{
			return FMath::Clamp(W.SnowFrac_01, 0.0f, 1.0f);
		}

		if (LocalTemperatureC > TSnowB)
		{
			return 0.0f;
		}

		const float Denom = (TSnowB - TSnowA);
		if (FMath::Abs(Denom) > KINDA_SMALL_NUMBER)
		{
			return FMath::Clamp(1.0f - (LocalTemperatureC - TSnowA) / Denom, 0.0f, 1.0f);
		}

		return (LocalTemperatureC <= TSnowA) ? 1.0f : 0.0f;
	};

	const float SolarForcing_Wm2 = FMath::Max(0.0f, W.SWdown_Wm2);

	// Deprecated bScaleUERadiationByForcing logic removed.


	const float LocalPressurePa = (W.Pressure_Pa > 100.0f) ? W.Pressure_Pa : 101325.0f;

	ASnowSimulationActor* ActorPtr = OwningSimulationActor.Get();
	const float ActorLongitudeDeg = (ActorPtr) ? ActorPtr->Longitude : 0.0f;
	const bool bDirectOnlyRadiationIndex = ActorPtr && ActorPtr->bUseDirectRadiationIndexOnly && RadiationIndexMethod == ERadiationIndexMethod::UnrealEngine;
	const bool bTotalOnlyRadiationIndex = ActorPtr && ActorPtr->bUseTotalRadiationIndexOnly && RadiationIndexMethod == ERadiationIndexMethod::UnrealEngine;
	const float RadiationIndexClampMax = (ActorPtr)
		? ActorPtr->GetMaxRadiationIndexClamp()
		: 5.0f;

	FDateTime ActiveTimestamp = W.Timestamp;
	if (!ActiveTimestamp.GetTicks() && ActorPtr)
	{
		ActiveTimestamp = ActorPtr->CurrentSimulationTime;
	}
	if (!ActiveTimestamp.GetTicks())
	{
		ActiveTimestamp = FDateTime::Now();
	}

	// Fetch Light Intensities for Diagnostics (Constant for this timestep)
	float CurrentSunLightIntensity = 0.0f;
	float CurrentSkyLightIntensity = 0.0f;
	if (ActorPtr)
	{
		CurrentSunLightIntensity = ActorPtr->GetLastSunLightIntensity();
		CurrentSkyLightIntensity = ActorPtr->GetLastSkyLightIntensity();
	}

	// Update Pellicciotti albedo accumulation at day boundaries (uses daily max positive air temperature)
	if (LastAlbedoUpdateDayOfYear < 0)
	{
		LastAlbedoUpdateDayOfYear = InDayOfYear;
	}
	else if (LastAlbedoUpdateDayOfYear != InDayOfYear)
	{
		ParallelFor(CellCount, [&](int32 Idx)
		{
			const float DayMaxTemp = CellDailyMaxTemp.IsValidIndex(Idx) ? CellDailyMaxTemp[Idx] : -FLT_MAX;
			if (CellAlbedoTempSum.IsValidIndex(Idx) && DayMaxTemp > 0.0f)
			{
				CellAlbedoTempSum[Idx] += DayMaxTemp;
			}
			if (CellDailyMaxTemp.IsValidIndex(Idx))
			{
				CellDailyMaxTemp[Idx] = -FLT_MAX;
			}
		});
		LastAlbedoUpdateDayOfYear = InDayOfYear;
	}

	// Get solar geometry from UE SunSky actor (if available) to ensure alignment
	// between radiation calculations and actual sun position in the scene
	float SolarDeclinationRad = 0.0f;
	float HourAngleRad = 0.0f;
	bool bUseSunSkyGeometry = false;

	if (ActorPtr)
	{
		const float UE_CosSolarZenith = ActorPtr->GetCachedCaptureCosSolarZenith();
		const UDirectionalLightComponent* SunLight = ActorPtr->GetSunDirectionalLight();

		if (SunLight && FMath::Abs(UE_CosSolarZenith) <= 1.0f)
		{
			// Extract azimuth from sun direction
			const FVector SunDir = SunLight->GetDirection();
			float SolarAzimuthRad = FMath::Atan2(SunDir.Y, SunDir.X);
			if (SolarAzimuthRad < 0.0f)
			{
				SolarAzimuthRad += 2.0f * PI;
			}

			// Get latitude from first cell (assuming colocated weather station)
			const float LatitudeRad = CellLatitudeRad.IsValidIndex(0) ? CellLatitudeRad[0] : 0.0f;

			if (FMath::Abs(LatitudeRad) > 0.001f)  // Valid latitude
			{
				// Derive solar declination and hour angle from actual UE sun position
				// Using spherical astronomy equations:
				// cos(zenith) = sin(lat)*sin(decl) + cos(lat)*cos(decl)*cos(hourAngle)
				// We solve for declination and hour angle that match the observed zenith and azimuth

				const float SolarZenithRad = FMath::Acos(FMath::Clamp(UE_CosSolarZenith, -1.0f, 1.0f));
				const float SolarElevationRad = (PI / 2.0f) - SolarZenithRad;

				// Calculate solar declination from elevation and azimuth at solar noon approximation
				// For better accuracy, we use the azimuth to determine hour angle first
				// sin(elevation) = sin(lat)*sin(decl) + cos(lat)*cos(decl)*cos(hourAngle)


	// Debug logging for reference vs cell 2525 comparison
	static int32 DebugComparisonCounter = 0;
	if (DebugComparisonCounter++ % 60 == 0) // Log once every ~60 steps
	{
		const int32 TargetIdx = 2525;
		if (CellCount > TargetIdx)
		{
			// Gather properties
			auto GetCellInfo = [&](int32 Idx) -> FString {
				return FString::Printf(TEXT("Alt=%.1f, Slope=%.1f°, Aspect=%.1f°, RTY_Dir=%.4f, RTY_Diff=%.4f, RI=%.4f"),
					CellAltitudeCm.IsValidIndex(Idx) ? CellAltitudeCm[Idx] : 0.0f,
					CellInclinationRad.IsValidIndex(Idx) ? FMath::RadiansToDegrees(CellInclinationRad[Idx]) : 0.0f,
					CellAspectRad.IsValidIndex(Idx) ? FMath::RadiansToDegrees(CellAspectRad[Idx]) : 0.0f,
					CellLastRTY_Direct.IsValidIndex(Idx) ? CellLastRTY_Direct[Idx] : -1.0f,
					CellLastRTY_Diffuse.IsValidIndex(Idx) ? CellLastRTY_Diffuse[Idx] : -1.0f,
					CellLastRadiationIndex_UE.IsValidIndex(Idx) ? CellLastRadiationIndex_UE[Idx] : -1.0f
				);
			};

			UE_LOG(LogTemp, Warning, TEXT("[RefDebug] Comparison at Step %d (Time=%.2fh):"), 
				SimulationStepCounter, ActiveTimestamp.GetTimeOfDay().GetTotalHours());
			UE_LOG(LogTemp, Warning, TEXT("[RefDebug]   Ref Cell (0): %s"), *GetCellInfo(0));
			UE_LOG(LogTemp, Warning, TEXT("[RefDebug]   Tgt Cell (%d): %s"), TargetIdx, *GetCellInfo(TargetIdx));
			
			if (ActorPtr)
			{
				UE_LOG(LogTemp, Warning, TEXT("[RefDebug]   Actor RefLum: Total=%.4f, Direct=%.4f, Diffuse=%.4f"),
					ActorPtr->ReferenceLuminance_Total, 
					ActorPtr->ReferenceLuminance_Direct,
					ActorPtr->ReferenceLuminance_Diffuse);
			}
		}
	}

		// Calculate solar declination from elevation and azimuth at solar noon approximation
				const float DeclFromDayOfYear = ComputeSolarDeclinationRad(InDayOfYear);

				// Then compute hour angle using the spherical law of cosines rearranged
				const float SinElevation = FMath::Sin(SolarElevationRad);
				const float CosElevation = FMath::Cos(SolarElevationRad);
				const float SinLat = FMath::Sin(LatitudeRad);
				const float CosLat = FMath::Cos(LatitudeRad);
				const float SinDecl = FMath::Sin(DeclFromDayOfYear);
				const float CosDecl = FMath::Cos(DeclFromDayOfYear);

				// cos(hourAngle) = (sin(elevation) - sin(lat)*sin(decl)) / (cos(lat)*cos(decl))
				const float CosHourAngle = (SinElevation - SinLat * SinDecl) / (CosLat * CosDecl + 1e-6f);

				if (FMath::Abs(CosHourAngle) <= 1.0f)
				{
					// Determine sign of hour angle from azimuth
					// Morning (azimuth < 180Â°): hour angle negative
					// Afternoon (azimuth > 180Â°): hour angle positive
					float ComputedHourAngle = FMath::Acos(FMath::Clamp(CosHourAngle, -1.0f, 1.0f));
					if (SolarAzimuthRad > PI)
					{
						ComputedHourAngle = -ComputedHourAngle;
					}

					SolarDeclinationRad = DeclFromDayOfYear;
					HourAngleRad = ComputedHourAngle;
					bUseSunSkyGeometry = true;

					static int32 SunSkyGeometryLogCount = 0;
					if (SunSkyGeometryLogCount < 3)
					{
						SunSkyGeometryLogCount++;
						const float HourAngleDeg = FMath::RadiansToDegrees(HourAngleRad);
						const float LocalSolarTime = 12.0f + HourAngleDeg / 15.0f;
						UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Using UE sun geometry: Zenith=%.2fÂ°, Azimuth=%.2fÂ°, Lat=%.2fÂ°, Decl=%.2fÂ°, HourAngle=%.2fÂ° (LST=%.2fh)"),
							FMath::RadiansToDegrees(SolarZenithRad),
							FMath::RadiansToDegrees(SolarAzimuthRad),
							FMath::RadiansToDegrees(LatitudeRad),
							FMath::RadiansToDegrees(SolarDeclinationRad),
							HourAngleDeg,
							LocalSolarTime);
					}
				}
			}
		}
	}

	// Fallback to computed solar time if UE sun geometry not available
	if (!bUseSunSkyGeometry)
	{
		SolarDeclinationRad = ComputeSolarDeclinationRad(InDayOfYear);
		const float LocalSolarTimeHours = ComputeLocalSolarTimeHours(ActiveTimestamp, InDayOfYear, ActorLongitudeDeg);
		HourAngleRad = FMath::DegreesToRadians(15.0f * (LocalSolarTimeHours - 12.0f));

		static bool bLoggedFallback = false;
		if (!bLoggedFallback)
		{
			bLoggedFallback = true;
			UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] UE sun geometry not available, using computed solar time (LST=%.2fh)"), LocalSolarTimeHours);
		}
	}


	// Compute CosZenith for global use (needed for calibrated radiation logic)
	float GlobalCosZenith = 0.0f;
	if (bUseSunSkyGeometry)
	{
		GlobalCosZenith = ActorPtr->GetCachedCaptureCosSolarZenith();
	}
	else
	{
		float OutCosZenith, OutSinZenith, OutAz;
		ComputeSolarAngles(CellLatitudeRad.IsValidIndex(0) ? CellLatitudeRad[0] : 0.0f, SolarDeclinationRad, HourAngleRad, OutCosZenith, OutSinZenith, OutAz);
		GlobalCosZenith = OutCosZenith;
	}

	// Build forcing radiation once per step so diagnostics, indices, and calibration stay in sync.
	const float StepSolarZenithRad = FMath::Acos(FMath::Clamp(GlobalCosZenith, -1.0f, 1.0f));
	FForcingRadiation StepForcingRad(W.SWdown_Wm2, StepSolarZenithRad);
	if (W.DirectSWdown_Wm2 >= 0.0f && W.DiffuseSWdown_Wm2 >= 0.0f)
	{
		StepForcingRad.DNI = (GlobalCosZenith > 0.01f) ? (W.DirectSWdown_Wm2 / GlobalCosZenith) : 0.0f;
		StepForcingRad.DHI = W.DiffuseSWdown_Wm2;
		StepForcingRad.DiffuseFraction = (StepForcingRad.GHI > KINDA_SMALL_NUMBER)
			? (StepForcingRad.DHI / StepForcingRad.GHI)
			: 0.0f;
	}
	else
	{
		ComputeDNI_DHI(StepForcingRad);
	}

	if (bDirectOnlyRadiationIndex)
	{
		StepForcingRad.DNI = 0.0f;
		StepForcingRad.DHI = 0.0f;
		StepForcingRad.DiffuseFraction = 0.0f;

		static bool bLoggedDirectOnly = false;
		if (!bLoggedDirectOnly)
		{
			bLoggedDirectOnly = true;
			UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] Direct-only radiation index enabled: DNI/DHI disabled; melt uses GHI only."));
		}
	}

	if (bTotalOnlyRadiationIndex && !bDirectOnlyRadiationIndex)
	{
		StepForcingRad.DNI = 0.0f;
		StepForcingRad.DHI = 0.0f;
		StepForcingRad.DiffuseFraction = 0.0f;

		static bool bLoggedTotalOnly = false;
		if (!bLoggedTotalOnly)
		{
			bLoggedTotalOnly = true;
			UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] Total-only radiation index enabled: split indices ignored; melt uses GHI only."));
		}
	}

	LastTimestepForcingRad = StepForcingRad;
	bLastTimestepHadValidRadiationForcing = (StepForcingRad.GHI > 0.0f || StepForcingRad.DNI > 0.0f || StepForcingRad.DHI > 0.0f);

	const float StepDNI = StepForcingRad.DNI;
	const float StepDHI = StepForcingRad.DHI;
	const float StepDNI_Horiz = StepDNI * FMath::Max(0.0f, GlobalCosZenith);
	const float GlobalSunElevationDeg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(GlobalCosZenith, -1.0f, 1.0f)));

	const float EarthSunFactor = 1.0f + 0.033f * FMath::Cos(2.0f * PI * static_cast<float>(InDayOfYear) / 365.0f);
	const float ExtraterrestrialNormal = 1361.0f * EarthSunFactor;

	// Debug logging for first step with precipitation
	static bool bLoggedFirstPrecip = false;
	if (!bLoggedFirstPrecip && PrecipBase_mm > 0.01f && BaseTempC < 2.0f)
	{
		bLoggedFirstPrecip = true;
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] First cold precip: TempC=%.2f, PrecipBase_mm=%.4f, DtSec=%.0f, CellCount=%d"),
			BaseTempC, PrecipBase_mm, DtSeconds, CellCount);
	}

	float StepSnowFractionForRedistribution = 0.0f;
	if (PrecipBase_mm > 0.0f)
	{
		StepSnowFractionForRedistribution = ResolveSnowFraction(BaseTempC);
	}

	const float StepSnowfallForRedistribution_mm = PrecipBase_mm * StepSnowFractionForRedistribution;
	const bool bUseRedistributionThisStep = bApplySlopeCurvatureRedistribution
		&& StepSnowfallForRedistribution_mm >= MinSnowfallForRedistribution_mm;

	// ========== MASS CONSERVATION: PASS 1 - Compute redistribution normalization factor ==========
	float ConservationFactor = 1.0f;
	if (bUseRedistributionThisStep && bConserveMassDuringRedistribution && PrecipBase_mm > 0.0f)
	{
		float TotalPrecipBeforeRedist = 0.0f;
		float TotalPrecipAfterRedist = 0.0f;

		for (int32 Idx = 0; Idx < CellCount; ++Idx)
		{
			const bool bEdgeCell = bExcludeEdgeCellsFromMassConservation && IsRedistributionEdgeCell(Idx);
			const float AreaSqMeters = CellAreaSqMeters.IsValidIndex(Idx) ? CellAreaSqMeters[Idx] : 0.0f;
			float AltitudeCm = CellAltitudeCm.IsValidIndex(Idx) ? (CellAltitudeCm[Idx] - MeasurementAltitude) : 0.0f;
			if (!FMath::IsFinite(AltitudeCm) || FMath::Abs(AltitudeCm) > MaxReasonableAbsAltitudeDeltaCm)
			{
				if (!bLoggedAltitudeDeltaOutlier)
				{
					bLoggedAltitudeDeltaOutlier = true;
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] Ignoring unrealistic altitude delta in lapse correction (idx=%d, Delta=%.2f cm, CellAlt=%.2f cm, MeasurementAlt=%.2f cm)."),
						Idx,
						AltitudeCm,
						CellAltitudeCm.IsValidIndex(Idx) ? CellAltitudeCm[Idx] : 0.0f,
						MeasurementAltitude);
				}
				AltitudeCm = 0.0f;
			}
			const float Inclination = CellInclinationRad.IsValidIndex(Idx) ? CellInclinationRad[Idx] : 0.0f;

			float LocalPrecip_mm = PrecipBase_mm;
			if (!bDisableLapseRateAdjustments && (bApplyPrecipLapseBelowStation || AltitudeCm > 0.0f))
			{
				const float AltitudeKm = AltitudeCm / 100000.0f;
				const float PrecipScale = FMath::Max(0.0f, 1.0f + PrecipitationLapseRate_FractionPerKm * AltitudeKm);
				LocalPrecip_mm *= PrecipScale;
			}
			LocalPrecip_mm = FMath::Max(0.0f, LocalPrecip_mm);

			// Calculate snow fraction for this cell
			float LocalTemperatureC = BaseTempC;
			if (!bDisableLapseRateAdjustments)
			{
				LocalTemperatureC += (-0.5f * AltitudeCm) / (100.0f * 100.0f);
			}

			const float SnowRateUsed = (LocalPrecip_mm > 0.0f)
				? ResolveSnowFraction(LocalTemperatureC)
				: 0.0f;

			float SnowGainLiters = LocalPrecip_mm * AreaSqMeters * SnowRateUsed;

			// Apply slope filtering
			if (TerrainSlopeDegrees.IsValidIndex(Idx) && TerrainSlopeDegrees[Idx] > SlopeThreshold)
			{
				SnowGainLiters = 0.0f;
			}

			const float RedistFactor = bUseRedistributionThisStep ? ComputeSnowRedistributionFactor(Idx, Inclination) : 1.0f;
			if (!bEdgeCell)
			{
				TotalPrecipBeforeRedist += SnowGainLiters;
				TotalPrecipAfterRedist += SnowGainLiters * RedistFactor;
			}
		}

		// Compute normalization factor to conserve mass
		if (TotalPrecipAfterRedist > KINDA_SMALL_NUMBER)
		{
			ConservationFactor = TotalPrecipBeforeRedist / TotalPrecipAfterRedist;
		}

		// Log mass conservation info periodically
		static int32 LogCounter = 0;
		if (LogCounter++ % 100 == 0 && TotalPrecipBeforeRedist > 0.0f)
		{
			UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Mass Conservation: Before=%.2f L, After=%.2f L, Factor=%.4f"),
				TotalPrecipBeforeRedist, TotalPrecipAfterRedist, ConservationFactor);
		}

		// Store conservation factor for diagnostics
		LastMassConservationFactor = ConservationFactor;
	}

	const bool bUsePellicciottiModel = (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3_UE_SWR);
	const bool bUsePellicciottiPBR = (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_PBR);
	const bool bUsePellicciottiFluxCalibrated = (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated);

	// ========== PASS 2 - Main simulation loop with normalized redistribution ==========
	// Parallelized: each iteration only reads/writes arrays at [Idx]. Static-bool "log-once"
	// patterns inside may race benignly (at worst a few extra log lines).
	ParallelFor(CellCount, [&](int32 Idx)
	{
		float& SWE = CellSnowWaterEquivalentLiters[Idx];
		float& SnowAlbedoState = CellSnowAlbedoState[Idx];
		float& DaysSinceSnow = CellDaysSinceSnowfall[Idx];

			const float AreaSqMeters = CellAreaSqMeters.IsValidIndex(Idx) ? CellAreaSqMeters[Idx] : 0.0f;
			float AltitudeCm = CellAltitudeCm.IsValidIndex(Idx) ? (CellAltitudeCm[Idx] - MeasurementAltitude) : 0.0f;
			if (!FMath::IsFinite(AltitudeCm) || FMath::Abs(AltitudeCm) > MaxReasonableAbsAltitudeDeltaCm)
			{
				if (!bLoggedAltitudeDeltaOutlier)
				{
					bLoggedAltitudeDeltaOutlier = true;
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] Ignoring unrealistic altitude delta in lapse correction (idx=%d, Delta=%.2f cm, CellAlt=%.2f cm, MeasurementAlt=%.2f cm)."),
						Idx,
						AltitudeCm,
						CellAltitudeCm.IsValidIndex(Idx) ? CellAltitudeCm[Idx] : 0.0f,
						MeasurementAltitude);
				}
				AltitudeCm = 0.0f;
			}
			const float Inclination = CellInclinationRad.IsValidIndex(Idx) ? CellInclinationRad[Idx] : 0.0f;
			const float Aspect = CellAspectRad.IsValidIndex(Idx) ? CellAspectRad[Idx] : 0.0f;
			const float Latitude = CellLatitudeRad.IsValidIndex(Idx) ? CellLatitudeRad[Idx] : 0.0f;
			const float RedistributionFactor = bUseRedistributionThisStep ? ComputeSnowRedistributionFactor(Idx, Inclination) : 1.0f;
			const bool bEdgeCell = bExcludeEdgeCellsFromMassConservation && IsRedistributionEdgeCell(Idx);
			const float EffectiveConservationFactor = bEdgeCell ? 1.0f : ConservationFactor;

		float LocalTemperatureC = BaseTempC;
		float LocalPrecip_mm = PrecipBase_mm;

		// Temperature and precipitation lapse rates (Premoze / BlÃ¶schl inspired)
		if (!bDisableLapseRateAdjustments)
		{
			LocalTemperatureC += (-0.5f * AltitudeCm) / (100.0f * 100.0f);

			// Precipitation lapse: by default apply for both above and below station.
			// When disabled for below-station, only increase precip for cells above measurement station.
			if (bApplyPrecipLapseBelowStation || AltitudeCm > 0.0f)
			{
				const float AltitudeKm = AltitudeCm / 100000.0f;
				const float PrecipScale = FMath::Max(0.0f, 1.0f + PrecipitationLapseRate_FractionPerKm * AltitudeKm);
				LocalPrecip_mm *= PrecipScale;
			}
		}
		LocalPrecip_mm = FMath::Max(0.0f, LocalPrecip_mm);

		// Track daily maximum local temperature for Pellicciotti albedo parameterisation
		if (CellDailyMaxTemp.IsValidIndex(Idx))
		{
			CellDailyMaxTemp[Idx] = FMath::Max(CellDailyMaxTemp[Idx], LocalTemperatureC);
		}

		// Debug log for first cold precip event on cell 0
		if (!bLoggedFirstPrecip && Idx == 0 && LocalPrecip_mm > 0.0f && LocalTemperatureC < 2.0f)
		{
			UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Cell[0]: Area=%.2f mÂ², Alt=%.0f cm (%.0f m above station), LocalTemp=%.2f C, LocalPrecip=%.4f mm"),
				AreaSqMeters, CellAltitudeCm[Idx], AltitudeCm/100.0f, LocalTemperatureC, LocalPrecip_mm);
		}

		if (CellLastLocalAirTempC.IsValidIndex(Idx))
		{
			CellLastLocalAirTempC[Idx] = LocalTemperatureC;
		}

		float AccumDepth_m = 0.0f;
		float MeltDepth_m = 0.0f;
		float ComputedSnowRate = 0.0f;  // Snow fraction actually used this step
		uint8 bSlopeFiltered = 0;
		const float WeatherSnowFracRaw = FMath::Clamp(W.SnowFrac_01, 0.0f, 1.0f);
		float EffectiveSnowFrac = 0.0f;

		if (LocalPrecip_mm > 0.0f)
		{
			// Don't reset DaysSinceSnow yet - only reset if actual snow falls
			float SnowRateUsed = ResolveSnowFraction(LocalTemperatureC);
			if (!bHasUsableExplicitPrecipitation && !bUseWeatherSnowFraction && LocalTemperatureC > TSnowB)
			{
				// Rain event - reduce albedo when the temperature heuristic classifies all precip as rain.
				SnowAlbedoState = OldSnowAlbedo;
			}
			else if (false)
			{
				if (LocalTemperatureC > TSnowB)
				{
					// Rain event â reduce albedo
					SnowAlbedoState = OldSnowAlbedo;
					SnowRateUsed = 0.0f;
				}
				else
				{
					float SnowRate = 1.0f;
					const float Denom = (TSnowB - TSnowA);
					if (FMath::Abs(Denom) > KINDA_SMALL_NUMBER)
					{
						SnowRate = FMath::Clamp(1.0f - (LocalTemperatureC - TSnowA) / Denom, 0.0f, 1.0f);
					}
					else
					{
						SnowRate = (LocalTemperatureC <= TSnowA) ? 1.0f : 0.0f;
					}

					SnowRateUsed = SnowRate;
				}
			}

			SnowRateUsed = FMath::Clamp(SnowRateUsed, 0.0f, 1.0f);
			EffectiveSnowFrac = SnowRateUsed;

			const float RainFraction = 1.0f - SnowRateUsed;
			// For Pellicciotti Model D variants, keep albedo unchanged on rain/mixed events (paper formulation)
			if (DegreeDayMeltModel != EDegreeDayMeltModel::PellicciottiModelD && DegreeDayMeltModel != EDegreeDayMeltModel::PellicciottiModelD_RI && DegreeDayMeltModel != EDegreeDayMeltModel::PellicciottiModelD_PBR && DegreeDayMeltModel != EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated && DegreeDayMeltModel != EDegreeDayMeltModel::HockModel3_UE_SWR)
			{
				if (SnowRateUsed <= 0.0f)
				{
					SnowAlbedoState = OldSnowAlbedo;
				}
				else if (SnowRateUsed < 1.0f && RainFraction >= 0.75f)
				{
					// Mixed precipitation: degrade albedo when rain dominates
					SnowAlbedoState = OldSnowAlbedo;
				}
			}

			float SnowGainLiters = LocalPrecip_mm * AreaSqMeters * SnowRateUsed;
			if (TerrainSlopeDegrees.IsValidIndex(Idx) && TerrainSlopeDegrees[Idx] > SlopeThreshold)
			{
				SnowGainLiters = 0.0f;
				bSlopeFiltered = 1;
			}

			SnowGainLiters *= RedistributionFactor * EffectiveConservationFactor;

			if (SnowGainLiters > 0.0f)
			{
				SWE += SnowGainLiters;
				SnowAlbedoState = FreshSnowAlbedo;
				if (CellAlbedoTempSum.IsValidIndex(Idx))
				{
					CellAlbedoTempSum[Idx] = 0.0f;
					if (CellDailyMaxTemp.IsValidIndex(Idx))
					{
						CellDailyMaxTemp[Idx] = FMath::Max(0.0f, LocalTemperatureC);
					}
				}
				AccumDepth_m = ConvertSWEToDepthMeters(SnowGainLiters, AreaSqMeters);

				// Reset albedo aging timer only when actual snow falls
				DaysSinceSnow = 0.0f;
			}

			ComputedSnowRate = SnowRateUsed;
		}

		// Age albedo after potential snowfall
		if (SWE > 0.0f)
		{
			if (bUsePellicciottiModel)
			{
				const float TempSumBase = CellAlbedoTempSum.IsValidIndex(Idx) ? CellAlbedoTempSum[Idx] : 0.0f;
				const float CurrentDayMax = CellDailyMaxTemp.IsValidIndex(Idx) ? FMath::Max(0.0f, CellDailyMaxTemp[Idx]) : 0.0f;
				const float TempAccumulator = FMath::Max(PellicciottiAlbedoMinTempSum, TempSumBase + CurrentDayMax);
				const float TargetTempForOld = FMath::Max(PellicciottiAlbedoTempSumForOldSnow, PellicciottiAlbedoMinTempSum);
				const float LogDenom = FMath::Max(KINDA_SMALL_NUMBER, FMath::LogX(10.0f, TargetTempForOld));
				const float p1 = FreshSnowAlbedo;
				const float p2 = (OldSnowAlbedo - p1) / LogDenom;
				const float SnowAlbedoComputed = p1 + p2 * FMath::LogX(10.0f, TempAccumulator);
				const float AlbedoMin = FMath::Min(OldSnowAlbedo, p1);
				const float AlbedoMax = FMath::Max(OldSnowAlbedo, p1);
				SnowAlbedoState = FMath::Clamp(SnowAlbedoComputed, AlbedoMin, AlbedoMax);
			}
			else
			{
				const float ExpTerm = FMath::Exp(-k_e * FMath::Max(0.0f, DaysSinceSnow));
				const float TargetAlbedo = OldSnowAlbedo * (1.0f + ExpTerm);
				SnowAlbedoState = FMath::Clamp(TargetAlbedo, OldSnowAlbedo, FreshSnowAlbedo);
			}
		}
		else
		{
			SnowAlbedoState = bUsePellicciottiModel ? PellicciottiIceAlbedo : 0.0f;
		}

		// Always compute Swift radiation for diagnostics/comparison
		float RadiationIndex_Swift = ComputeSolarRadiationIndex(Inclination, Aspect, Latitude, static_cast<float>(InDayOfYear));

		float RadiationMetricUsed = RadiationIndex_Swift;
		float PellicciottiRadiationIndex = 1.0f;
		float StoredRadiationIndexUE = 0.0f;
		float RadiationIndex_UE_Raw = 0.0f;  // Default to 0 (not 1) - will be set if valid data available
		float RadiationIndex_UE = 0.0f;      // Default to 0 (not 1) - will be set if valid data available

		// Separate component variables
		float RadiationDirect_UE = 0.0f;
		float RadiationDiffuse_UE = 0.0f;
		float RadiationTotal_UE = 0.0f;
		float RadiationTotal_UE_GHIScaled = 0.0f;
		float RadiationTerrain_UE = 0.0f;
		float RadiationIndex_Direct = 0.0f;
		float RadiationIndex_Diffuse = 0.0f;
		const bool bUseDualReferenceStrip = ActorPtr && ActorPtr->bUseDualReferenceStrip;
		const float SnowDepth = DepthMeters.IsValidIndex(Idx) ? DepthMeters[Idx] : 0.0f;
		const bool bPreferTotalReferencePlausibility = ActorPtr && ActorPtr->bUseTotalRadiationIndexOnly;
		bool bUsedRenderSurfaceStateReference = false;
		bool bUsedDualReferencePlausibilityOverride = false;
		const float DualReferenceSnowBlendWeight = DD_ResolveDualReferenceSnowBlendWeight(
			ActorPtr,
			Idx,
			SnowDepth,
			bPreferTotalReferencePlausibility,
			bUsedRenderSurfaceStateReference,
			bUsedDualReferencePlausibilityOverride);
		(void)bUsedRenderSurfaceStateReference;
		(void)bUsedDualReferencePlausibilityOverride;
		float ReferenceLuminanceTotalUsed = 0.0f;
		float ReferenceLuminanceDirectUsed = 0.0f;
		float ReferenceLuminanceDiffuseUsed = 0.0f;
		float ReferenceLuminanceDiffuseNoGIUsed = 0.0f;
		float ReferenceLuminanceTotalNoGIUsed = 0.0f;
		float ReferenceLuminanceTerrainUsed = 0.0f;
		float ReferenceScaleTotal = 0.0f;
		float ReferenceScaleDirect = 0.0f;
		float ReferenceScaleDiffuse = 0.0f;
		float ReferenceScaleTotalNoGI = 0.0f;
		bool bReferenceBaseValid = false;
		bool bReferenceTotalValid = false;
		bool bReferenceDirectValid = false;
		bool bReferenceDiffuseValid = false;
		bool bSkyOnlyReferenceUsable = false;
		bool bSkyOnlyReferenceMeetsMinLuminance = false;
		bool bUseSkyOnlyDiffuseScaling = false;

		if (ActorPtr)
		{
			ReferenceLuminanceDirectUsed = bUseDualReferenceStrip
				? DD_BlendDualReferenceValue(
					ActorPtr->ReferenceLuminance_Direct_Ground,
					ActorPtr->ReferenceLuminance_Direct_Snow,
					DualReferenceSnowBlendWeight,
					ActorPtr->ReferenceLuminance_Direct)
				: ActorPtr->ReferenceLuminance_Direct;
			ReferenceLuminanceTotalUsed = bUseDualReferenceStrip
				? DD_BlendDualReferenceValue(
					ActorPtr->ReferenceLuminance_Total_Ground,
					ActorPtr->ReferenceLuminance_Total_Snow,
					DualReferenceSnowBlendWeight,
					ActorPtr->ReferenceLuminance_Total)
				: ActorPtr->ReferenceLuminance_Total;
			ReferenceLuminanceDiffuseUsed = bUseDualReferenceStrip
				? DD_BlendDualReferenceValue(
					ActorPtr->ReferenceLuminance_Diffuse_Ground,
					ActorPtr->ReferenceLuminance_Diffuse_Snow,
					DualReferenceSnowBlendWeight,
					ActorPtr->ReferenceLuminance_Diffuse)
				: ActorPtr->ReferenceLuminance_Diffuse;
			ReferenceLuminanceDiffuseNoGIUsed = bUseDualReferenceStrip
				? DD_BlendDualReferenceValue(
					ActorPtr->ReferenceLuminance_DiffuseNoGI_Ground,
					ActorPtr->ReferenceLuminance_DiffuseNoGI_Snow,
					DualReferenceSnowBlendWeight,
					ActorPtr->ReferenceLuminance_DiffuseNoGI)
				: ActorPtr->ReferenceLuminance_DiffuseNoGI;
			ReferenceLuminanceTotalNoGIUsed = bUseDualReferenceStrip
				? DD_BlendDualReferenceValue(
					ActorPtr->ReferenceLuminance_TotalNoGI_Ground,
					ActorPtr->ReferenceLuminance_TotalNoGI_Snow,
					DualReferenceSnowBlendWeight,
					ActorPtr->ReferenceLuminance_TotalNoGI)
				: ActorPtr->ReferenceLuminance_TotalNoGI;

			// Sanitize reference channels before diagnostics/scaling.
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

			bSkyOnlyReferenceUsable = HasUsableSkyOnlyReference(
				ReferenceLuminanceDiffuseNoGIUsed,
				ReferenceLuminanceDiffuseUsed,
				ReferenceLuminanceTotalUsed);
			bSkyOnlyReferenceMeetsMinLuminance = ReferenceLuminanceDiffuseNoGIUsed >= ReferenceStripMinLuminance;
			bUseSkyOnlyDiffuseScaling = (bSkyOnlyReferenceUsable && bSkyOnlyReferenceMeetsMinLuminance)
				&& ReferenceLuminanceTotalNoGIUsed > 1e-6f;

			ReferenceLuminanceTerrainUsed = bUseSkyOnlyDiffuseScaling
				? FMath::Max(ReferenceLuminanceTotalUsed - ReferenceLuminanceTotalNoGIUsed, 0.0f)
				: FMath::Max(ReferenceLuminanceTotalUsed - ReferenceLuminanceDirectUsed - ReferenceLuminanceDiffuseUsed, 0.0f);
			bReferenceBaseValid = ActorPtr->bReferenceLuminanceValid;
			bReferenceTotalValid = bReferenceBaseValid;
			bReferenceDirectValid = bReferenceBaseValid;
			bReferenceDiffuseValid = bReferenceBaseValid;

			if (bReferenceBaseValid && bUseReferenceStripGuard)
			{
				const bool bSunHighEnough = GlobalSunElevationDeg >= ReferenceStripMinSunElevation_deg;
				const float ReferenceDiffuseForGuard = bUseSkyOnlyDiffuseScaling
					? ReferenceLuminanceTotalNoGIUsed
					: ReferenceLuminanceDiffuseUsed;
				bReferenceTotalValid = bSunHighEnough && ReferenceLuminanceTotalUsed >= ReferenceStripMinLuminance;
				bReferenceDirectValid = bSunHighEnough && ReferenceLuminanceDirectUsed >= ReferenceStripMinLuminance;
				bReferenceDiffuseValid = ReferenceDiffuseForGuard >= ReferenceStripMinLuminance;
			}
		}

		const bool bUseSplitPellicciottiLambertian = bUseSplitRadiationForPellicciotti
			&& DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD
			&& RadiationIndexMethod == ERadiationIndexMethod::UnrealEngine
			&& !bDirectOnlyRadiationIndex
			&& !bTotalOnlyRadiationIndex;

		bool bHasUERadiation = false;

		if (DegreeDayMeltModel == EDegreeDayMeltModel::Enhanced || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3_UE_SWR || bUseSplitPellicciottiLambertian)
		{
			static bool bLoggedRadiationStatus = false;
			if (!bLoggedRadiationStatus)
			{
				if (!ActorPtr)
				{
					UE_LOG(LogTemp, Error, TEXT("[DegreeDay] ===== UE RADIATION DIAGNOSTIC ====="));
					UE_LOG(LogTemp, Error, TEXT("[DegreeDay]   CRITICAL: ActorPtr is NULL"));
					UE_LOG(LogTemp, Error, TEXT("[DegreeDay]   Cannot access UE radiation indices"));
					UE_LOG(LogTemp, Error, TEXT("[DegreeDay]   Impact: Falling back to RadIdx=1.0 (no UE radiation)"));
					UE_LOG(LogTemp, Error, TEXT("[DegreeDay] ===================================="));
					bLoggedRadiationStatus = true;
				}
				else
				{
					const bool bHasValid = ActorPtr->HasValidRadiationIndices();
					const TArray<float>& DirectIndices = ActorPtr->GetCachedDirectIndices();
					const TArray<float>& TotalIndices = ActorPtr->GetCachedRadiationIndices();
					const bool bUseDirectIndices = bDirectOnlyRadiationIndex && DirectIndices.Num() == TotalIndices.Num() && DirectIndices.Num() > 0;
					const TArray<float>& RadIndices = bUseDirectIndices ? DirectIndices : TotalIndices;

					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] ===== UE RADIATION DIAGNOSTIC ====="));
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay]   Selected Method: %s"),
						RadiationIndexMethod == ERadiationIndexMethod::UnrealEngine ? TEXT("UnrealEngine") : TEXT("Swift (Geometric)"));
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay]   DirectOnlyIndex: %s"), bDirectOnlyRadiationIndex ? TEXT("TRUE") : TEXT("FALSE"));
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay]   TotalOnlyIndex: %s"), bTotalOnlyRadiationIndex ? TEXT("TRUE") : TEXT("FALSE"));
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay]   Using Indices: %s"), bUseDirectIndices ? TEXT("Direct") : TEXT("Total"));
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay]   HasValidRadiationIndices(): %s"), bHasValid ? TEXT("TRUE") : TEXT("FALSE"));
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay]   CachedRadiationIndices.Num(): %d"), RadIndices.Num());
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay]   Expected cell count: %d"), CellCount);

					if (!bHasValid)
					{
						UE_LOG(LogTemp, Warning, TEXT("[DegreeDay]   >>> UE RADIATION UNAVAILABLE <<<"));
						if (RadiationIndexMethod == ERadiationIndexMethod::UnrealEngine)
						{
							UE_LOG(LogTemp, Error, TEXT("[DegreeDay]   >>> METHOD MISMATCH: UnrealEngine selected but not available <<<"));
							UE_LOG(LogTemp, Error, TEXT("[DegreeDay]   FIX: Enable bEnableRadiationCapture in SnowSimulationActor Blueprint"));
							UE_LOG(LogTemp, Error, TEXT("[DegreeDay]        Path: Snow|Radiation -> bEnableRadiationCapture = true"));
						}
						UE_LOG(LogTemp, Warning, TEXT("[DegreeDay]   Current behavior: Using Swift geometric method (RadIdx varies by slope/aspect only)"));
					}
					else
					{
						float SampleSum = 0.0f;
						int32 SampleCount = FMath::Min(RadIndices.Num(), 10);
						for (int32 i = 0; i < SampleCount; ++i)
						{
							SampleSum += RadIndices[i];
						}
						float SampleAvg = SampleSum / FMath::Max(SampleCount, 1);

						UE_LOG(LogTemp, Display, TEXT("[DegreeDay]   >>> UE RADIATION ACTIVE <<<"));
						UE_LOG(LogTemp, Display, TEXT("[DegreeDay]   Sample average (first %d cells): %.3f"), SampleCount, SampleAvg);
						if (FMath::IsNearlyEqual(SampleAvg, 1.0f, 0.001f))
						{
							UE_LOG(LogTemp, Warning, TEXT("[DegreeDay]   WARNING: All sampled values are 1.0 - may indicate extraction issue"));
						}
					}
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] ===================================="));
					bLoggedRadiationStatus = true;
				}
			}

			if (ActorPtr && ActorPtr->HasValidRadiationIndices())
			{
				const TArray<float>& DirectIndices = ActorPtr->GetCachedDirectIndices();
				const TArray<float>& TotalIndices = ActorPtr->GetCachedRadiationIndices();
				const bool bUseDirectIndices = bDirectOnlyRadiationIndex && DirectIndices.Num() == TotalIndices.Num() && DirectIndices.Num() > 0;
				const TArray<float>& RadIndices = bUseDirectIndices ? DirectIndices : TotalIndices;
				if (RadIndices.IsValidIndex(Idx))
				{
					if (bUseDualReferenceStrip)
					{
						if (bUseDirectIndices)
						{
							const float RTYDirect = CellLastRTY_Direct.IsValidIndex(Idx) ? CellLastRTY_Direct[Idx] : 0.0f;
							const float SafeRefDirect = FMath::Max(ReferenceLuminanceDirectUsed, 1e-6f);
							RadiationIndex_UE_Raw = RTYDirect / SafeRefDirect;
						}
						else
						{
							const float RTYTotal = CellLastRTY_Total.IsValidIndex(Idx) ? CellLastRTY_Total[Idx] : 0.0f;
							const float SafeRefTotal = FMath::Max(ReferenceLuminanceTotalUsed, 1e-6f);
							RadiationIndex_UE_Raw = RTYTotal / SafeRefTotal;
						}
					}
					else
					{
						RadiationIndex_UE_Raw = RadIndices[Idx];
					}

					RadiationIndex_UE = FMath::Clamp(RadiationIndex_UE_Raw, 0.0f, RadiationIndexClampMax);
					bHasUERadiation = true;

					// Apply Hock (1999) low sun angle suppression directly to RadiationIndex_UE
					// This ensures the suppression is visible in diagnostics and stored values
					if (bUseLowSunAngleFallback)
					{
						const float CosZenith = FMath::Clamp(
							FMath::Sin(Latitude) * FMath::Sin(SolarDeclinationRad) +
							FMath::Cos(Latitude) * FMath::Cos(SolarDeclinationRad) * FMath::Cos(HourAngleRad),
							-1.0f, 1.0f);
						const float SolarZenith_rad = FMath::Acos(CosZenith);
						const float SunElevation_deg = FMath::RadiansToDegrees(HALF_PI - SolarZenith_rad);
						const float Threshold_deg = LowSunAngleThreshold_deg;

						if (SunElevation_deg < Threshold_deg)
						{
							// Suppress radiation ratio per Hock (1999) - treat as shaded
							RadiationIndex_UE = 0.0f;
							RadiationIndex_UE_Raw = 0.0f;

							static int32 SuppressionLogCount = 0;
							if (SuppressionLogCount < 3 && Idx < 10)
							{
								SuppressionLogCount++;
								UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] Cell %d: Suppressing UE radiation at low sun angle (%.2fÂ° < %.2fÂ°). RadIdx_UE set to 0."),
									Idx, SunElevation_deg, Threshold_deg);
							}
						}
					}

					static int32 LogCounter = 0;
					if (LogCounter < 5 && Idx < 10)
					{
						UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Cell %d: RadIdx_UE=%.4f, RadIdx_Swift=%.4f (using UE radiation)"),
							Idx, RadiationIndex_UE, RadiationIndex_Swift);
						LogCounter++;
					}
				}
				else if (bEnableHotPathLogs)
				{
					UE_LOG(LogTemp, Error, TEXT("[DegreeDay] CRITICAL: Invalid cell index %d (CachedRadiationIndices.Num()=%d)"),
						Idx, RadIndices.Num());
				}
			}

	// Deprecated scaling logic removed

			if (DegreeDayMeltModel == EDegreeDayMeltModel::Enhanced)
			{
				if (RadiationIndexMethod == ERadiationIndexMethod::UnrealEngine)
				{
					// RadiationIndex_UE already has Hock (1999) low sun angle suppression applied above
					// (set to 0 when sun elevation < threshold)
					if (bHasUERadiation)
					{
						RadiationMetricUsed = RadiationIndex_UE;
					}
					else
					{
						static bool bLoggedMissingCache = false;
						if (!bLoggedMissingCache)
						{
							bLoggedMissingCache = true;
							UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] UnrealEngine radiation method selected but no cached indices available. Using Swift fallback."));
						}
						RadiationMetricUsed = RadiationIndex_Swift;
					}
				}
				else
				{
					RadiationMetricUsed = RadiationIndex_Swift;

					static int32 LogCount = 0;
					if (LogCount < 5 && Idx < 3)
					{
						LogCount++;
						UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Swift radiation cell %d: RadIdx=%.4f, Incl=%.4f rad (%.1f deg), Aspect=%.4f rad (%.1f deg), Lat=%.4f rad (%.1f deg), DayOfYear=%d"),
							Idx, RadiationIndex_Swift,
							Inclination, FMath::RadiansToDegrees(Inclination),
							Aspect, FMath::RadiansToDegrees(Aspect),
							Latitude, FMath::RadiansToDegrees(Latitude),
							InDayOfYear);
					}

					static bool bLoggedZeroRadiation = false;
					if (!bLoggedZeroRadiation && SWE > 0.0f && FMath::IsNearlyZero(RadiationIndex_Swift))
					{
						bLoggedZeroRadiation = true;
						UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] RadiationIndex_Swift=0.0 for cell %d: Inclination=%.4f rad, Aspect=%.4f rad, Latitude=%.4f rad, DayOfYear=%d"),
							Idx, Inclination, Aspect, Latitude, InDayOfYear);
					}
				}
			}

			StoredRadiationIndexUE = bHasUERadiation ? RadiationIndex_UE : 0.0f;
			
			// Calculate separate direct/diffuse diagnostics if available
			if (bHasUERadiation && ActorPtr && !bDirectOnlyRadiationIndex && !bTotalOnlyRadiationIndex)
			{
				const TArray<float>& DirectIndices = ActorPtr->GetCachedDirectIndices();
				const TArray<float>& DiffuseIndices = ActorPtr->GetCachedDiffuseIndices();

				const float DNI_Horiz = StepDNI_Horiz;
				const float DHI = StepDHI;

				// Baseline: derive split indices from RTY luminance and reference means.
				const float RTYTotal = CellLastRTY_Total.IsValidIndex(Idx) ? CellLastRTY_Total[Idx] : 0.0f;
				const float RTYDirect = CellLastRTY_Direct.IsValidIndex(Idx) ? CellLastRTY_Direct[Idx] : 0.0f;
				const float RTYDiffuse = CellLastRTY_Diffuse.IsValidIndex(Idx) ? CellLastRTY_Diffuse[Idx] : 0.0f;
				const float RTYDiffuseNoGI = (bHasDiffuseNoGI && CellLastRTY_DiffuseNoGI.IsValidIndex(Idx)) ? CellLastRTY_DiffuseNoGI[Idx] : 0.0f;
				const float RTYTotalNoGI = CellLastRTY_TotalNoGI.IsValidIndex(Idx)
					? CellLastRTY_TotalNoGI[Idx]
					: (RTYDirect + RTYDiffuseNoGI);
				const float RTYTerrain = (bHasTerrainRTY && CellLastRTY_Terrain.IsValidIndex(Idx)) ? CellLastRTY_Terrain[Idx] : 0.0f;
				const float R_direct = FMath::Max(ReferenceLuminanceDirectUsed, 1e-6f);
				const float R_total = FMath::Max(ReferenceLuminanceTotalUsed, 1e-6f);
				const float R_total_nogi = FMath::Max(ReferenceLuminanceTotalNoGIUsed, 1e-6f);
				const float ReferenceDiffuseForScaling = bUseSkyOnlyDiffuseScaling
					? ReferenceLuminanceDiffuseNoGIUsed
					: ReferenceLuminanceDiffuseUsed;
				const float R_diffuse_raw = (ReferenceDiffuseForScaling > 0.0f)
					? ReferenceDiffuseForScaling
					: (R_total - R_direct);
				const float R_diffuse = FMath::Max(R_diffuse_raw, 1e-6f);
				const bool bUseTotalNoGIClosure = bUseSkyOnlyDiffuseScaling && CellLastRTY_TotalNoGI.IsValidIndex(Idx);

				float DirectIndex = (R_direct > 0.0f) ? (RTYDirect / R_direct) : 0.0f;
				const float DiffuseNumerator = (bUseSkyOnlyDiffuseScaling && bHasDiffuseNoGI) ? RTYDiffuseNoGI : RTYDiffuse;
				float DiffuseIndex = (R_diffuse > 0.0f) ? (DiffuseNumerator / R_diffuse) : 0.0f;

				// Prefer cached split indices if they look valid (single-strip mode only).
				if (!bUseDualReferenceStrip && !bHasDiffuseNoGI && DirectIndices.IsValidIndex(Idx) && DiffuseIndices.IsValidIndex(Idx))
				{
					const float CachedDirect = DirectIndices[Idx];
					const float CachedDiffuse = DiffuseIndices[Idx];
					if (CachedDirect > 0.0f || CachedDiffuse > 0.0f)
					{
						DirectIndex = CachedDirect;
						DiffuseIndex = CachedDiffuse;
					}
				}

				DirectIndex = FMath::Clamp(DirectIndex, 0.0f, RadiationIndexClampMax);
				DiffuseIndex = FMath::Clamp(DiffuseIndex, 0.0f, RadiationIndexClampMax);

				RadiationIndex_Direct = DirectIndex;
				RadiationDirect_UE = (DNI_Horiz > 0.1f) ? (DirectIndex * DNI_Horiz) : 0.0f;

				ReferenceScaleDirect = (bReferenceDirectValid && R_direct > 1e-6f && DNI_Horiz > 0.0f) ? (DNI_Horiz / R_direct) : 0.0f;
				ReferenceScaleDiffuse = (bReferenceDiffuseValid && R_diffuse > 1e-6f && DHI > 0.0f) ? (DHI / R_diffuse) : 0.0f;
				ReferenceScaleTotalNoGI = (bReferenceDiffuseValid && R_total_nogi > 1e-6f && SolarForcing_Wm2 > 0.0f)
					? (SolarForcing_Wm2 / R_total_nogi)
					: 0.0f;
				ReferenceScaleTotal = (bReferenceTotalValid && R_total > 1e-6f && SolarForcing_Wm2 > 0.0f) ? (SolarForcing_Wm2 / R_total) : 0.0f;
				RadiationTotal_UE_GHIScaled = (ReferenceScaleTotal > 0.0f && FMath::IsFinite(RTYTotal))
					? FMath::Max(0.0f, RTYTotal * ReferenceScaleTotal)
					: 0.0f;
				const float RadiationTotalNoGI_UE = (bUseTotalNoGIClosure && ReferenceScaleTotalNoGI > 0.0f && FMath::IsFinite(RTYTotalNoGI))
					? FMath::Max(0.0f, RTYTotalNoGI * ReferenceScaleTotalNoGI)
					: 0.0f;

				RadiationDiffuse_UE = bUseTotalNoGIClosure
					? FMath::Max(0.0f, RadiationTotalNoGI_UE - RadiationDirect_UE)
					: ((DHI > 0.1f) ? (DiffuseIndex * DHI) : 0.0f);
				DiffuseIndex = (DHI > 0.1f) ? (RadiationDiffuse_UE / DHI) : 0.0f;
				RadiationIndex_Diffuse = FMath::Clamp(DiffuseIndex, 0.0f, RadiationIndexClampMax);

				const bool bUseTerrainResidual = bUseTotalReferenceForTerrainResidual
					&& bHasTerrainRTY
					&& ReferenceScaleTotal > 0.0f
					&& ReferenceScaleTotal <= ReferenceStripMaxTotalScale;

				if (bUseTerrainResidual)
				{
					RadiationTerrain_UE = bUseTotalNoGIClosure
						? FMath::Max(0.0f, RadiationTotal_UE_GHIScaled - RadiationTotalNoGI_UE)
						: FMath::Max(0.0f, RTYTerrain * ((ReferenceScaleDirect > 0.0f) ? ReferenceScaleDirect : ReferenceScaleTotal));
					RadiationTotal_UE = RadiationDirect_UE + RadiationDiffuse_UE + RadiationTerrain_UE;
				}
				else
				{
					RadiationTerrain_UE = 0.0f;
					RadiationTotal_UE = RadiationDirect_UE + RadiationDiffuse_UE;
				}
			}
			else if (bHasUERadiation && ActorPtr && bDirectOnlyRadiationIndex)
			{
				// Direct-only mode: keep diagnostics but force melt to use GHI-only path.
				RadiationIndex_Direct = RadiationIndex_UE;
				RadiationIndex_Diffuse = 0.0f;
				RadiationDirect_UE = 0.0f;
				RadiationDiffuse_UE = 0.0f;
			}
			else if (bHasUERadiation && ActorPtr && bTotalOnlyRadiationIndex)
			{
				// Total-only mode: no split direct/diffuse indices.
				RadiationIndex_Direct = 0.0f;
				RadiationIndex_Diffuse = 0.0f;
				RadiationDirect_UE = 0.0f;
				RadiationDiffuse_UE = 0.0f;

				// For flux-calibrated model, compute RadiationTotal_UE from total RTY
				// so the melt path uses scene-based radiation instead of falling back to plain GHI.
				if (bUsePellicciottiFluxCalibrated)
				{
					const float R_total = FMath::Max(ReferenceLuminanceTotalUsed, 1e-6f);
					ReferenceScaleTotal = (bReferenceTotalValid && R_total > 1e-6f && SolarForcing_Wm2 > 0.0f)
						? (SolarForcing_Wm2 / R_total)
						: 0.0f;
					RadiationTotal_UE = (ReferenceScaleTotal > 0.0f && FMath::IsFinite(RadiationIndex_UE))
						? FMath::Max(0.0f, SolarForcing_Wm2 * RadiationIndex_UE)
						: 0.0f;
				}
			}
		}

		if (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3_UE_SWR)
		{
			// Legacy/Combined Index Fallback
			PellicciottiRadiationIndex = (RadiationIndexMethod == ERadiationIndexMethod::UnrealEngine && bHasUERadiation)
				? RadiationIndex_UE
				: RadiationIndex_Swift;
		}

		const float Absorptivity = (bUsePellicciottiPBR && !bUseAlbedoAbsorptivityInPBR)
			? 1.0f
			: FMath::Clamp(1.0f - SnowAlbedoState, 0.0f, 1.0f);
		float PotentialDirect_Wm2 = 0.0f;
		float PotentialHorizontal_Wm2 = 0.0f;
		float CloudinessRatio = 0.0f;  // G_s/I_s ratio (computed for all models for diagnostics)

		const bool bUsePellicciottiRI = (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3_UE_SWR);

		if (bUsePellicciottiRI)
		{
			// Re-evaluate index if needed, but we essentially just need it for legacy support or single-index logic
			PellicciottiRadiationIndex = (RadiationIndexMethod == ERadiationIndexMethod::UnrealEngine && bHasUERadiation)
				? RadiationIndex_UE
				: RadiationIndex_Swift;
		}

		// Compute potential radiation terms for ALL models (needed for diagnostics)
		PotentialDirect_Wm2 = ComputePotentialDirectRadiationHock(Latitude, Inclination, Aspect, SolarDeclinationRad, HourAngleRad, ExtraterrestrialNormal, LocalPressurePa);
		PotentialHorizontal_Wm2 = ComputePotentialDirectRadiationHock(Latitude, 0.0f, 0.0f, SolarDeclinationRad, HourAngleRad, ExtraterrestrialNormal, LocalPressurePa);

		if (bUseLowSunAngleFallback)
		{
			const float CosZenith = FMath::Clamp(
				FMath::Sin(Latitude) * FMath::Sin(SolarDeclinationRad) +
				FMath::Cos(Latitude) * FMath::Cos(SolarDeclinationRad) * FMath::Cos(HourAngleRad),
				-1.0f, 1.0f);
			const float SolarZenith_rad = FMath::Acos(CosZenith);
			const float SunElevation_deg = FMath::RadiansToDegrees(HALF_PI - SolarZenith_rad);
			const float Threshold_deg = LowSunAngleThreshold_deg;

			if (SunElevation_deg < Threshold_deg)
			{
				PotentialDirect_Wm2 = 0.0f;
				PotentialHorizontal_Wm2 = 0.0f;

				static int32 SuppressionLogCountPotential = 0;
				if (SuppressionLogCountPotential < 3 && Idx < 10)
				{
					SuppressionLogCountPotential++;
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] Cell %d: Suppressing potential direct radiation at low sun angle (%.2f deg < %.2f deg). I set to 0."),
						Idx, SunElevation_deg, Threshold_deg);
				}
			}
		}



		// Compute cloudiness ratio (GHI/Is_station) for ALL models for diagnostics
		// This represents the fraction of potential clear-sky radiation that reaches the surface
		{
			const float I_s_Wm2 = ComputePotentialDirectRadiationHock(Latitude, 0.0f, 0.0f, SolarDeclinationRad, HourAngleRad, ExtraterrestrialNormal, LocalPressurePa);
			const float CosStationZenith = FMath::Clamp(
				FMath::Sin(Latitude) * FMath::Sin(SolarDeclinationRad) +
				FMath::Cos(Latitude) * FMath::Cos(SolarDeclinationRad) * FMath::Cos(HourAngleRad),
				-1.0f, 1.0f);
			const float StationZenith_rad = FMath::Acos(CosStationZenith);
			const float StationSunElevation_deg = 90.0f - FMath::RadiansToDegrees(StationZenith_rad);
			const float RatioSunElevationThreshold_deg = 20.0f;
			const bool bRatioSunElevationOK = (StationSunElevation_deg > RatioSunElevationThreshold_deg);

			if (bRatioSunElevationOK && I_s_Wm2 > 1.0f && SolarForcing_Wm2 > 0.0f)
			{
				const float Ratio = SolarForcing_Wm2 / I_s_Wm2;
				CloudinessRatio = FMath::Clamp(Ratio, 0.0f, RadiationIndexClampMax);
			}
			else
			{
				if (!bRatioSunElevationOK)
				{
					CloudinessRatio = 0.0f;
				}
				else
				{
					CloudinessRatio = 1.0f;
				}
			}
		}

		// For HockModel2 and HockModel3Exact, override CloudinessRatio for melt calculation (not diagnostics)
		float CloudinessRatioForMelt = CloudinessRatio;
		if (DegreeDayMeltModel == EDegreeDayMeltModel::HockModel2)
		{
			CloudinessRatioForMelt = 1.0f;
		}

		float DiagnosticNetShortwave_Wm2 = 0.0f;
		float DiagnosticCloudiness = CloudinessRatio;  // Always use actual GHI/Is for diagnostics
		float DiagnosticRadiationIdx = bUsePellicciottiRI ? PellicciottiRadiationIndex : 1.0f;
		float RadiationComponent = 0.0f;
		float MeltRate_mmph = 0.0f;
		float MeltFactorDiag = 0.0f;

		if (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3_UE_SWR)
		{
			const float PositiveTempC = FMath::Max(0.0f, LocalTemperatureC - PellicciottiTempThresholdC);
			if (PositiveTempC > 0.0f)
			{
				if (bUsePellicciottiFluxCalibrated)
				{
					if (RadiationTotal_UE > 0.0f)
					{
						RadiationComponent = PellicciottiShortwaveFactor_RI * Absorptivity * RadiationTotal_UE;
					}
					else
					{
						RadiationComponent = PellicciottiShortwaveFactor_RI * Absorptivity * SolarForcing_Wm2;
					}

					MeltRate_mmph = PellicciottiTemperatureFactor * PositiveTempC + RadiationComponent;
					DiagnosticRadiationIdx = (SolarForcing_Wm2 > 0.0f) ? (RadiationTotal_UE / SolarForcing_Wm2) : 0.0f;
					DiagnosticCloudiness = CloudinessRatio;
					DiagnosticNetShortwave_Wm2 = RadiationComponent;
					MeltFactorDiag = MeltRate_mmph;
				}
				else if (bUsePellicciottiRI)
				{
					// DegreeDay RI/PBR/legacy UE+SWR stay in total/GHI mode.
					// Use UE total shortwave scaled against horizontal GHI instead of a split
					// direct/diffuse interpretation, because terrain bounce does not have a clean
					// place in the empirical DD formulation unless we use the fully flux-calibrated mode.
					if (RadiationTotal_UE_GHIScaled > 0.0f)
					{
						RadiationComponent = PellicciottiShortwaveFactor_RI * Absorptivity * RadiationTotal_UE_GHIScaled;
						DiagnosticRadiationIdx = (SolarForcing_Wm2 > 0.0f)
							? (RadiationTotal_UE_GHIScaled / SolarForcing_Wm2)
							: 0.0f;
					}
					else
					{
						// Fallback to legacy total index scaling when total flux calibration is unavailable.
						RadiationComponent = PellicciottiShortwaveFactor_RI * PellicciottiRadiationIndex * Absorptivity * SolarForcing_Wm2;
						DiagnosticRadiationIdx = PellicciottiRadiationIndex;
					}
					
					MeltRate_mmph = PellicciottiTemperatureFactor * PositiveTempC + RadiationComponent;
					DiagnosticCloudiness = CloudinessRatio;
					DiagnosticNetShortwave_Wm2 = RadiationComponent;
					MeltFactorDiag = MeltRate_mmph;
				}
				else
				{
				if (bUseSplitPellicciottiLambertian && (StepDNI > 0.0f || StepDHI > 0.0f))
				{
					// For Pellicciotti Model D (non-RI), use forcing-based split (DNI+DHI),
					// not UE RTY-derived radiation.
					RadiationComponent = PellicciottiShortwaveFactor * Absorptivity * (StepDNI + StepDHI);
				}
				else
				{
					RadiationComponent = PellicciottiShortwaveFactor * Absorptivity * SolarForcing_Wm2;
				}
					MeltRate_mmph = PellicciottiTemperatureFactor * PositiveTempC + RadiationComponent;
					DiagnosticRadiationIdx = 1.0f;
					DiagnosticCloudiness = 1.0f;
					DiagnosticNetShortwave_Wm2 = RadiationComponent;
					MeltFactorDiag = MeltRate_mmph;
				}
			}
		}
		else if (DegreeDayMeltModel == EDegreeDayMeltModel::HockModel2 || DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3Exact)
		{
			const float PositiveTempC = FMath::Max(0.0f, LocalTemperatureC - HockMeltThresholdC);
			if (PositiveTempC > 0.0f)
			{
				const float EffectiveRadiation_Wm2 = PotentialDirect_Wm2 * CloudinessRatioForMelt;
				const float RadiationFactorDaily = HockSnowRadiationFactor * 24.0f;
				const float MeltFactorPerDay = HockSnowMeltFactor + RadiationFactorDaily * EffectiveRadiation_Wm2;
				MeltFactorDiag = MeltFactorPerDay;
				MeltRate_mmph = (MeltFactorPerDay * PositiveTempC) / 24.0f;
				DiagnosticNetShortwave_Wm2 = EffectiveRadiation_Wm2;
				// Note: DiagnosticCloudiness is always set to actual GHI/Is for diagnostics, regardless of melt model
				DiagnosticRadiationIdx = 1.0f;
				RadiationComponent = HockSnowRadiationFactor * PotentialDirect_Wm2 * (DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3Exact ? CloudinessRatioForMelt : 1.0f);
			}
		}
		else
		{
			// Enhanced model deprecated per request
			MeltRate_mmph = 0.0f;
			MeltFactorDiag = 0.0f;
			RadiationComponent = 0.0f;
			DiagnosticRadiationIdx = 1.0f;
			DiagnosticCloudiness = 1.0f;
			DiagnosticNetShortwave_Wm2 = 0.0f;
		}

		if (CellLastPotentialDirect_Wm2.IsValidIndex(Idx))
		{
			CellLastPotentialDirect_Wm2[Idx] = PotentialDirect_Wm2;
		}
		if (CellLastPotentialHorizontal_Wm2.IsValidIndex(Idx))
		{
			CellLastPotentialHorizontal_Wm2[Idx] = PotentialHorizontal_Wm2;
		}
		if (CellLastNetShortwaveAbsorbed_Wm2.IsValidIndex(Idx))
		{
			CellLastNetShortwaveAbsorbed_Wm2[Idx] = DiagnosticNetShortwave_Wm2;
		}


		if (CellLastCloudinessRatio.IsValidIndex(Idx))
		{
			CellLastCloudinessRatio[Idx] = DiagnosticCloudiness;
		}
		if (CellLastRadiationIndex_Swift.IsValidIndex(Idx))
		{
			CellLastRadiationIndex_Swift[Idx] = RadiationIndex_Swift;
		}
		if (CellLastRadiationIndex_UE.IsValidIndex(Idx))
		{
			CellLastRadiationIndex_UE[Idx] = StoredRadiationIndexUE;
		}
		if (CellLastRadiationIndex.IsValidIndex(Idx))
		{
			CellLastRadiationIndex[Idx] = DiagnosticRadiationIdx;
		}
		if (CellLastRadiationIndex_UE_Raw.IsValidIndex(Idx))
		{
			CellLastRadiationIndex_UE_Raw[Idx] = RadiationIndex_UE_Raw;
		}

		if (MeltRate_mmph > 0.0f)
		{
			const float MeltDepth_m_per_step = (MeltRate_mmph / 1000.0f) * (DtSeconds / 3600.0f);
			float MeltVolumeLiters = MeltDepth_m_per_step * AreaSqMeters * 1000.0f;
			MeltVolumeLiters = FMath::Min(MeltVolumeLiters, SWE);

			if (MeltVolumeLiters > 0.0f)
			{
				SWE -= MeltVolumeLiters;
				MeltDepth_m = ConvertSWEToDepthMeters(MeltVolumeLiters, AreaSqMeters);
			}
		}

		CellLastComputedSnowRate[Idx] = ComputedSnowRate;
		CellLastWeatherSnowFrac[Idx] = WeatherSnowFracRaw;
		CellLastEffectiveSnowFrac[Idx] = EffectiveSnowFrac;
		CellLastAccumulationDepth_m[Idx] = AccumDepth_m;
		CellLastMeltDepth_m[Idx] = MeltDepth_m;
		CellLastMeltFactor[Idx] = MeltFactorDiag;
		CellLastSlopeFiltered[Idx] = bSlopeFiltered;
		CellLastRedistributionFactor[Idx] = RedistributionFactor;
		

		if (CellLastRadiationDirect_UE.IsValidIndex(Idx))
		{
			CellLastRadiationDirect_UE[Idx] = RadiationDirect_UE;
		}
		if (CellLastRadiationDiffuse_UE.IsValidIndex(Idx))
		{
			CellLastRadiationDiffuse_UE[Idx] = RadiationDiffuse_UE;
		}
		if (CellLastRadiationIndex_Direct.IsValidIndex(Idx))
		{
			CellLastRadiationIndex_Direct[Idx] = RadiationIndex_Direct;
		}
		if (CellLastRadiationIndex_Diffuse.IsValidIndex(Idx))
		{
			CellLastRadiationIndex_Diffuse[Idx] = RadiationIndex_Diffuse;
		}
		if (CellLastRadiationTotal_UE.IsValidIndex(Idx))
		{
			CellLastRadiationTotal_UE[Idx] = RadiationTotal_UE;
		}
		if (CellLastRadiationTerrain_UE.IsValidIndex(Idx))
		{
			CellLastRadiationTerrain_UE[Idx] = RadiationTerrain_UE;
		}
		if (CellLastReferenceScale_Total.IsValidIndex(Idx))
		{
			CellLastReferenceScale_Total[Idx] = ReferenceScaleTotal;
		}
		if (CellLastReferenceScale_Direct.IsValidIndex(Idx))
		{
			CellLastReferenceScale_Direct[Idx] = ReferenceScaleDirect;
		}
		if (CellLastReferenceScale_Diffuse.IsValidIndex(Idx))
		{
			CellLastReferenceScale_Diffuse[Idx] = ReferenceScaleDiffuse;
		}
		if (CellLastReferenceScale_TotalNoGI.IsValidIndex(Idx))
		{
			CellLastReferenceScale_TotalNoGI[Idx] = ReferenceScaleTotalNoGI;
		}
		if (CellLastReferenceLuminance_Total.IsValidIndex(Idx))
		{
			CellLastReferenceLuminance_Total[Idx] = ReferenceLuminanceTotalUsed;
		}
		if (CellLastReferenceLuminance_Direct.IsValidIndex(Idx))
		{
			CellLastReferenceLuminance_Direct[Idx] = ReferenceLuminanceDirectUsed;
		}
		if (CellLastReferenceLuminance_Diffuse.IsValidIndex(Idx))
		{
			CellLastReferenceLuminance_Diffuse[Idx] = ReferenceLuminanceDiffuseUsed;
		}
		if (CellLastReferenceLuminance_DiffuseNoGI.IsValidIndex(Idx))
		{
			CellLastReferenceLuminance_DiffuseNoGI[Idx] = ReferenceLuminanceDiffuseNoGIUsed;
		}
		if (CellLastReferenceLuminance_TotalNoGI.IsValidIndex(Idx))
		{
			CellLastReferenceLuminance_TotalNoGI[Idx] = ReferenceLuminanceTotalNoGIUsed;
		}
		if (CellLastReferenceLuminance_Terrain.IsValidIndex(Idx))
		{
			CellLastReferenceLuminance_Terrain[Idx] = ReferenceLuminanceTerrainUsed;
		}

		DaysSinceSnow += DtDays;

	});

	UpdateDepthFromSnowWaterEquivalent();

	// Update dynamic surface geometry if enabled
	if (bUseDynamicSurfaceGeometry && ActorPtr)
	{
		UpdateDynamicSurfaceGeometry(ActorPtr->North);
	}
}

void UDegreeDaySimulation::UpdateDepthFromSnowWaterEquivalent()
{
	const int32 CellCount = CellSnowWaterEquivalentLiters.Num();
	if (CellCount <= 0)
	{
		return;
	}

	if (CellInterpolatedSWE_Liters.Num() != CellCount)
	{
		CellInterpolatedSWE_Liters.SetNum(CellCount, EAllowShrinking::No);
	}
	if (CellLastRedistributionFactor.Num() != CellCount)
	{
		CellLastRedistributionFactor.SetNumZeroed(CellCount);
	}
	if (CellLastRadiationDirect_UE.Num() != CellCount)
	{
		CellLastRadiationDirect_UE.SetNumZeroed(CellCount);
	}
	if (CellLastRadiationDiffuse_UE.Num() != CellCount)
	{
		CellLastRadiationDiffuse_UE.SetNum(CellCount);
	}
	if (CellLastRadiationTotal_UE.Num() != CellCount)
	{
		CellLastRadiationTotal_UE.SetNumZeroed(CellCount);
	}
	if (CellLastRadiationTerrain_UE.Num() != CellCount)
	{
		CellLastRadiationTerrain_UE.SetNumZeroed(CellCount);
	}
	if (CellLastRadiationIndex_Direct.Num() != CellCount)
	{
		CellLastRadiationIndex_Direct.SetNum(CellCount);
	}
	if (CellLastRadiationIndex_Diffuse.Num() != CellCount)
	{
		CellLastRadiationIndex_Diffuse.SetNum(CellCount, EAllowShrinking::No);
	}
	if (CellLastReferenceScale_Total.Num() != CellCount)
	{
		CellLastReferenceScale_Total.SetNumZeroed(CellCount);
	}
	if (CellLastReferenceScale_Direct.Num() != CellCount)
	{
		CellLastReferenceScale_Direct.SetNumZeroed(CellCount);
	}
	if (CellLastReferenceScale_Diffuse.Num() != CellCount)
	{
		CellLastReferenceScale_Diffuse.SetNumZeroed(CellCount);
	}
	if (CellLastReferenceScale_TotalNoGI.Num() != CellCount)
	{
		CellLastReferenceScale_TotalNoGI.SetNumZeroed(CellCount);
	}
	if (CellLastReferenceLuminance_Total.Num() != CellCount)
	{
		CellLastReferenceLuminance_Total.SetNumZeroed(CellCount);
	}
	if (CellLastReferenceLuminance_Direct.Num() != CellCount)
	{
		CellLastReferenceLuminance_Direct.SetNumZeroed(CellCount);
	}
	if (CellLastReferenceLuminance_Diffuse.Num() != CellCount)
	{
		CellLastReferenceLuminance_Diffuse.SetNumZeroed(CellCount);
	}
	if (CellLastReferenceLuminance_DiffuseNoGI.Num() != CellCount)
	{
		CellLastReferenceLuminance_DiffuseNoGI.SetNumZeroed(CellCount);
	}
	if (CellLastReferenceLuminance_TotalNoGI.Num() != CellCount)
	{
		CellLastReferenceLuminance_TotalNoGI.SetNumZeroed(CellCount);
	}
	if (CellLastReferenceLuminance_Terrain.Num() != CellCount)
	{
		CellLastReferenceLuminance_Terrain.SetNumZeroed(CellCount);
	}

	CurrentMaxSnowMM = 0.0f;

	// Parallel depth compute; max reduction done serially after.
	ParallelFor(CellCount, [&](int32 Idx)
	{
		const float SWE = CellSnowWaterEquivalentLiters[Idx];
		const float AreaSqMeters = CellAreaSqMeters.IsValidIndex(Idx) ? CellAreaSqMeters[Idx] : 0.0f;

		const float InterpolatedSWE = FMath::Max(0.0f, SWE);
		CellInterpolatedSWE_Liters[Idx] = InterpolatedSWE;

		const float Depth_m = ConvertSWEToDepthMeters(InterpolatedSWE, AreaSqMeters);
		if (DepthMeters.IsValidIndex(Idx))
		{
			DepthMeters[Idx] = Depth_m;
		}
	});

	for (int32 Idx = 0; Idx < CellCount; ++Idx)
	{
		if (DepthMeters.IsValidIndex(Idx))
		{
			CurrentMaxSnowMM = FMath::Max(CurrentMaxSnowMM, DepthMeters[Idx] * 1000.0f);
		}
	}
}

float UDegreeDaySimulation::ComputeSolarRadiationIndex(float Inclination, float Aspect, float Latitude, float DayOfYear) const
{
	const float ClampedInclination = FMath::Clamp(Inclination, 0.0f, PI * 0.5f);
	const float L1 = FMath::Asin(FMath::Cos(ClampedInclination) * FMath::Sin(Latitude) + FMath::Sin(ClampedInclination) * FMath::Cos(Latitude) * FMath::Cos(Aspect));
	const float D1 = FMath::Cos(ClampedInclination) * FMath::Cos(Latitude) - FMath::Sin(ClampedInclination) * FMath::Sin(Latitude) * FMath::Cos(Aspect);
	const float L2 = FMath::Atan2(FMath::Sin(ClampedInclination) * FMath::Sin(Aspect), D1);

	const float J = DayOfYear;
	const float D = 0.007f - 0.4067f * FMath::Cos((J + 10.0f) * 0.0172f);
	float E = 1.0f - 0.0167f * FMath::Cos((J - 3.0f) * 0.0172f);
	E = (FMath::Abs(E) < KINDA_SMALL_NUMBER) ? KINDA_SMALL_NUMBER : E;

	const float R0 = 1.95f;
	const float R1 = 60.0f * R0 / (E * E);

	const float T = SolarFunc2(Latitude, D);
	const float T0 = -T;
	const float T1 = T;

	const float T7 = SolarFunc2(L1, D) - L2;
	const float T6 = -SolarFunc2(L1, D) - L2;

	float T3 = FMath::Min(T7, T1);
	float T2 = FMath::Max(T6, T0);

	float T4 = T2 * (12.0f / PI);
	float T5 = T3 * (12.0f / PI);
	(void)T4;
	(void)T5;

	if (T3 < T2)
	{
		T2 = T3 = 0.0f;
	}

	float R4 = 0.0f;
	float T6Wrapped = T6 + PI * 2.0f;

	if (T6Wrapped < T1)
	{
		const float T8 = T6Wrapped;
		const float T9 = T1;
		R4 = SolarFunc3(L2, L1, T3, T2, R1, D) + SolarFunc3(L2, L1, T9, T8, R1, D);
	}
	else
	{
		const float T7Wrapped = T7 - PI * 2.0f;
		if (T7Wrapped > T0)
		{
			const float T8 = T0;
			const float T9 = T0;
			R4 = SolarFunc3(L2, L1, T3, T2, R1, D) + SolarFunc3(L2, L1, T9, T8, R1, D);
		}
		else
		{
			R4 = SolarFunc3(L2, L1, T3, T2, R1, D);
		}
	}

	const float R3 = SolarFunc3(0.0f, Latitude, T1, T0, R1, D);

	// Log the issue when R3 is near zero (this shouldn't normally happen)
	static bool bLoggedR3Zero = false;
	if (!bLoggedR3Zero && FMath::IsNearlyZero(R3, 0.001f))
	{
		bLoggedR3Zero = true;
		UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] Swift radiation: R3 is nearly zero (%.6f). Latitude=%.4f rad (%.1f deg), DayOfYear=%.0f, D=%.4f, T0=%.4f, T1=%.4f, R1=%.2f"),
			R3, Latitude, FMath::RadiansToDegrees(Latitude), DayOfYear, D, T0, T1, R1);
		UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] This may indicate incorrect latitude or an edge case in Swift's algorithm. Consider using UnrealEngine radiation method instead."));
	}

	if (FMath::IsNearlyZero(R3, 0.001f))
	{
		// If denominator is zero, the radiation ratio is undefined
		// Return 1.0 as a neutral fallback (no enhancement/reduction)
		return 1.0f;
	}

	return R4 / R3;
}

float UDegreeDaySimulation::SolarFunc2(float L, float D) const
{
	return FMath::Acos(FMath::Clamp(-FMath::Tan(L) * FMath::Tan(D), -1.0f, 1.0f));
}

float UDegreeDaySimulation::SolarFunc3(float V, float W, float X, float Y, float R1, float D) const
{
	return R1 * (FMath::Sin(D) * FMath::Sin(W) * (X - Y) * (12.0f / PI) +
		FMath::Cos(D) * FMath::Cos(W) * (FMath::Sin(X + V) - FMath::Sin(Y + V)) * (12.0f / PI));
}

float UDegreeDaySimulation::ComputeSnowRedistributionFactor(int32 CellIdx, float InclinationRad) const
{
	if (!bApplySlopeCurvatureRedistribution)
	{
		return 1.0f;
	}

	const float InclinationDeg = FMath::RadiansToDegrees(InclinationRad);
	const float Curvature = (TerrainCurvature.IsValidIndex(CellIdx)) ? TerrainCurvature[CellIdx] : 0.0f;

	const float StartDeg = FMath::Max(0.0f, SlopeRedistributionStartDeg);
	const float ZeroDeg = FMath::Max(StartDeg + KINDA_SMALL_NUMBER, SlopeRedistributionZeroDeg);
	const float SlopeFactor = (InclinationDeg <= StartDeg)
		? 0.0f
		: FMath::Clamp((InclinationDeg - StartDeg) / (ZeroDeg - StartDeg), 0.0f, 1.0f);
	const float CurvatureFactor = 1.0f + CurvatureRedistributionGain * Curvature;

	const float RawFactor = (1.0f - SlopeFactor) * CurvatureFactor;
	const float MinFactor = FMath::Max(0.0f, MinRedistributionFactor);
	const float MaxFactor = FMath::Max(MinFactor + KINDA_SMALL_NUMBER, MaxRedistributionFactor);
	const float ClampedFactor = FMath::Clamp(RawFactor, MinFactor, MaxFactor);
	const float EdgeWeight = GetRedistributionEdgeWeight(CellIdx);
	return FMath::Lerp(1.0f, ClampedFactor, EdgeWeight);
}

float UDegreeDaySimulation::GetRedistributionEdgeWeight(int32 CellIdx) const
{
	if (RedistributionEdgeFadeCells <= 0 || GridX <= 0 || GridY <= 0)
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

	if (DistToEdge >= RedistributionEdgeFadeCells)
	{
		return 1.0f;
	}

	return FMath::Clamp(static_cast<float>(DistToEdge) / static_cast<float>(RedistributionEdgeFadeCells), 0.0f, 1.0f);
}

bool UDegreeDaySimulation::IsRedistributionEdgeCell(int32 CellIdx) const
{
	return GetRedistributionEdgeWeight(CellIdx) < (1.0f - KINDA_SMALL_NUMBER);
}

float UDegreeDaySimulation::ConvertSWEToDepthMeters(float SnowWaterEquivalentLiters, float AreaSquareMeters) const
{
	if (AreaSquareMeters <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	// Degree-day stores SWE as liters (1 L water == 1 kg). The shared depth buffer should expose
	// physical snow depth, so convert mass per area using the configured bulk snow density.
	const float BulkSnowDensity_kgm3 = (FreshSnowDensity_kgm3 > 1.0f) ? FreshSnowDensity_kgm3 : 100.0f;
	const float SnowMass_kgm2 = SnowWaterEquivalentLiters / AreaSquareMeters;
	return SnowMass_kgm2 / BulkSnowDensity_kgm3;
}


void UDegreeDaySimulation::ComputeSolarAngles(float LatitudeRad, float SolarDeclinationRad, float HourAngleRad,
	float& OutCosZenith, float& OutSinZenith, float& OutSolarAzimuthRad) const
{
	const float SinLat = FMath::Sin(LatitudeRad);
	const float CosLat = FMath::Cos(LatitudeRad);
	const float SinDecl = FMath::Sin(SolarDeclinationRad);
	const float CosDecl = FMath::Cos(SolarDeclinationRad);
	const float CosH = FMath::Cos(HourAngleRad);
	const float SinH = FMath::Sin(HourAngleRad);

	float CosZenith = SinLat * SinDecl + CosLat * CosDecl * CosH;
	CosZenith = FMath::Clamp(CosZenith, -1.0f, 1.0f);
	OutCosZenith = CosZenith;

	float SinZenith = FMath::Sqrt(FMath::Max(0.0f, 1.0f - CosZenith * CosZenith));
	OutSinZenith = SinZenith;

	float SolarAzimuth = 0.0f;
	if (SinZenith > 1e-4f)
	{
		const float SinAz = CosDecl * SinH / SinZenith;
		const float CosAz = (SinDecl * CosLat - CosDecl * SinLat * CosH) / SinZenith;
		SolarAzimuth = FMath::Atan2(SinAz, CosAz);
		if (SolarAzimuth < 0.0f)
		{
			SolarAzimuth += 2.0f * PI;
		}
	}
	else
	{
		SolarAzimuth = 0.0f;
	}

	OutSolarAzimuthRad = SolarAzimuth;
}

float UDegreeDaySimulation::ComputeSolarDeclinationRad(int32 DayOfYear)
{
	const float Gamma = 2.0f * PI * static_cast<float>(DayOfYear - 1) / 365.0f;
	return 0.006918f - 0.399912f * FMath::Cos(Gamma) + 0.070257f * FMath::Sin(Gamma)
		- 0.006758f * FMath::Cos(2.0f * Gamma) + 0.000907f * FMath::Sin(2.0f * Gamma)
		- 0.002697f * FMath::Cos(3.0f * Gamma) + 0.00148f * FMath::Sin(3.0f * Gamma);
}

float UDegreeDaySimulation::ComputeEquationOfTimeMinutes(int32 DayOfYear)
{
	const float B = 2.0f * PI * static_cast<float>(DayOfYear - 81) / 364.0f;
	return 9.87f * FMath::Sin(2.0f * B) - 7.53f * FMath::Cos(B) - 1.5f * FMath::Sin(B);
}

float UDegreeDaySimulation::ComputeLocalSolarTimeHours(const FDateTime& Timestamp, int32 DayOfYear, float LongitudeDegrees)
{
	if (!Timestamp.GetTicks())
	{
		return 12.0f;
	}

	const float Hours = static_cast<float>(Timestamp.GetHour())
		+ static_cast<float>(Timestamp.GetMinute()) / 60.0f
		+ static_cast<float>(Timestamp.GetSecond()) / 3600.0f;
	const float TimezoneHours = static_cast<float>(FMath::RoundToInt(LongitudeDegrees / 15.0f));
	const float EquationOfTime = ComputeEquationOfTimeMinutes(DayOfYear);
	float SolarMinutes = Hours * 60.0f + 4.0f * (LongitudeDegrees - TimezoneHours * 15.0f) + EquationOfTime;
	SolarMinutes = FMath::Fmod(SolarMinutes, 1440.0f);
	if (SolarMinutes < 0.0f)
	{
		SolarMinutes += 1440.0f;
	}
	return SolarMinutes / 60.0f;
}

void UDegreeDaySimulation::ComputeDNI_DHI(FForcingRadiation& InOut, float ExtraterrestrialIrradiance /*= 1361.0f*/)
{
	const float GHI = FMath::Max(InOut.GHI, 0.0f);
	if (GHI <= KINDA_SMALL_NUMBER)
	{
		InOut.DHI = 0.0f;
		InOut.DNI = 0.0f;
		InOut.DiffuseFraction = 0.0f;
		return;
	}

	// Ensure solar geometry is valid
	const float CosZ = FMath::Max(FMath::Cos(InOut.SolarZenith_rad), 1e-6f);

	// Validate solar zenith - reject if sun is at/below horizon
	// cos(89°) = 0.0175, so we use 0.02 as threshold (sun must be >88° from zenith = <2° elevation)
	if (CosZ < 0.02f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ComputeDNI_DHI] Sun at/below horizon (cosZ=%.6f, zenith=%.2f°, elevation=%.2f°) - DNI calculation unreliable. Treating all GHI as diffuse."),
			CosZ, FMath::RadiansToDegrees(InOut.SolarZenith_rad), 90.0f - FMath::RadiansToDegrees(InOut.SolarZenith_rad));

		// When sun is at/below horizon, all radiation is diffuse (scattered skylight)
		InOut.DHI = GHI;
		InOut.DNI = 0.0f;
		InOut.DiffuseFraction = 1.0f;
		return;
	}

	// If diffuse fraction already provided, respect it
	if (InOut.DiffuseFraction >= 0.0f)
	{
		const float Fd = FMath::Clamp(InOut.DiffuseFraction, 0.0f, 1.0f);
		InOut.DHI = Fd * GHI;
		InOut.DNI = (1.0f - Fd) * GHI / CosZ;
		InOut.DiffuseFraction = Fd;
		return;
	}

	// Erbs et al. (1982) correlation
	const float So = ExtraterrestrialIrradiance * CosZ;
	const float kt = (So > KINDA_SMALL_NUMBER) ? FMath::Clamp(GHI / So, 0.0f, 2.0f) : 0.0f;

	float Fd = 0.0f;
	if (kt <= 0.22f)
	{
		Fd = 1.0f - 0.09f * kt;
	}
	else if (kt <= 0.80f)
	{
		const float kt2 = kt * kt;
		const float kt3 = kt2 * kt;
		const float kt4 = kt3 * kt;
		Fd = 0.9511f - 0.1604f * kt + 4.388f * kt2 - 16.638f * kt3 + 12.336f * kt4;
	}
	else
	{
		Fd = 0.165f;
	}

	Fd = FMath::Clamp(Fd, 0.0f, 1.0f);

	InOut.DiffuseFraction = Fd;
	InOut.DHI = Fd * GHI;
	InOut.DNI = (1.0f - Fd) * GHI / CosZ;
}

void UDegreeDaySimulation::Simulate(ASnowSimulationActor* SimulationActor, int32 CurrentSimulationStep, int32 Timesteps,
	bool SaveSnowMap, bool CaptureDebugInformation, TArray<FDebugCell>& DebugCells)
{
	if (!SimulationActor) return;

	// Pull weather forcing for current time
	FWeatherForcingData W;
	bool bWeatherValid = false;
	if (USimulationWeatherDataProviderBase* ForcingProvider = SimulationActor->GetActiveWeatherProvider())
	{
		W = ForcingProvider->GetWeatherForcing(SimulationActor->CurrentSimulationTime);
		bWeatherValid = true;
	}
	else if (LegacyClimateData.Num() > 0)
	{
		const int32 LegacyIndex = FMath::Clamp(CurrentSimulationStep, 0, LegacyClimateData.Num() - 1);
		const FClimateData& Legacy = LegacyClimateData[LegacyIndex];

		const float TempC = Legacy.Temperature;
		const float DtSeconds = FMath::Max(SimulationActor->TimeStepSeconds, KINDA_SMALL_NUMBER);
		const float Precip_mm = FMath::Max(0.0f, Legacy.Precipitation);

		W.Timestamp = SimulationActor->CurrentSimulationTime;
		W.Temperature_K = TempC + 273.15f;
		W.PrecipRate_kgm2s = Precip_mm / DtSeconds;

		if (TempC <= TSnowA)
		{
			W.SnowFrac_01 = 1.0f;
		}
		else if (TempC >= TSnowB)
		{
			W.SnowFrac_01 = 0.0f;
		}
		else
		{
			const float Den = FMath::Max(KINDA_SMALL_NUMBER, TSnowB - TSnowA);
			W.SnowFrac_01 = FMath::Clamp(1.0f - (TempC - TSnowA) / Den, 0.0f, 1.0f);
		}

		W.SWdown_Wm2 = 0.0f;
		W.LWdown_Wm2 = 0.0f;
		W.Wind_mps = 0.0f;
		W.RH_01 = 0.6f;
		W.Pressure_Pa = 101325.0f;
		bWeatherValid = true;
	}

	const bool bCollectDiagnostics = bEnableDiagnostics && DiagnosticsEveryNSteps > 0;
	TArray<FDegreeDayCellDiagnostics> DiagnosticsBuffer;
	TArray<int32> ValidTrackedIndices;

	// Debug logging
	if (SimulationStepCounter == 0 && bEnableDiagnostics)
	{
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Diagnostics enabled: bEnableDiagnostics=%d, EveryNSteps=%d, CellCount=%d"),
			bEnableDiagnostics, DiagnosticsEveryNSteps, DepthMeters.Num());

		// Log cell areas and altitudes to diagnose potential issues
		if (CellAreaSqMeters.Num() > 0 && CellAltitudeCm.Num() > 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Cell[0]: Area=%.2f mÂ², Altitude=%.0f cm, MeasurementAlt=%.0f cm"),
				CellAreaSqMeters[0], CellAltitudeCm[0], MeasurementAltitudeCm);
		}
	}

	// Determine which cells to track
	if (bCollectDiagnostics)
	{
		const int32 CellCount = DepthMeters.Num();
		if (DiagnosticsTrackedCellIndices.Num() == 0)
		{
			// Default: track first cell only
			if (CellCount > 0)
			{
				ValidTrackedIndices.Add(0);
			}
		}
		else
		{
			// Track specified cells
			for (int32 RawIndex : DiagnosticsTrackedCellIndices)
			{
				if (RawIndex >= 0 && RawIndex < CellCount)
				{
					ValidTrackedIndices.AddUnique(RawIndex);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] Invalid cell index %d (CellCount=%d)"), RawIndex, CellCount);
				}
			}
		}

		if (SimulationStepCounter == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Tracking %d cells for diagnostics"), ValidTrackedIndices.Num());
		}
	}

	// Store pre-step depths for diagnostics
	TArray<float> DepthsBefore;
	if (bCollectDiagnostics && ValidTrackedIndices.Num() > 0)
	{
		DepthsBefore = DepthMeters;
	}

	// Step the simulation
	const float DtSeconds = SimulationActor->TimeStepSeconds;
	OwningSimulationActor = SimulationActor;
	if (bWeatherValid)
	{
		Step(DtSeconds, W, DepthMeters);
	}
	else
	{
		// No weather data available. Maintain existing depth but still advance counters.
	}

	// Cache solar/reference telemetry for diagnostics
	const float SimulationActorLongitudeDeg = (SimulationActor) ? SimulationActor->Longitude : 0.0f;
	float CaptureCosSolarZenith = 0.0f;
	float ClampedCaptureCosSolarZenith = 0.0f;
	float SunVisibilityCapture = 0.0f;
	float ReferenceLuminanceTotal = 0.0f;
	float ReferenceLuminanceDirect = 0.0f;
	float ReferenceLuminanceDiffuse = 0.0f;
	bool bReferenceTelemetryValid = false;
	float SolarZenithWorld_deg = 0.0f;
	float SolarElevationWorld_deg = 0.0f;
	float SolarAzimuthWorld_deg = 0.0f;
	if (SimulationActor)
	{
		CaptureCosSolarZenith = SimulationActor->GetCachedCaptureCosSolarZenith();
		ClampedCaptureCosSolarZenith = FMath::Clamp(CaptureCosSolarZenith, -1.0f, 1.0f);
		const float SolarZenithWorld_rad = FMath::Acos(ClampedCaptureCosSolarZenith);
		SolarZenithWorld_deg = FMath::RadiansToDegrees(SolarZenithWorld_rad);
		SolarElevationWorld_deg = 90.0f - SolarZenithWorld_deg;
		SunVisibilityCapture = SimulationActor->GetCachedSunVisibility();
		ReferenceLuminanceTotal = SimulationActor->GetReferenceLuminance_Total();
		ReferenceLuminanceDirect = SimulationActor->GetReferenceLuminance_Direct();
		ReferenceLuminanceDiffuse = SimulationActor->GetReferenceLuminance_Diffuse();
		bReferenceTelemetryValid = SimulationActor->bReferenceLuminanceValid;

		if (const UDirectionalLightComponent* SunLight = SimulationActor->GetSunDirectionalLight())
		{
			// Build a 2D basis using the simulation actor's configured North vector so
			// geographic azimuth reporting always matches the landscape orientation.
			FVector SafeNorth = SimulationActor->North;
			if (!SafeNorth.Normalize())
			{
				SafeNorth = FVector(0.0f, -1.0f, 0.0f);
			}

			FVector NorthXY = FVector(SafeNorth.X, SafeNorth.Y, 0.0f);
			if (!NorthXY.Normalize())
			{
				NorthXY = FVector(0.0f, -1.0f, 0.0f);
			}

			FVector EastXY = FVector::CrossProduct(FVector::UpVector, SafeNorth);
			if (!EastXY.Normalize())
			{
				EastXY = FVector(1.0f, 0.0f, 0.0f);
			}
			EastXY.Z = 0.0f;
			if (!EastXY.Normalize())
			{
				EastXY = FVector(1.0f, 0.0f, 0.0f);
			}

			// UE's directional light reports the direction light travels (sun -> ground).
			// For azimuth we need direction from the surface toward the sun, so flip it.
			FVector SunDir = -SunLight->GetDirection();
			FVector SunDirXY = FVector(SunDir.X, SunDir.Y, 0.0f);
			if (!SunDirXY.Normalize())
			{
				SunDirXY = FVector(0.0f, 1.0f, 0.0f);
			}

			const float NorthComponent = FVector::DotProduct(SunDirXY, NorthXY);
			const float EastComponent = FVector::DotProduct(SunDirXY, EastXY);

			float AzimuthRad = FMath::Atan2(EastComponent, NorthComponent);
			if (AzimuthRad < 0.0f)
			{
				AzimuthRad += 2.0f * PI;
			}

			SolarAzimuthWorld_deg = FMath::RadiansToDegrees(AzimuthRad);
		}
	}

	// Collect diagnostics after stepping
	if (bCollectDiagnostics && ValidTrackedIndices.Num() > 0)
	{
		// Log array sizes on first diagnostic collection
		static bool bLoggedDiagnosticArrays = false;
		if (!bLoggedDiagnosticArrays)
		{
			bLoggedDiagnosticArrays = true;
			UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Diagnostics collection: ValidTrackedIndices.Num()=%d, CellAreaSqMeters.Num()=%d, CellAltitudeCm.Num()=%d"),
				ValidTrackedIndices.Num(), CellAreaSqMeters.Num(), CellAltitudeCm.Num());
			if (ValidTrackedIndices.Num() > 0)
			{
				int32 FirstIdx = ValidTrackedIndices[0];
				UE_LOG(LogTemp, Display, TEXT("[DegreeDay] First tracked cell index: %d, IsValidIndex(Area)=%d, IsValidIndex(Alt)=%d"),
					FirstIdx, CellAreaSqMeters.IsValidIndex(FirstIdx) ? 1 : 0, CellAltitudeCm.IsValidIndex(FirstIdx) ? 1 : 0);
				if (CellAreaSqMeters.IsValidIndex(FirstIdx) && CellAltitudeCm.IsValidIndex(FirstIdx))
				{
					UE_LOG(LogTemp, Display, TEXT("[DegreeDay] First tracked cell data: Area=%.2f mÂ², Altitude=%.0f cm"),
						CellAreaSqMeters[FirstIdx], CellAltitudeCm[FirstIdx]);
				}
			}
		}

		for (int32 CellIdx : ValidTrackedIndices)
		{
			FDegreeDayCellDiagnostics Diag;
			Diag.StepIndex = SimulationStepCounter;
			Diag.CellIndex = CellIdx;
			Diag.SimulationTimeSeconds = ElapsedSimulationSeconds;
			Diag.Timestamp = SimulationActor->CurrentSimulationTime;

			// State
			Diag.SnowDepth_m = DepthMeters[CellIdx];

			// Forcing
			Diag.ForcingAirTemperatureC = W.Temperature_K - 273.15f;
			Diag.ForcingPrecipRate_kgm2s = W.PrecipRate_kgm2s;
			Diag.ForcingPrecipRate_mmph = W.PrecipRate_kgm2s * 3600.0f;
			const float WeatherSnowFrac = CellLastWeatherSnowFrac.IsValidIndex(CellIdx) ? CellLastWeatherSnowFrac[CellIdx] : W.SnowFrac_01;
			const float EffectiveSnowFrac = CellLastEffectiveSnowFrac.IsValidIndex(CellIdx) ? CellLastEffectiveSnowFrac[CellIdx] : WeatherSnowFrac;
			Diag.ForcingSnowFrac = FMath::Clamp(EffectiveSnowFrac, 0.0f, 1.0f);
			Diag.WeatherSnowFracRaw = FMath::Clamp(WeatherSnowFrac, 0.0f, 1.0f);
			Diag.ForcingSWdown_Wm2 = W.SWdown_Wm2;
			Diag.ForcingLWdown_Wm2 = W.LWdown_Wm2;
			Diag.ForcingWindSpeed_mps = W.Wind_mps;
			Diag.ForcingRH = W.RH_01;
			Diag.ForcingPressure_Pa = W.Pressure_Pa;
			Diag.ForcingCloudCover_01 = W.CloudCover_01;

			// Computed values from per-cell state
			const float AccumDepth = CellLastAccumulationDepth_m.IsValidIndex(CellIdx) ? CellLastAccumulationDepth_m[CellIdx] : 0.0f;
			const float MeltDepth = CellLastMeltDepth_m.IsValidIndex(CellIdx) ? CellLastMeltDepth_m[CellIdx] : 0.0f;
			const float MeltFactor = CellLastMeltFactor.IsValidIndex(CellIdx) ? CellLastMeltFactor[CellIdx] : 0.0f;
			const float CellSlope = TerrainSlopeDegrees.IsValidIndex(CellIdx) ? TerrainSlopeDegrees[CellIdx] : 0.0f;
			const float RadiationIdx = CellLastRadiationIndex.IsValidIndex(CellIdx) ? CellLastRadiationIndex[CellIdx] : 0.0f;
			const float RadiationIdx_Swift = CellLastRadiationIndex_Swift.IsValidIndex(CellIdx) ? CellLastRadiationIndex_Swift[CellIdx] : 0.0f;
			const float RadiationIdx_UE = CellLastRadiationIndex_UE.IsValidIndex(CellIdx) ? CellLastRadiationIndex_UE[CellIdx] : 0.0f;
			const float CloudinessRatioForDiag = CellLastCloudinessRatio.IsValidIndex(CellIdx) ? CellLastCloudinessRatio[CellIdx] : 1.0f;
			const float SnowAlbedoState = CellSnowAlbedoState.IsValidIndex(CellIdx) ? CellSnowAlbedoState[CellIdx] : 0.0f;
			const float ComputedSnowRate = CellLastComputedSnowRate.IsValidIndex(CellIdx) ? CellLastComputedSnowRate[CellIdx] : 0.0f;
			const float NetShortwaveForDiagnostics_Wm2 = CellLastNetShortwaveAbsorbed_Wm2.IsValidIndex(CellIdx) ? CellLastNetShortwaveAbsorbed_Wm2[CellIdx] : 0.0f;
			const float PotentialDirect = CellLastPotentialDirect_Wm2.IsValidIndex(CellIdx) ? CellLastPotentialDirect_Wm2[CellIdx] : 0.0f;
			const float PotentialHorizontal = CellLastPotentialHorizontal_Wm2.IsValidIndex(CellIdx) ? CellLastPotentialHorizontal_Wm2[CellIdx] : 0.0f;
			const float RTYTotal = CellLastRTY_Total.IsValidIndex(CellIdx) ? CellLastRTY_Total[CellIdx] : 0.0f;
			const float RTYDirect = CellLastRTY_Direct.IsValidIndex(CellIdx) ? CellLastRTY_Direct[CellIdx] : 0.0f;
			const float RTYDiffuse = CellLastRTY_Diffuse.IsValidIndex(CellIdx) ? CellLastRTY_Diffuse[CellIdx] : 0.0f;
			const float RTYDiffuseNoGI = (bHasDiffuseNoGI && CellLastRTY_DiffuseNoGI.IsValidIndex(CellIdx)) ? CellLastRTY_DiffuseNoGI[CellIdx] : 0.0f;
			const float RTYTotalNoGI = CellLastRTY_TotalNoGI.IsValidIndex(CellIdx) ? CellLastRTY_TotalNoGI[CellIdx] : (RTYDirect + RTYDiffuseNoGI);
			const float RTYTerrain = (bHasTerrainRTY && CellLastRTY_Terrain.IsValidIndex(CellIdx)) ? CellLastRTY_Terrain[CellIdx] : 0.0f;
			const float RadiationTotal_UE = CellLastRadiationTotal_UE.IsValidIndex(CellIdx) ? CellLastRadiationTotal_UE[CellIdx] : 0.0f;
			const float RadiationTerrain_UE = CellLastRadiationTerrain_UE.IsValidIndex(CellIdx) ? CellLastRadiationTerrain_UE[CellIdx] : 0.0f;
			const float ReferenceScaleTotal = CellLastReferenceScale_Total.IsValidIndex(CellIdx) ? CellLastReferenceScale_Total[CellIdx] : 0.0f;
			const float ReferenceScaleDirect = CellLastReferenceScale_Direct.IsValidIndex(CellIdx) ? CellLastReferenceScale_Direct[CellIdx] : 0.0f;
			const float ReferenceScaleDiffuse = CellLastReferenceScale_Diffuse.IsValidIndex(CellIdx) ? CellLastReferenceScale_Diffuse[CellIdx] : 0.0f;
			const float ReferenceScaleTotalNoGI = CellLastReferenceScale_TotalNoGI.IsValidIndex(CellIdx) ? CellLastReferenceScale_TotalNoGI[CellIdx] : 0.0f;
			const float ReferenceLuminanceTotalUsed = CellLastReferenceLuminance_Total.IsValidIndex(CellIdx) ? CellLastReferenceLuminance_Total[CellIdx] : ReferenceLuminanceTotal;
			const float ReferenceLuminanceDirectUsed = CellLastReferenceLuminance_Direct.IsValidIndex(CellIdx) ? CellLastReferenceLuminance_Direct[CellIdx] : ReferenceLuminanceDirect;
			const float ReferenceLuminanceDiffuseUsed = CellLastReferenceLuminance_Diffuse.IsValidIndex(CellIdx) ? CellLastReferenceLuminance_Diffuse[CellIdx] : ReferenceLuminanceDiffuse;
			const float ReferenceLuminanceDiffuseNoGIUsed = CellLastReferenceLuminance_DiffuseNoGI.IsValidIndex(CellIdx) ? CellLastReferenceLuminance_DiffuseNoGI[CellIdx] : 0.0f;
			const float ReferenceLuminanceTotalNoGIUsed = CellLastReferenceLuminance_TotalNoGI.IsValidIndex(CellIdx) ? CellLastReferenceLuminance_TotalNoGI[CellIdx] : (ReferenceLuminanceDirectUsed + ReferenceLuminanceDiffuseNoGIUsed);
			const bool bSkyOnlyReferenceUsableForDiag = HasUsableSkyOnlyReference(
				ReferenceLuminanceDiffuseNoGIUsed,
				ReferenceLuminanceDiffuseUsed,
				ReferenceLuminanceTotalUsed);
			const bool bSkyOnlyReferenceMeetsMinLuminanceForDiag = ReferenceLuminanceDiffuseNoGIUsed >= ReferenceStripMinLuminance;
			const bool bUseSkyOnlyDiffuseScalingForDiag = (bSkyOnlyReferenceUsableForDiag && bSkyOnlyReferenceMeetsMinLuminanceForDiag)
				&& ReferenceLuminanceTotalNoGIUsed > 1e-6f;
			const float ReferenceLuminanceTerrainUsed = CellLastReferenceLuminance_Terrain.IsValidIndex(CellIdx)
				? CellLastReferenceLuminance_Terrain[CellIdx]
				: (bUseSkyOnlyDiffuseScalingForDiag
					? FMath::Max(ReferenceLuminanceTotalUsed - ReferenceLuminanceTotalNoGIUsed, 0.0f)
					: FMath::Max(
						ReferenceLuminanceTotalUsed
						- ReferenceLuminanceDirectUsed
						- ReferenceLuminanceDiffuseUsed,
						0.0f));
			const float RedistFactor = CellLastRedistributionFactor.IsValidIndex(CellIdx) ? CellLastRedistributionFactor[CellIdx] : 1.0f;

			const float ConservationFactorForDiag = LastMassConservationFactor;
			const float r_UE_raw = CellLastRadiationIndex_UE_Raw.IsValidIndex(CellIdx) ? CellLastRadiationIndex_UE_Raw[CellIdx] : 0.0f;

			Diag.r_UE_raw = r_UE_raw;

			float RadiationComponentDiag = 0.0f;
			if (DegreeDayMeltModel == EDegreeDayMeltModel::HockModel2)
			{
				RadiationComponentDiag = HockSnowRadiationFactor * PotentialDirect;
			}
			else if (DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3Exact)
			{
				RadiationComponentDiag = HockSnowRadiationFactor * PotentialDirect * CloudinessRatioForDiag;
			}
			else if (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated || DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3_UE_SWR)
			{
				const bool bUsePellicciottiPBR = (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_PBR);
				const float AbsorptivityDiag = (bUsePellicciottiPBR && !bUseAlbedoAbsorptivityInPBR)
					? 1.0f
					: FMath::Clamp(1.0f - SnowAlbedoState, 0.0f, 1.0f);
				if (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated)
				{
					const float CalibTotal = CellLastRadiationTotal_UE.IsValidIndex(CellIdx) ? CellLastRadiationTotal_UE[CellIdx] : 0.0f;
					const float CalibDirect = CellLastRadiationDirect_UE.IsValidIndex(CellIdx) ? CellLastRadiationDirect_UE[CellIdx] : 0.0f;
					const float CalibDiffuse = CellLastRadiationDiffuse_UE.IsValidIndex(CellIdx) ? CellLastRadiationDiffuse_UE[CellIdx] : 0.0f;
					const float CalibTerrain = CellLastRadiationTerrain_UE.IsValidIndex(CellIdx) ? CellLastRadiationTerrain_UE[CellIdx] : 0.0f;
					const float CalibSum = (CalibTotal > 0.0f) ? CalibTotal : (CalibDirect + CalibDiffuse + CalibTerrain);
					RadiationComponentDiag = PellicciottiShortwaveFactor_RI * AbsorptivityDiag * CalibSum;
				}
				else if (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_RI || DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_PBR || DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3_UE_SWR)
				{
					RadiationComponentDiag = PellicciottiShortwaveFactor_RI * RadiationIdx * AbsorptivityDiag * Diag.ForcingSWdown_Wm2;
				}
				else
				{
					RadiationComponentDiag = PellicciottiShortwaveFactor * AbsorptivityDiag * Diag.ForcingSWdown_Wm2;
				}
			}

			Diag.SnowAccumulation_m = AccumDepth;
			Diag.MeltAmount_m = MeltDepth;
			Diag.MeltFactor = MeltFactor;
			Diag.MeltRate_mmph = (DtSeconds > 0.0f) ? (MeltDepth / DtSeconds) * 1000.0f * 3600.0f : 0.0f;
			Diag.ComputedSnowRate = ComputedSnowRate;
			Diag.SnowAlbedo = SnowAlbedoState;
			Diag.Qsw_abs_Wm2 = NetShortwaveForDiagnostics_Wm2;
			Diag.RadiationComponent = RadiationComponentDiag;
			Diag.RadiationIndex_Swift = RadiationIdx_Swift;
			Diag.RadiationIndex_UE = RadiationIdx_UE;
			Diag.CloudinessRatio = CloudinessRatioForDiag;  // Always write (GHI/Is for all models)
			Diag.CellSlopeDegrees = CellSlope;
			Diag.RedistributionFactor = RedistFactor;
			Diag.MassConservationFactor = ConservationFactorForDiag;
			Diag.I_Wm2 = PotentialDirect;
			Diag.Is_Wm2 = PotentialHorizontal;
			Diag.RTY_Total = RTYTotal;
			Diag.RTY_Direct = RTYDirect;
			Diag.RTY_Diffuse = RTYDiffuse;
			Diag.RTY_DiffuseNoGI = RTYDiffuseNoGI;
			Diag.RTY_TotalNoGI = RTYTotalNoGI;
			Diag.RTY_Terrain = RTYTerrain;
			Diag.SolarZenith_deg = SolarZenithWorld_deg;
			Diag.SolarElevation_deg = SolarElevationWorld_deg;
			Diag.SolarAzimuth_deg = SolarAzimuthWorld_deg;
			Diag.CosSolarZenith = ClampedCaptureCosSolarZenith;
			Diag.CosSolarZenith_UECapture = ClampedCaptureCosSolarZenith;
			Diag.SunVisibility_UECapture = SunVisibilityCapture;
			Diag.ReferenceLuminance_Total = ReferenceLuminanceTotalUsed;
			Diag.ReferenceLuminance_Direct = ReferenceLuminanceDirectUsed;
			Diag.ReferenceLuminance_Diffuse = ReferenceLuminanceDiffuseUsed;
			Diag.ReferenceLuminance_DiffuseNoGI = ReferenceLuminanceDiffuseNoGIUsed;
			Diag.ReferenceLuminance_TotalNoGI = ReferenceLuminanceTotalNoGIUsed;
			Diag.ReferenceLuminance_Terrain = ReferenceLuminanceTerrainUsed;
			Diag.ReferenceScale_Total = ReferenceScaleTotal;
			Diag.ReferenceScale_Direct = ReferenceScaleDirect;
			Diag.ReferenceScale_Diffuse = ReferenceScaleDiffuse;
			Diag.ReferenceScale_TotalNoGI = ReferenceScaleTotalNoGI;
			Diag.bSkyOnlyReferenceUsable = bSkyOnlyReferenceUsableForDiag;
			Diag.bSkyOnlyReferenceMeetsMinLuminance = bSkyOnlyReferenceMeetsMinLuminanceForDiag;
			Diag.bSkyOnlyDiffuseScalingUsed = bUseSkyOnlyDiffuseScalingForDiag;
			Diag.SkyOnlyReferenceRatio =
				(FMath::IsFinite(ReferenceLuminanceDiffuseNoGIUsed)
					&& FMath::IsFinite(ReferenceLuminanceDiffuseUsed)
					&& ReferenceLuminanceDiffuseUsed > 1e-6f)
				? (ReferenceLuminanceDiffuseNoGIUsed / ReferenceLuminanceDiffuseUsed)
				: 0.0f;
			Diag.SkyOnlyRTYRatio =
				(FMath::IsFinite(RTYDiffuseNoGI)
					&& FMath::IsFinite(RTYDiffuse)
					&& RTYDiffuse > 1e-6f)
				? (RTYDiffuseNoGI / RTYDiffuse)
				: 0.0f;
			bool bReferenceGuardOk = true;
			if (bUseReferenceStripGuard)
			{
				bReferenceGuardOk = (Diag.SolarElevation_deg >= ReferenceStripMinSunElevation_deg)
					&& (ReferenceLuminanceTotalUsed >= ReferenceStripMinLuminance);
			}
			Diag.bReferenceValid = bReferenceTelemetryValid && bReferenceGuardOk;

			const int32 TimestampDayOfYear = Diag.Timestamp.GetDayOfYear();
			const float TimestampHourOfDay = Diag.Timestamp.GetHour() + Diag.Timestamp.GetMinute() / 60.0f;
			const float LocalSolarTimeDiagHours = ComputeLocalSolarTimeHours(Diag.Timestamp, TimestampDayOfYear, SimulationActorLongitudeDeg);
			const float HourAngleDiagRad = FMath::DegreesToRadians(15.0f * (LocalSolarTimeDiagHours - 12.0f));
			const float SolarDeclinationDiagRad = ComputeSolarDeclinationRad(TimestampDayOfYear);
			const float CellAltitudeCmValue = CellAltitudeCm.IsValidIndex(CellIdx) ? CellAltitudeCm[CellIdx] : 0.0f;
			Diag.CellAltitude_m = CellAltitudeCmValue / 100.0f;
			Diag.MeasurementAltitude_m = MeasurementAltitudeCm / 100.0f;
			Diag.GeoOriginElevation_m = GeoReferencingOriginUpCm / 100.0f;
			Diag.AltitudeDelta_m = (CellAltitudeCmValue - MeasurementAltitudeCm) / 100.0f;

			// Terrain geometry for radiation analysis
			const float AspectRad = CellAspectRad.IsValidIndex(CellIdx) ? CellAspectRad[CellIdx] : 0.0f;
			const float InclinationRad = CellInclinationRad.IsValidIndex(CellIdx) ? CellInclinationRad[CellIdx] : 0.0f;
			const float LatitudeRad = CellLatitudeRad.IsValidIndex(CellIdx) ? CellLatitudeRad[CellIdx] : 0.0f;
			Diag.CellAspect_deg = FMath::RadiansToDegrees(AspectRad);
			Diag.CellInclination_deg = FMath::RadiansToDegrees(InclinationRad);
			Diag.CellLatitude_deg = FMath::RadiansToDegrees(LatitudeRad);
			const float CosSolarZenithForcing = FMath::Clamp(
				FMath::Sin(LatitudeRad) * FMath::Sin(SolarDeclinationDiagRad) +
				FMath::Cos(LatitudeRad) * FMath::Cos(SolarDeclinationDiagRad) * FMath::Cos(HourAngleDiagRad),
				-1.0f, 1.0f);
			const float SolarZenithForcing_deg = FMath::RadiansToDegrees(FMath::Acos(CosSolarZenithForcing));
			Diag.CosSolarZenith_Forcing = CosSolarZenithForcing;
			Diag.SolarZenith_Forcing_deg = SolarZenithForcing_deg;
			Diag.SolarElevation_Forcing_deg = 90.0f - SolarZenithForcing_deg;
			Diag.bHasSolarForcing = true;

			// Populate radiation diagnostics from forcing
			Diag.GHI_Wm2 = W.SWdown_Wm2;

			if (bLastTimestepHadValidRadiationForcing)
			{
				Diag.DNI_Wm2 = LastTimestepForcingRad.DNI;
				Diag.DHI_Wm2 = LastTimestepForcingRad.DHI;
				Diag.DiffuseFraction = (LastTimestepForcingRad.DiffuseFraction >= 0.0f)
					? LastTimestepForcingRad.DiffuseFraction
					: ((Diag.GHI_Wm2 > KINDA_SMALL_NUMBER) ? (Diag.DHI_Wm2 / Diag.GHI_Wm2) : 0.0f);
			}
			else
			{
				// Check if forcing data provides direct/diffuse split
				const bool bForcingHasDirectDiffuse = (W.DirectSWdown_Wm2 >= 0.0f && W.DiffuseSWdown_Wm2 >= 0.0f);

				if (bForcingHasDirectDiffuse)
				{
					// Use direct/diffuse values from forcing data (e.g., ERA5 provides this)
					Diag.DNI_Wm2 = W.DirectSWdown_Wm2;
					Diag.DHI_Wm2 = W.DiffuseSWdown_Wm2;
					Diag.DiffuseFraction = (Diag.GHI_Wm2 > KINDA_SMALL_NUMBER) ? (Diag.DHI_Wm2 / Diag.GHI_Wm2) : 0.0f;

					// Create forcing struct for state tracking using measured values
					const float ZenithRad = FMath::Acos(Diag.CosSolarZenith);
					FForcingRadiation RadForcing(Diag.GHI_Wm2, ZenithRad);
					RadForcing.DNI = Diag.DNI_Wm2;
					RadForcing.DHI = Diag.DHI_Wm2;
					RadForcing.DiffuseFraction = Diag.DiffuseFraction;

					// Update state tracking
					if (CellIdx == 0)
					{
						LastTimestepForcingRad = RadForcing;
						bLastTimestepHadValidRadiationForcing = true;
					}
				}
				else
				{
					// Forcing lacks direct/diffuse split - derive using Erbs correlation
					// IMPORTANT: Use UE capture solar zenith (CosSolarZenith) NOT forcing-based (CosSolarZenith_Forcing)
					// The forcing timestamp solar position may differ from UE directional light position,
					// especially at dawn/dusk. Using forcing zenith when sun is below horizon causes
					// catastrophic DNI values (division by near-zero cosine). The UE capture zenith
					// matches the actual light direction used in radiation captures.
					const float ZenithRad = FMath::Acos(Diag.CosSolarZenith);
					FForcingRadiation RadForcing(Diag.GHI_Wm2, ZenithRad);

					// Calculate Direct/Diffuse split using Erbs model
					ComputeDNI_DHI(RadForcing);

					// Populate diagnostics with computed values
					Diag.DNI_Wm2 = RadForcing.DNI;
					Diag.DHI_Wm2 = RadForcing.DHI;
					Diag.DiffuseFraction = RadForcing.DiffuseFraction;

					// Update state tracking
					if (CellIdx == 0) // Only update shared state once per step
					{
						LastTimestepForcingRad = RadForcing;
						bLastTimestepHadValidRadiationForcing = true;
					}
				}
			}

			Diag.DNI_Horiz_Wm2 = Diag.DNI_Wm2 * FMath::Max(0.0f, Diag.CosSolarZenith);

			// Populate new diagnostics
			Diag.RadiationDirect_UE = CellLastRadiationDirect_UE.IsValidIndex(CellIdx) ? CellLastRadiationDirect_UE[CellIdx] : 0.0f;
			Diag.RadiationDiffuse_UE = CellLastRadiationDiffuse_UE.IsValidIndex(CellIdx) ? CellLastRadiationDiffuse_UE[CellIdx] : 0.0f;
			Diag.RadiationTotal_UE = RadiationTotal_UE;
			Diag.RadiationTerrain_UE = RadiationTerrain_UE;
			if (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated)
			{
				const float CalibratedTotal = (RadiationTotal_UE > 0.0f)
					? RadiationTotal_UE
					: (Diag.RadiationDirect_UE + Diag.RadiationDiffuse_UE + Diag.RadiationTerrain_UE);
				Diag.RadiationTotal_UsedForMelt = FMath::Max(0.0f, CalibratedTotal);
			}
			else if (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_RI
				|| DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD_PBR
				|| DegreeDayMeltModel == EDegreeDayMeltModel::HockModel3_UE_SWR)
			{
				Diag.RadiationTotal_UsedForMelt = FMath::Max(0.0f, RadiationIdx * Diag.ForcingSWdown_Wm2);
			}
			else if (DegreeDayMeltModel == EDegreeDayMeltModel::PellicciottiModelD)
			{
				Diag.RadiationTotal_UsedForMelt = FMath::Max(0.0f, Diag.ForcingSWdown_Wm2);
			}
			else if (false)
			{
				Diag.RadiationTotal_UsedForMelt = 0.0f;
			}
			Diag.RadiationIndex_Direct = CellLastRadiationIndex_Direct.IsValidIndex(CellIdx) ? CellLastRadiationIndex_Direct[CellIdx] : 0.0f;
			Diag.RadiationIndex_Diffuse = CellLastRadiationIndex_Diffuse.IsValidIndex(CellIdx) ? CellLastRadiationIndex_Diffuse[CellIdx] : 0.0f;

			if (SimulationActor)
			{
				Diag.SunLightIntensity = SimulationActor->GetLastSunLightIntensity();
				Diag.SkyLightIntensity = SimulationActor->GetLastSkyLightIntensity();
			}

			DiagnosticsBuffer.Add(Diag);
		}
	}

	// Write diagnostics if needed
	if (bCollectDiagnostics && DiagnosticsBuffer.Num() > 0)
	{
		EnsureDiagnosticsFileInitialized();
		EnsureRadiationDiagnosticsFileInitialized();
		EnsureRadiativeTransferDiagnosticsFileInitialized();

		if (SimulationStepCounter % FMath::Max(1, DiagnosticsEveryNSteps) == 0)
		{
			WriteDiagnostics(DiagnosticsBuffer);
			WriteRadiationDiagnostics(DiagnosticsBuffer);
			WriteRadiativeTransferDiagnostics(DiagnosticsBuffer);
		}
	}

	// Upload to PF_R16F texture
	UploadDepthToTexture();

	// Update counters
	SimulationStepCounter++;
	ElapsedSimulationSeconds += DtSeconds;

	// TODO: Handle debug cells if needed
}

void UDegreeDaySimulation::EnsureDiagnosticsFileInitialized()
{
	if (bDiagnosticsFileInitialized)
	{
		return;
	}

	ASnowSimulationActor* Actor = OwningSimulationActor.Get();

	// Create diagnostics directory if it doesn't exist
	FString DiagnosticsDir = Actor
		? Actor->GetOutputCategoryDirectory(TEXT("Diagnostics"))
		: (FPaths::ProjectDir() / DiagnosticsDirectory);
	IFileManager::Get().MakeDirectory(*DiagnosticsDir, true);

	// Generate unique filename with timestamp
	FDateTime Now = FDateTime::Now();
	FString Timestamp = FString::Printf(TEXT("%04d%02d%02d_%02d%02d%02d"),
		Now.GetYear(), Now.GetMonth(), Now.GetDay(),
		Now.GetHour(), Now.GetMinute(), Now.GetSecond());
	const FString RunTag = Actor ? Actor->BuildRunTag(Timestamp) : Timestamp;

	DiagnosticsFilePath = DiagnosticsDir / FString::Printf(TEXT("DegreeDayDiagnostics_%s.csv"), *RunTag);
	MeltModelConfigFilename = DiagnosticsDir / FString::Printf(TEXT("DegreeDay_MeltModel_%s.txt"), *RunTag);

	bDiagnosticsFileInitialized = true;
	bDiagnosticsHeaderWritten = false;

	UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Diagnostics will be written to: %s"), *DiagnosticsFilePath);
	WriteMeltModelConfigFile();
}

void UDegreeDaySimulation::WriteDiagnostics(const TArray<FDegreeDayCellDiagnostics>& CellDiagnostics)
{
	if (!bDiagnosticsFileInitialized || CellDiagnostics.Num() == 0)
	{
		return;
	}

	FString Output;
	bool bIsFirstWrite = !bDiagnosticsHeaderWritten;

	// Write header if this is the first write
	if (bIsFirstWrite)
	{
		Output += TEXT("StepIndex,CellIndex,SimulationTimeSeconds,Timestamp,");
		Output += TEXT("SnowDepth_m,");
		Output += TEXT("ForcingAirTempC,ForcingPrecipRate_kgm2s,ForcingSnowFrac,WeatherSnowFracRaw,");
		Output += TEXT("ForcingPrecipRate_mmph,ForcingSWdown_Wm2,ForcingLWdown_Wm2,ForcingWindSpeed_mps,ForcingRH,ForcingPressure_Pa,");
		Output += FString::Printf(TEXT("SnowAccumulation_m,MeltAmount_m,MeltFactor,MeltRate_mmph,ComputedSnowRate,SnowAlbedo,RadiationComponent,"));
		Output += FString::Printf(TEXT("SunLightIntensity,SkyLightIntensity,RadiationDirect_UE,RadiationDiffuse_UE,RadiationTotal_UsedForMelt,RadiationIndex_Direct,RadiationIndex_Diffuse,"));
		Output += FString::Printf(TEXT("RedistributionFactor,MassConservationFactor,"));
		Output += FString::Printf(TEXT("CellSlopeDegrees,CellAltitude_m,MeasurementAltitude_m,GeoOriginElevation_m,AltitudeDelta_m"));
		Output.AppendChar(TEXT('\n'));
		bDiagnosticsHeaderWritten = true;
		}

	// Write data rows
	for (const FDegreeDayCellDiagnostics& Diag : CellDiagnostics)
	{
		Output += FString::Printf(TEXT("%d,%d,%.2f,%s,"),
			Diag.StepIndex, Diag.CellIndex, Diag.SimulationTimeSeconds,
			*Diag.Timestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")));

		Output += FString::Printf(TEXT("%.6f,"),
			Diag.SnowDepth_m);

		Output += FString::Printf(TEXT("%.2f,%.8f,%.4f,%.4f,"),
			Diag.ForcingAirTemperatureC,
			Diag.ForcingPrecipRate_kgm2s, Diag.ForcingSnowFrac, Diag.WeatherSnowFracRaw);

		Output += FString::Printf(TEXT("%.4f,%.2f,%.2f,%.2f,%.4f,%.1f,"),
			Diag.ForcingPrecipRate_mmph,
			Diag.ForcingSWdown_Wm2, Diag.ForcingLWdown_Wm2,
			Diag.ForcingWindSpeed_mps, Diag.ForcingRH,
			Diag.ForcingPressure_Pa);

		Output += FString::Printf(TEXT("%.6f,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,"),
			Diag.SnowAccumulation_m, Diag.MeltAmount_m,
			Diag.MeltFactor,
			Diag.MeltRate_mmph,
			Diag.ComputedSnowRate,
			Diag.SnowAlbedo, Diag.RadiationComponent);

		// New Radiation Fields
		Output += FString::Printf(TEXT("%.2f,%.4f,%.2f,%.2f,%.2f,%.4f,%.4f,"),
			Diag.SunLightIntensity, Diag.SkyLightIntensity,
			Diag.RadiationDirect_UE, Diag.RadiationDiffuse_UE, Diag.RadiationTotal_UsedForMelt,
			Diag.RadiationIndex_Direct, Diag.RadiationIndex_Diffuse);

		Output += FString::Printf(TEXT("%.6f,%.6f,"),
			Diag.RedistributionFactor, Diag.MassConservationFactor);

		Output += FString::Printf(TEXT("%.2f,%.2f,%.2f,%.2f,%.2f"),
			Diag.CellSlopeDegrees,
			Diag.CellAltitude_m, Diag.MeasurementAltitude_m,
			Diag.GeoOriginElevation_m, Diag.AltitudeDelta_m);

		Output.AppendChar(TEXT('\n'));
	}

	// If we have already written the header (meaning we are past the first step of THIS run),
	// we must append, otherwise we overwrite the previous steps of the current simulation.
	// bAppendDiagnostics controls whether we append to a PREVIOUS run's file.
	const bool bShouldAppend = bAppendDiagnostics || bDiagnosticsHeaderWritten;
	const uint32 WriteFlags = bShouldAppend ? FILEWRITE_Append : FILEWRITE_None;
	bool bSuccess = FFileHelper::SaveStringToFile(Output, *DiagnosticsFilePath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), WriteFlags);

	if (!bSuccess)
	{
		UE_LOG(LogTemp, Error, TEXT("[DegreeDay] Failed to write diagnostics to: %s"), *DiagnosticsFilePath);
	}
	else if (bIsFirstWrite)
	{
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Started diagnostics file: %s (tracking %d cells)"), *DiagnosticsFilePath, CellDiagnostics.Num());
		
		// Save cell location snapshot for visualization
		SaveCellLocationSnapshot(CellDiagnostics);
	}
}

void UDegreeDaySimulation::WriteMeltModelConfigFile()
{
	if (MeltModelConfigFilename.IsEmpty())
	{
		return;
	}

	auto MeltModelToString = [](EDegreeDayMeltModel Model) -> const TCHAR*
	{
		switch (Model)
		{
		case EDegreeDayMeltModel::Enhanced: return TEXT("Enhanced");
		case EDegreeDayMeltModel::HockModel2: return TEXT("Hock Model 2");
		case EDegreeDayMeltModel::HockModel3Exact: return TEXT("Hock Model 3 (Exact)");
		case EDegreeDayMeltModel::HockModel3_UE_SWR: return TEXT("Pellicciotti Model D (Legacy UE+SWR)");
		case EDegreeDayMeltModel::PellicciottiModelD: return TEXT("Pellicciotti Model D");
		case EDegreeDayMeltModel::PellicciottiModelD_RI: return TEXT("Pellicciotti Model D (Lambertian)");
		case EDegreeDayMeltModel::PellicciottiModelD_PBR: return TEXT("Pellicciotti Model D (PBR Material)");
		case EDegreeDayMeltModel::PellicciottiModelD_FluxCalibrated: return TEXT("Pellicciotti Model D (Flux-Calibrated)");
		default: return TEXT("Unknown");
		}
	};

	FString Config;
	Config += TEXT("# DegreeDay Melt Model Configuration\n");
	Config += TEXT("# Generated: ") + FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")) + TEXT("\n\n");

	if (OwningSimulationActor.IsValid())
	{
		const ASnowSimulationActor* Actor = OwningSimulationActor.Get();
		Config += TEXT("[Simulation]\n");
		Config += FString::Printf(TEXT("StartTime = %s\n"), *Actor->GetRunStartTime().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
		Config += FString::Printf(TEXT("EndTime = %s\n"), *Actor->GetRunEndTime().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
		Config += TEXT("\n");
	}

	Config += TEXT("[Melt Model]\n");
	Config += FString::Printf(TEXT("DegreeDayMeltModel = %s\n"), MeltModelToString(this->DegreeDayMeltModel));
	Config += FString::Printf(TEXT("TSnowA = %.2f\n"), this->TSnowA);
	Config += FString::Printf(TEXT("TSnowB = %.2f\n"), this->TSnowB);
	Config += FString::Printf(TEXT("TMeltA = %.2f\n"), this->TMeltA);
	Config += FString::Printf(TEXT("TMeltB = %.2f\n"), this->TMeltB);
	Config += FString::Printf(TEXT("k_m = %.2f\n"), this->k_m);
	Config += TEXT("\n");

	Config += TEXT("[Albedo]\n");
	Config += FString::Printf(TEXT("FreshSnowAlbedo = %.3f\n"), this->FreshSnowAlbedo);
	Config += FString::Printf(TEXT("OldSnowAlbedo = %.3f\n"), this->OldSnowAlbedo);
	Config += FString::Printf(TEXT("k_e = %.3f\n"), this->k_e);
	Config += TEXT("\n");

	Config += TEXT("[Pellicciotti Model D]\n");
	Config += FString::Printf(TEXT("PellicciottiTemperatureFactor = %.5f\n"), this->PellicciottiTemperatureFactor);
	Config += FString::Printf(TEXT("PellicciottiShortwaveFactor = %.5f\n"), this->PellicciottiShortwaveFactor);
	Config += FString::Printf(TEXT("PellicciottiShortwaveFactor_RI = %.5f\n"), this->PellicciottiShortwaveFactor_RI);
	Config += FString::Printf(TEXT("PellicciottiTempThresholdC = %.2f\n"), this->PellicciottiTempThresholdC);
	Config += FString::Printf(TEXT("PellicciottiAlbedoTempSumForOldSnow = %.2f\n"), this->PellicciottiAlbedoTempSumForOldSnow);
	Config += FString::Printf(TEXT("PellicciottiAlbedoMinTempSum = %.2f\n"), this->PellicciottiAlbedoMinTempSum);
	Config += FString::Printf(TEXT("PellicciottiIceAlbedo = %.3f\n"), this->PellicciottiIceAlbedo);
	Config += TEXT("\n");

	Config += TEXT("[Hock Models]\n");
	Config += FString::Printf(TEXT("HockMeltThresholdC = %.2f\n"), this->HockMeltThresholdC);
	Config += FString::Printf(TEXT("HockSnowMeltFactor = %.4f\n"), this->HockSnowMeltFactor);
	Config += FString::Printf(TEXT("HockSnowRadiationFactor = %.6f\n"), this->HockSnowRadiationFactor);
	Config += FString::Printf(TEXT("HockClearSkyTransmissivity = %.3f\n"), this->HockClearSkyTransmissivity);
	Config += TEXT("\n");

	Config += TEXT("[Snow Redistribution]\n");
	Config += FString::Printf(TEXT("bApplySlopeCurvatureRedistribution = %s\n"), this->bApplySlopeCurvatureRedistribution ? TEXT("true") : TEXT("false"));
	Config += FString::Printf(TEXT("MinSnowfallForRedistribution_mm = %.3f\n"), this->MinSnowfallForRedistribution_mm);
	Config += FString::Printf(TEXT("CurvatureRedistributionGain = %.3f\n"), this->CurvatureRedistributionGain);
	Config += FString::Printf(TEXT("SlopeRedistributionStartDeg = %.2f\n"), this->SlopeRedistributionStartDeg);
	Config += FString::Printf(TEXT("SlopeRedistributionZeroDeg = %.2f\n"), this->SlopeRedistributionZeroDeg);
	Config += FString::Printf(TEXT("RedistributionEdgeFadeCells = %d\n"), this->RedistributionEdgeFadeCells);
	Config += FString::Printf(TEXT("MinRedistributionFactor = %.3f\n"), this->MinRedistributionFactor);
	Config += FString::Printf(TEXT("MaxRedistributionFactor = %.3f\n"), this->MaxRedistributionFactor);
	Config += FString::Printf(TEXT("bConserveMassDuringRedistribution = %s\n"), this->bConserveMassDuringRedistribution ? TEXT("true") : TEXT("false"));
	Config += FString::Printf(TEXT("bExcludeEdgeCellsFromMassConservation = %s\n"), this->bExcludeEdgeCellsFromMassConservation ? TEXT("true") : TEXT("false"));
	Config += TEXT("\n");

	Config += TEXT("[Snowpack & Forcing]\n");
	Config += FString::Printf(TEXT("FreshSnowDensity_kgm3 = %.2f\n"), this->FreshSnowDensity_kgm3);
	Config += FString::Printf(TEXT("bUseWeatherSnowFraction = %s\n"), this->bUseWeatherSnowFraction ? TEXT("true") : TEXT("false"));
	Config += FString::Printf(TEXT("bDisableLapseRateAdjustments = %s\n"), this->bDisableLapseRateAdjustments ? TEXT("true") : TEXT("false"));
	Config += FString::Printf(TEXT("bApplyPrecipLapseBelowStation = %s\n"), this->bApplyPrecipLapseBelowStation ? TEXT("true") : TEXT("false"));
	Config += FString::Printf(TEXT("PrecipitationLapseRate_FractionPerKm = %.3f\n"), this->PrecipitationLapseRate_FractionPerKm);
	Config += FString::Printf(TEXT("MeasurementAltitude_m = %.2f\n"), this->MeasurementAltitudeCm / 100.0f);
	Config += FString::Printf(TEXT("bUseDynamicSurfaceGeometry = %s\n"), this->bUseDynamicSurfaceGeometry ? TEXT("true") : TEXT("false"));

	const bool bSuccess = FFileHelper::SaveStringToFile(Config, *MeltModelConfigFilename);
	if (bSuccess)
	{
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Wrote melt model configuration file: %s"), *MeltModelConfigFilename);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[DegreeDay] Failed to write melt model configuration file: %s"), *MeltModelConfigFilename);
	}
}

void UDegreeDaySimulation::SaveCellLocationSnapshot(const TArray<FDegreeDayCellDiagnostics>& CellDiagnostics)
{
	if (!OwningSimulationActor.IsValid() || CellDiagnostics.Num() == 0)
	{
		return;
	}

	ASnowSimulationActor* Actor = OwningSimulationActor.Get();
	if (!Actor)
	{
		return;
	}

	// Build JSON with cell location metadata
	FString Json = TEXT("{\n");
	
	// Grid dimensions
	Json += FString::Printf(TEXT("  \"grid_width\": %d,\n"), Actor->CellsDimensionX);
	Json += FString::Printf(TEXT("  \"grid_height\": %d,\n"), Actor->CellsDimensionY);
	Json += FString::Printf(TEXT("  \"total_cells\": %d,\n"), Actor->CellsDimensionX * Actor->CellsDimensionY);
	
	// Tracked cell indices
	Json += TEXT("  \"tracked_cells\": [\n");
	for (int32 i = 0; i < CellDiagnostics.Num(); ++i)
	{
		const FDegreeDayCellDiagnostics& Diag = CellDiagnostics[i];
		Json += TEXT("    {\n");
		Json += FString::Printf(TEXT("      \"cell_index\": %d,\n"), Diag.CellIndex);
		
		// Grid coordinates
		int32 CellGridX = Diag.CellIndex % Actor->CellsDimensionX;
		int32 CellGridY = Diag.CellIndex / Actor->CellsDimensionX;
		Json += FString::Printf(TEXT("      \"grid_x\": %d,\n"), CellGridX);
		Json += FString::Printf(TEXT("      \"grid_y\": %d,\n"), CellGridY);
		
		// Terrain info
		Json += FString::Printf(TEXT("      \"altitude_m\": %.2f,\n"), Diag.CellAltitude_m);
		Json += FString::Printf(TEXT("      \"slope_deg\": %.2f,\n"), Diag.CellSlopeDegrees);
		
		// World position (if available from landscape cells)
		const TArray<FLandscapeCell>& Cells = Actor->LandscapeCells;
		if (Cells.IsValidIndex(Diag.CellIndex))
		{
			const FLandscapeCell& Cell = Cells[Diag.CellIndex];
			Json += FString::Printf(TEXT("      \"world_x\": %.2f,\n"), Cell.Centroid.X);
			Json += FString::Printf(TEXT("      \"world_y\": %.2f,\n"), Cell.Centroid.Y);
			Json += FString::Printf(TEXT("      \"world_z\": %.2f,\n"), Cell.Centroid.Z);
			Json += FString::Printf(TEXT("      \"aspect_deg\": %.2f\n"), FMath::RadiansToDegrees(Cell.Aspect));
		}
		else
		{
			Json += TEXT("      \"world_x\": 0.0,\n");
			Json += TEXT("      \"world_y\": 0.0,\n");
			Json += TEXT("      \"world_z\": 0.0,\n");
			Json += TEXT("      \"aspect_deg\": 0.0\n");
		}
		
		Json += TEXT("    }");
		if (i < CellDiagnostics.Num() - 1)
		{
			Json += TEXT(",");
		}
		Json += TEXT("\n");
	}
	Json += TEXT("  ],\n");
	
	// Timestamp
	FDateTime Now = FDateTime::Now();
	Json += FString::Printf(TEXT("  \"timestamp\": \"%s\",\n"), *Now.ToString(TEXT("%Y-%m-%d %H:%M:%S")));
	Json += FString::Printf(TEXT("  \"diagnostics_file\": \"%s\"\n"), *FPaths::GetCleanFilename(DiagnosticsFilePath));
	
	Json += TEXT("}\n");
	
	// Save to file
	FString JsonPath = DiagnosticsFilePath.Replace(TEXT(".csv"), TEXT("_CellLocations.json"));
	bool bSuccess = FFileHelper::SaveStringToFile(Json, *JsonPath);
	
	if (bSuccess)
	{
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Saved cell location snapshot: %s"), *JsonPath);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] Failed to save cell location snapshot: %s"), *JsonPath);
	}
}

// ============================================================================
// Radiation Diagnostics Implementation
// ============================================================================

void UDegreeDaySimulation::EnsureRadiationDiagnosticsFileInitialized()
{
	if (!OwningSimulationActor.IsValid())
	{
		return;
	}

	ASnowSimulationActor* Actor = OwningSimulationActor.Get();
	if (!Actor->bEnableRadiationDiagnostics)
	{
		return;
	}

	// If already initialized, return early
	if (!RadiationDiagnosticsFilename.IsEmpty())
	{
		return;
	}

	// Build directory path
	FString DiagDir = Actor->GetOutputCategoryDirectory(TEXT("Radiation"));

	// Create directory if it doesn't exist
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*DiagDir))
	{
		PlatformFile.CreateDirectoryTree(*DiagDir);
	}

	// Generate filename with timestamp
	FDateTime Now = FDateTime::Now();
	FString Timestamp = Now.ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString RunTag = Actor->BuildRunTag(Timestamp);
	FString BaseFilename = FString::Printf(TEXT("DegreeDay_Radiation_%s"), *RunTag);

	RadiationDiagnosticsFilename = FPaths::Combine(DiagDir, BaseFilename + TEXT(".csv"));
	RadiationConfigFilename = FPaths::Combine(DiagDir, BaseFilename + TEXT("_config.txt"));

	UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Radiation diagnostics files initialized:"));
	UE_LOG(LogTemp, Display, TEXT("  Data: %s"), *RadiationDiagnosticsFilename);
	UE_LOG(LogTemp, Display, TEXT("  Config: %s"), *RadiationConfigFilename);

	// Write configuration file immediately
	bRadiationDiagnosticsHeaderWritten = false;
	WriteRadiationConfigFile();
}

void UDegreeDaySimulation::WriteRadiationDiagnostics(const TArray<FDegreeDayCellDiagnostics>& CellDiagnostics)
{
	if (!OwningSimulationActor.IsValid() || CellDiagnostics.Num() == 0)
	{
		return;
	}

	ASnowSimulationActor* Actor = OwningSimulationActor.Get();
	if (!Actor->bEnableRadiationDiagnostics || RadiationDiagnosticsFilename.IsEmpty())
	{
		return;
	}

	FString Output;
	bool bFileExists = FPaths::FileExists(RadiationDiagnosticsFilename);
	bool bIsFirstWrite = !bFileExists || !Actor->bAppendRadiationDiagnostics;

	// Write header if this is the first write
	if (bIsFirstWrite)
	{
		Output += TEXT("StepIndex,CellIndex,SimulationTimeSeconds,Timestamp,");
		Output += TEXT("GHI_Wm2,DNI_Wm2,DNI_Horiz_Wm2,DHI_Wm2,DiffuseFraction,CloudCover_01,");
		Output += TEXT("SolarZenith_Forcing_deg,SolarElevation_Forcing_deg,CosSolarZenith_Forcing,");
		Output += TEXT("SolarZenith_deg,SolarElevation_deg,SolarAzimuth_deg,CosSolarZenith,SunVisibility_UECapture,");
		Output += TEXT("ReferenceValid,ReferenceLuminance_Total,ReferenceLuminance_Direct,ReferenceLuminance_Diffuse,ReferenceLuminance_DiffuseNoGI,ReferenceLuminance_TotalNoGI,ReferenceLuminance_Terrain,");
		Output += TEXT("DualReferenceStripEnabled,ReferenceSurfaceClass,ReferenceSelectionSnowDepth_m,ReferenceSelectionThreshold_m,");
		Output += TEXT("ReferenceLuminance_Direct_Ground_Current,ReferenceLuminance_Direct_Snow_Current,");
		Output += TEXT("ReferenceScale_Total,ReferenceScale_Direct,ReferenceScale_Diffuse,ReferenceScale_TotalNoGI,");
		Output += TEXT("DiffuseScalingMode,SkyOnlyReferenceUsable,SkyOnlyReferenceMeetsMinLuminance,SkyOnlyDiffuseScalingUsed,SkyOnlyReferenceRatio,SkyOnlyRTYRatio,");
		Output += TEXT("RTY_Total,RTY_Direct,RTY_Diffuse,RTY_DiffuseNoGI,RTY_TotalNoGI,RTY_Terrain,");
		Output += TEXT("r_UE_raw,RadiationIndex_UE,RadiationIndex_Swift,RadiationIndex_Direct,RadiationIndex_Diffuse,RadiationComponent,CloudinessRatio,");
		Output += TEXT("RadiationDirect_UE,RadiationDiffuse_UE,RadiationTotal_UE,RadiationTotal_UsedForMelt,RadiationTerrain_UE,");
		Output += TEXT("SunLightIntensity,SkyLightIntensity,");
		Output += TEXT("I_Wm2,Is_Wm2,");
		Output += TEXT("CellSlopeDegrees,CellAspect_deg,CellLatitude_deg,");
        Output += TEXT("Qsw_abs_Wm2,SnowAlbedo");
		Output.AppendChar(TEXT('\n'));
		bRadiationDiagnosticsHeaderWritten = true;
	}

	// Write data rows
	for (const FDegreeDayCellDiagnostics& Diag : CellDiagnostics)
	{
		const bool bDualReferenceStripEnabled = Actor ? Actor->bUseDualReferenceStrip : false;
		const float ReferenceSelectionThreshold_m = Actor ? FMath::Max(0.0f, Actor->DualReferenceSnowDepthThreshold_m) : 0.0f;
		bool bUsedRenderSurfaceStateReference = false;
		bool bUsedDualReferencePlausibilityOverride = false;
		const float ReferenceSnowBlendWeight = DD_ResolveDualReferenceSnowBlendWeight(
			Actor,
			Diag.CellIndex,
			Diag.SnowDepth_m,
			Actor ? Actor->bUseTotalRadiationIndexOnly : false,
			bUsedRenderSurfaceStateReference,
			bUsedDualReferencePlausibilityOverride);
		(void)bUsedRenderSurfaceStateReference;
		(void)bUsedDualReferencePlausibilityOverride;
		FString ReferenceSurfaceClass = TEXT("SingleStrip");
		if (bDualReferenceStripEnabled)
		{
			if (ReferenceSnowBlendWeight <= 1.0e-3f)
			{
				ReferenceSurfaceClass = TEXT("GroundHalf");
			}
			else if (ReferenceSnowBlendWeight >= (1.0f - 1.0e-3f))
			{
				ReferenceSurfaceClass = TEXT("SnowHalf");
			}
			else
			{
				ReferenceSurfaceClass = TEXT("Blended");
			}
		}
		const float ReferenceLuminanceDirectGroundCurrent = Actor ? Actor->ReferenceLuminance_Direct_Ground : 0.0f;
		const float ReferenceLuminanceDirectSnowCurrent = Actor ? Actor->ReferenceLuminance_Direct_Snow : 0.0f;

		Output += FString::Printf(TEXT("%d,%d,%.2f,%s,"),
			Diag.StepIndex, Diag.CellIndex, Diag.SimulationTimeSeconds,
			*Diag.Timestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")));

		Output += FString::Printf(TEXT("%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,"),
			Diag.GHI_Wm2, Diag.DNI_Wm2, Diag.DNI_Horiz_Wm2, Diag.DHI_Wm2, Diag.DiffuseFraction, Diag.ForcingCloudCover_01);

		Output += FString::Printf(TEXT("%.4f,%.4f,%.4f,"),
			Diag.SolarZenith_Forcing_deg, Diag.SolarElevation_Forcing_deg, Diag.CosSolarZenith_Forcing);

		Output += FString::Printf(TEXT("%.4f,%.4f,%.4f,%.4f,%.4f,"),
			Diag.SolarZenith_deg, Diag.SolarElevation_deg, Diag.SolarAzimuth_deg, Diag.CosSolarZenith, Diag.SunVisibility_UECapture);

		Output += FString::Printf(TEXT("%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"),
			Diag.bReferenceValid ? TEXT("true") : TEXT("false"),
			Diag.ReferenceLuminance_Total,
			Diag.ReferenceLuminance_Direct,
			Diag.ReferenceLuminance_Diffuse,
			Diag.ReferenceLuminance_DiffuseNoGI,
			Diag.ReferenceLuminance_TotalNoGI,
			Diag.ReferenceLuminance_Terrain);

		Output += FString::Printf(TEXT("%d,%s,%.6f,%.6f,%.4f,%.4f,%.6f,%.6f,%.6f,%.6f,"),
			bDualReferenceStripEnabled ? 1 : 0,
			*ReferenceSurfaceClass,
			Diag.SnowDepth_m,
			ReferenceSelectionThreshold_m,
			ReferenceLuminanceDirectGroundCurrent,
			ReferenceLuminanceDirectSnowCurrent,
			Diag.ReferenceScale_Total,
			Diag.ReferenceScale_Direct,
			Diag.ReferenceScale_Diffuse,
			Diag.ReferenceScale_TotalNoGI);

		Output += FString::Printf(TEXT("%s,%d,%d,%d,%.6f,%.6f,"),
			Diag.bSkyOnlyDiffuseScalingUsed ? TEXT("total_nogi_closure") : TEXT("full_diffuse"),
			Diag.bSkyOnlyReferenceUsable ? 1 : 0,
			Diag.bSkyOnlyReferenceMeetsMinLuminance ? 1 : 0,
			Diag.bSkyOnlyDiffuseScalingUsed ? 1 : 0,
			Diag.SkyOnlyReferenceRatio,
			Diag.SkyOnlyRTYRatio);

		Output += FString::Printf(TEXT("%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"),
			Diag.RTY_Total, Diag.RTY_Direct, Diag.RTY_Diffuse, Diag.RTY_DiffuseNoGI, Diag.RTY_TotalNoGI, Diag.RTY_Terrain);

		Output += FString::Printf(TEXT("%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"),
			Diag.r_UE_raw,
			Diag.RadiationIndex_UE,
			Diag.RadiationIndex_Swift,
			Diag.RadiationIndex_Direct,
			Diag.RadiationIndex_Diffuse,
			Diag.RadiationComponent,
			Diag.CloudinessRatio);

		Output += FString::Printf(TEXT("%.2f,%.2f,%.2f,%.2f,%.2f,"),
			Diag.RadiationDirect_UE, Diag.RadiationDiffuse_UE, Diag.RadiationTotal_UE, Diag.RadiationTotal_UsedForMelt, Diag.RadiationTerrain_UE);

		Output += FString::Printf(TEXT("%.2f,%.4f,"),
			Diag.SunLightIntensity, Diag.SkyLightIntensity);

		Output += FString::Printf(TEXT("%.2f,%.2f,"),
			Diag.I_Wm2, Diag.Is_Wm2);

		Output += FString::Printf(TEXT("%.2f,%.2f,%.4f,"),
			Diag.CellSlopeDegrees, Diag.CellAspect_deg, Diag.CellLatitude_deg);

		Output += FString::Printf(TEXT("%.4f,%.4f"),
			Diag.Qsw_abs_Wm2, Diag.SnowAlbedo);

		Output.AppendChar(TEXT('\n'));
	}

	const uint32 WriteFlags = (bFileExists && Actor->bAppendRadiationDiagnostics) ? FILEWRITE_Append : FILEWRITE_None;
	bool bSuccess = FFileHelper::SaveStringToFile(Output, *RadiationDiagnosticsFilename, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), WriteFlags);

	if (!bSuccess)
	{
		UE_LOG(LogTemp, Error, TEXT("[DegreeDay] Failed to write radiation diagnostics to: %s"), *RadiationDiagnosticsFilename);
	}
	else if (bIsFirstWrite)
	{
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Started radiation diagnostics file: %s (tracking %d cells)"),
			*RadiationDiagnosticsFilename, CellDiagnostics.Num());
	}
}

void UDegreeDaySimulation::EnsureRadiativeTransferDiagnosticsFileInitialized()
{
	if (!OwningSimulationActor.IsValid())
	{
		return;
	}

	ASnowSimulationActor* Actor = OwningSimulationActor.Get();
	if (!Actor->bEnableRadiationDiagnostics)
	{
		return;
	}

	if (!RadiativeTransferDiagnosticsFilename.IsEmpty())
	{
		return;
	}

	FString DiagDir = Actor->GetOutputCategoryDirectory(TEXT("Radiation"));

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*DiagDir))
	{
		PlatformFile.CreateDirectoryTree(*DiagDir);
	}

	FString Timestamp;
	if (!RadiationDiagnosticsFilename.IsEmpty())
	{
		const FString BaseName = FPaths::GetBaseFilename(RadiationDiagnosticsFilename);
		const FString Prefix = TEXT("DegreeDay_Radiation_");
		if (BaseName.StartsWith(Prefix))
		{
			Timestamp = BaseName.RightChop(Prefix.Len());
		}
	}

	if (Timestamp.IsEmpty())
	{
		Timestamp = Actor->BuildRunTag(FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	const FString BaseFilename = FString::Printf(TEXT("RadiativeTransfer_%s"), *Timestamp);
	RadiativeTransferDiagnosticsFilename = FPaths::Combine(DiagDir, BaseFilename + TEXT(".csv"));

	UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Radiative transfer diagnostics initialized: %s"), *RadiativeTransferDiagnosticsFilename);
	bRadiativeTransferDiagnosticsHeaderWritten = false;
}

void UDegreeDaySimulation::WriteRadiativeTransferDiagnostics(const TArray<FDegreeDayCellDiagnostics>& CellDiagnostics)
{
	if (!OwningSimulationActor.IsValid() || CellDiagnostics.Num() == 0)
	{
		return;
	}

	ASnowSimulationActor* Actor = OwningSimulationActor.Get();
	if (!Actor->bEnableRadiationDiagnostics || RadiativeTransferDiagnosticsFilename.IsEmpty())
	{
		return;
	}

	FString Output;
	const bool bFileExists = FPaths::FileExists(RadiativeTransferDiagnosticsFilename);
	const bool bIsFirstWrite = !bFileExists || !Actor->bAppendRadiationDiagnostics;

	if (bIsFirstWrite)
	{
		Output += TEXT("StepIndex,CellIndex,SimulationTimeSeconds,Timestamp,SnowDepth_m,");
		Output += TEXT("GHI_Wm2,DNI_Wm2,DNI_Horiz_Wm2,DHI_Wm2,DiffuseFraction,");
		Output += TEXT("ReferenceValid,ReferenceLuminance_Total,ReferenceLuminance_Direct,ReferenceLuminance_Diffuse,ReferenceLuminance_DiffuseNoGI,ReferenceLuminance_TotalNoGI,ReferenceLuminance_Terrain,");
		Output += TEXT("ReferenceScale_Total,ReferenceScale_Direct,ReferenceScale_Diffuse,ReferenceScale_TotalNoGI,");
		Output += TEXT("DiffuseScalingMode,SkyOnlyReferenceUsable,SkyOnlyReferenceMeetsMinLuminance,SkyOnlyDiffuseScalingUsed,SkyOnlyReferenceRatio,SkyOnlyRTYRatio,");
		Output += TEXT("RTY_Total,RTY_Direct,RTY_Diffuse,RTY_DiffuseNoGI,RTY_TotalNoGI,RTY_Terrain,");
		Output += TEXT("RadiationDirect_UE,RadiationDiffuse_UE,RadiationTerrain_UE,RadiationTotal_UE,RadiationTotal_UsedForMelt,");
		Output += TEXT("RadiationIndex_Direct,RadiationIndex_Diffuse");
		Output.AppendChar(TEXT('\n'));
		bRadiativeTransferDiagnosticsHeaderWritten = true;
	}

	for (const FDegreeDayCellDiagnostics& Diag : CellDiagnostics)
	{
		Output += FString::Printf(TEXT("%d,%d,%.2f,%s,%.4f,"),
			Diag.StepIndex, Diag.CellIndex, Diag.SimulationTimeSeconds,
			*Diag.Timestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")),
			Diag.SnowDepth_m);

		Output += FString::Printf(TEXT("%.2f,%.2f,%.2f,%.2f,%.4f,"),
			Diag.GHI_Wm2, Diag.DNI_Wm2, Diag.DNI_Horiz_Wm2, Diag.DHI_Wm2, Diag.DiffuseFraction);

		Output += FString::Printf(TEXT("%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"),
			Diag.bReferenceValid ? TEXT("true") : TEXT("false"),
			Diag.ReferenceLuminance_Total,
			Diag.ReferenceLuminance_Direct,
			Diag.ReferenceLuminance_Diffuse,
			Diag.ReferenceLuminance_DiffuseNoGI,
			Diag.ReferenceLuminance_TotalNoGI,
			Diag.ReferenceLuminance_Terrain);

		Output += FString::Printf(TEXT("%.6f,%.6f,%.6f,%.6f,"),
			Diag.ReferenceScale_Total,
			Diag.ReferenceScale_Direct,
			Diag.ReferenceScale_Diffuse,
			Diag.ReferenceScale_TotalNoGI);

		Output += FString::Printf(TEXT("%s,%d,%d,%d,%.6f,%.6f,"),
			Diag.bSkyOnlyDiffuseScalingUsed ? TEXT("total_nogi_closure") : TEXT("full_diffuse"),
			Diag.bSkyOnlyReferenceUsable ? 1 : 0,
			Diag.bSkyOnlyReferenceMeetsMinLuminance ? 1 : 0,
			Diag.bSkyOnlyDiffuseScalingUsed ? 1 : 0,
			Diag.SkyOnlyReferenceRatio,
			Diag.SkyOnlyRTYRatio);

		Output += FString::Printf(TEXT("%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"),
			Diag.RTY_Total, Diag.RTY_Direct, Diag.RTY_Diffuse, Diag.RTY_DiffuseNoGI, Diag.RTY_TotalNoGI, Diag.RTY_Terrain);

		Output += FString::Printf(TEXT("%.2f,%.2f,%.2f,%.2f,%.2f,"),
			Diag.RadiationDirect_UE, Diag.RadiationDiffuse_UE, Diag.RadiationTerrain_UE, Diag.RadiationTotal_UE, Diag.RadiationTotal_UsedForMelt);

		Output += FString::Printf(TEXT("%.4f,%.4f"),
			Diag.RadiationIndex_Direct, Diag.RadiationIndex_Diffuse);

		Output.AppendChar(TEXT('\n'));
	}

	const uint32 WriteFlags = (bFileExists && Actor->bAppendRadiationDiagnostics) ? FILEWRITE_Append : FILEWRITE_None;
	const bool bSuccess = FFileHelper::SaveStringToFile(Output, *RadiativeTransferDiagnosticsFilename, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), WriteFlags);

	if (!bSuccess)
	{
		UE_LOG(LogTemp, Error, TEXT("[DegreeDay] Failed to write radiative transfer diagnostics to: %s"), *RadiativeTransferDiagnosticsFilename);
	}
	else if (bIsFirstWrite)
	{
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Started radiative transfer diagnostics file: %s (tracking %d cells)"),
			*RadiativeTransferDiagnosticsFilename, CellDiagnostics.Num());
	}
}

void UDegreeDaySimulation::WriteRadiationConfigFile()
{
	if (!OwningSimulationActor.IsValid() || RadiationConfigFilename.IsEmpty())
	{
		return;
	}

	ASnowSimulationActor* Actor = OwningSimulationActor.Get();

	FString Config;
	Config += TEXT("# DegreeDay Radiation Diagnostics Configuration\n");
	Config += TEXT("# Generated: ") + FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")) + TEXT("\n\n");

	Config += TEXT("[Radiation Index Method]\n");
	Config += FString::Printf(TEXT("RadiationIndexMethod = %s\n"),
		this->RadiationIndexMethod == ERadiationIndexMethod::Swift ? TEXT("Swift (Geometric)") : TEXT("UnrealEngine (Scene-based)"));
	Config += FString::Printf(TEXT("ReferenceLuminance = %.3f\n"), this->ReferenceLuminance);
	Config += FString::Printf(TEXT("bUseLowSunAngleFallback = %s\n"), this->bUseLowSunAngleFallback ? TEXT("true") : TEXT("false"));
	Config += FString::Printf(TEXT("LowSunAngleThreshold_deg = %.1f\n"), this->LowSunAngleThreshold_deg);
	Config += FString::Printf(TEXT("bUseReferenceStripGuard = %s\n"), this->bUseReferenceStripGuard ? TEXT("true") : TEXT("false"));
	Config += FString::Printf(TEXT("ReferenceStripMinSunElevation_deg = %.1f\n"), this->ReferenceStripMinSunElevation_deg);
	Config += FString::Printf(TEXT("ReferenceStripMinLuminance = %.3f\n"), this->ReferenceStripMinLuminance);
	Config += FString::Printf(TEXT("bUseTotalReferenceForTerrainResidual = %s\n"), this->bUseTotalReferenceForTerrainResidual ? TEXT("true") : TEXT("false"));
	Config += FString::Printf(TEXT("ReferenceStripMaxTotalScale = %.2f\n"), this->ReferenceStripMaxTotalScale);
	Config += FString::Printf(TEXT("bUseSplitRadiationForPellicciotti = %s\n"), this->bUseSplitRadiationForPellicciotti ? TEXT("true") : TEXT("false"));

	Config += TEXT("[Radiation Capture Settings]\n");
	Config += FString::Printf(TEXT("bEnableRadiationCapture = %s\n"), Actor->bEnableRadiationCapture ? TEXT("true") : TEXT("false"));
	if (Actor->bEnableRadiationCapture)
	{
		Config += FString::Printf(TEXT("bDirectCaptureIncludesAtmosphere = %s\n"), Actor->bDirectCaptureIncludesAtmosphere ? TEXT("true") : TEXT("false"));
		Config += FString::Printf(TEXT("RadiationPrimingFrameCount = %d\n"), Actor->RadiationPrimingFrameCount);
		Config += FString::Printf(TEXT("RadiationCaptureEV100 = %.1f\n"), Actor->RadiationCaptureEV100);
		Config += FString::Printf(TEXT("bBlurDiffuse = %s\n"), Actor->bBlurDiffuse ? TEXT("true") : TEXT("false"));
		Config += FString::Printf(TEXT("MaxRadiationIndexClamp = %.1f\n"), Actor->MaxRadiationIndexClamp);
		Config += FString::Printf(TEXT("RadiationSunVisibilityThreshold = %.4f\n"), Actor->RadiationSunVisibilityThreshold);
		Config += FString::Printf(TEXT("bEnableTimeIntegratedRadiation = %s\n"), Actor->bEnableTimeIntegratedRadiation ? TEXT("true") : TEXT("false"));
		if (Actor->bEnableTimeIntegratedRadiation)
		{
			Config += FString::Printf(TEXT("bCenterTimeIntegrationWindow = %s\n"), Actor->bCenterTimeIntegrationWindow ? TEXT("true") : TEXT("false"));
			Config += FString::Printf(TEXT("RadiationIntegrationSubstepMinutes = %d\n"), Actor->RadiationIntegrationSubstepMinutes);
		}
	}
	Config += TEXT("\n");

	Config += TEXT("[DegreeDay Model Parameters]\n");
	Config += FString::Printf(TEXT("DegreeDayMeltModel = %d\n"), static_cast<int32>(this->DegreeDayMeltModel));
	Config += FString::Printf(TEXT("RadiationIndexMethod = %d\n"), static_cast<int32>(this->RadiationIndexMethod));
	Config += FString::Printf(TEXT("PellicciottiTemperatureFactor = %.5f\n"), this->PellicciottiTemperatureFactor);
	Config += FString::Printf(TEXT("PellicciottiShortwaveFactor = %.5f\n"), this->PellicciottiShortwaveFactor);
	Config += FString::Printf(TEXT("PellicciottiShortwaveFactor_RI = %.5f\n"), this->PellicciottiShortwaveFactor_RI);
	Config += FString::Printf(TEXT("PellicciottiTempThresholdC = %.2f\n"), this->PellicciottiTempThresholdC);
	Config += FString::Printf(TEXT("PellicciottiAlbedoTempSumForOldSnow = %.2f\n"), this->PellicciottiAlbedoTempSumForOldSnow);
	Config += FString::Printf(TEXT("PellicciottiAlbedoMinTempSum = %.2f\n"), this->PellicciottiAlbedoMinTempSum);
	Config += FString::Printf(TEXT("PellicciottiIceAlbedo = %.3f\n"), this->PellicciottiIceAlbedo);
	Config += TEXT("\n");

	Config += TEXT("[Tracked Cells]\n");
	Config += FString::Printf(TEXT("TrackedCellCount = %d\n"), this->DiagnosticsTrackedCellIndices.Num());
	Config += TEXT("TrackedCellIndices = [");
	for (int32 i = 0; i < this->DiagnosticsTrackedCellIndices.Num(); ++i)
	{
		Config += FString::Printf(TEXT("%d"), this->DiagnosticsTrackedCellIndices[i]);
		if (i < this->DiagnosticsTrackedCellIndices.Num() - 1)
		{
			Config += TEXT(", ");
		}
	}
	Config += TEXT("]\n");

	bool bSuccess = FFileHelper::SaveStringToFile(Config, *RadiationConfigFilename);

	if (bSuccess)
	{
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Wrote radiation configuration file: %s"), *RadiationConfigFilename);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[DegreeDay] Failed to write radiation configuration file: %s"), *RadiationConfigFilename);
	}
}

void UDegreeDaySimulation::DrawDiagnosticCellsDebug()
{
	static bool bLoggedLegacyMarkerDeprecation = false;
	if (bShowDiagnosticCells && !bLoggedLegacyMarkerDeprecation)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[DegreeDay] Legacy diagnostic cell markers are disabled. Use SnowSimulationActor -> Snow Debug overlay instead."));
		bLoggedLegacyMarkerDeprecation = true;
	}
}

float UDegreeDaySimulation::ComputePotentialDirectRadiationHock(float LatitudeRad, float SlopeRad, float AspectRad,
	float SolarDeclinationRad, float HourAngleRad, float ExtraterrestrialNormal, float PressurePa) const
{
	float CosZenith = 0.0f;
	float SinZenith = 0.0f;
	float SolarAzimuth = 0.0f;
	ComputeSolarAngles(LatitudeRad, SolarDeclinationRad, HourAngleRad, CosZenith, SinZenith, SolarAzimuth);

	if (CosZenith <= 0.0f)
	{
		return 0.0f;
	}

	// Hock (1999) Model 2 Equations 3 & 4:
	// Equation 3: I = Iâ (Râ/R)Â² (P/Pâ)^(cos Z/Ïâ) cos Î¸
	// Equation 4: cos Î¸ = cos Î² cos Z + sin Î² sin Z cos(Ï_sun - Ï_slope)
	// where Î² is slope angle, Z is zenith angle, Ï_sun is solar azimuth, Ï_slope is slope aspect
	//
	// COORDINATE CONVENTION FIX:
	// - ComputeSolarAngles() uses: 0Â° = North, 90Â° = West, 180Â° = South, 270Â° = East (clockwise from North, positive WEST)
	// - AspectRad uses: 0Â° = North, 90Â° = East, 180Â° = South, 270Â° = West (clockwise from North, positive EAST)
	// - Both reference North at 0Â°, but East-West are MIRRORED!
	// - Solar: clockwise is WEST, Aspect: clockwise is EAST
	// - Conversion: Negate the azimuth to flip East-West, then wrap to [0, 2Ï]
	float SolarAzimuthTerrain = -SolarAzimuth;
	if (SolarAzimuthTerrain < 0.0f)
	{
		SolarAzimuthTerrain += 2.0f * PI;
	}

	const float ClampedSlope = FMath::Clamp(SlopeRad, 0.0f, PI * 0.5f);
	const float SinSlope = FMath::Sin(ClampedSlope);
	const float CosSlope = FMath::Cos(ClampedSlope);
	const float CosIncidence = CosSlope * CosZenith + SinSlope * SinZenith * FMath::Cos(SolarAzimuthTerrain - AspectRad);

	if (CosIncidence <= 0.0f)
	{
		return 0.0f;
	}

	// Hock (1999) Equation 3: I = Iâ Ïâ^m cos Î¸
	// where m is optical air mass = P/(PâÂ·cos Z) for direct radiation
	const float SeaLevelPressure = 101325.0f;
	const float PressureRatio = (PressurePa > 0.0f) ? FMath::Clamp(PressurePa / SeaLevelPressure, 0.0f, 10.0f) : 1.0f;
	const float OpticalAirMass = PressureRatio / FMath::Max(CosZenith, 0.1f);
	const float Transmissivity = FMath::Pow(FMath::Clamp(HockClearSkyTransmissivity, 0.0f, 1.0f), OpticalAirMass);
	const float Potential = ExtraterrestrialNormal * Transmissivity * CosIncidence;
	return FMath::Max(0.0f, Potential);
}

void UDegreeDaySimulation::UpdateDynamicSurfaceGeometry(const FVector& North)
{
	// Recompute aspect and inclination for each cell based on current snow surface (VHM = terrain + snow depth)
	// This accounts for how snow accumulation smooths terrain and changes local slope/aspect
	const ASnowSimulationActor* ActorPtr = OwningSimulationActor.Get();
	const bool bUseLegacyCurvatureCellSizeScaling = ActorPtr ? ActorPtr->bNormalizeCurvatureByCellSize : false;
	const float CurvatureReferenceMeters = ActorPtr ? FMath::Max(0.001f, ActorPtr->CurvatureReferenceMeters) : 10.0f;
	const float CurvatureClampAbs = ActorPtr ? ActorPtr->CurvatureClampAbs : 0.0f;

	const int32 CellCount = DepthMeters.Num();
	if (CellCount <= 0 || CellCount != CellBaseP0.Num())
	{
		return;
	}

	const auto GetCornerDepthMeters = [&](int32 CornerX, int32 CornerY) -> float
	{
		float Sum = 0.0f;
		int32 Count = 0;
		for (int32 OffsetY = -1; OffsetY <= 0; ++OffsetY)
		{
			const int32 CellY = CornerY + OffsetY;
			if (CellY < 0 || CellY >= GridY)
			{
				continue;
			}
			for (int32 OffsetX = -1; OffsetX <= 0; ++OffsetX)
			{
				const int32 CellX = CornerX + OffsetX;
				if (CellX < 0 || CellX >= GridX)
				{
					continue;
				}
				const int32 CellIdx = CellX + CellY * GridX;
				Sum += DepthMeters[CellIdx];
				++Count;
			}
		}
		return (Count > 0) ? (Sum / static_cast<float>(Count)) : 0.0f;
	};

	// Precompute North direction vectors for aspect calculation
	FVector SafeNorth = North;
	if (!SafeNorth.Normalize())
	{
		SafeNorth = FVector(0.0f, 1.0f, 0.0f);
	}

	FVector NorthXY3D(SafeNorth.X, SafeNorth.Y, 0.0f);
	if (!NorthXY3D.Normalize())
	{
		NorthXY3D = FVector(0.0f, 1.0f, 0.0f);
	}
	FVector2D North2D(NorthXY3D.X, NorthXY3D.Y);

	// Cross product gives perpendicular vector, but we need to ensure it points East
	// For right-handed coordinate system: East = Up Ã North (reversed order)
	FVector East3D = FVector::CrossProduct(FVector::UpVector, SafeNorth);
	if (!East3D.Normalize())
	{
		East3D = FVector(1.0f, 0.0f, 0.0f);
	}
	FVector2D East2D(East3D.X, East3D.Y);
	if (!East2D.Normalize())
	{
		East2D = FVector2D(1.0f, 0.0f);
	}

	ParallelFor(CellCount, [&](int32 Idx)
	{
		const int32 X = Idx % GridX;
		const int32 Y = Idx / GridX;

		// Get base terrain corners (in cm, UE coordinates)
		const FVector& BaseP0 = CellBaseP0[Idx];
		const FVector& BaseP1 = CellBaseP1[Idx];
		const FVector& BaseP2 = CellBaseP2[Idx];
		const FVector& BaseP3 = CellBaseP3[Idx];

		// Add interpolated snow depth (convert from meters to cm) to Z component
		const float SnowDepthP0_cm = GetCornerDepthMeters(X, Y) * 100.0f;
		const float SnowDepthP1_cm = GetCornerDepthMeters(X + 1, Y) * 100.0f;
		const float SnowDepthP2_cm = GetCornerDepthMeters(X, Y + 1) * 100.0f;
		const float SnowDepthP3_cm = GetCornerDepthMeters(X + 1, Y + 1) * 100.0f;
		FVector P0 = BaseP0 + FVector(0.0f, 0.0f, SnowDepthP0_cm);
		FVector P1 = BaseP1 + FVector(0.0f, 0.0f, SnowDepthP1_cm);
		FVector P2 = BaseP2 + FVector(0.0f, 0.0f, SnowDepthP2_cm);
		FVector P3 = BaseP3 + FVector(0.0f, 0.0f, SnowDepthP3_cm);

		// Recompute surface normal from snow surface
		FVector Normal = FVector::CrossProduct(P1 - P0, P2 - P0);
		Normal += FVector::CrossProduct(P3 - P1, P2 - P1);
		if (!Normal.Normalize())
		{
			// Degenerate cell, keep original values
			return;
		}
		if (Normal.Z < 0.0f)
		{
			Normal *= -1.0f;
		}

		// Recompute inclination (slope angle) from the upward-facing normal
		const float Inclination = FMath::Acos(FMath::Clamp(Normal.Z, -1.0f, 1.0f));

		// Recompute aspect from surface normal
		FVector2D NormalProjXY(Normal.X, Normal.Y);
		FVector2D Normal2D = NormalProjXY.GetSafeNormal();

		float NorthComponent = FVector2D::DotProduct(Normal2D, North2D);
		float EastComponent = FVector2D::DotProduct(Normal2D, East2D);
		// The horizontal component of the upward normal points upslope; keep this convention consistent
		// with the initial terrain aspect.
		float Aspect = FMath::Atan2(EastComponent, NorthComponent);
		Aspect = NormalizeAngle360(Aspect);

		// Update cell geometry
		CellAspectRad[Idx] = Aspect;
		CellInclinationRad[Idx] = Inclination;
		if (TerrainSlopeDegrees.IsValidIndex(Idx))
		{
			TerrainSlopeDegrees[Idx] = FMath::RadiansToDegrees(Inclination);
		}
	});

	// Recompute curvature from snow surface using 3x3 kernel (same algorithm as initialization)
	if (CellSpacingMeters > KINDA_SMALL_NUMBER && GridX > 0 && GridY > 0)
	{
		const float L = CellSpacingMeters;  // Cell spacing in meters
		const float L2 = L * L;

		const auto ClampIndex = [&](int32 X, int32 Y) -> int32
		{
			X = FMath::Clamp(X, 0, GridX - 1);
			Y = FMath::Clamp(Y, 0, GridY - 1);
			return X + Y * GridX;
		};

		for (int32 Y = 0; Y < GridY; ++Y)
		{
			for (int32 X = 0; X < GridX; ++X)
			{
				const int32 Idx = X + Y * GridX;

				// Get snow surface heights (terrain + snow depth) in meters for 3x3 kernel
				// Layout:  Z3  Z2  Z1
				//          Z6  Z5  Z4
				//          Z9  Z8  Z7
				const int32 IdxNW = ClampIndex(X - 1, Y - 1);  // Z3
				const int32 IdxN  = ClampIndex(X,     Y - 1);  // Z2
				const int32 IdxNE = ClampIndex(X + 1, Y - 1);  // Z1
				const int32 IdxW  = ClampIndex(X - 1, Y);      // Z6
				const int32 IdxC  = ClampIndex(X,     Y);      // Z5 (center)
				const int32 IdxE  = ClampIndex(X + 1, Y);      // Z4
				const int32 IdxSW = ClampIndex(X - 1, Y + 1);  // Z9
				const int32 IdxS  = ClampIndex(X,     Y + 1);  // Z8
				const int32 IdxSE = ClampIndex(X + 1, Y + 1);  // Z7

				// Surface height = base terrain altitude + snow depth
				const float Z1 = (CellAltitudeCm[IdxNE] / 100.0f) + DepthMeters[IdxNE];  // NE
				const float Z2 = (CellAltitudeCm[IdxN]  / 100.0f) + DepthMeters[IdxN];   // N
				const float Z3 = (CellAltitudeCm[IdxNW] / 100.0f) + DepthMeters[IdxNW];  // NW
				const float Z4 = (CellAltitudeCm[IdxE]  / 100.0f) + DepthMeters[IdxE];   // E
				const float Z5 = (CellAltitudeCm[IdxC]  / 100.0f) + DepthMeters[IdxC];   // Center
				const float Z6 = (CellAltitudeCm[IdxW]  / 100.0f) + DepthMeters[IdxW];   // W
				const float Z7 = (CellAltitudeCm[IdxSE] / 100.0f) + DepthMeters[IdxSE];  // SE
				const float Z8 = (CellAltitudeCm[IdxS]  / 100.0f) + DepthMeters[IdxS];   // S
				const float Z9 = (CellAltitudeCm[IdxSW] / 100.0f) + DepthMeters[IdxSW];  // SW

				// Compute second derivatives (curvature)
				// D = second derivative in X direction
				// E = second derivative in Y direction
				const float D = ((Z4 + Z6) / 2.0f - Z5) / L2;
				const float E = ((Z2 + Z8) / 2.0f - Z5) / L2;
				float Curv = 2.0f * (D + E);

				if (bUseLegacyCurvatureCellSizeScaling)
				{
					const float ScaleL2 = (L * L) / (CurvatureReferenceMeters * CurvatureReferenceMeters);
					Curv *= ScaleL2;

					static bool bLoggedLegacyCurvatureScalingDynamic = false;
					if (!bLoggedLegacyCurvatureScalingDynamic)
					{
						bLoggedLegacyCurvatureScalingDynamic = true;
						UE_LOG(LogTemp, Warning, TEXT("[DegreeDay] Dynamic geometry uses legacy curvature cell-size scaling ((L/L_ref)^2)."));
					}
				}

				if (CurvatureClampAbs > 0.0f)
				{
					Curv = FMath::Clamp(Curv, -CurvatureClampAbs, CurvatureClampAbs);
				}

				// Update curvature (inherited TerrainCurvature array from base class)
				if (TerrainCurvature.IsValidIndex(Idx))
				{
					TerrainCurvature[Idx] = Curv;
				}
			}
		}
	}

	static bool bLoggedDynamicGeometry = false;
	if (!bLoggedDynamicGeometry)
	{
		bLoggedDynamicGeometry = true;
		UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Dynamic surface geometry enabled - aspect/slope/curvature updated from snow surface"));
		if (CellCount > 0 && TerrainCurvature.Num() > 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[DegreeDay] Cell[0]: SnowDepth=%.3f m, Aspect=%.1fÂ°, Inclination=%.1fÂ°, Curvature=%.4f"),
				DepthMeters[0], FMath::RadiansToDegrees(CellAspectRad[0]), FMath::RadiansToDegrees(CellInclinationRad[0]), TerrainCurvature[0]);
		}
	}
}
