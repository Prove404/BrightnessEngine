#pragma once

#include "CoreMinimal.h"
#include "SimulationWeatherDataProviderBase.h"
#include "Frost/FrostTypes.h"
#include "ERA5WeatherProvider.generated.h"

class UFrostWeatherProvider;

/** Weather provider that reads ERA5 JSON files produced by the editor fetcher. */
UCLASS(ClassGroup=(Weather), Blueprintable, BlueprintType, meta=(BlueprintSpawnableComponent))
class SIMULATIONDATA_API UERA5WeatherProvider : public USimulationWeatherDataProviderBase
{
    GENERATED_BODY()

public:
    UERA5WeatherProvider();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ERA5")
    FFilePath JsonPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ERA5")
    bool UseDerivedRadiationIfMissing = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ERA5")
    FSiteMeta Site;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ERA5")
    TArray<FWeatherSample> Samples;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ERA5")
    FDateTime RequestedStartUtc;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ERA5")
    FDateTime RequestedEndUtc;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ERA5")
    FDateTime DatasetStartUtc;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ERA5")
    FDateTime DatasetEndUtc;

    // USimulationWeatherDataProviderBase interface
    virtual void Initialize(FDateTime StartTime, FDateTime EndTime) override;
    virtual float GetMeasurementAltitude() override { return Site.ElevM * 100.0f; }
    virtual TResourceArray<FClimateData>* CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime) override;
    virtual FWeatherForcingData GetWeatherForcing(FDateTime Time, int32 GridX = 0, int32 GridY = 0) override;

    UFUNCTION(BlueprintCallable, Category="ERA5")
    int32 LoadFromJson();

    bool GetWeatherAtUTC(const FDateTime& TimeUTC, FWeatherSample& OutSample) const;

    void ApplySiteOverride(const FSiteMeta& InSite);

private:
    void SyncSettingsToBackend();
    void SyncCacheFromBackend();

    UPROPERTY()
    TObjectPtr<UFrostWeatherProvider> Backend;
};

/** Actor wrapper for the ERA5 weather provider. */
UCLASS(Blueprintable, meta=(DisplayName="ERA5 Weather Provider"))
class SIMULATIONDATA_API AERA5WeatherProviderActor : public AActor
{
    GENERATED_BODY()

public:
    AERA5WeatherProviderActor();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ERA5")
    USceneComponent* SceneRoot;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ERA5")
    UERA5WeatherProvider* Provider;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ERA5")
    FFilePath JsonPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ERA5")
    bool UseDerivedRadiationIfMissing = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ERA5")
    FSiteMeta Site;

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void BeginPlay() override;

    UFUNCTION(CallInEditor, BlueprintCallable, Category="ERA5")
    void RefreshFromJson();

#if WITH_EDITOR
    UFUNCTION(CallInEditor, Category="ERA5|Editor")
    void PullERA5Now();
#endif

private:
    void SyncComponentSettings();
};
