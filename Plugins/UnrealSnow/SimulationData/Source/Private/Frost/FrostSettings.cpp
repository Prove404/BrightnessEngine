#include "Frost/FrostSettings.h"

UFrostSettings::UFrostSettings()
{
    CategoryName = TEXT("Frost (Weather)");
    ClientId = TEXT("YOUR_CLIENT_ID_HERE");
    ClientSecret = TEXT("YOUR_CLIENT_SECRET_HERE");
    Sources = TEXT("SN18700");
    HoursBack = 48;
    bTreatSimulationTimesAsLocal = false;
    SimulationUtcOffsetHours = 0;
    OutputJsonPath.FilePath = TEXT("{ProjectDir}/analysis_results/frost_weather.json");
    AirTemperatureElementId = TEXT("air_temperature");
    RelativeHumidityElementId = TEXT("relative_humidity");
    WindSpeedElementId = TEXT("wind_speed");
    WindDirectionElementId = TEXT("wind_from_direction");
	PrecipitationAmountElementId = TEXT("precipitation_amount");
	PrecipitationTypeElementId = TEXT("precipitation_type");
	ShortwaveFluxElementId = TEXT("surface_downwelling_shortwave_flux_in_air");
	DiffuseShortwaveFluxElementId = TEXT("diffuse_downwelling_shortwave_flux_in_air");
	LongwaveFluxElementId = TEXT("surface_downwelling_longwave_flux_in_air");
	SurfacePressureElementId = TEXT("surface_air_pressure");
}

FName UFrostSettings::GetCategoryName() const
{
	return FName(TEXT("Project"));
}
