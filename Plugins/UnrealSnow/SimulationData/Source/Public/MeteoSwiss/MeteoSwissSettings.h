#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Frost/FrostTypes.h"
#include "MeteoSwissSettings.generated.h"

USTRUCT(BlueprintType)
struct SIMULATIONDATA_API FMeteoSwissVariableSource
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, config, Category="MeteoSwiss")
	bool bInclude = true;

	UPROPERTY(EditAnywhere, config, Category="MeteoSwiss", meta=(ToolTip="Leave empty to use the primary StationId"))
	FString StationIdOverride;
};

/** Project-wide settings for retrieving meteorological data from the MeteoSwiss OGD station files. */
UCLASS(config=Engine, defaultconfig, meta=(DisplayName="MeteoSwiss Settings"))
class SIMULATIONDATA_API UMeteoSwissSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UMeteoSwissSettings();

	UPROPERTY(EditAnywhere, config, Category="Request", meta=(ToolTip="Station identifier, e.g. WFJ or DAV"))
	FString StationId;

	UPROPERTY(EditAnywhere, config, Category="Request", meta=(ToolTip="Base STAC items URL. Wrap the value in quotes when editing ini files to prevent // from being treated as a comment."))
	FString StacItemsBaseUrl;

	UPROPERTY(EditAnywhere, config, Category="Request", meta=(ClampMin="1", ClampMax="87600"))
	int32 HoursBack;

	UPROPERTY(EditAnywhere, config, Category="Variables")
	FMeteoSwissVariableSource TemperatureSource;

	UPROPERTY(EditAnywhere, config, Category="Variables")
	FMeteoSwissVariableSource RelativeHumiditySource;

	UPROPERTY(EditAnywhere, config, Category="Variables")
	FMeteoSwissVariableSource WindSpeedSource;

	UPROPERTY(EditAnywhere, config, Category="Variables")
	FMeteoSwissVariableSource WindDirectionSource;

	UPROPERTY(EditAnywhere, config, Category="Variables")
	FMeteoSwissVariableSource PrecipitationSource;

	UPROPERTY(EditAnywhere, config, Category="Variables")
	FMeteoSwissVariableSource ShortwaveSource;

	UPROPERTY(EditAnywhere, config, Category="Variables")
	FMeteoSwissVariableSource DiffuseShortwaveSource;

	UPROPERTY(EditAnywhere, config, Category="Variables")
	FMeteoSwissVariableSource LongwaveSource;

	UPROPERTY(EditAnywhere, config, Category="Variables")
	FMeteoSwissVariableSource PressureSource;

	UPROPERTY(EditAnywhere, config, Category="Site", meta=(ClampMin="-90", ClampMax="90"))
	double Latitude;

	UPROPERTY(EditAnywhere, config, Category="Site", meta=(ClampMin="-180", ClampMax="180"))
	double Longitude;

	UPROPERTY(EditAnywhere, config, Category="Site", meta=(ClampMin="-500", ClampMax="9000"))
	float ElevationM;

	UPROPERTY(EditAnywhere, config, Category="Output")
	FFilePath OutputJsonPath;

	static const UMeteoSwissSettings* Get()
	{
		return GetDefault<UMeteoSwissSettings>();
	}

	virtual FName GetCategoryName() const override;

	FSiteMeta BuildSiteMeta() const;
	FString GetResolvedStacItemsBaseUrl(bool& bOutUsedFallback) const;
	FString GetNormalizedStationIdLower() const;
	FString GetNormalizedStationIdUpper() const;
	FString NormalizeStationId(const FString& InStationId, bool bUppercase) const;
};
