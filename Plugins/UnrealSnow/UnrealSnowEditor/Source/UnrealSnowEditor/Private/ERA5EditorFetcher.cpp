#include "ERA5/ERA5EditorFetcher.h"

#include "Async/Async.h"
#include "Containers/Set.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ERA5/ERA5Settings.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SnowSimulationActor.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "GeoReferencingSystem.h"
#include "GeographicCoordinates.h"

#include <limits>

namespace ERA5EditorFetcherInternal
{
    float NormalizeElevationMeters(double InElevation, const TCHAR* SourceTag)
    {
        float ElevMeters = static_cast<float>(InElevation);
        if (!FMath::IsFinite(ElevMeters))
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] %s elevation is non-finite (%.6f). Using 0 m."), SourceTag, InElevation);
            return 0.0f;
        }

        // Guard against cm values ending up in meter fields.
        if (FMath::Abs(ElevMeters) > 10000.0f)
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] %s elevation appears too large (%.2f). Assuming centimeters and converting to meters."), SourceTag, ElevMeters);
            ElevMeters /= 100.0f;
        }

        return ElevMeters;
    }

    struct FERA5PullConfig
    {
        EERA5DataSource DataSource = EERA5DataSource::OpenMeteo;
        FString OpenMeteoBaseUrl;
        FString CdsApiUrl;
        FString CdsApiKey;
        FString CdsDataset;
        double Latitude = 0.0;
        double Longitude = 0.0;
        float ElevationM = 0.0f;
        FString OutputPath;
        int32 HoursBack = 0;
        bool bUseExplicitRange = false;
        FDateTime RangeStartUtc = FDateTime::MinValue();
        FDateTime RangeEndUtc = FDateTime::MinValue();
        double AreaPaddingDegrees = 0.0;
        TArray<FString> HourlyVariables;
        FString TemperatureVariable;
        FString RelativeHumidityVariable;
        FString WindSpeedVariable;
        FString WindDirectionVariable;
        FString PrecipitationVariable;
        FString RainVariable;
        FString SnowfallVariable;
        FString CloudCoverVariable;
        FString ShortwaveVariable;
        FString DirectShortwaveVariable;
        FString DiffuseShortwaveVariable;
        FString LongwaveVariable;
        FString SurfacePressureVariable;
    };

    struct FERA5PullOutcome
    {
        bool bSuccess = false;
        int32 SampleCount = 0;
    };

    FString ResolveEra5Path(const FString& InPath)
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

    static void EnsureOutputDirectory(const FString& FilePath)
    {
        const FString Directory = FPaths::GetPath(FilePath);
        if (!Directory.IsEmpty())
        {
            IFileManager::Get().MakeDirectory(*Directory, true);
        }
    }

    bool ExecuteGetRequest(const FString& Url, FString& OutContent, FString& OutError)
    {
        FHttpModule& HttpModule = FHttpModule::Get();
        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = HttpModule.CreateRequest();
        Request->SetURL(Url);
        Request->SetVerb(TEXT("GET"));
        Request->SetHeader(TEXT("Accept"), TEXT("application/json"));

        FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);

        bool bRequestSuccess = false;
        Request->OnProcessRequestComplete().BindLambda([&](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
        {
            if (bSucceeded && Response.IsValid())
            {
                if (EHttpResponseCodes::IsOk(Response->GetResponseCode()))
                {
                    OutContent = Response->GetContentAsString();
                    bRequestSuccess = true;
                }
                else
                {
                    OutError = FString::Printf(TEXT("HTTP %d: %s"), Response->GetResponseCode(), *Response->GetContentAsString());
                }
            }
            else
            {
                OutError = TEXT("Request failed or no response received");
            }

            CompletionEvent->Trigger();
        });

        if (!Request->ProcessRequest())
        {
            OutError = TEXT("Unable to dispatch HTTP request");
            CompletionEvent->Trigger();
        }

        CompletionEvent->Wait();
        FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
        return bRequestSuccess;
    }

    double GetValueAt(const TArray<double>& Values, int32 Index)
    {
        return Values.IsValidIndex(Index) ? Values[Index] : std::numeric_limits<double>::quiet_NaN();
    }

    void SetOptionalNumber(TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, double Value)
    {
        if (FMath::IsFinite(Value))
        {
            Object->SetNumberField(FieldName, Value);
        }
        else
        {
            Object->SetField(FieldName, MakeShared<FJsonValueNull>());
        }
    }
    bool ExtractDoubleArray(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, TArray<double>& OutValues)
    {
        OutValues.Reset();

        if (!Object.IsValid() || FieldName.IsEmpty())
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
        if (!Object->TryGetArrayField(FieldName, JsonArray) || !JsonArray)
        {
            return false;
        }

        OutValues.Reserve(JsonArray->Num());
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            double Number = std::numeric_limits<double>::quiet_NaN();
            if (Value.IsValid())
            {
                if (!Value->TryGetNumber(Number))
                {
                    Number = std::numeric_limits<double>::quiet_NaN();
                }
            }
            OutValues.Add(Number);
        }
        return true;
    }

    FString BuildRequestUrl(const FERA5PullConfig& Config, const FString& RangeStartDate, const FString& RangeEndDate)
    {
        FString HourlyParamList = FString::Join(Config.HourlyVariables, TEXT(","));
        FString Url = FString::Printf(
            TEXT("%s?latitude=%.6f&longitude=%.6f&start_date=%s&end_date=%s&hourly=%s&timezone=UTC"),
            *Config.OpenMeteoBaseUrl,
            Config.Latitude,
            Config.Longitude,
            *RangeStartDate,
            *RangeEndDate,
            *HourlyParamList);
        return Url;
    }

    FString NormalizeTimestamp(const FString& Timestamp)
    {
        FString Result = Timestamp;
        if (Result.Len() == 16)
        {
            Result += TEXT(":00Z");
        }
        else if (!Result.EndsWith(TEXT("Z")))
        {
            Result += TEXT("Z");
        }
        return Result;
    }

    bool ParseEra5Response(const FString& JsonPayload, const FERA5PullConfig& Config, const FDateTime& RangeStartUtc, const FDateTime& RangeEndUtc, TArray<TSharedPtr<FJsonValue>>& OutSeries, FDateTime& OutObservedStart, FDateTime& OutObservedEnd)
    {
        TSharedPtr<FJsonObject> RootObject;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
        if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Failed to parse JSON response."));
            return false;
        }

        const TSharedPtr<FJsonObject>* HourlyObjPtr = nullptr;
        if (!RootObject->TryGetObjectField(TEXT("hourly"), HourlyObjPtr) || !HourlyObjPtr || !HourlyObjPtr->IsValid())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Response missing 'hourly' object."));
            return false;
        }

        const TSharedPtr<FJsonObject>& HourlyObj = *HourlyObjPtr;
        const TArray<TSharedPtr<FJsonValue>>* TimeValues = nullptr;
        if (!HourlyObj->TryGetArrayField(TEXT("time"), TimeValues) || !TimeValues)
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Response missing time array."));
            return false;
        }

        const int32 SampleCount = TimeValues->Num();
        if (SampleCount == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Response contains no samples."));
            return false;
        }

        TArray<double> TemperatureValues;
        TArray<double> HumidityValues;
        TArray<double> WindSpeedValues;
        TArray<double> WindDirectionValues;
        TArray<double> PrecipitationValues;
        TArray<double> RainValues;
        TArray<double> SnowfallValues;
        TArray<double> CloudCoverValues;
        TArray<double> ShortwaveValues;
        TArray<double> DirectShortwaveValues;
        TArray<double> DiffuseValues;
        TArray<double> LongwaveValues;
        TArray<double> SurfacePressureValues;

        // Extract variables with warnings for missing ones
        if (!ExtractDoubleArray(HourlyObj, Config.TemperatureVariable, TemperatureValues) && !Config.TemperatureVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.TemperatureVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.RelativeHumidityVariable, HumidityValues) && !Config.RelativeHumidityVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.RelativeHumidityVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.WindSpeedVariable, WindSpeedValues) && !Config.WindSpeedVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.WindSpeedVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.WindDirectionVariable, WindDirectionValues) && !Config.WindDirectionVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.WindDirectionVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.PrecipitationVariable, PrecipitationValues) && !Config.PrecipitationVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.PrecipitationVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.RainVariable, RainValues) && !Config.RainVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.RainVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.SnowfallVariable, SnowfallValues) && !Config.SnowfallVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.SnowfallVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.CloudCoverVariable, CloudCoverValues) && !Config.CloudCoverVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.CloudCoverVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.ShortwaveVariable, ShortwaveValues) && !Config.ShortwaveVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.ShortwaveVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.DirectShortwaveVariable, DirectShortwaveValues) && !Config.DirectShortwaveVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.DirectShortwaveVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.DiffuseShortwaveVariable, DiffuseValues) && !Config.DiffuseShortwaveVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.DiffuseShortwaveVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.LongwaveVariable, LongwaveValues) && !Config.LongwaveVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.LongwaveVariable);
        }
        if (!ExtractDoubleArray(HourlyObj, Config.SurfacePressureVariable, SurfacePressureValues) && !Config.SurfacePressureVariable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Variable '%s' not found in API response, will use NaN values"), *Config.SurfacePressureVariable);
        }

        OutSeries.Reset();
        OutObservedStart = FDateTime::MaxValue();
        OutObservedEnd = FDateTime::MinValue();

        for (int32 Index = 0; Index < SampleCount; ++Index)
        {
            const TSharedPtr<FJsonValue>& TimeValue = (*TimeValues)[Index];
            if (!TimeValue.IsValid())
            {
                continue;
            }

            FString TimestampStr = NormalizeTimestamp(TimeValue->AsString());
            FDateTime SampleTime;
            if (!FDateTime::ParseIso8601(*TimestampStr, SampleTime))
            {
                continue;
            }

            if (SampleTime < RangeStartUtc || SampleTime >= RangeEndUtc)
            {
                continue;
            }

            OutObservedStart = FMath::Min(OutObservedStart, SampleTime);
            OutObservedEnd = FMath::Max(OutObservedEnd, SampleTime + FTimespan::FromHours(1));

            TSharedPtr<FJsonObject> SampleObject = MakeShared<FJsonObject>();
            SampleObject->SetStringField(TEXT("time_utc"), SampleTime.ToIso8601());

            const double TempC = GetValueAt(TemperatureValues, Index);
            const double RH = GetValueAt(HumidityValues, Index);
            const double WindSpeedKmh = GetValueAt(WindSpeedValues, Index);
            const double WindDir = GetValueAt(WindDirectionValues, Index);
            const double PrecipMm = GetValueAt(PrecipitationValues, Index);
            const double RainMm = GetValueAt(RainValues, Index);
            const double SnowfallCm = GetValueAt(SnowfallValues, Index);
            const double CloudCover = GetValueAt(CloudCoverValues, Index);
            const double Shortwave = GetValueAt(ShortwaveValues, Index);
            const double DirectShortwave = GetValueAt(DirectShortwaveValues, Index);
            const double Diffuse = GetValueAt(DiffuseValues, Index);
            const double Longwave = GetValueAt(LongwaveValues, Index);
            const double Pressure = GetValueAt(SurfacePressureValues, Index);

            const double WindSpeedMps = FMath::IsFinite(WindSpeedKmh) ? WindSpeedKmh / 3.6 : WindSpeedKmh;

            SetOptionalNumber(SampleObject, TEXT("T_C"), TempC);
            SetOptionalNumber(SampleObject, TEXT("RH_pct"), RH);
            SetOptionalNumber(SampleObject, TEXT("Wind_mps"), WindSpeedMps);
            SetOptionalNumber(SampleObject, TEXT("WindDir_deg"), WindDir);
            SetOptionalNumber(SampleObject, TEXT("Precip_mmph"), PrecipMm);
            SetOptionalNumber(SampleObject, TEXT("Rain_mmph"), RainMm);
            SetOptionalNumber(SampleObject, TEXT("snowfall"), SnowfallCm);
            SetOptionalNumber(SampleObject, TEXT("cloud_cover"), CloudCover);
            SetOptionalNumber(SampleObject, TEXT("SWdown_Wm2"), Shortwave);
            SetOptionalNumber(SampleObject, TEXT("DirectSWdown_Wm2"), DirectShortwave);
            SetOptionalNumber(SampleObject, TEXT("DiffuseSWdown_Wm2"), Diffuse);
            SetOptionalNumber(SampleObject, TEXT("LWdown_Wm2"), Longwave);
            SetOptionalNumber(SampleObject, TEXT("Pressure_hPa"), Pressure);

            if (FMath::IsFinite(SnowfallCm) && SnowfallCm > 0.0)
            {
                SampleObject->SetNumberField(TEXT("SnowFrac"), 1.0);
            }
            else
            {
                SampleObject->SetNumberField(TEXT("SnowFrac"), 0.0);
            }

            OutSeries.Add(MakeShared<FJsonValueObject>(SampleObject));
        }

        if (OutSeries.Num() == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] No samples inside requested window %s -> %s."), *RangeStartUtc.ToIso8601(), *RangeEndUtc.ToIso8601());
            return false;
        }

        return true;
    }

    bool ExecuteCdsPull(const FERA5PullConfig& Config, FERA5PullOutcome& OutOutcome)
    {
        if (Config.CdsApiUrl.IsEmpty() || Config.CdsApiKey.IsEmpty() || Config.CdsDataset.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] CDS configuration is incomplete (ApiUrl/ApiKey/Dataset)."));
            return false;
        }

        TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealSnow"));
        FString ScriptPath = Plugin.IsValid()
            ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("UnrealSnowEditor/Scripts/era5_cds_fetch.py"))
            : FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/UnrealSnow/UnrealSnowEditor/Scripts/era5_cds_fetch.py"));
        ScriptPath = FPaths::ConvertRelativePathToFull(ScriptPath);

        if (!FPaths::FileExists(ScriptPath))
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] CDS helper script missing: %s"), *ScriptPath);
            return false;
        }

        const FString ResolvedOutputPath = ResolveEra5Path(Config.OutputPath);
        if (ResolvedOutputPath.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Output path is not configured for CDS pull."));
            return false;
        }

        auto ClampLat = [](double Value)
        {
            return FMath::Clamp(Value, -90.0, 90.0);
        };

        auto ClampLon = [](double Value)
        {
            return FMath::Clamp(Value, -180.0, 180.0);
        };

        const double Padding = FMath::Max(Config.AreaPaddingDegrees, 0.0);
        double North = ClampLat(Config.Latitude + Padding);
        double South = ClampLat(Config.Latitude - Padding);
        double East = ClampLon(Config.Longitude + Padding);
        double West = ClampLon(Config.Longitude - Padding);

        if (North < South)
        {
            Swap(North, South);
        }

        if (East < West)
        {
            Swap(East, West);
        }

        TArray<FString> Hours;
        for (int32 Hour = 0; Hour < 24; ++Hour)
        {
            Hours.Add(FString::Printf(TEXT("%02d:00"), Hour));
        }
        const FString HoursCsv = FString::Join(Hours, TEXT(","));
        const FString VariablesCsv = FString::Join(Config.HourlyVariables, TEXT(","));

        FDateTime InclusiveEnd = Config.RangeEndUtc - FTimespan::FromMinutes(1);
        if (InclusiveEnd < Config.RangeStartUtc)
        {
            InclusiveEnd = Config.RangeStartUtc;
        }

        const FString MetaSource = FString::Printf(TEXT("CDS dataset %s"), *Config.CdsDataset);

        EnsureOutputDirectory(ResolvedOutputPath);

        const FString CommandLine = FString::Printf(
            TEXT("\"%s\" --api-url \"%s\" --api-key \"%s\" --dataset \"%s\" --variables \"%s\" --start \"%s\" --end \"%s\" --hours \"%s\" --north %f --south %f --east %f --west %f --latitude %f --longitude %f --elevation %f --output \"%s\" --meta-source \"%s\""),
            *ScriptPath,
            *Config.CdsApiUrl,
            *Config.CdsApiKey,
            *Config.CdsDataset,
            *VariablesCsv,
            *Config.RangeStartUtc.ToIso8601(),
            *InclusiveEnd.ToIso8601(),
            *HoursCsv,
            North,
            South,
            East,
            West,
            Config.Latitude,
            Config.Longitude,
            Config.ElevationM,
            *ResolvedOutputPath,
            *MetaSource);

        FString PythonExecutable = TEXT("python");
        FString StdOut;
        FString StdErr;
        int32 ReturnCode = 0;
        if (!FPlatformProcess::ExecProcess(*PythonExecutable, *CommandLine, &ReturnCode, &StdOut, &StdErr) || ReturnCode != 0)
        {
            UE_LOG(LogTemp, Error, TEXT("[ERA5] CDS script failed (code %d). Output: %s"), ReturnCode, *StdErr);
            return false;
        }

        int32 SamplesFromScript = 0;
        TArray<FString> StdOutLines;
        StdOut.ParseIntoArrayLines(StdOutLines, false);
        for (const FString& Line : StdOutLines)
        {
            if (Line.StartsWith(TEXT("SAMPLES:")))
            {
                const FString CountString = Line.RightChop(8);
                SamplesFromScript = FCString::Atoi(*CountString);
                break;
            }
        }

        int32 SampleCount = SamplesFromScript;
        FString JsonText;
        if (FFileHelper::LoadFileToString(JsonText, *ResolvedOutputPath))
        {
            TSharedPtr<FJsonObject> RootObject;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
            if (FJsonSerializer::Deserialize(Reader, RootObject) && RootObject.IsValid())
            {
                const TArray<TSharedPtr<FJsonValue>>* SeriesArray = nullptr;
                if (RootObject->TryGetArrayField(TEXT("series"), SeriesArray) && SeriesArray)
                {
                    SampleCount = SeriesArray->Num();
                }
            }
        }

        OutOutcome.SampleCount = SampleCount;
        OutOutcome.bSuccess = SampleCount > 0;

        if (OutOutcome.bSuccess)
        {
            UE_LOG(LogTemp, Display, TEXT("[ERA5] CDS wrote %d samples to %s"), SampleCount, *ResolvedOutputPath);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] CDS script completed but produced no samples."));
        }

        return OutOutcome.bSuccess;
    }

FERA5PullOutcome PerformERA5Pull(const FERA5PullConfig& Config)
    {
        FERA5PullOutcome Outcome;
        EERA5DataSource EffectiveSource = Config.DataSource;

        if (Config.DataSource == EERA5DataSource::CopernicusCDS)
        {
            if (ExecuteCdsPull(Config, Outcome))
            {
                return Outcome;
            }

            UE_LOG(LogTemp, Warning, TEXT("[ERA5] CDS pull failed. Falling back to Open-Meteo ERA5."));
            EffectiveSource = EERA5DataSource::OpenMeteo;
        }

        if (Config.OpenMeteoBaseUrl.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] API base URL not configured."));
            return Outcome;
        }

        FString OutputPath = ResolveEra5Path(Config.OutputPath);
        if (OutputPath.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Output path is not configured."));
            return Outcome;
        }

        if (Config.HourlyVariables.Num() == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] No variables configured for download."));
            return Outcome;
        }

        FDateTime RangeStart = Config.RangeStartUtc;
        FDateTime RangeEnd = Config.RangeEndUtc;
        if (RangeStart == FDateTime::MinValue() || RangeEnd == FDateTime::MinValue() || RangeEnd <= RangeStart)
        {
            RangeEnd = FDateTime::UtcNow();
            const int32 EffectiveHoursBack = FMath::Max(Config.HoursBack, 1);
            RangeStart = RangeEnd - FTimespan::FromHours(EffectiveHoursBack);
        }

        const FDateTime NowUtc = FDateTime::UtcNow();
        if (RangeEnd > NowUtc)
        {
            RangeEnd = NowUtc;
        }

        if (RangeEnd <= RangeStart)
        {
            RangeEnd = RangeStart + FTimespan::FromHours(1);
        }

        FDateTime InclusiveEnd = RangeEnd - FTimespan::FromMinutes(1);
        if (InclusiveEnd < RangeStart)
        {
            InclusiveEnd = RangeStart;
        }

        const FString StartDate = RangeStart.ToString(TEXT("%Y-%m-%d"));
        const FString EndDate = InclusiveEnd.ToString(TEXT("%Y-%m-%d"));
        const FString RequestUrl = BuildRequestUrl(Config, StartDate, EndDate);

        UE_LOG(LogTemp, Display, TEXT("[ERA5] Requesting %s -> %s (%d variables)."), *StartDate, *EndDate, Config.HourlyVariables.Num());
        UE_LOG(LogTemp, Display, TEXT("[ERA5] URL: %s"), *RequestUrl);

        FString JsonPayload;
        FString RequestError;
        if (!ExecuteGetRequest(RequestUrl, JsonPayload, RequestError))
        {
            UE_LOG(LogTemp, Error, TEXT("[ERA5] HTTP request failed: %s"), *RequestError);
            UE_LOG(LogTemp, Error, TEXT("[ERA5] This often means one or more variables are not supported by the API."));
            UE_LOG(LogTemp, Error, TEXT("[ERA5] Check that all variable names are correct for the Open-Meteo ERA5 API."));
            UE_LOG(LogTemp, Error, TEXT("[ERA5] Common issue: 'direct_radiation' is not available in Open-Meteo ERA5 dataset."));
            return Outcome;
        }

        TArray<TSharedPtr<FJsonValue>> SeriesArray;
        FDateTime ObservedStart;
        FDateTime ObservedEnd;
        if (!ParseEra5Response(JsonPayload, Config, RangeStart, RangeEnd, SeriesArray, ObservedStart, ObservedEnd))
        {
            return Outcome;
        }

        TSharedPtr<FJsonObject> OutputRoot = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
        Meta->SetStringField(TEXT("created_utc"), FDateTime::UtcNow().ToIso8601());
        const FString SourceLabel = (EffectiveSource == EERA5DataSource::CopernicusCDS) ? FString::Printf(TEXT("CDS dataset %s"), *Config.CdsDataset) : FString::Printf(TEXT("ERA5 lat=%.4f lon=%.4f"), Config.Latitude, Config.Longitude);
        Meta->SetStringField(TEXT("sources"), SourceLabel);
        Meta->SetStringField(TEXT("range_start_utc"), ObservedStart.ToIso8601());
        Meta->SetStringField(TEXT("range_end_utc_exclusive"), ObservedEnd.ToIso8601());
        Meta->SetNumberField(TEXT("latitude_deg"), Config.Latitude);
        Meta->SetNumberField(TEXT("longitude_deg"), Config.Longitude);
        Meta->SetNumberField(TEXT("site_elevation_m"), Config.ElevationM);
        Meta->SetStringField(TEXT("data_source"), EffectiveSource == EERA5DataSource::CopernicusCDS ? TEXT("CopernicusCDS") : TEXT("OpenMeteo"));
        OutputRoot->SetObjectField(TEXT("meta"), Meta);
        OutputRoot->SetArrayField(TEXT("series"), SeriesArray);

        FString OutputString;
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
        FJsonSerializer::Serialize(OutputRoot.ToSharedRef(), Writer);

        EnsureOutputDirectory(OutputPath);
        if (FFileHelper::SaveStringToFile(OutputString, *OutputPath))
        {
            UE_LOG(LogTemp, Display, TEXT("[ERA5] Wrote %d samples to %s"), SeriesArray.Num(), *OutputPath);
            Outcome.bSuccess = true;
            Outcome.SampleCount = SeriesArray.Num();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] Failed to write %s"), *OutputPath);
        }

        return Outcome;
    }

    bool TryGetGeospatialOrigin(UWorld* World, double& OutLat, double& OutLon, double& OutAlt)
    {
        if (!World)
        {
            return false;
        }

        if (AGeoReferencingSystem* Geo = AGeoReferencingSystem::GetGeoReferencingSystem(World))
        {
            if (Geo->bOriginAtPlanetCenter)
            {
                return false;
            }

            if (Geo->bOriginLocationInProjectedCRS)
            {
                FGeographicCoordinates GeoCoords;
                Geo->ProjectedToGeographic(FVector(Geo->OriginProjectedCoordinatesEasting, Geo->OriginProjectedCoordinatesNorthing, Geo->OriginProjectedCoordinatesUp), GeoCoords);
                OutLat = GeoCoords.Latitude;
                OutLon = GeoCoords.Longitude;
                OutAlt = NormalizeElevationMeters(GeoCoords.Altitude, TEXT("GeoReferencing projected"));
            }
            else
            {
                OutLat = Geo->OriginLatitude;
                OutLon = Geo->OriginLongitude;
                OutAlt = NormalizeElevationMeters(Geo->OriginAltitude, TEXT("GeoReferencing geographic"));
            }

            return true;
        }

        return false;
    }
}

using namespace ERA5EditorFetcherInternal;

void UERA5EditorFetcher::PullERA5Now()
{
    UE_LOG(LogTemp, Warning, TEXT("[ERA5] PullERA5Now called."));

    if (bPullInProgress)
    {
        UE_LOG(LogTemp, Warning, TEXT("[ERA5] Pull already in progress."));
        return;
    }

    const UERA5Settings* Settings = GetDefault<UERA5Settings>();
    if (!Settings)
    {
        UE_LOG(LogTemp, Warning, TEXT("[ERA5] Settings not found."));
        OnERA5PullFinished.Broadcast(false, 0);
        return;
    }

    FERA5PullConfig Config;
    Config.DataSource = Settings->DataSource;
    UE_LOG(LogTemp, Display, TEXT("[ERA5] Data source: %s"), Config.DataSource == EERA5DataSource::CopernicusCDS ? TEXT("Copernicus CDS") : TEXT("Open-Meteo ERA5"));

    bool bUsedDefaultOpenMeteoBaseUrl = false;
    Config.OpenMeteoBaseUrl = Settings->GetResolvedOpenMeteoBaseUrl(bUsedDefaultOpenMeteoBaseUrl);
    if (bUsedDefaultOpenMeteoBaseUrl)
    {
        UE_LOG(LogTemp, Warning, TEXT("[ERA5] ApiBaseUrl configuration lost its host (likely due to // being treated as a comment). Using default %s. Wrap the value in quotes if you need to override it."), *Config.OpenMeteoBaseUrl);
    }

    bool bUsedDefaultCdsBaseUrl = false;
    Config.CdsApiUrl = Settings->GetResolvedCdsApiUrl(bUsedDefaultCdsBaseUrl);
    if (bUsedDefaultCdsBaseUrl)
    {
        UE_LOG(LogTemp, Warning, TEXT("[ERA5] CDS ApiUrl missing host. Using default %s."), *Config.CdsApiUrl);
    }
    Config.CdsApiKey = Settings->CdsApiKey;
    Config.CdsDataset = Settings->CdsDataset;

    Config.Latitude = Settings->Latitude;
    Config.Longitude = Settings->Longitude;
    Config.ElevationM = Settings->ElevationM;
    Config.OutputPath = Settings->OutputJsonPath.FilePath;
    Config.HoursBack = Settings->HoursBack;
    Config.AreaPaddingDegrees = Settings->AreaPaddingDegrees;
    Config.TemperatureVariable = Settings->TemperatureVariable;
    Config.RelativeHumidityVariable = Settings->RelativeHumidityVariable;
    Config.WindSpeedVariable = Settings->WindSpeedVariable;
    Config.WindDirectionVariable = Settings->WindDirectionVariable;
    Config.PrecipitationVariable = Settings->PrecipitationVariable;
    Config.RainVariable = Settings->RainVariable;
    Config.SnowfallVariable = Settings->SnowfallVariable;
    Config.CloudCoverVariable = Settings->CloudCoverVariable;
    Config.ShortwaveVariable = Settings->ShortwaveVariable;
    Config.DirectShortwaveVariable = Settings->DirectShortwaveVariable;
    Config.DiffuseShortwaveVariable = Settings->DiffuseShortwaveVariable;
    Config.LongwaveVariable = Settings->LongwaveVariable;
    Config.SurfacePressureVariable = Settings->SurfacePressureVariable;
    Settings->BuildHourlyVariableList(Config.HourlyVariables);

    FDateTime RangeStart = FDateTime::MinValue();
    FDateTime RangeEnd = FDateTime::MinValue();
    bool bFoundRange = false;

    double AccLatitude = 0.0;
    double AccLongitude = 0.0;
    int32 CoordinateSamples = 0;
    bool bGeoRefOverride = false;

    if (GEditor)
    {
        UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
        if (EditorWorld)
        {
            double GeoLat = 0.0;
            double GeoLon = 0.0;
            double GeoAlt = 0.0;
            if (TryGetGeospatialOrigin(EditorWorld, GeoLat, GeoLon, GeoAlt))
            {
                Config.Latitude = GeoLat;
                Config.Longitude = GeoLon;
                Config.ElevationM = GeoAlt;
                bGeoRefOverride = true;
                UE_LOG(LogTemp, Display, TEXT("[ERA5] Using GeoReferencingSystem origin lat=%.6f lon=%.6f elev=%.2f m."), Config.Latitude, Config.Longitude, Config.ElevationM);
            }

            for (TActorIterator<ASnowSimulationActor> It(EditorWorld); It; ++It)
            {
                const FDateTime RunStart = It->GetRunStartTime();
                const FDateTime RunEnd = It->GetRunEndTime();
                if (RunEnd > RunStart)
                {
                    if (!bFoundRange)
                    {
                        RangeStart = RunStart;
                        RangeEnd = RunEnd;
                        bFoundRange = true;
                    }
                    else
                    {
                        RangeStart = FMath::Min(RangeStart, RunStart);
                        RangeEnd = FMath::Max(RangeEnd, RunEnd);
                    }
                }

                if (!bGeoRefOverride && FMath::IsFinite(It->Latitude) && FMath::IsFinite(It->Longitude))
                {
                    AccLatitude += It->Latitude;
                    AccLongitude += It->Longitude;
                    ++CoordinateSamples;
                }
            }
        }
    }

    if (!bGeoRefOverride)
    {
        if (CoordinateSamples > 0)
        {
            Config.Latitude = AccLatitude / static_cast<double>(CoordinateSamples);
            Config.Longitude = AccLongitude / static_cast<double>(CoordinateSamples);
            UE_LOG(LogTemp, Display, TEXT("[ERA5] Derived coordinates from %d simulation actor(s): lat=%.6f lon=%.6f"), CoordinateSamples, Config.Latitude, Config.Longitude);
        }
        else
        {
            UE_LOG(LogTemp, Verbose, TEXT("[ERA5] No simulation actors with coordinates found. Using settings defaults (lat=%.6f lon=%.6f)."), Config.Latitude, Config.Longitude);
        }
    }

    if (bFoundRange)
    {
        Config.bUseExplicitRange = true;
        Config.RangeStartUtc = RangeStart;
        Config.RangeEndUtc = RangeEnd;
        UE_LOG(LogTemp, Display, TEXT("[ERA5] Using simulation span %s -> %s"), *RangeStart.ToIso8601(), *RangeEnd.ToIso8601());
    }
    else
    {
        const int32 EffectiveHoursBack = FMath::Max(Settings->HoursBack, 1);
        Config.RangeEndUtc = FDateTime::UtcNow();
        Config.RangeStartUtc = Config.RangeEndUtc - FTimespan::FromHours(EffectiveHoursBack);
    }

    if (Config.HourlyVariables.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[ERA5] No hourly variables configured."));
        OnERA5PullFinished.Broadcast(false, 0);
        return;
    }

    bPullInProgress = true;

    TWeakObjectPtr<UERA5EditorFetcher> WeakThis(this);
    Async(EAsyncExecution::ThreadPool, [WeakThis, Config]()
    {
        const FERA5PullOutcome Outcome = PerformERA5Pull(Config);
        AsyncTask(ENamedThreads::GameThread, [WeakThis, Outcome]()
        {
            if (UERA5EditorFetcher* StrongPtr = WeakThis.Get())
            {
                StrongPtr->HandlePullFinished(Outcome.bSuccess, Outcome.SampleCount);
            }
        });
    });
}

void UERA5EditorFetcher::HandlePullFinished(bool bSuccess, int32 NumSamples)
{
    bPullInProgress = false;
    OnERA5PullFinished.Broadcast(bSuccess, NumSamples);
}
