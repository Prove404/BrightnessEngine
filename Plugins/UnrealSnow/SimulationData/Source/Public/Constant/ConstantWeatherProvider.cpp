#include "ConstantWeatherProvider.h"
#include "ClimateData.h"

UConstantWeatherProvider::UConstantWeatherProvider()
{
}

void UConstantWeatherProvider::Initialize(FDateTime StartTime, FDateTime EndTime)
{
	UE_LOG(LogTemp, Display, TEXT("[Weather] Constant provider initialized: T=%.1f°C, RH=%.1f%%, Wind=%.1f m/s, SW=%.0f W/m², LW=%.0f W/m², Precip=%.2f mm/h, SnowFrac=%.2f, Pressure=%.0f hPa"),
		   Temperature_C, RH_Percent, Wind_mps, SWdown_Wm2, LWdown_Wm2, Precipitation_mmph, SnowFraction, SurfacePressure_hPa);
}

TResourceArray<FClimateData>* UConstantWeatherProvider::CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime)
{
	auto* ResourceArray = new TResourceArray<FClimateData>();

	// Create dummy climate data for compatibility with existing system
	FTimespan Duration = EndTime - StartTime;
	int32 Hours = FMath::CeilToInt(Duration.GetTotalHours());

	for (int32 i = 0; i < Hours; ++i)
	{
		ResourceArray->Add(FClimateData(Temperature_C, Precipitation_mmph / 1000.0f)); // Convert mm/h to m/h
	}

	return ResourceArray;
}

FWeatherForcingData UConstantWeatherProvider::GetWeatherForcing(FDateTime Time, int32 GridX, int32 GridY)
{
	// Convert units to simulation format
	float TempK = Temperature_C + 273.15f;  // Celsius to Kelvin
	float RH_01 = FMath::Clamp(RH_Percent / 100.0f, 0.0f, 1.0f);  // Percent to 0-1
	float Precip_kgm2s = Precipitation_mmph / 3600.0f;  // mm/h to kg/m²/s (1 mm = 1 kg/m²)

	const float PressurePa = SurfacePressure_hPa * 100.0f;

	return FWeatherForcingData(
		Time,
		TempK,
		SWdown_Wm2,
		LWdown_Wm2,
		Wind_mps,
		RH_01,
		Precip_kgm2s,
		FMath::Clamp(SnowFraction, 0.0f, 1.0f),
		PressurePa
	);
}
