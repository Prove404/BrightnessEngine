#pragma once

#include "CoreMinimal.h"
#include "SimulationBase.h"
#include "Engine/Texture2D.h"
#include "Util/TextureUtil.h"
#include "Cells/LandscapeCell.h"
#include "SnowSimulation.generated.h"

/**
 * Shared base for snow simulations that manage a PF_R16F snow depth texture and CPU depth buffer.
 */
USTRUCT(BlueprintType)
struct SIMULATION_API FTerrainRedistributionSettings
{
	GENERATED_BODY()

	/** If true, apply terrain redistribution of snowfall using slope/curvature metadata. */
	UPROPERTY(EditAnywhere)
	bool bApplySlopeCurvatureRedistribution = true;

	/** If true, recompute slope/curvature from the evolving snow surface after each timestep. */
	UPROPERTY(EditAnywhere)
	bool bUseDynamicSurfaceGeometry = false;

	/** Minimum snowfall (mm per timestep) required before redistribution is applied. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.0", UIMin = "0.0"))
	float MinSnowfallForRedistribution_mm = 1.0f;

	/** Slope (deg) where redistribution starts reducing accumulation. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float SlopeRedistributionStartDeg = 10.0f;

	/** Slope (deg) where redistribution reaches the minimum factor before curvature weighting. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1.0", ClampMax = "89.0"))
	float SlopeRedistributionZeroDeg = 60.0f;

	/** Linear gain used in the curvature term (1 + gain * curvature). */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.0", UIMin = "0.0"))
	float CurvatureRedistributionGain = 50.0f;

	/** Number of cells near the boundary where redistribution fades back toward 1.0. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0"))
	int32 RedistributionEdgeFadeCells = 5;

	/** Lower clamp bound for the redistribution factor. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.0", UIMin = "0.0"))
	float MinRedistributionFactor = 0.0f;

	/** Upper clamp bound for the redistribution factor. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.0", UIMin = "0.0"))
	float MaxRedistributionFactor = 2.0f;

	/** If true, normalize redistribution factors so the domain conserves snowfall mass. */
	UPROPERTY(EditAnywhere)
	bool bConserveMassDuringRedistribution = true;

	/** If true, exclude edge-faded cells from the mass-conservation normalization. */
	UPROPERTY(EditAnywhere)
	bool bExcludeEdgeCellsFromMassConservation = true;
};

UCLASS(Abstract, BlueprintType, EditInlineNew, DefaultToInstanced)
class SIMULATION_API USnowSimulation : public USimulationBase
{
	GENERATED_BODY()

public:
	// Snow material properties
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Simulation | Snow Properties", meta=(ClampMin="10.0", ClampMax="600.0"))
	float FreshSnowDensity_kgm3 = 100.0f; // user-tunable density for converting precipitation mass to snow depth

	// Bring base overload into scope to avoid name-hiding warnings
	using USimulationBase::Initialize;
	// Blueprint-friendly initializer for grid-based snow simulations
	UFUNCTION(BlueprintNativeEvent, Category="Snow|Sim")
	void Initialize(int32 GX, int32 GY, float CellMeters);
	virtual void Initialize_Implementation(int32 GX, int32 GY, float CellMeters)
	{
		InitializeGrid(GX, GY, CellMeters);
	}

	// Transient PF_R16F texture holding depth in meters
	UPROPERTY(Transient)
	UTexture2D* SnowMapTexture = nullptr;

	// Grid resolution
	int32 GridX = 0;
	int32 GridY = 0;

	// CPU depth buffer in meters
	TArray<float> DepthMeters;

protected:
	// Optional per-cell terrain metadata aligned to DepthMeters (GridX * GridY)
	TArray<float> TerrainAltitudeCm;     // cm
	TArray<float> TerrainSlopeDegrees;   // degrees
	TArray<float> TerrainCurvature;      // unitless curvature
	bool bHasTerrainMetadata = false;

public:
	// Ensure texture exists and matches size/format
	virtual void EnsureSnowTexture(int32 InWidth, int32 InHeight, EPixelFormat InFormat = PF_R16F)
	{
		if (InWidth <= 0 || InHeight <= 0)
		{
			return;
		}

		bool bNeedsCreate = (SnowMapTexture == nullptr)
			|| (SnowMapTexture->GetSizeX() != InWidth)
			|| (SnowMapTexture->GetSizeY() != InHeight)
			|| (SnowMapTexture->GetPixelFormat() != InFormat);

		if (bNeedsCreate)
		{
			SnowMapTexture = UTexture2D::CreateTransient(InWidth, InHeight, InFormat);
			if (SnowMapTexture)
			{
				SnowMapTexture->SRGB = false;
				SnowMapTexture->CompressionSettings = TC_HDR;
				SnowMapTexture->MipGenSettings = TMGS_NoMipmaps;
				SnowMapTexture->NeverStream = true;
				SnowMapTexture->AddressX = TA_Clamp;
				SnowMapTexture->AddressY = TA_Clamp;
				SnowMapTexture->Filter = TF_Bilinear;
				SnowMapTexture->UpdateResource();
				
				// One-time log after texture creation
				UE_LOG(LogTemp, Display, TEXT("[Snow] Created SnowMapTexture: PF=%s, sRGB=%s, size=%dx%d"), 
					*UEnum::GetValueAsString(SnowMapTexture->GetPixelFormat()),
					SnowMapTexture->SRGB ? TEXT("true") : TEXT("false"),
					SnowMapTexture->GetSizeX(), SnowMapTexture->GetSizeY());
			}
		}
	}

	// Helper to setup grid and allocate CPU buffer
	virtual void InitializeGrid(int32 InGridX, int32 InGridY, float /*CellMeters*/)
	{
		GridX = InGridX;
		GridY = InGridY;
		DepthMeters.SetNum(GridX * GridY, EAllowShrinking::No);
		for (float& V : DepthMeters) { V = 0.0f; }
		EnsureSnowTexture(GridX, GridY, PF_R16F);
	}

	// Optional: supply terrain metadata for redistribution models
	virtual void SetTerrainMetadata(const TArray<FLandscapeCell>& Cells, int32 DimX, int32 DimY)
	{
		if (DimX <= 0 || DimY <= 0 || Cells.Num() != DimX * DimY)
		{
			bHasTerrainMetadata = false;
			TerrainAltitudeCm.Reset();
			TerrainSlopeDegrees.Reset();
			TerrainCurvature.Reset();
			return;
		}
		TerrainAltitudeCm.SetNum(DimX * DimY, EAllowShrinking::No);
		TerrainSlopeDegrees.SetNum(DimX * DimY, EAllowShrinking::No);
		TerrainCurvature.SetNum(DimX * DimY, EAllowShrinking::No);
		for (int32 idx = 0; idx < Cells.Num(); ++idx)
		{
			TerrainAltitudeCm[idx] = Cells[idx].Altitude;
			TerrainSlopeDegrees[idx] = FMath::RadiansToDegrees(Cells[idx].Inclination);
			TerrainCurvature[idx] = Cells[idx].Curvature;
		}
		bHasTerrainMetadata = true;
	}

	// Never return nullptr when GridX/Y are valid
	virtual UTexture* GetSnowMapTexture() override
	{
		EnsureSnowTexture(GridX, GridY, PF_R16F);
		return SnowMapTexture;
	}

	// Upload DepthMeters (float meters) to PF_R16F texture using correct strides
	virtual void UploadDepthToTexture()
	{
		if (!SnowMapTexture || GridX <= 0 || GridY <= 0)
		{
			return;
		}
		if (DepthMeters.Num() != GridX * GridY)
		{
			// resize or bail; safest is to bail to avoid garbage
			return;
		}
		UpdateTextureR16FFromFloat(SnowMapTexture, GridX, GridY, DepthMeters);
	}

	// Optional stepping interface for simple sims
	virtual void Step(float /*DtSeconds*/, const FWeatherForcingData& /*W*/, TArray<float>& /*OutDepthMeters*/)
	{
		// no-op by default
	}

	// Virtual accessor for per-cell albedo state; simulations that track albedo should override
	virtual TArray<float> GetCellSnowAlbedoState() const
	{
		return TArray<float>();
	}

	UFUNCTION(CallInEditor, Category="Snow Simulation | Actions")
	void DebugFillDepth(float MaxDepthMeters = 0.2f)
	{
#if !UE_BUILD_SHIPPING
		if (!GIsEditor)
		{
			checkf(false, TEXT("Debug depth writer is active — disable before shipping."));
			return;
		}
#else
		checkf(false, TEXT("Debug depth writer is active — disable before shipping."));
		return;
#endif

		if (GridX <= 0 || GridY <= 0)
		{
			return;
		}
		DepthMeters.SetNum(GridX * GridY, EAllowShrinking::No);
		for (int32 y = 0; y < GridY; ++y)
		{
			for (int32 x = 0; x < GridX; ++x)
			{
				const float t = (GridX > 1) ? (static_cast<float>(x) / static_cast<float>(GridX - 1)) : 0.0f;
				DepthMeters[y * GridX + x] = t * MaxDepthMeters;
			}
		}
		UploadDepthToTexture();
	}
};
