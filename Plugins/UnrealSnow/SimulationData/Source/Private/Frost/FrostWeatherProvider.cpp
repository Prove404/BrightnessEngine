#include "Frost/FrostWeatherProvider.h"
#include "Frost/AtmosRadiation.h"
#include "Frost/FrostSettings.h"

#include "Algo/Sort.h"
#include "Containers/Map.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h" // Ensure this is included for file writing if needed, though we use UE_LOG for now


#include <limits>

namespace
{
    FString ResolveFrostPath(const FString& InPath)
    {
        FString Resolved = InPath;
        if (Resolved.IsEmpty())
        {
            return Resolved;
        }

        const TPair<FString, FString> Tokens[] = {
            { TEXT("{ProjectDir}"), FPaths::ProjectDir() },
            { TEXT("{ProjectContentDir}"), FPaths::ProjectContentDir() },
            { TEXT("{ProjectSavedDir}"), FPaths::ProjectSavedDir() }
        };

        for (const TPair<FString, FString>& Token : Tokens)
        {
            Resolved.ReplaceInline(*Token.Key, *Token.Value, ESearchCase::CaseSensitive);
        }

        return FPaths::ConvertRelativePathToFull(Resolved);
    }
}

#if WITH_EDITOR
#include "Kismet/KismetSystemLibrary.h"
#endif

UFrostWeatherProvider::UFrostWeatherProvider()
{
    PrimaryComponentTick.bCanEverTick = false;
    bWantsInitializeComponent = true;
    bSamplesSorted = false;
    RequestedStartUtc = FDateTime::MinValue();
    RequestedEndUtc = FDateTime::MinValue();
    DatasetStartUtc = FDateTime::MinValue();
    DatasetEndUtc = FDateTime::MinValue();

    if (const UFrostSettings* Settings = UFrostSettings::Get())
    {
        JsonPath.FilePath = Settings->OutputJsonPath.FilePath;
    }
}

void UFrostWeatherProvider::Initialize(FDateTime StartTime, FDateTime EndTime)
{
    RequestedStartUtc = StartTime;
    RequestedEndUtc = EndTime;
    LoadFromJson();
    EnsureSamplesSorted();
    if (AuxiliaryProvider)
    {
        AuxiliaryProvider->Initialize(StartTime, EndTime);
        BuildGapFills();
    }
}

int32 UFrostWeatherProvider::LoadFromJson()
{
	Samples.Reset();
	bSamplesSorted = false;

	DatasetStartUtc = FDateTime::MaxValue();
	DatasetEndUtc = FDateTime::MinValue();
	bool bFoundSample = false;

    FString FullPath = JsonPath.FilePath;
    if (FullPath.IsEmpty())
    {
        if (const UFrostSettings* Settings = UFrostSettings::Get())
        {
            FullPath = Settings->OutputJsonPath.FilePath;
        }
    }

    FullPath = ResolveFrostPath(FullPath);

    FString JsonText;
    if (FullPath.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("[Frost] JSON path is not set on %s"), *GetNameSafe(this));
        return 0;
    }

    if (!FPaths::FileExists(FullPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("[Frost] JSON file missing: %s"), *FullPath);
        return 0;
    }

    if (!FFileHelper::LoadFileToString(JsonText, *FullPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("[Frost] Failed to read JSON from %s"), *FullPath);
        return 0;
    }

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Frost] Invalid JSON format in %s"), *FullPath);
		return 0;
	}


	const TSharedPtr<FJsonObject>* MetaObj = nullptr;
	if (RootObject->TryGetObjectField(TEXT("meta"), MetaObj) && MetaObj && MetaObj->IsValid())
	{
		FString RangeStartStr;
		if (RequestedStartUtc == FDateTime::MinValue() && (*MetaObj)->TryGetStringField(TEXT("range_start_utc"), RangeStartStr))
		{
			FDateTime Parsed;
			if (FDateTime::ParseIso8601(*RangeStartStr, Parsed))
			{
				RequestedStartUtc = Parsed;
			}
		}

		FString RangeEndExclusiveStr;
		if (RequestedEndUtc == FDateTime::MinValue() && (*MetaObj)->TryGetStringField(TEXT("range_end_utc_exclusive"), RangeEndExclusiveStr))
		{
			FDateTime Parsed;
			if (FDateTime::ParseIso8601(*RangeEndExclusiveStr, Parsed))
			{
				RequestedEndUtc = Parsed - FTimespan::FromHours(1);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* SeriesArray = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("series"), SeriesArray))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Frost] No 'series' array in %s"), *FullPath);
		return 0;
	}

	for (const TSharedPtr<FJsonValue>& EntryValue : *SeriesArray)
	{
		const TSharedPtr<FJsonObject>* EntryObjectPtr = nullptr;
		if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObjectPtr))
		{
			continue;
		}

		const TSharedPtr<FJsonObject>& EntryObj = *EntryObjectPtr;

		FString TimeUtcStr;
		if (!EntryObj->TryGetStringField(TEXT("time_utc"), TimeUtcStr))
		{
			continue;
		}

		FDateTime TimeUtc;
		if (!FDateTime::ParseIso8601(*TimeUtcStr, TimeUtc))
		{
			UE_LOG(LogTemp, Verbose, TEXT("[Frost] Could not parse time %s"), *TimeUtcStr);
			continue;
		}

		FWeatherSample Sample;
		// Use NaN sentinels to detect whether rain/snow split was present in the JSON
		Sample.Rain_mmph = std::numeric_limits<float>::quiet_NaN();
		Sample.Snow_mmph = std::numeric_limits<float>::quiet_NaN();
		Sample.TimeUTC = TimeUtc;
		bFoundSample = true;
		if (TimeUtc < DatasetStartUtc)
		{
			DatasetStartUtc = TimeUtc;
		}
		if (TimeUtc > DatasetEndUtc)
		{
			DatasetEndUtc = TimeUtc;
		}

		double Value = 0.0;
		if (EntryObj->TryGetNumberField(TEXT("T_C"), Value))
		{
			Sample.T_C = static_cast<float>(Value);
			Sample.bTemperatureValid = true;
		}
		if (EntryObj->TryGetNumberField(TEXT("RH_pct"), Value))
		{
			Sample.RH_pct = static_cast<float>(Value);
		}
		if (EntryObj->TryGetNumberField(TEXT("Wind_mps"), Value))
		{
			Sample.Wind_mps = static_cast<float>(Value);
		}
		if (EntryObj->TryGetNumberField(TEXT("WindDir_deg"), Value))
		{
			Sample.WindDir_deg = static_cast<float>(Value);
		}
		if (EntryObj->TryGetNumberField(TEXT("Precip_mmph"), Value))
		{
			Sample.Precip_mmph = static_cast<float>(Value);
		}
		if (EntryObj->TryGetNumberField(TEXT("Rain_mmph"), Value) || EntryObj->TryGetNumberField(TEXT("Rainfall_mmph"), Value))
		{
			Sample.Rain_mmph = static_cast<float>(Value);
		}
		if (EntryObj->TryGetNumberField(TEXT("Snow_mmph"), Value) || EntryObj->TryGetNumberField(TEXT("Snowfall_mmph"), Value))
		{
			Sample.Snow_mmph = static_cast<float>(Value);
		}
		else if (EntryObj->TryGetNumberField(TEXT("snowfall"), Value)) // OpenMeteo "snowfall" in cm
		{
			// OpenMeteo: Snowfall amount of the preceding hour in centimeters.
			// User rule: 7 cm snow = 10 mm water equivalent.
			Sample.Snow_mmph = static_cast<float>(Value * (10.0 / 7.0)); 
		}

		if (EntryObj->TryGetNumberField(TEXT("cloud_cover"), Value) || EntryObj->TryGetNumberField(TEXT("cloudcover"), Value))
		{
			Sample.CloudCover_pct = static_cast<float>(Value);
		}
		else
		{
			Sample.CloudCover_pct = std::numeric_limits<float>::quiet_NaN();
		}

		if (EntryObj->TryGetNumberField(TEXT("SWdown_Wm2"), Value))
		{
			Sample.SWdown_Wm2 = static_cast<float>(Value);
		}
		else
		{
			Sample.SWdown_Wm2 = std::numeric_limits<float>::quiet_NaN();
		}

		if (EntryObj->TryGetNumberField(TEXT("DiffuseSWdown_Wm2"), Value))
		{
			Sample.DiffuseSWdown_Wm2 = static_cast<float>(Value);
		}
		else
		{
			Sample.DiffuseSWdown_Wm2 = std::numeric_limits<float>::quiet_NaN();
		}

		if (EntryObj->TryGetNumberField(TEXT("LWdown_Wm2"), Value))
		{
			Sample.LWdown_Wm2 = static_cast<float>(Value);
		}
		else
		{
			Sample.LWdown_Wm2 = std::numeric_limits<float>::quiet_NaN();
		}

		if (EntryObj->TryGetNumberField(TEXT("Pressure_hPa"), Value))
		{
			Sample.Pressure_hPa = static_cast<float>(Value);
		}
		else
		{
			Sample.Pressure_hPa = std::numeric_limits<float>::quiet_NaN();
		}

		if (EntryObj->TryGetNumberField(TEXT("SnowFrac"), Value))
		{
			Sample.SnowFrac_0_1 = static_cast<float>(Value);
		}
		else
		{
			Sample.SnowFrac_0_1 = (Sample.Precip_mmph > 0.05f && Sample.T_C <= 0.5f) ? 1.0f : 0.0f;
		}

		Samples.Add(MoveTemp(Sample));

        // DEBUG: Log temperature for specific date to debug flux discrepancy
        if (Sample.TimeUTC.GetYear() == 2017 && Sample.TimeUTC.GetMonth() == 5 && Sample.TimeUTC.GetDay() == 15 && Sample.TimeUTC.GetHour() == 12)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost DEBUG] Parsed Sample 2017-05-15 12:00: T_C=%.2f, RH=%.2f, Wind=%.2f"), 
                Sample.T_C, Sample.RH_pct, Sample.Wind_mps);
        }
	}

	if (!bFoundSample)
	{
		DatasetStartUtc = FDateTime::MinValue();
		DatasetEndUtc = FDateTime::MinValue();
	}

	EnsureSamplesSorted();

	if (Samples.Num() > 0)
	{
		UE_LOG(LogTemp, Display, TEXT("[Frost] Loaded %d samples from %s (coverage %s -> %s)"), Samples.Num(), *FullPath, *DatasetStartUtc.ToString(), *DatasetEndUtc.ToString());

		if (RequestedStartUtc != FDateTime::MinValue() && RequestedEndUtc != FDateTime::MinValue() &&
			DatasetStartUtc != FDateTime::MinValue() && DatasetEndUtc != FDateTime::MinValue())
		{
			if (DatasetStartUtc > RequestedStartUtc || DatasetEndUtc < RequestedEndUtc)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Frost] Dataset does not fully cover requested simulation window %s -> %s"), *RequestedStartUtc.ToString(), *RequestedEndUtc.ToString());
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[Frost] Loaded 0 samples from %s"), *FullPath);
	}

	return Samples.Num();
}

void UFrostWeatherProvider::EnsureSamplesSorted()
{
	if (bSamplesSorted)
	{
		return;
	}

	Samples.Sort([](const FWeatherSample& A, const FWeatherSample& B)
	{
		return A.TimeUTC < B.TimeUTC;
	});
	bSamplesSorted = true;
}

int32 UFrostWeatherProvider::FindSampleIndex(const FDateTime& TimeUTC) const
{
	if (Samples.Num() <= 1)
	{
		return INDEX_NONE;
	}

	int32 Low = 0;
	int32 High = Samples.Num() - 1;

	while (High - Low > 1)
	{
		const int32 Mid = (Low + High) / 2;
		if (Samples[Mid].TimeUTC <= TimeUTC)
		{
			Low = Mid;
		}
		else
		{
			High = Mid;
		}
	}

	return Low;
}

bool UFrostWeatherProvider::GetWeatherAtUTC(const FDateTime& TimeUTC, FWeatherSample& OutSample) const
{
	if (Samples.Num() == 0)
	{
		return false;
	}

	const_cast<UFrostWeatherProvider*>(this)->EnsureSamplesSorted();

	if (TimeUTC <= Samples[0].TimeUTC)
	{
		OutSample = Samples[0];
		return true;
	}

	const int32 LastIndex = Samples.Num() - 1;
	if (TimeUTC >= Samples[LastIndex].TimeUTC)
	{
		OutSample = Samples[LastIndex];
		return true;
	}

	const int32 Index = FindSampleIndex(TimeUTC);
	if (Index == INDEX_NONE || Index >= Samples.Num() - 1)
	{
		OutSample = Samples[LastIndex];
		return true;
	}

	const FWeatherSample& A = Samples[Index];
	const FWeatherSample& B = Samples[Index + 1];
	const double TotalSeconds = (B.TimeUTC - A.TimeUTC).GetTotalSeconds();
	const double TargetSeconds = (TimeUTC - A.TimeUTC).GetTotalSeconds();
	const float Alpha = (TotalSeconds > 0.0) ? static_cast<float>(TargetSeconds / TotalSeconds) : 0.0f;

	OutSample.TimeUTC = TimeUTC;
	OutSample.T_C = FMath::Lerp(A.T_C, B.T_C, Alpha);
	OutSample.bTemperatureValid = A.bTemperatureValid && B.bTemperatureValid;
	OutSample.RH_pct = FMath::Lerp(A.RH_pct, B.RH_pct, Alpha);
	OutSample.Wind_mps = FMath::Lerp(A.Wind_mps, B.Wind_mps, Alpha);
	OutSample.WindDir_deg = FMath::Lerp(A.WindDir_deg, B.WindDir_deg, Alpha);
	OutSample.Precip_mmph = FMath::Lerp(A.Precip_mmph, B.Precip_mmph, Alpha);
	OutSample.Rain_mmph = FMath::Lerp(A.Rain_mmph, B.Rain_mmph, Alpha);
	OutSample.Snow_mmph = FMath::Lerp(A.Snow_mmph, B.Snow_mmph, Alpha);
	OutSample.SnowFrac_0_1 = FMath::Clamp(FMath::Lerp(A.SnowFrac_0_1, B.SnowFrac_0_1, Alpha), 0.0f, 1.0f);

	if (A.HasCloudCover() && B.HasCloudCover())
	{
		OutSample.CloudCover_pct = FMath::Lerp(A.CloudCover_pct, B.CloudCover_pct, Alpha);
	}
	else if (A.HasCloudCover())
	{
		OutSample.CloudCover_pct = A.CloudCover_pct;
	}
	else if (B.HasCloudCover())
	{
		OutSample.CloudCover_pct = B.CloudCover_pct;
	}
	else
	{
		OutSample.CloudCover_pct = std::numeric_limits<float>::quiet_NaN();
	}

	if (A.HasSW() && B.HasSW())
	{
		OutSample.SWdown_Wm2 = FMath::Lerp(A.SWdown_Wm2, B.SWdown_Wm2, Alpha);
	}
	else if (A.HasSW())
	{
		OutSample.SWdown_Wm2 = A.SWdown_Wm2;
	}
	else if (B.HasSW())
	{
		OutSample.SWdown_Wm2 = B.SWdown_Wm2;
	}
	else
	{
		OutSample.SWdown_Wm2 = std::numeric_limits<float>::quiet_NaN();
	}

	if (A.HasDiffuseSW() && B.HasDiffuseSW())
	{
		OutSample.DiffuseSWdown_Wm2 = FMath::Lerp(A.DiffuseSWdown_Wm2, B.DiffuseSWdown_Wm2, Alpha);
	}
	else if (A.HasDiffuseSW())
	{
		OutSample.DiffuseSWdown_Wm2 = A.DiffuseSWdown_Wm2;
	}
	else if (B.HasDiffuseSW())
	{
		OutSample.DiffuseSWdown_Wm2 = B.DiffuseSWdown_Wm2;
	}
	else
	{
		OutSample.DiffuseSWdown_Wm2 = std::numeric_limits<float>::quiet_NaN();
	}

	if (A.HasLW() && B.HasLW())
	{
		OutSample.LWdown_Wm2 = FMath::Lerp(A.LWdown_Wm2, B.LWdown_Wm2, Alpha);
	}
	else if (A.HasLW())
	{
		OutSample.LWdown_Wm2 = A.LWdown_Wm2;
	}
	else if (B.HasLW())
	{
		OutSample.LWdown_Wm2 = B.LWdown_Wm2;
	}
	else
	{
		OutSample.LWdown_Wm2 = std::numeric_limits<float>::quiet_NaN();
	}

	if (A.HasPressure() && B.HasPressure())
	{
		OutSample.Pressure_hPa = FMath::Lerp(A.Pressure_hPa, B.Pressure_hPa, Alpha);
	}
	else if (A.HasPressure())
	{
		OutSample.Pressure_hPa = A.Pressure_hPa;
	}
	else if (B.HasPressure())
	{
		OutSample.Pressure_hPa = B.Pressure_hPa;
	}
	else
	{
		OutSample.Pressure_hPa = std::numeric_limits<float>::quiet_NaN();
	}

	return true;
}

TResourceArray<FClimateData>* UFrostWeatherProvider::CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime)
{
	auto* Resource = new TResourceArray<FClimateData>();
	for (const FWeatherSample& Sample : Samples)
	{
		const float Precip_m = Sample.Precip_mmph / 1000.0f; // mm -> m water equivalent
		Resource->Add(FClimateData(Precip_m, Sample.T_C));
	}
	return Resource;
}

FWeatherForcingData UFrostWeatherProvider::GetWeatherForcing(FDateTime Time, int32 GridX, int32 GridY)
{
	FWeatherSample Sample;
	if (!GetWeatherAtUTC(Time, Sample))
	{
		return FWeatherForcingData();
	}

	// Gap-fill temperature from auxiliary provider (e.g. ERA5) when primary observation is absent.
	if (!Sample.HasTemperature() && AuxiliaryProvider)
	{
		const FDateTime RoundedHour(Time.GetYear(), Time.GetMonth(), Time.GetDay(), Time.GetHour());
		const float Bias = GapFillBiasCache.FindRef(RoundedHour); // 0.0f if no precomputed entry

		FWeatherForcingData Aux = AuxiliaryProvider->GetWeatherForcing(Time);
		if (FMath::IsFinite(Aux.Temperature_K))
		{
			constexpr float LapseRate_Km = -0.0065f; // K/m, standard atmosphere
			Sample.T_C = (Aux.Temperature_K - 273.15f) + LapseRate_Km * (Site.ElevM - AuxiliaryElevM) + Bias;
			Sample.bTemperatureValid = true;
		}
	}
	if (!Sample.HasTemperature())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Frost] Missing temperature at %s — no auxiliary provider or auxiliary failed; substituting 0°C."), *Time.ToString());
		Sample.T_C = 0.0f;
		Sample.bTemperatureValid = true;
	}

	// Derive radiation if it is missing *or* zero/negative (e.g. nulls in JSON become NaN -> HasSW/LW=false,
	// or upstream data writes 0 which starves the energy balance). UAtmosRadiation will still return ~0 at night.
	if (UseDerivedRadiationIfMissing && Sample.HasTemperature() &&
		(!Sample.HasSW() || Sample.SWdown_Wm2 <= 0.0f || !Sample.HasLW() || Sample.LWdown_Wm2 <= 0.0f))
	{
		float EstimatedSW = Sample.HasSW() ? Sample.SWdown_Wm2 : 0.0f;
		float EstimatedLW = Sample.HasLW() ? Sample.LWdown_Wm2 : 0.0f;
		float CloudCover = Sample.HasCloudCover() ? Sample.CloudCover_pct : -1.0f; // -1 indicates unavailable
		
		UAtmosRadiation::EstimateSWLW(Sample.TimeUTC, Site, Sample.T_C, Sample.RH_pct, EstimatedSW, EstimatedLW, CloudCover);
		
		// Only overwrite components that were actually missing or invalid
		if (!Sample.HasSW() || Sample.SWdown_Wm2 <= 0.0f)
		{
			Sample.SWdown_Wm2 = EstimatedSW;
		}
		if (!Sample.HasLW() || Sample.LWdown_Wm2 <= 0.0f)
		{
			Sample.LWdown_Wm2 = EstimatedLW;
		}
	}

	FWeatherForcingData Forcing;
	Forcing.Timestamp = Sample.TimeUTC;
	Forcing.Temperature_K = Sample.T_C + 273.15f;
	Forcing.RH_01 = FMath::Clamp(Sample.RH_pct / 100.0f, 0.0f, 1.0f);
	Forcing.CloudCover_01 = Sample.HasCloudCover() ? FMath::Clamp(Sample.CloudCover_pct / 100.0f, 0.0f, 1.0f) : 0.0f;
	Forcing.Wind_mps = Sample.Wind_mps;
	Forcing.SWdown_Wm2 = Sample.HasSW() ? Sample.SWdown_Wm2 : 0.0f;
	Forcing.LWdown_Wm2 = Sample.HasLW() ? Sample.LWdown_Wm2 : 0.0f;
	Forcing.PrecipRate_kgm2s = Sample.Precip_mmph / 3600.0f;
	Forcing.SnowFrac_01 = FMath::Clamp(Sample.SnowFrac_0_1, 0.0f, 1.0f);

	// If the JSON lacks explicit snowfall (common for Open-Meteo ERA5), partition total precip by temperature
	// so cold events still deliver snow rather than being forced to rain-only.
	float Rain_mmph = Sample.Rain_mmph;
	float Snow_mmph = Sample.Snow_mmph;
	const bool bHasExplicitSnow = FMath::IsFinite(Snow_mmph);
	const bool bHasExplicitRain = FMath::IsFinite(Rain_mmph);
	const bool bHasAnyPrecip = Sample.Precip_mmph > KINDA_SMALL_NUMBER;

	if (!bHasExplicitSnow && bHasAnyPrecip)
	{
		const float WarmThresholdC = 0.5f;
		const float ColdThresholdC = -0.5f;
		float SnowFrac = 0.0f;

		if (Sample.T_C <= ColdThresholdC)
		{
			SnowFrac = 1.0f;
		}
		else if (Sample.T_C < WarmThresholdC)
		{
			SnowFrac = (WarmThresholdC - Sample.T_C) / (WarmThresholdC - ColdThresholdC);
		}

		Snow_mmph = Sample.Precip_mmph * SnowFrac;
		Rain_mmph = Sample.Precip_mmph - Snow_mmph;
		Forcing.SnowFrac_01 = SnowFrac;
	}

	// Populate explicit rain/snow using parsed or partitioned values
	bool bUseExplicit = bHasAnyPrecip || bHasExplicitSnow || bHasExplicitRain || (Rain_mmph > 0.0f || Snow_mmph > 0.0f);
	if (bUseExplicit)
	{
		const float SafeRain = FMath::IsFinite(Rain_mmph) ? Rain_mmph : 0.0f;
		const float SafeSnow = FMath::IsFinite(Snow_mmph) ? Snow_mmph : 0.0f;

		Forcing.RainRate_kgm2s = SafeRain / 3600.0f;
		Forcing.SnowRate_kgm2s = SafeSnow / 3600.0f;
		Forcing.bHasExplicitPrecipitation = true;

		const float ExplicitTotal = Forcing.RainRate_kgm2s + Forcing.SnowRate_kgm2s;
		if (ExplicitTotal > 0.0f)
		{
			Forcing.PrecipRate_kgm2s = ExplicitTotal;
			Forcing.SnowFrac_01 = Forcing.SnowRate_kgm2s / ExplicitTotal;
		}
	}

	Forcing.Pressure_Pa = Sample.HasPressure() ? Sample.Pressure_hPa * 100.0f : 101325.0f;

	// Populate direct/diffuse components if available in source data
	// ERA5 and other sources may provide this split; if so, use it instead of deriving with Erbs
	if (Sample.HasDiffuseSW())
	{
		Forcing.DiffuseSWdown_Wm2 = Sample.DiffuseSWdown_Wm2;
		Forcing.DirectSWdown_Wm2 = FMath::Max(0.0f, Sample.SWdown_Wm2 - Sample.DiffuseSWdown_Wm2);
	}
	else
	{
		// Mark as unavailable - simulation will derive using Erbs correlation
		Forcing.DirectSWdown_Wm2 = -1.0f;
		Forcing.DiffuseSWdown_Wm2 = -1.0f;
	}

	return Forcing;
}

void UFrostWeatherProvider::BuildGapFills()
{
	GapFillBiasCache.Empty();
	if (!AuxiliaryProvider || Samples.Num() == 0) return;

	constexpr float LapseRate_Km = -0.0065f; // K/m, standard atmosphere
	const float ElevDelta = Site.ElevM - AuxiliaryElevM; // positive when station is higher than aux grid cell

	// 1. Identify contiguous blocks of samples with missing temperature.
	struct FGapBlock { FDateTime Start; FDateTime End; };
	TArray<FGapBlock> Blocks;
	for (const FWeatherSample& S : Samples)
	{
		if (S.HasTemperature()) continue;
		if (Blocks.Num() == 0 || (S.TimeUTC - Blocks.Last().End) > FTimespan::FromHours(2))
			Blocks.Add({ S.TimeUTC, S.TimeUTC });
		else
			Blocks.Last().End = S.TimeUTC;
	}
	if (Blocks.Num() == 0) return;

	UE_LOG(LogTemp, Display, TEXT("[FrostGapFill] %d temperature gap block(s) detected."), Blocks.Num());

	// 2. For each block compute a single additive bias from the overlap window on both sides.
	for (const FGapBlock& Block : Blocks)
	{
		const FDateTime WinStart = Block.Start - FTimespan::FromHours(BiasWindowHours);
		const FDateTime WinEnd   = Block.End   + FTimespan::FromHours(BiasWindowHours);

		double BiasSum = 0.0;
		int32  BiasN   = 0;

		for (const FWeatherSample& S : Samples)
		{
			if (S.TimeUTC < WinStart) continue;
			if (S.TimeUTC > WinEnd)   break;
			if (!S.HasTemperature())  continue; // skip the gap itself
			if (S.TimeUTC >= Block.Start && S.TimeUTC <= Block.End) continue;

			const FWeatherForcingData Aux = AuxiliaryProvider->GetWeatherForcing(S.TimeUTC);
			if (!FMath::IsFinite(Aux.Temperature_K)) continue;

			// Lapse-rate correct aux to station elevation, then measure residual bias.
			const float AuxT_C = (Aux.Temperature_K - 273.15f) + LapseRate_Km * ElevDelta;
			BiasSum += static_cast<double>(S.T_C - AuxT_C);
			++BiasN;
		}

		const float Bias = (BiasN > 0) ? static_cast<float>(BiasSum / BiasN) : 0.0f;
		UE_LOG(LogTemp, Display,
			TEXT("[FrostGapFill] Gap %s -> %s  |  lapse correction %.2f K  |  bias %.3f K  |  overlap samples %d"),
			*Block.Start.ToString(), *Block.End.ToString(),
			LapseRate_Km * ElevDelta, Bias, BiasN);

		// 3. Cache the bias keyed to every hourly timestamp inside the gap.
		for (FDateTime T = Block.Start; T <= Block.End + FTimespan::FromMinutes(30); T += FTimespan::FromHours(1))
			GapFillBiasCache.Add(T, Bias);
	}
}

AFrostWeatherProviderActor::AFrostWeatherProviderActor()
{
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	Provider = CreateDefaultSubobject<UFrostWeatherProvider>(TEXT("FrostProvider"));

	if (Provider)
	{
		JsonPath = Provider->JsonPath;
		UseDerivedRadiationIfMissing = Provider->UseDerivedRadiationIfMissing;
		Site = Provider->Site;
	}
}

void AFrostWeatherProviderActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	SyncComponentSettings();
}

void AFrostWeatherProviderActor::BeginPlay()
{
	Super::BeginPlay();

	if (Provider)
	{
		SyncComponentSettings();
		Provider->LoadFromJson();
	}
}

void AFrostWeatherProviderActor::RefreshFromJson()
{
	if (Provider)
	{
		SyncComponentSettings();
		Provider->LoadFromJson();
	}
}

void AFrostWeatherProviderActor::SyncComponentSettings()
{
	if (!Provider)
	{
		return;
	}

	Provider->JsonPath = JsonPath;
	Provider->UseDerivedRadiationIfMissing = UseDerivedRadiationIfMissing;
	Provider->Site = Site;
}




#if WITH_EDITOR
void AFrostWeatherProviderActor::PullFrostNow()
{
	if (UWorld* World = GetWorld())
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(World, TEXT("PullFrostNow"));
	}
}
#endif
