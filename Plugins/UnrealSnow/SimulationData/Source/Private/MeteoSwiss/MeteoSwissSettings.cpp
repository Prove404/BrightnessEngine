#include "MeteoSwiss/MeteoSwissSettings.h"

namespace
{
	static const TCHAR* GMeteoSwissDefaultStacItemsBaseUrl = TEXT("https://data.geo.admin.ch/api/stac/v1/collections/ch.meteoschweiz.ogd-smn/items");

	FString ResolveMeteoSwissBaseUrl(const FString& Input, const TCHAR* Default, bool& bOutUsedFallback)
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

	FString NormalizeStationId(const FString& Input, bool bUppercase)
	{
		FString Value = Input;
		Value.TrimStartAndEndInline();
		if (bUppercase)
		{
			Value.ToUpperInline();
		}
		else
		{
			Value.ToLowerInline();
		}
		return Value;
	}
}

UMeteoSwissSettings::UMeteoSwissSettings()
{
	CategoryName = TEXT("MeteoSwiss (Weather)");
	StationId = TEXT("WFJ");
	StacItemsBaseUrl = GMeteoSwissDefaultStacItemsBaseUrl;
	HoursBack = 168;
	TemperatureSource.bInclude = true;
	RelativeHumiditySource.bInclude = true;
	WindSpeedSource.bInclude = true;
	WindDirectionSource.bInclude = true;
	PrecipitationSource.bInclude = true;
	ShortwaveSource.bInclude = true;
	DiffuseShortwaveSource.bInclude = true;
	LongwaveSource.bInclude = true;
	PressureSource.bInclude = true;
	Latitude = 46.833325;
	Longitude = 9.806394;
	ElevationM = 2691.0f;
	OutputJsonPath.FilePath = TEXT("{ProjectDir}/analysis_results/meteoswiss_weather.json");
}

FName UMeteoSwissSettings::GetCategoryName() const
{
	return FName(TEXT("Project"));
}

FSiteMeta UMeteoSwissSettings::BuildSiteMeta() const
{
	FSiteMeta Meta;
	Meta.LatDeg = static_cast<float>(Latitude);
	Meta.LonDeg = static_cast<float>(Longitude);
	Meta.ElevM = ElevationM;
	return Meta;
}

FString UMeteoSwissSettings::GetResolvedStacItemsBaseUrl(bool& bOutUsedFallback) const
{
	return ResolveMeteoSwissBaseUrl(StacItemsBaseUrl, GMeteoSwissDefaultStacItemsBaseUrl, bOutUsedFallback);
}

FString UMeteoSwissSettings::GetNormalizedStationIdLower() const
{
	return NormalizeStationId(StationId, false);
}

FString UMeteoSwissSettings::GetNormalizedStationIdUpper() const
{
	return NormalizeStationId(StationId, true);
}

FString UMeteoSwissSettings::NormalizeStationId(const FString& InStationId, bool bUppercase) const
{
	return ::NormalizeStationId(InStationId, bUppercase);
}
