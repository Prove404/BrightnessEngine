#include "ERA5/ERA5Settings.h"

#include "Containers/Set.h"

namespace
{
    void AddIfNotEmpty(const FString& Value, TArray<FString>& Array)
    {
        if (!Value.IsEmpty())
        {
            Array.Add(Value);
        }
    }
}

namespace
{
    static const TCHAR* GEra5DefaultOpenMeteoBaseUrl = TEXT("https://archive-api.open-meteo.com/v1/era5");
    static const TCHAR* GEra5DefaultCdsBaseUrl = TEXT("https://cds.climate.copernicus.eu/api");
}

UERA5Settings::UERA5Settings()
{
    CategoryName = TEXT("ERA5 (Weather)");
    DataSource = EERA5DataSource::OpenMeteo;
    ApiBaseUrl = GEra5DefaultOpenMeteoBaseUrl;
    CdsApiUrl = GEra5DefaultCdsBaseUrl;
    CdsApiKey = TEXT("YOUR_CDS_API_KEY_HERE");
    CdsDataset = TEXT("reanalysis-era5-single-levels");
    Latitude = 59.9139;   // Oslo
    Longitude = 10.7522;
    ElevationM = 23.0f;
    HoursBack = 168;
    AreaPaddingDegrees = 0.0;
    TemperatureVariable = TEXT("temperature_2m");
    RelativeHumidityVariable = TEXT("relative_humidity_2m");
    WindSpeedVariable = TEXT("wind_speed_10m");
    WindDirectionVariable = TEXT("wind_direction_10m");
    PrecipitationVariable = TEXT("precipitation");
    RainVariable = TEXT("rain");
    SnowfallVariable = TEXT("snowfall");
    ShortwaveVariable = TEXT("shortwave_radiation");
    DirectShortwaveVariable = TEXT("direct_radiation");
    DiffuseShortwaveVariable = TEXT("diffuse_radiation");
    LongwaveVariable = TEXT("longwave_radiation");
    SurfacePressureVariable = TEXT("surface_pressure");
    CloudCoverVariable = TEXT("cloud_cover");
    OutputJsonPath.FilePath = TEXT("{ProjectDir}/analysis_results/era5_weather.json");
}

FName UERA5Settings::GetCategoryName() const
{
    return FName(TEXT("Project"));
}

void UERA5Settings::BuildHourlyVariableList(TArray<FString>& OutVariables) const
{
    OutVariables.Reset();
    AddIfNotEmpty(TemperatureVariable, OutVariables);
    AddIfNotEmpty(RelativeHumidityVariable, OutVariables);
    AddIfNotEmpty(WindSpeedVariable, OutVariables);
    AddIfNotEmpty(WindDirectionVariable, OutVariables);
    AddIfNotEmpty(PrecipitationVariable, OutVariables);
    AddIfNotEmpty(RainVariable, OutVariables);
    AddIfNotEmpty(SnowfallVariable, OutVariables);
    AddIfNotEmpty(ShortwaveVariable, OutVariables);
    AddIfNotEmpty(DirectShortwaveVariable, OutVariables);
    AddIfNotEmpty(DiffuseShortwaveVariable, OutVariables);
    AddIfNotEmpty(LongwaveVariable, OutVariables);
    AddIfNotEmpty(SurfacePressureVariable, OutVariables);
    AddIfNotEmpty(CloudCoverVariable, OutVariables);

    TSet<FString> Seen;
    TArray<FString> Deduped;
    Deduped.Reserve(OutVariables.Num());
    for (const FString& Var : OutVariables)
    {
        if (!Seen.Contains(Var))
        {
            Seen.Add(Var);
            Deduped.Add(Var);
        }
    }
    OutVariables = MoveTemp(Deduped);
}

FSiteMeta UERA5Settings::BuildSiteMeta() const
{
    FSiteMeta Meta;
    Meta.LatDeg = static_cast<float>(Latitude);
    Meta.LonDeg = static_cast<float>(Longitude);
    Meta.ElevM = ElevationM;
    return Meta;
}

static FString ResolveBaseUrl(const FString& Input, const TCHAR* Default, bool& bOutUsedFallback)
{
    FString Value = Input;
    Value.TrimStartAndEndInline();

    bOutUsedFallback = false;

    if (Value.IsEmpty())
    {
        bOutUsedFallback = true;
        return FString(Default);
    }

    const FString Normalized = Value.ToLower();
    if (Normalized == TEXT("https:") || Normalized == TEXT("http:"))
    {
        bOutUsedFallback = true;
        return FString(Default);
    }

    if (!Value.Contains(TEXT("://")))
    {
        Value = FString::Printf(TEXT("https://%s"), *Value);
    }

    return Value;
}

FString UERA5Settings::GetResolvedOpenMeteoBaseUrl(bool& bOutUsedFallback) const
{
    return ResolveBaseUrl(ApiBaseUrl, GEra5DefaultOpenMeteoBaseUrl, bOutUsedFallback);
}

FString UERA5Settings::GetResolvedCdsApiUrl(bool& bOutUsedFallback) const
{
    return ResolveBaseUrl(CdsApiUrl, GEra5DefaultCdsBaseUrl, bOutUsedFallback);
}
