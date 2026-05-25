#pragma once

#include "CoreMinimal.h"
#include "SimulationWeatherDataProviderBase.h"
#include "Frost/FrostTypes.h"
#include "Components/SceneComponent.h"
#include "Engine/EngineTypes.h"
#include "FrostWeatherProvider.generated.h"

/** Weather provider that reads Frost observation JSON files. */
UCLASS(ClassGroup=(Weather), Blueprintable, BlueprintType, meta=(BlueprintSpawnableComponent))
class SIMULATIONDATA_API UFrostWeatherProvider : public USimulationWeatherDataProviderBase
{
	GENERATED_BODY()

public:
	UFrostWeatherProvider();

	// Path to the Frost JSON output file produced by the editor fetcher.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost")
	FFilePath JsonPath;

	// If true, approximate radiation fluxes when the dataset is missing them.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost")
	bool UseDerivedRadiationIfMissing = true;

	// Metadata describing the observation site (required for derived radiation).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost")
	FSiteMeta Site;

	// Optional provider (e.g. ERA5) used to fill gaps when primary observations are missing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost|GapFill")
	TObjectPtr<USimulationWeatherDataProviderBase> AuxiliaryProvider;

	// Elevation (m ASL) of the auxiliary source's grid cell — used for lapse-rate correction to station elevation.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost|GapFill")
	float AuxiliaryElevM = 0.0f;

	// Hours on each side of a detected gap used to estimate the primary-vs-auxiliary temperature bias.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost|GapFill")
	int32 BiasWindowHours = 48;

	// Access raw samples (read only).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Frost")
	TArray<FWeatherSample> Samples;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Frost")
	FDateTime RequestedStartUtc;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Frost")
	FDateTime RequestedEndUtc;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Frost")
	FDateTime DatasetStartUtc;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Frost")
	FDateTime DatasetEndUtc;

	// USimulationWeatherDataProviderBase interface
	virtual void Initialize(FDateTime StartTime, FDateTime EndTime) override;
	virtual float GetMeasurementAltitude() override { return Site.ElevM * 100.0f; }
	virtual TResourceArray<FClimateData>* CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime) override;
	virtual FWeatherForcingData GetWeatherForcing(FDateTime Time, int32 GridX = 0, int32 GridY = 0) override;

	// Reload JSON from disk. Returns number of samples parsed.
	UFUNCTION(BlueprintCallable, Category="Frost")
	int32 LoadFromJson();

	bool GetWeatherAtUTC(const FDateTime& TimeUTC, FWeatherSample& OutSample) const;


private:
	void EnsureSamplesSorted();
	int32 FindSampleIndex(const FDateTime& TimeUTC) const;
	void BuildGapFills();

	mutable bool bSamplesSorted;

	// Per-hour additive bias (°C) to apply when gap-filling from the auxiliary provider.
	TMap<FDateTime, float> GapFillBiasCache;
};

/** Actor wrapper so Frost providers can be placed directly in a level. */
UCLASS(Blueprintable, meta=(DisplayName="Frost Weather Provider"))
class SIMULATIONDATA_API AFrostWeatherProviderActor : public AActor
{
	GENERATED_BODY()

public:
	AFrostWeatherProviderActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Frost")
	USceneComponent* SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Frost")
	UFrostWeatherProvider* Provider;

	// Path for the Frost JSON file on disk.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost")
	FFilePath JsonPath;

	// Whether to approximate radiation fluxes when the dataset omits them.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost")
	bool UseDerivedRadiationIfMissing = true;

	// Site metadata used by the radiation estimator.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost")
	FSiteMeta Site;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

	UFUNCTION(CallInEditor, BlueprintCallable, Category="Frost")
	void RefreshFromJson();

#if WITH_EDITOR
	UFUNCTION(CallInEditor, Category="Frost|Editor")
	void PullFrostNow();
#endif


private:
	void SyncComponentSettings();
};
