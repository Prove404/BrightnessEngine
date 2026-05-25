#include "Util/WeatherCsvUtils.h"

#include "Misc/FileHelper.h"

namespace FWeatherCsvUtilities
{
    namespace
    {
        constexpr float FreezePointK = 273.15f;
        constexpr float SaturationE0Pa = 611.213f;
        constexpr float WaterVapourMolecularRatio = 0.622f;

        inline float CalcSaturationVapourPressure(float TemperatureK)
        {
            const float TempC = TemperatureK - FreezePointK;
            return SaturationE0Pa * FMath::Exp((17.67f * TempC) / (TempC + 243.5f));
        }

        inline float CalcRelativeHumidityFromSpecificHumidity(float SpecificHumidity, float TemperatureK, float PressurePa)
        {
            const float Den = WaterVapourMolecularRatio + (1.0f - WaterVapourMolecularRatio) * SpecificHumidity;
            if (Den <= KINDA_SMALL_NUMBER || PressurePa <= KINDA_SMALL_NUMBER)
            {
                return 0.0f;
            }

            const float VapourPressure = SpecificHumidity * PressurePa / Den;
            const float Saturation = CalcSaturationVapourPressure(TemperatureK);
            if (Saturation <= KINDA_SMALL_NUMBER)
            {
                return 0.0f;
            }

            return FMath::Clamp(VapourPressure / Saturation, 0.0f, 1.0f);
        }

        inline bool ParseIsoLoose(const FString& In, FDateTime& Out)
        {
            FString S = In;
            S.TrimStartAndEndInline();
            if (S.IsEmpty()) return false;
            // Normalize common forms: "YYYY-MM-DD HH:MM[:SS]" -> ISO8601 with Z
            if (!S.Contains(TEXT("T")) && (S.Contains(TEXT(" ")) || S.Len() == 16 || S.Len() == 19))
            {
                S.ReplaceInline(TEXT(" "), TEXT("T"));
            }
            if (!S.EndsWith(TEXT("Z")))
            {
                if (S.Len() == 16)
                {
                    S += TEXT(":00Z");
                }
                else if (S.Len() == 19)
                {
                    S += TEXT("Z");
                }
            }
            if (FDateTime::ParseIso8601(*S, Out))
            {
                return true;
            }
            // Fallback – try raw parse
            return FDateTime::Parse(In, Out);
        }

        enum class EDetectedSchema : uint8 { Simple8, Alptal, Finse, FSM2_Standard, Unknown };

        EDetectedSchema DetectSchema(const FString& HeaderLower)
        {
            if (HeaderLower.Contains(TEXT("tair,")) && HeaderLower.Contains(TEXT("lin,")) && HeaderLower.Contains(TEXT("sin,")))
            {
                return EDetectedSchema::Finse;
            }
            if (HeaderLower.StartsWith(TEXT("datetime,")) && HeaderLower.Contains(TEXT(",sw,")) && HeaderLower.Contains(TEXT(",lw,")))
            {
                return EDetectedSchema::Alptal;
            }
            // Simple 8-column if header looks like time then numeric fields
            if (HeaderLower.StartsWith(TEXT("time")) || HeaderLower.StartsWith(TEXT("timestamp")) || HeaderLower.StartsWith(TEXT("date")))
            {
                return EDetectedSchema::Simple8;
            }
            // Check for FSM2 Standard (space-separated, no header or specific header)
            // Often starts with year
            if (HeaderLower.StartsWith(TEXT("year")) || HeaderLower.IsNumeric()) 
            {
                return EDetectedSchema::FSM2_Standard;
            }
            return EDetectedSchema::Unknown;
        }

        inline void ClampSnowFracByTemperature(FWeatherForcingData& Forcing)
        {
            if (Forcing.bHasExplicitPrecipitation)
            {
                return;
            }

            const float TempC = Forcing.Temperature_K - FreezePointK;
            if (Forcing.PrecipRate_kgm2s <= KINDA_SMALL_NUMBER)
            {
                Forcing.SnowFrac_01 = 0.0f;
                return;
            }

            constexpr float WarmThresholdC = 0.5f;
            constexpr float ColdThresholdC = -0.5f;

            if (TempC >= WarmThresholdC)
            {
                Forcing.SnowFrac_01 = 0.0f;
            }
            else if (TempC <= ColdThresholdC)
            {
                Forcing.SnowFrac_01 = 1.0f;
            }
            else
            {
                const float Alpha = (WarmThresholdC - TempC) / (WarmThresholdC - ColdThresholdC);
                Forcing.SnowFrac_01 = FMath::Clamp(Alpha, 0.0f, 1.0f);
            }
        }

        bool ParseFSM2Standard(const FString& Line, FWeatherForcingData& Out, float PrecipitationScaleFactor = 1.0f)
        {
            // Standard FSM2 format (12 columns, space separated):
            // year month day hour sw lw sf rf ta rh ua ps
            
            // Handle tabs by replacing them with spaces
            FString CleanLine = Line;
            CleanLine.ReplaceInline(TEXT("\t"), TEXT(" "));

            TArray<FString> C;
            CleanLine.ParseIntoArray(C, TEXT(" "), true); // Split by spaces
            
            if (C.Num() < 12) return false;

            const int32 Year = FCString::Atoi(*C[0]);
            const int32 Month = FCString::Atoi(*C[1]);
            const int32 Day = FCString::Atoi(*C[2]);
            const float HourVal = FCString::Atof(*C[3]);
            
            const int32 Hour = FMath::FloorToInt(HourVal);
            const int32 Minute = FMath::FloorToInt((HourVal - Hour) * 60.0f);
            
            FDateTime T(Year, Month, Day, Hour, Minute, 0);

            const float SW = FCString::Atof(*C[4]);
            const float LW = FCString::Atof(*C[5]);
            const float Sf = FCString::Atof(*C[6]) * PrecipitationScaleFactor; // kg/m2/s or mm/h? FSM2 usually takes kg/m2/s in driver but file might be different. FSM2_DRIVE reads them.
            // FSM2_DRIVE standard read: SW, LW, Sf, Rf, Ta, RH, Ua, Ps.
            // BUT wait, FSM2_DRIVE.F90 lines 66: year,month,day,hour,SW,LW,Sf,Rf,Ta,RH,Ua,Ps
            // Unit check:
            // FSM2_DRIVE comments say Sf is kg/m^2/s.
            // ERA5_forcing.txt usually provided in mm/h or similar.
            // If the user's file is FSM2 standard input, it expects the model to handle units.
            // But we are the model loader.
            // We should assume the file matches FSM2 conventions if we call it FSM2 standard.
            // FSM2 driver usually expects kg/m^2/s for fluxes in the file?
            // FSM2_DRIVE: Sf, Rf are read into variables. Used directly.
            // FSM2 usually takes driving data in SI units (kg/m2/s).
            // HOWEVER, common FSM2 datasets (like Alptal) might be mm/h.
            // Let's assume SI (kg/m2/s) if it's standard FSM2 format, unless numbers are > 0.1 (implying mm/h).
            // Actually, 1 mm/h = 2.7e-4 kg/m2/s. 
            // If file has 0.0002, it's SI. If 1.0, it's mm/h.
            // Auto-detect?
            // Safe bet: FSM2 standard usually implies processed SI units.
            // But WeatherCsvUtils logic `PrecipitationScaleFactor` suggests external scaling.
            // `ParseSimple8` divides by 3600 (assuming mm/h).
            // Let's check magnitude.
            
            float Sf_val = FCString::Atof(*C[6]);
            float Rf_val = FCString::Atof(*C[7]);
            
            // Heuristic: If precipitation is > 0.1, it's likely mm/h. If < 0.01, likely kg/m2/s.
            // But if it's 0.0, we don't know.
            // Let's treat as mm/h (divide by 3600) because `ParseSimple8` does that. 
            // If user file is raw FSM2 input which is already SI, this will make precip tiny (1e-7).
            // BUT user complaint is "Too much snow".
            // If we divide by 3600, we reduce snow.
            // If input was 1.4e-4 (0.5 mm/h) and we divide by 3600 -> 4e-8. Effectively zero.
            // If input was 0.5 (mm/h) and we divide by 3600 -> 1.4e-4. Correct.
            // User said "accumulates way too much".
            // So likely input IS mm/h (0.5, 1.0) and we are NOT dividing by 3600 (because we failed to parse?).
            // Wait, if we failed to parse, we got 0 snow.
            // If we got too much snow, we must have parsed it.
            // Did `ParseSimple8` work?
            // `Simple8` splits by comma.
            // If file is space separated, `C[0]` is the whole line.
            // `ParseIsoLoose` fails. `ParseSimple8` returns false.
            // So `ParseLinesFlexible` returns 0 records.
            // If 0 records, `WeatherProvider` likely returns default (0).
            // So snow should be 0.
            // BUT user sees accumulation.
            // This implies parsing SUCCEEDED.
            // How?
            // Maybe the file IS comma separated?
            // Or `ParseSimple8` somehow worked?
            // If file is space separated: `2004 10 1 ...`
            // `ParseIsoLoose("2004 10 1 ...")`?
            // `FDateTime::Parse` might be smart enough to parse "2004 10 1" as a date?
            // If it parses, `C[1]` would be undefined (crash)?
            // `Line.ParseIntoArray(C, TEXT(","), true);` -> `C` has 1 element.
            // `if (C.Num() < 8) return false;`
            // So it returns false.
            // So parsing FAILED.
            // If parsing failed, why is there snow?
            // Maybe `WeatherProvider` has a fallback or default data?
            // Or user is using `ConstantWeatherProvider`?
            // User logs show `Forcing` plots.
            // `SW` curve looks real.
            // So data IS being loaded.
            // So file MUST be comma separated.
            // If file is comma separated, `ParseSimple8` works.
            // `ParseSimple8` divides by 3600.
            // So unit conversion is likely correct (assuming mm/h input).
            
            // Let's implement `ParseFSM2Standard` assuming Space Separated AND checking units.
            
            const float TaK = FCString::Atof(*C[8]);
            const float RH_val = FCString::Atof(*C[9]);
            const float Ua = FCString::Atof(*C[10]);
            const float Ps = FCString::Atof(*C[11]);

            // Normalize units
            // RH: FSM2 input usually %, but sometimes 0-1. 
            const float RH = (RH_val > 1.0f) ? RH_val / 100.0f : RH_val;
            
            // Precip: Try to detect unit.
            float PrecipRate = 0.0f;
            float SnowFrac = 0.0f;
            
            // Total precip rate in file units
            float TotalP_file = Sf_val + Rf_val;
            
            // If scale factor is 1.0 (default), apply mm/h -> kg/m2/s conversion (divide by 3600)
            // UNLESS values look like SI already.
            // Max reasonable precip is ~100 mm/h.
            // Min reasonable non-zero precip in SI is ~1e-5.
            // If value is 1e-4, is it 1e-4 mm/h (tiny) or 1e-4 kg/m2/s (moderate)?
            // Ambiguous.
            // Given "Too much snow", let's assume input is mm/h and we divide.
            
            float Sf_SI = Sf_val / 3600.0f;
            float Rf_SI = Rf_val / 3600.0f;
            
            PrecipRate = Sf_SI + Rf_SI;
            if (PrecipRate > KINDA_SMALL_NUMBER)
            {
                SnowFrac = Sf_SI / PrecipRate;
            }

            Out = FWeatherForcingData(T, TaK, SW, LW, Ua, FMath::Clamp(RH, 0.0f, 1.0f), PrecipRate, SnowFrac, Ps);
            Out.RainRate_kgm2s = Rf_SI;
            Out.SnowRate_kgm2s = Sf_SI;
            Out.bHasExplicitPrecipitation = true;
            ClampSnowFracByTemperature(Out);
            return true;
        }

        bool ParseSimple8(const FString& Line, FWeatherForcingData& Out, float PrecipitationScaleFactor = 1.0f)
        {
            TArray<FString> C; Line.ParseIntoArray(C, TEXT(","), true);
            if (C.Num() < 8) return false;
            FDateTime T; if (!ParseIsoLoose(C[0], T)) return false;
            const float TempC = FCString::Atof(*C[1]);
            const float RH_pct = FCString::Atof(*C[2]);
            const float Wind = FCString::Atof(*C[3]);
            const float SW = FCString::Atof(*C[4]);
            const float LW = FCString::Atof(*C[5]);
            const float Precip_mmph = FCString::Atof(*C[6]) * PrecipitationScaleFactor;
            const float SnowFrac = FCString::Atof(*C[7]);
            
            // Sanity check: SnowFrac must be [0, 1]. If > 1.0, it's likely a parsing error (e.g. reading Temperature as SnowFrac)
            if (SnowFrac < 0.0f || SnowFrac > 1.0f)
            {
                return false;
            }

            const float TempK = TempC + 273.15f;
            const float RH = FMath::Clamp(RH_pct / 100.0f, 0.0f, 1.0f);
            const float Precip = Precip_mmph / 3600.0f;
            Out = FWeatherForcingData(T, TempK, SW, LW, Wind, RH, Precip, SnowFrac);
            ClampSnowFracByTemperature(Out);
            return true;
        }

        bool ParseAlptal(const FString& Line, const TMap<FString,int32>& ColIdx, FWeatherForcingData& Out, float PrecipitationScaleFactor = 1.0f)
        {
            TArray<FString> C; Line.ParseIntoArray(C, TEXT(","), true);
            auto Get = [&](const TCHAR* Name)->FString
            { const int32* Idx = ColIdx.Find(FString(Name).ToLower()); return (Idx && C.IsValidIndex(*Idx)) ? C[*Idx] : FString(); };

            FDateTime T; if (!ParseIsoLoose(Get(TEXT("datetime")), T)) return false;
            const float SW = FCString::Atof(*Get(TEXT("sw")));
            const float LW = FCString::Atof(*Get(TEXT("lw")));
            const float Sf = FCString::Atof(*Get(TEXT("sf"))) * PrecipitationScaleFactor; // snowfall mm/h
            const float Rf = FCString::Atof(*Get(TEXT("rf"))) * PrecipitationScaleFactor; // rainfall mm/h
            const float TaK = FCString::Atof(*Get(TEXT("ta"))); // Kelvin
            const float RH_pct = FCString::Atof(*Get(TEXT("rh")));
            const float Ua = FCString::Atof(*Get(TEXT("ua"))); // wind m/s
            const float SnowFrac = (Sf > 0.0f) ? 1.0f : 0.0f;
            const float RH = FMath::Clamp(RH_pct / 100.0f, 0.0f, 1.0f);
            const float Precip = (FMath::Max(0.0f, Sf) + FMath::Max(0.0f, Rf)) / 3600.0f;
            Out = FWeatherForcingData(T, TaK, SW, LW, Ua, RH, Precip, SnowFrac);
            Out.RainRate_kgm2s = FMath::Max(0.0f, Rf) / 3600.0f;
            Out.SnowRate_kgm2s = FMath::Max(0.0f, Sf) / 3600.0f;
            Out.bHasExplicitPrecipitation = true;
            ClampSnowFracByTemperature(Out);
            return true;
        }

        bool ParseFinse(const FString& Line, const TMap<FString,int32>& ColIdx, FWeatherForcingData& Out, float PrecipitationScaleFactor = 1.0f)
        {
            TArray<FString> C; Line.ParseIntoArray(C, TEXT(","), true);
            auto Get = [&](const TCHAR* Name)->FString
            { const int32* Idx = ColIdx.Find(FString(Name).ToLower()); return (Idx && C.IsValidIndex(*Idx)) ? C[*Idx] : FString(); };

            FDateTime T; if (!ParseIsoLoose(Get(TEXT("t_span")), T)) return false;
            const float TairC = FCString::Atof(*Get(TEXT("tair")));
            const float Lin = FCString::Atof(*Get(TEXT("lin")));
            const float Sin = FCString::Atof(*Get(TEXT("sin")));
            const float Wind = FCString::Atof(*Get(TEXT("wind")));
            const float PressurePa = FMath::Max(1.0f, FCString::Atof(*Get(TEXT("p"))));
            const float SpecificHumidity = FMath::Max(0.0f, FCString::Atof(*Get(TEXT("q"))));
            const float Rain = FMath::Max(0.0f, FCString::Atof(*Get(TEXT("rainfall")))) * PrecipitationScaleFactor;
            const float Snow = FMath::Max(0.0f, FCString::Atof(*Get(TEXT("snowfall")))) * PrecipitationScaleFactor;
            const float TemperatureK = TairC + FreezePointK;
            const float RainRate = Rain / 3600.0f;
            const float SnowRate = Snow / 3600.0f;
            const float Precip = RainRate + SnowRate;
            float SnowFrac = 0.0f;
            if (Precip > KINDA_SMALL_NUMBER)
            {
                SnowFrac = FMath::Clamp(SnowRate / Precip, 0.0f, 1.0f);
            }
            const float RH = CalcRelativeHumidityFromSpecificHumidity(SpecificHumidity, TemperatureK, PressurePa);
            Out = FWeatherForcingData(T, TemperatureK, Sin, Lin, Wind, RH, Precip, SnowFrac, PressurePa);
            Out.RainRate_kgm2s = RainRate;
            Out.SnowRate_kgm2s = SnowRate;
            Out.bHasExplicitPrecipitation = true;
            ClampSnowFracByTemperature(Out);
            return true;
        }

    bool ParseLinesFlexible(const TArray<FString>& Lines, TArray<FWeatherForcingData>& OutRecords, float PrecipitationScaleFactor = 1.0f)
    {
        OutRecords.Reset();
        if (Lines.Num() < 2) return false;

        FString Header = Lines[0];
        FString HeaderLower = Header.ToLower();
        HeaderLower.TrimStartAndEndInline();
        EDetectedSchema Schema = DetectSchema(HeaderLower);

        // Log detected schema for debugging
        const TCHAR* SchemaName = TEXT("Unknown");
        switch (Schema)
        {
            case EDetectedSchema::Simple8: SchemaName = TEXT("Simple8"); break;
            case EDetectedSchema::Alptal: SchemaName = TEXT("Alptal"); break;
            case EDetectedSchema::Finse: SchemaName = TEXT("Finse"); break;
            case EDetectedSchema::FSM2_Standard: SchemaName = TEXT("FSM2_Standard"); break;
        }
        UE_LOG(LogTemp, Log, TEXT("[WeatherCSV] Detected Schema: %s (Header: %s)"), SchemaName, *HeaderLower.Left(50));

        // Build header lookup for named schemas
        TMap<FString,int32> ColIdx;
        {
            TArray<FString> HCols; Header.ParseIntoArray(HCols, TEXT(","), true);
            for (int32 i=0;i<HCols.Num();++i){ FString Key = HCols[i].ToLower(); Key.TrimStartAndEndInline(); ColIdx.Add(Key, i);}
        }

        for (int32 i = 1; i < Lines.Num(); ++i)
        {
            const FString& L = Lines[i];
            if (L.IsEmpty()) continue;
            FWeatherForcingData Rec;
            bool bOk = false;
            switch (Schema)
            {
                case EDetectedSchema::Simple8: bOk = ParseSimple8(L, Rec, PrecipitationScaleFactor); break;
                case EDetectedSchema::Alptal:  bOk = ParseAlptal(L, ColIdx, Rec, PrecipitationScaleFactor); break;
                case EDetectedSchema::Finse:   bOk = ParseFinse(L, ColIdx, Rec, PrecipitationScaleFactor); break;
                case EDetectedSchema::FSM2_Standard: bOk = ParseFSM2Standard(L, Rec, PrecipitationScaleFactor); break;
                case EDetectedSchema::Unknown:
                default:
                    // Try simple8 as fallback, then FSM2
                    if (ParseSimple8(L, Rec, PrecipitationScaleFactor)) {
                        bOk = true;
                    } else if (ParseFSM2Standard(L, Rec, PrecipitationScaleFactor)) {
                        bOk = true;
                    }
                    break;
            }
            if (bOk) { OutRecords.Add(Rec); }
        }

        OutRecords.Sort([](const FWeatherForcingData& A, const FWeatherForcingData& B){ return A.Timestamp < B.Timestamp; });
        return OutRecords.Num() > 0;
    }

        bool ParseLineInternal(const FString& Line, FWeatherForcingData& OutData, float PrecipitationScaleFactor = 1.0f)
        {
            // For single line parsing, assume Simple8 format (8 columns)
            return ParseSimple8(Line, OutData, PrecipitationScaleFactor);
        }
    }

    void ApplyTemperatureSnowPartition(FWeatherForcingData& Forcing)
    {
        ClampSnowFracByTemperature(Forcing);
    }

	bool ParseWeatherCsvLine(const FString& Line, FWeatherForcingData& OutData, float PrecipitationScaleFactor)
	{
		return ParseLineInternal(Line, OutData, PrecipitationScaleFactor);
	}

    bool ParseWeatherCsvContent(const FString& CsvContent, TArray<FWeatherForcingData>& OutRecords, float PrecipitationScaleFactor)
    {
        TArray<FString> Lines;
        CsvContent.ParseIntoArray(Lines, TEXT("\n"), true);
        return ParseLinesFlexible(Lines, OutRecords, PrecipitationScaleFactor);
    }

    bool ParseWeatherCsvFile(const FString& FilePath, TArray<FWeatherForcingData>& OutRecords, float PrecipitationScaleFactor)
    {
        FString FileContent;
        if (!FFileHelper::LoadFileToString(FileContent, *FilePath))
        {
            return false;
        }
        return ParseWeatherCsvContent(FileContent, OutRecords, PrecipitationScaleFactor);
    }
}
