#include "CsvWeatherProvider.h"
#include "Util/WeatherCsvUtils.h"
#include "Util/WeatherLegacyConversion.h"
#include "Misc/DateTime.h"
#include "Csv/CsvSettings.h"
#include "Frost/AtmosRadiation.h"

UCsvWeatherProvider::UCsvWeatherProvider()
{
}

void UCsvWeatherProvider::Initialize(FDateTime StartTime, FDateTime EndTime)
{
	WeatherRecords.Empty();

    if (!LoadCsvData())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weather] Failed to load CSV data from %s"), *CsvFilePath.FilePath);
		return;
	}

	UE_LOG(LogTemp, Display, TEXT("[Weather] CSV provider initialized with %d records from %s"),
		   WeatherRecords.Num(), *CsvFilePath.FilePath);

	// Log statistics
	if (WeatherRecords.Num() > 0)
	{
		float MinTemp = FLT_MAX, MaxTemp = -FLT_MAX;
		float MinPrecip = FLT_MAX, MaxPrecip = -FLT_MAX;
		float MinSW = FLT_MAX, MaxSW = -FLT_MAX;

		for (const auto& Record : WeatherRecords)
		{
			MinTemp = FMath::Min(MinTemp, Record.Temperature_K);
			MaxTemp = FMath::Max(MaxTemp, Record.Temperature_K);
			MinPrecip = FMath::Min(MinPrecip, Record.PrecipRate_kgm2s);
			MaxPrecip = FMath::Max(MaxPrecip, Record.PrecipRate_kgm2s);
			MinSW = FMath::Min(MinSW, Record.SWdown_Wm2);
			MaxSW = FMath::Max(MaxSW, Record.SWdown_Wm2);
		}

		float MeanTemp = (MinTemp + MaxTemp) / 2.0f;
		float MeanPrecip = (MinPrecip + MaxPrecip) / 2.0f;
		float MeanSW = (MinSW + MaxSW) / 2.0f;

		UE_LOG(LogTemp, Display, TEXT("[Weather] Stats: T=%.1f-%.1f??C (mean %.1f??C), Precip=%.3f-%.3f kg/m??/s (mean %.3f), SW=%.0f-%.0f W/m?? (mean %.0f)"),
			   MinTemp - 273.15f, MaxTemp - 273.15f, MeanTemp - 273.15f,
			   MinPrecip, MaxPrecip, MeanPrecip,
			   MinSW, MaxSW, MeanSW);
	}
}

bool UCsvWeatherProvider::LoadCsvData()
{
	WeatherRecords.Reset();
    if (CsvFilePath.FilePath.IsEmpty())
	{
        if (const UCsvSettings* Settings = UCsvSettings::Get())
        {
            CsvFilePath.FilePath = Settings->InputCsvPath.FilePath;
        }
        if (CsvFilePath.FilePath.IsEmpty())
        {
            return false;
        }
	}

	if (!FWeatherCsvUtilities::ParseWeatherCsvFile(CsvFilePath.FilePath, WeatherRecords, PrecipitationScaleFactor))
	{
		return false;
	}

	return WeatherRecords.Num() > 0;
}
TResourceArray<FClimateData>* UCsvWeatherProvider::CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime)
{
	return FWeatherLegacyConversion::CreateLegacyClimateArray(WeatherRecords);
}
FWeatherForcingData UCsvWeatherProvider::GetWeatherForcing(FDateTime Time, int32 GridX, int32 GridY)
{
	if (WeatherRecords.Num() == 0)
	{
		return FWeatherForcingData();
	}

	if (WeatherRecords.Num() == 1)
	{
		return WeatherRecords[0];
	}

	// Find bracketing records
	int32 Index1, Index2;
	float Alpha;
	FindBracketingRecords(Time, Index1, Index2, Alpha);

	if (Index1 == INDEX_NONE || Index2 == INDEX_NONE)
	{
		// Extrapolate using the closest record
		int32 ClosestIndex = (Time < WeatherRecords[0].Timestamp) ? 0 : WeatherRecords.Num() - 1;
		return WeatherRecords[ClosestIndex];
	}

    // Interpolate between bracketing records
    FWeatherForcingData Out = InterpolateRecords(WeatherRecords[Index1], WeatherRecords[Index2], Alpha);

    // If LW (or SW) is missing/zero-ish, optionally derive using radiation model
    if (UseDerivedRadiationIfMissing && (Out.LWdown_Wm2 <= 0.0f || Out.SWdown_Wm2 < 0.0f))
    {
        float SW = FMath::Max(0.0f, Out.SWdown_Wm2);
        float LW = FMath::Max(0.0f, Out.LWdown_Wm2);
        const float TempC = Out.Temperature_K - 273.15f;
        const float RH_pct = FMath::Clamp(Out.RH_01 * 100.0f, 0.0f, 100.0f);
        UAtmosRadiation::EstimateSWLW(Out.Timestamp, Site, TempC, RH_pct, SW, LW);
        // Only replace components that were missing
        if (Out.SWdown_Wm2 < 0.0f) Out.SWdown_Wm2 = SW;
        if (Out.LWdown_Wm2 <= 0.0f) Out.LWdown_Wm2 = LW;
    }

    return Out;
}

void UCsvWeatherProvider::FindBracketingRecords(FDateTime Time, int32& OutIndex1, int32& OutIndex2, float& OutAlpha)
{
	OutIndex1 = INDEX_NONE;
	OutIndex2 = INDEX_NONE;
	OutAlpha = 0.0f;

	if (WeatherRecords.Num() < 2)
	{
		return;
	}

	// Binary search for the right insertion point
	int32 Left = 0;
	int32 Right = WeatherRecords.Num() - 1;

	while (Left <= Right)
	{
		int32 Mid = Left + (Right - Left) / 2;

		if (WeatherRecords[Mid].Timestamp < Time)
		{
			Left = Mid + 1;
		}
		else if (WeatherRecords[Mid].Timestamp > Time)
		{
			Right = Mid - 1;
		}
		else
		{
			// Exact match
			OutIndex1 = Mid;
			OutIndex2 = Mid;
			OutAlpha = 0.0f;
			return;
		}
	}

	// Left is now the insertion point
	if (Left == 0)
	{
		// Before first record
		OutIndex1 = 0;
		OutIndex2 = 0;
		OutAlpha = 0.0f;
	}
	else if (Left >= WeatherRecords.Num())
	{
		// After last record
		OutIndex1 = WeatherRecords.Num() - 1;
		OutIndex2 = WeatherRecords.Num() - 1;
		OutAlpha = 0.0f;
	}
	else
	{
		// Between two records
		OutIndex1 = Left - 1;
		OutIndex2 = Left;

		FTimespan TimeSpan = WeatherRecords[OutIndex2].Timestamp - WeatherRecords[OutIndex1].Timestamp;
		FTimespan TargetSpan = Time - WeatherRecords[OutIndex1].Timestamp;

		if (TimeSpan.GetTotalSeconds() > 0)
		{
			OutAlpha = TargetSpan.GetTotalSeconds() / TimeSpan.GetTotalSeconds();
		}
		else
		{
			OutAlpha = 0.0f;
		}
	}
}

FWeatherForcingData UCsvWeatherProvider::InterpolateRecords(const FWeatherForcingData& Record1, const FWeatherForcingData& Record2, float Alpha)
{
	const FDateTime InterpTime = Record1.Timestamp + (Record2.Timestamp - Record1.Timestamp) * Alpha;
	const float InterpTempK = FMath::Lerp(Record1.Temperature_K, Record2.Temperature_K, Alpha);
	const float InterpSW = FMath::Lerp(Record1.SWdown_Wm2, Record2.SWdown_Wm2, Alpha);
	const float InterpLW = FMath::Lerp(Record1.LWdown_Wm2, Record2.LWdown_Wm2, Alpha);
	const float InterpWind = FMath::Lerp(Record1.Wind_mps, Record2.Wind_mps, Alpha);
	const float InterpRH = FMath::Lerp(Record1.RH_01, Record2.RH_01, Alpha);
	const float InterpPressure = FMath::Lerp(Record1.Pressure_Pa, Record2.Pressure_Pa, Alpha);

	const bool bHasExplicit = Record1.bHasExplicitPrecipitation || Record2.bHasExplicitPrecipitation;
	FWeatherForcingData Out;
	if (bHasExplicit)
	{
		const float InterpRainRate = FMath::Lerp(Record1.RainRate_kgm2s, Record2.RainRate_kgm2s, Alpha);
		const float InterpSnowRate = FMath::Lerp(Record1.SnowRate_kgm2s, Record2.SnowRate_kgm2s, Alpha);
		Out = FWeatherForcingData(
			InterpTime,
			InterpTempK,
			InterpSW,
			InterpLW,
			InterpWind,
			InterpRH,
			InterpRainRate,
			InterpSnowRate,
			true,
			InterpPressure
		);
	}
	else
	{
		Out = FWeatherForcingData(
			InterpTime,
			InterpTempK,
			InterpSW,
			InterpLW,
			InterpWind,
			InterpRH,
			FMath::Lerp(Record1.PrecipRate_kgm2s, Record2.PrecipRate_kgm2s, Alpha),
			FMath::Lerp(Record1.SnowFrac_01, Record2.SnowFrac_01, Alpha),
			InterpPressure
		);
	}

	Out.CloudCover_01 = FMath::Lerp(Record1.CloudCover_01, Record2.CloudCover_01, Alpha);
	if (Record1.DirectSWdown_Wm2 >= 0.0f && Record2.DirectSWdown_Wm2 >= 0.0f)
	{
		Out.DirectSWdown_Wm2 = FMath::Lerp(Record1.DirectSWdown_Wm2, Record2.DirectSWdown_Wm2, Alpha);
	}
	else
	{
		Out.DirectSWdown_Wm2 = -1.0f;
	}

	if (Record1.DiffuseSWdown_Wm2 >= 0.0f && Record2.DiffuseSWdown_Wm2 >= 0.0f)
	{
		Out.DiffuseSWdown_Wm2 = FMath::Lerp(Record1.DiffuseSWdown_Wm2, Record2.DiffuseSWdown_Wm2, Alpha);
	}
	else
	{
		Out.DiffuseSWdown_Wm2 = -1.0f;
	}

	FWeatherCsvUtilities::ApplyTemperatureSnowPartition(Out);
	return Out;
}






