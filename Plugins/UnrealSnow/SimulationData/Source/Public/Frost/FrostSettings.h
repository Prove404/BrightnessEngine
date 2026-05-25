#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FrostSettings.generated.h"

/** Project-wide settings for retrieving meteorological data from the Frost API. */
UCLASS(config=Engine, defaultconfig, meta=(DisplayName="Frost Settings"))
class SIMULATIONDATA_API UFrostSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UFrostSettings();

	// The Frost API client identifier (username for basic auth).
	UPROPERTY(EditAnywhere, config, Category="Authentication")
	FString ClientId;

	// The Frost API client secret (password for basic auth).
	UPROPERTY(EditAnywhere, config, Category="Authentication")
	FString ClientSecret;

	// Comma separated list of Frost station identifiers.
	UPROPERTY(EditAnywhere, config, Category="Request", meta=(ToolTip="Station identifiers, e.g. SN18700"))
	FString Sources;

	// Frost element identifiers. Leave empty to omit an element from the pull.
	UPROPERTY(EditAnywhere, config, Category="Request|Elements", meta=(DisplayName="Air Temperature Element"))
	FString AirTemperatureElementId;

	UPROPERTY(EditAnywhere, config, Category="Request|Elements", meta=(DisplayName="Relative Humidity Element"))
	FString RelativeHumidityElementId;

	UPROPERTY(EditAnywhere, config, Category="Request|Elements", meta=(DisplayName="Wind Speed Element"))
	FString WindSpeedElementId;

	UPROPERTY(EditAnywhere, config, Category="Request|Elements", meta=(DisplayName="Wind Direction Element"))
	FString WindDirectionElementId;

	UPROPERTY(EditAnywhere, config, Category="Request|Elements", meta=(DisplayName="Precipitation Amount Element"))
	FString PrecipitationAmountElementId;

	UPROPERTY(EditAnywhere, config, Category="Request|Elements", meta=(DisplayName="Precipitation Type Element"))
	FString PrecipitationTypeElementId;

	UPROPERTY(EditAnywhere, config, Category="Request|Elements", meta=(DisplayName="Shortwave Downwelling Element"))
	FString ShortwaveFluxElementId;

	UPROPERTY(EditAnywhere, config, Category="Request|Elements", meta=(DisplayName="Diffuse Shortwave Downwelling Element"))
	FString DiffuseShortwaveFluxElementId;

	UPROPERTY(EditAnywhere, config, Category="Request|Elements", meta=(DisplayName="Longwave Downwelling Element"))
	FString LongwaveFluxElementId;

	UPROPERTY(EditAnywhere, config, Category="Request|Elements", meta=(DisplayName="Surface Air Pressure Element"))
	FString SurfacePressureElementId;

	/** Get the global Frost settings instance. */
	static const UFrostSettings* Get()
	{
		return GetDefault<UFrostSettings>();
	}

    // Number of hours back from now to request.
    UPROPERTY(EditAnywhere, config, Category="Request", meta=(ClampMin="1", ClampMax="168"))
    int32 HoursBack;

    // If true, interpret SnowSimulationActor StartTime/EndTime as local clock time
    // and convert them to UTC using SimulationUtcOffsetHours when pulling data
    // and when initializing providers. If false, all times are treated as UTC.
    UPROPERTY(EditAnywhere, config, Category="Request", meta=(DisplayName="Treat Simulation Times As Local"))
    bool bTreatSimulationTimesAsLocal = false;

    // The fixed UTC offset (in hours) to apply if bTreatSimulationTimesAsLocal is true.
    // Example: +2 for CEST summer time, +1 for CET winter time. Does not auto-handle DST.
    UPROPERTY(EditAnywhere, config, Category="Request", meta=(ClampMin="-12", ClampMax="14", DisplayName="Simulation UTC Offset (hours)", EditCondition="bTreatSimulationTimesAsLocal"))
    int32 SimulationUtcOffsetHours = 0;

    // Destination for the generated JSON weather series file (supports {ProjectDir}, {ProjectSavedDir}, {ProjectContentDir}).
    UPROPERTY(EditAnywhere, config, Category="Output")
    FFilePath OutputJsonPath;

    // UDeveloperSettings interface
	virtual FName GetCategoryName() const override;
};
