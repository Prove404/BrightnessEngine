#include "Util/WeatherLegacyConversion.h"

namespace FWeatherLegacyConversion
{
	TResourceArray<FClimateData>* CreateLegacyClimateArray(const TArray<FWeatherForcingData>& ForcingSeries)
	{
		auto* Resource = new TResourceArray<FClimateData>();
		Resource->Reserve(ForcingSeries.Num());
		for (const FWeatherForcingData& Record : ForcingSeries)
		{
			const float TempC = Record.Temperature_K - 273.15f;
			const float Precip_m = Record.PrecipRate_kgm2s * 3600.0f / 1000.0f; // convert kg/m^2/s to m/h
			Resource->Add(FClimateData(Precip_m, TempC));
		}
		return Resource;
	}
}


