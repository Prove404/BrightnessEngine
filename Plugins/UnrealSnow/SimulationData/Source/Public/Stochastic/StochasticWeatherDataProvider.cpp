#include "StochasticWeatherDataProvider.h"
#include "SimulationData.h"
#include "SimulationWeatherDataProviderBase.h"
#include "SimplexNoiseBPLibrary.h"
#include "Math/UnrealMathUtility.h"

UStochasticWeatherDataProvider::UStochasticWeatherDataProvider()
{

}

void UStochasticWeatherDataProvider::Initialize(FDateTime StartTime, FDateTime EndTime)
{
	// Initial state
	State = (FMath::FRand() < P_I_W) ? WeatherState::WET : WeatherState::DRY;

	// Generate Temperature Noise which is assumed to be constant
	const float TemperatureNoiseScale = 0.01f;
	auto TemperatureNoise = std::vector<std::vector<float>>(Resolution, std::vector<float>(Resolution));
	for (int32 Y = 0; Y < Resolution; ++Y)
	{
		for (int32 X = 0; X < Resolution; ++X)
		{
			float Noise = USimplexNoiseBPLibrary::SimplexNoiseScaled2D(X * TemperatureNoiseScale, Y * TemperatureNoiseScale, 2.0f);
			TemperatureNoise[X][Y] = Noise;
		}
	}

	// Generate precipitation
	StartTimeRef = StartTime;
	auto TimeSpan = EndTime - StartTime;
	TotalHours =  static_cast<int32>(TimeSpan.GetTotalHours());
	FDateTime CurrentTime = StartTime;

	ClimateData = std::vector<std::vector<FClimateData>>(TotalHours, std::vector<FClimateData>(Resolution * Resolution));

	auto Measurement = std::vector<std::vector<float>>(Resolution, std::vector<float>(Resolution));

	for (int32 Hour = 0; Hour < TotalHours; ++Hour)
	{
		for (int32 Y = 0; Y < Resolution; ++Y)
		{
			for (int32 X = 0; X < Resolution; ++X)
			{
				float Precipitation = 0.0f;

				// Precipitation
				if (State == WeatherState::WET)
				{
					// Generate Precipitation noise for each station
					const float PrecipitationNoiseScale = 0.01;
					for (int32 NoiseY = 0; NoiseY < Resolution; ++NoiseY)
					{
						for (int32 NoiseX = 0; NoiseX < Resolution; ++NoiseX)
						{
							float Noise = FMath::Max(USimplexNoiseBPLibrary::SimplexNoiseScaled2D(NoiseX * PrecipitationNoiseScale, NoiseY * PrecipitationNoiseScale, 0.9f) + 0.2f, 0.0f);
							Measurement[NoiseX][NoiseY] = Noise;
						}
					}

					// @TODO paper?
					const float RainFallMM = 2.5f * FMath::Exp(2.5f * FMath::FRand()) / 24.0f;
					Precipitation = RainFallMM * Measurement[X][Y];
				}

				// @TODO paper?
				// Temperature
				float SeasonalOffset = -FMath::Cos(CurrentTime.GetDayOfYear() * 2 * PI / 365.0f) * 9 + FMath::FRandRange(-0.5f, 0.5f);
				const float BaseTemperature = 10;
				const float OvercastTemperatureOffset = State == WeatherState::WET ? -8 : 0;
				const float T = BaseTemperature + SeasonalOffset + OvercastTemperatureOffset + TemperatureNoise[X][Y];

				ClimateData[Hour][X + Y * Resolution] = FClimateData(Precipitation, T);
			}
		}

		// Next state
		WeatherState NextState;
		switch (State)
		{
		case WeatherState::WET:
			NextState = (FMath::FRand() < P_WW) ? WeatherState::WET : WeatherState::DRY;
			break;
		case WeatherState::DRY:
			NextState = (FMath::FRand() < P_WD) ? WeatherState::WET : WeatherState::DRY;
			break;
		default:
			NextState = WeatherState::DRY;
			break;
		}

		State = NextState;

		CurrentTime += FTimespan(1, 0, 0);
		USimplexNoiseBPLibrary::SetNoiseSeed(FMath::Rand());
	}
}

TResourceArray<FClimateData>* UStochasticWeatherDataProvider::CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime)
{
	auto TimeSpan = EndTime - StartTime;
	int32 TotalHoursLocal = static_cast<int>(TimeSpan.GetTotalHours());

	TResourceArray<FClimateData>* ClimateResourceArray = new TResourceArray<FClimateData>();
	ClimateResourceArray->Reserve(TotalHoursLocal * Resolution * Resolution);

	for (int32 Hour = 0; Hour < TotalHoursLocal; ++Hour)
	{
		for (int32 ClimateIndex = 0; ClimateIndex < Resolution * Resolution; ++ClimateIndex)
		{
			ClimateResourceArray->Add(ClimateData[Hour][ClimateIndex]);
		}
	}

	return ClimateResourceArray;
}

FWeatherForcingData UStochasticWeatherDataProvider::GetWeatherForcing(FDateTime Time, int32 GridX, int32 GridY)
{
	if (TotalHours <= 0 || Resolution <= 0 || ClimateData.empty())
	{
		return FWeatherForcingData();
	}

	int32 HourIndex = static_cast<int32>((Time - StartTimeRef).GetTotalHours());
	HourIndex = FMath::Clamp(HourIndex, 0, TotalHours - 1);

	// Wrap grid indices into [0, Resolution)
	const int32 X = (Resolution > 0) ? (GridX % Resolution + Resolution) % Resolution : 0;
	const int32 Y = (Resolution > 0) ? (GridY % Resolution + Resolution) % Resolution : 0;
	const int32 StationIndex = X + Y * Resolution;

	const FClimateData& C = ClimateData[HourIndex][StationIndex];

	// Convert to forcing units: Temp K, precip kg/m²/s
	const float TempK = C.Temperature + 273.15f;
	const float Precip_kgm2s = C.Precipitation / 3600.0f; // mm/h ⇒ kg/m²/s
	const float RH_01 = 0.6f;
	const float Wind_mps = 2.0f;
	const float SWdown_Wm2 = 230.0f;
	const float LWdown_Wm2 = 210.0f;
	const float SnowFrac = (C.Temperature <= 0.0f) ? 1.0f : 0.0f;
    const float PressurePa = SurfacePressure_hPa * 100.0f;

	return FWeatherForcingData(Time, TempK, SWdown_Wm2, LWdown_Wm2, Wind_mps, RH_01, Precip_kgm2s, SnowFrac, PressurePa);
}

