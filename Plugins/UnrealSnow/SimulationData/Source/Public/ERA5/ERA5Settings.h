#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Frost/FrostTypes.h"
#include "ERA5Settings.generated.h"

UENUM(BlueprintType)
enum class EERA5DataSource : uint8
{
    OpenMeteo     UMETA(DisplayName="Open-Meteo ERA5"),
    CopernicusCDS UMETA(DisplayName="Copernicus CDS")
};

/** Project-wide settings for retrieving ERA5 reanalysis data. */
UCLASS(config=Engine, defaultconfig, meta=(DisplayName="ERA5 Settings"))
class SIMULATIONDATA_API UERA5Settings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UERA5Settings();

    /** Primary datasource used when pulling ERA5 data. */
    UPROPERTY(EditAnywhere, config, Category="Request")
    EERA5DataSource DataSource;

    /** Base URL for the Open-Meteo ERA5 endpoint. */
    UPROPERTY(EditAnywhere, config, Category="Request", meta=(ToolTip="Full ERA5 endpoint. Wrap the value in quotes when editing ini files to prevent // from being treated as a comment."))
    FString ApiBaseUrl;

    /** Copernicus Data Store API endpoint. */
    UPROPERTY(EditAnywhere, config, Category="CDS", meta=(EditCondition="DataSource == EERA5DataSource::CopernicusCDS"))
    FString CdsApiUrl;

    /** Copernicus Data Store access token. */
    UPROPERTY(EditAnywhere, config, Category="CDS", meta=(EditCondition="DataSource == EERA5DataSource::CopernicusCDS", PasswordField="true"))
    FString CdsApiKey;

    /** Dataset identifier used when talking to the Copernicus Data Store. */
    UPROPERTY(EditAnywhere, config, Category="CDS", meta=(EditCondition="DataSource == EERA5DataSource::CopernicusCDS"))
    FString CdsDataset;

    /** Latitude in decimal degrees. */
    UPROPERTY(EditAnywhere, config, Category="Request", meta=(ClampMin="-90", ClampMax="90"))
    double Latitude;

    /** Longitude in decimal degrees. */
    UPROPERTY(EditAnywhere, config, Category="Request", meta=(ClampMin="-180", ClampMax="180"))
    double Longitude;

    /** Site elevation used when deriving radiation. */
    UPROPERTY(EditAnywhere, config, Category="Request", meta=(ClampMin="-500", ClampMax="9000"))
    float ElevationM;

    /** Falling back range in hours when no explicit simulation span is detected. */
    UPROPERTY(EditAnywhere, config, Category="Request", meta=(ClampMin="1", ClampMax="720"))
    int32 HoursBack;

    /** Degrees to inflate detected landscape bounds when building an area request. */
    UPROPERTY(EditAnywhere, config, Category="Request", meta=(ClampMin="0.0"))
    double AreaPaddingDegrees;

    /** Hourly temperature variable identifier. */
    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString TemperatureVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString RelativeHumidityVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString WindSpeedVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString WindDirectionVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString PrecipitationVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString RainVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString SnowfallVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString ShortwaveVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString DirectShortwaveVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString DiffuseShortwaveVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString LongwaveVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString SurfacePressureVariable;

    UPROPERTY(EditAnywhere, config, Category="Request|Variables")
    FString CloudCoverVariable;

    /** Destination for the generated JSON weather series file (supports {ProjectDir}, {ProjectSavedDir}, {ProjectContentDir}). */
    UPROPERTY(EditAnywhere, config, Category="Output")
    FFilePath OutputJsonPath;

    /** Access global settings singleton. */
    static const UERA5Settings* Get()
    {
        return GetDefault<UERA5Settings>();
    }

    virtual FName GetCategoryName() const override;

    /** Build list of hourly variables to request, deduplicated. */
    void BuildHourlyVariableList(TArray<FString>& OutVariables) const;

    /** Convert configured site metadata into the Frost-compatible struct. */
    FSiteMeta BuildSiteMeta() const;

    FString GetResolvedOpenMeteoBaseUrl(bool& bOutUsedFallback) const;
    FString GetResolvedCdsApiUrl(bool& bOutUsedFallback) const;
};
