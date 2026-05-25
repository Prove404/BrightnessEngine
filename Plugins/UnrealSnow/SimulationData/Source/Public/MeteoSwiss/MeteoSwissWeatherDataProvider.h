#pragma once

#include "CoreMinimal.h"
#include "SimulationWeatherDataProviderBase.h"
#include "Frost/FrostTypes.h"
#include "MeteoSwissWeatherDataProvider.generated.h"

class UFrostWeatherProvider;
class USceneComponent;

/** Weather provider that reads MeteoSwiss JSON files produced by the editor fetcher. */
UCLASS(ClassGroup=(Weather), Blueprintable, BlueprintType, meta=(BlueprintSpawnableComponent))
class SIMULATIONDATA_API UMeteoSwissWeatherDataProvider : public USimulationWeatherDataProviderBase
{
	GENERATED_BODY()

public:
	UMeteoSwissWeatherDataProvider();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss")
	FFilePath JsonPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss")
	bool UseDerivedRadiationIfMissing = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss")
	FSiteMeta Site;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="MeteoSwiss")
	TArray<FWeatherSample> Samples;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="MeteoSwiss")
	FDateTime RequestedStartUtc;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="MeteoSwiss")
	FDateTime RequestedEndUtc;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="MeteoSwiss")
	FDateTime DatasetStartUtc;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="MeteoSwiss")
	FDateTime DatasetEndUtc;

	// Actor carrying the auxiliary weather provider (e.g. an ERA5WeatherProviderActor) used to
	// gap-fill missing primary observations.  Pick any level actor whose first
	// USimulationWeatherDataProviderBase component will be used automatically.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss|GapFill")
	TObjectPtr<AActor> AuxiliaryProviderActor;

	// Elevation (m ASL) of the auxiliary source's grid cell — used for lapse-rate correction.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss|GapFill")
	float AuxiliaryElevM = 0.0f;

	// Hours on each side of a gap used to estimate the primary-vs-auxiliary temperature bias.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss|GapFill")
	int32 BiasWindowHours = 48;

	virtual void Initialize(FDateTime StartTime, FDateTime EndTime) override;
	virtual float GetMeasurementAltitude() override { return Site.ElevM * 100.0f; }
	virtual TResourceArray<FClimateData>* CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime) override;
	virtual FWeatherForcingData GetWeatherForcing(FDateTime Time, int32 GridX = 0, int32 GridY = 0) override;

	UFUNCTION(BlueprintCallable, Category="MeteoSwiss")
	int32 LoadFromJson();

	bool GetWeatherAtUTC(const FDateTime& TimeUTC, FWeatherSample& OutSample) const;

	void ApplySiteOverride(const FSiteMeta& InSite);

private:
	void SyncSettingsToBackend();
	void SyncCacheFromBackend();

	UPROPERTY()
	TObjectPtr<UFrostWeatherProvider> Backend;
};

/** Actor wrapper for the MeteoSwiss weather provider. */
UCLASS(Blueprintable, meta=(DisplayName="MeteoSwiss Weather Provider"))
class SIMULATIONDATA_API AMeteoSwissWeatherProviderActor : public AActor
{
	GENERATED_BODY()

public:
	AMeteoSwissWeatherProviderActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="MeteoSwiss")
	USceneComponent* SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="MeteoSwiss")
	UMeteoSwissWeatherDataProvider* Provider;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss")
	FFilePath JsonPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss")
	bool UseDerivedRadiationIfMissing = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss")
	FSiteMeta Site;

	// Actor carrying the auxiliary weather provider used to gap-fill missing observations.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss|GapFill")
	TObjectPtr<AActor> AuxiliaryProviderActor;

	// Elevation (m ASL) of the auxiliary source's grid cell — used for lapse-rate correction.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss|GapFill")
	float AuxiliaryElevM = 0.0f;

	// Hours on each side of a gap used to estimate the primary-vs-auxiliary temperature bias.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MeteoSwiss|GapFill")
	int32 BiasWindowHours = 48;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

	UFUNCTION(CallInEditor, BlueprintCallable, Category="MeteoSwiss")
	void RefreshFromJson();

#if WITH_EDITOR
	UFUNCTION(CallInEditor, Category="MeteoSwiss|Editor")
	void PullMeteoSwissNow();
#endif

private:
	void SyncComponentSettings();
};
