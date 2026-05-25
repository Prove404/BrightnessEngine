#if WITH_EDITOR

#include "Frost/FrostEditorFetcher.h"

#include "Async/Async.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Frost/FrostSettings.h"
#include "SnowSimulationActor.h"
#include "Dom/JsonObject.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "PlatformHttp.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace FrostEditorFetcherInternal
{
    constexpr int32 GFrostMaxSplitDepth = 5;

    enum class EFrostElementKind : uint8
    {
        NumericAverage,
        SnowFlag
    };

    struct FFrostPullConfig
    {
        FString ClientId;
        FString ClientSecret;
        FString Sources;
        FString OutputPath;
        int32 HoursBack = 0;
        bool bUseExplicitRange = false;
        FDateTime RangeStartUtc;
        FDateTime RangeEndUtc;
        FString AirTemperatureElementId;
        FString RelativeHumidityElementId;
        FString WindSpeedElementId;
        FString WindDirectionElementId;
        FString PrecipitationAmountElementId;
        FString PrecipitationTypeElementId;
        FString ShortwaveFluxElementId;
        FString DiffuseShortwaveFluxElementId;
        FString LongwaveFluxElementId;
        FString SurfacePressureElementId;
    };

    struct FFrostPullOutcome
    {
        bool bSuccess = false;
        int32 SampleCount = 0;
    };

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

    static void EnsureOutputDirectory(const FString& FilePath)
    {
        const FString Directory = FPaths::GetPath(FilePath);
        if (!Directory.IsEmpty())
        {
            IFileManager::Get().MakeDirectory(*Directory, true);
        }
    }

    struct FFrostAccum
    {
        FDateTime TimeUTC;
        double TempSum = 0.0; int32 TempCount = 0;
        double RHSum = 0.0; int32 RHCount = 0;
        double WindSum = 0.0; int32 WindCount = 0;
        double WindDirSum = 0.0; int32 WindDirCount = 0;
        double PrecipSum = 0.0; int32 PrecipCount = 0;
        double SWSum = 0.0; int32 SWCount = 0;
        double DiffuseSWSum = 0.0; int32 DiffuseSWCount = 0;
        double LWSum = 0.0; int32 LWCount = 0;
        double PressureSum = 0.0; int32 PressureCount = 0;
        bool bSnowObserved = false;
    };

    void AccumulateField(double Value, double& Sum, int32& Count)
    {
        if (FMath::IsFinite(Value))
        {
            Sum += Value;
            ++Count;
        }
    }

    TSharedPtr<FJsonValue> MakeNumericOrNull(double Sum, int32 Count)
    {
        if (Count > 0)
        {
            return MakeShared<FJsonValueNumber>(Sum / Count);
        }
        return MakeShared<FJsonValueNull>();
    }

    struct FFrostErrorInfo
    {
        FString Message;
        FString Reason;
    };

    FFrostErrorInfo ParseErrorInfo(const FString& JsonString)
    {
        FFrostErrorInfo ErrorInfo;
        TSharedPtr<FJsonObject> RootObject;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
        if (FJsonSerializer::Deserialize(Reader, RootObject) && RootObject.IsValid())
        {
            const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
            if (RootObject->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject && ErrorObject->IsValid())
            {
                (*ErrorObject)->TryGetStringField(TEXT("message"), ErrorInfo.Message);
                (*ErrorObject)->TryGetStringField(TEXT("reason"), ErrorInfo.Reason);
            }
        }
        return ErrorInfo;
    }

    struct FFrostElementSelection
    {
        FString ElementId;
        FString ElementIdLower;
        EFrostElementKind Kind = EFrostElementKind::NumericAverage;
        FName OutputFieldName;
        double FFrostAccum::* SumPtr = nullptr;
        int32 FFrostAccum::* CountPtr = nullptr;
        int32 ObservationCount = 0;
        bool bSnowObservation = false;
    };

    void AccumulateDataArray(const TArray<TSharedPtr<FJsonValue>>& DataArray,
        const TMap<FString, int32>& ElementLookup,
        TArray<FFrostElementSelection>& Selections,
        TMap<FDateTime, FFrostAccum>& Aggregated)
    {
        for (const TSharedPtr<FJsonValue>& DataValue : DataArray)
        {
            const TSharedPtr<FJsonObject>* DataObjPtr = nullptr;
            if (!DataValue.IsValid() || !DataValue->TryGetObject(DataObjPtr))
            {
                continue;
            }

            const TSharedPtr<FJsonObject>& DataObj = *DataObjPtr;
            FString ReferenceTimeStr;
            if (!DataObj->TryGetStringField(TEXT("referenceTime"), ReferenceTimeStr))
            {
                continue;
            }

            FDateTime ReferenceTime;
            if (!FDateTime::ParseIso8601(*ReferenceTimeStr, ReferenceTime))
            {
                continue;
            }

            FFrostAccum& Accum = Aggregated.FindOrAdd(ReferenceTime);
            Accum.TimeUTC = ReferenceTime;

            const TArray<TSharedPtr<FJsonValue>>* Observations = nullptr;
            if (!DataObj->TryGetArrayField(TEXT("observations"), Observations))
            {
                continue;
            }

            for (const TSharedPtr<FJsonValue>& ObservationValue : *Observations)
            {
                const TSharedPtr<FJsonObject>* ObservationObjPtr = nullptr;
                if (!ObservationValue.IsValid() || !ObservationValue->TryGetObject(ObservationObjPtr))
                {
                    continue;
                }

                const TSharedPtr<FJsonObject>& ObsObj = *ObservationObjPtr;
                FString ElementId;
                if (!ObsObj->TryGetStringField(TEXT("elementId"), ElementId))
                {
                    continue;
                }

                FString ElementIdLower = ElementId;
                ElementIdLower.ToLowerInline();

                const int32* SelectionIndex = ElementLookup.Find(ElementIdLower);
                if (!SelectionIndex)
                {
                    continue;
                }

                FFrostElementSelection& Selection = Selections[*SelectionIndex];

                if (Selection.Kind == EFrostElementKind::NumericAverage)
                {
                    double NumericValue = 0.0;
                    if (!ObsObj->TryGetNumberField(TEXT("value"), NumericValue))
                    {
                        continue;
                    }

                    double& SumRef = Accum.*(Selection.SumPtr);
                    int32& CountRef = Accum.*(Selection.CountPtr);
                    AccumulateField(NumericValue, SumRef, CountRef);
                    Selection.ObservationCount++;
                }
                else if (Selection.Kind == EFrostElementKind::SnowFlag)
                {
                    FString TypeStr;
                    if (ObsObj->TryGetStringField(TEXT("value"), TypeStr))
                    {
                        if (TypeStr.Contains(TEXT("snow"), ESearchCase::IgnoreCase))
                        {
                            Accum.bSnowObserved = true;
                            Selection.bSnowObservation = true;
                        }
                    }
                    else
                    {
                        double NumericType = 0.0;
                        if (ObsObj->TryGetNumberField(TEXT("value"), NumericType))
                        {
                            if (FMath::IsNearlyEqual(NumericType, 15.0) || FMath::IsNearlyEqual(NumericType, 16.0) || FMath::IsNearlyEqual(NumericType, 17.0) || FMath::IsNearlyEqual(NumericType, 18.0))
                            {
                                Accum.bSnowObserved = true;
                                Selection.bSnowObservation = true;
                            }
                        }
                    }
                }
            }
        }
    }

    struct FFrostRawResponse
    {
        bool bHasResponse = false;
        bool bTransferSucceeded = false;
        int32 StatusCode = 0;
        FString Body;
    };

    FFrostRawResponse ExecuteFrostRequest(const FString& ClientId, const FString& ClientSecret, const FString& Url)
    {
        FFrostRawResponse Result;

        FHttpModule& Http = FHttpModule::Get();
        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
        Request->SetURL(Url);
        Request->SetVerb(TEXT("GET"));
        Request->SetHeader(TEXT("Accept"), TEXT("application/json"));

        FString AuthPayload = ClientId;
        AuthPayload += TEXT(":");
        if (!ClientSecret.IsEmpty() && !ClientSecret.Equals(TEXT("YOUR_CLIENT_SECRET_HERE"), ESearchCase::IgnoreCase))
        {
            AuthPayload += ClientSecret;
        }
        Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Basic %s"), *FBase64::Encode(AuthPayload)));

        FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
        Request->OnProcessRequestComplete().BindLambda(
            [&Result, CompletionEvent](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
            {
                Result.bTransferSucceeded = bSucceeded;
                Result.bHasResponse = Response.IsValid();
                if (Result.bHasResponse)
                {
                    Result.StatusCode = Response->GetResponseCode();
                    Result.Body = Response->GetContentAsString();
                }
                CompletionEvent->Trigger();
            });

        if (!Request->ProcessRequest())
        {
            Result.bTransferSucceeded = false;
            Result.bHasResponse = false;
            FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
            return Result;
        }

        CompletionEvent->Wait();
        FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
        return Result;
    }

    bool ShouldSplitRequest(const FFrostErrorInfo& ErrorInfo)
    {
        if (ErrorInfo.Reason.Equals(TEXT("TooManyObservations"), ESearchCase::IgnoreCase))
        {
            return true;
        }

        return ErrorInfo.Message.Contains(TEXT("too many observations"), ESearchCase::IgnoreCase);
    }

    bool FetchFrostRange(const FString& ClientId,
        const FString& ClientSecret,
        const FString& Sources,
        const FString& Elements,
        const TMap<FString, int32>& ElementLookup,
        TArray<FFrostElementSelection>& Selections,
        const FDateTime& RangeStart,
        const FDateTime& RangeEnd,
        int32 RemainingSplitDepth,
        TMap<FDateTime, FFrostAccum>& Aggregated,
        int32& OutRequestCount)
    {
        if (RangeEnd <= RangeStart)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost] Invalid range %s -> %s"), *RangeStart.ToIso8601(), *RangeEnd.ToIso8601());
            return false;
        }

        const FString ReferenceTime = FString::Printf(TEXT("%s/%s"), *RangeStart.ToIso8601(), *RangeEnd.ToIso8601());
        FString Url = TEXT("https://frost.met.no/observations/v0.jsonld");
        Url += TEXT("?sources=") + FPlatformHttp::UrlEncode(Sources);
        Url += TEXT("&elements=") + FPlatformHttp::UrlEncode(Elements);
        Url += TEXT("&referencetime=") + FPlatformHttp::UrlEncode(ReferenceTime);
        Url += TEXT("&timeoffsets=PT0H&timeresolutions=PT1H");

        ++OutRequestCount;
        const FFrostRawResponse Response = ExecuteFrostRequest(ClientId, ClientSecret, Url);
        if (!Response.bHasResponse)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost] HTTP request failed for %s"), *ReferenceTime);
            return false;
        }

        if (!Response.bTransferSucceeded)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost] HTTP transfer error for %s (code %d)"), *ReferenceTime, Response.StatusCode);
        }

        if (Response.StatusCode == EHttpResponseCodes::Ok)
        {
            TSharedPtr<FJsonObject> RootObject;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response.Body);
            if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
            {
                UE_LOG(LogTemp, Warning, TEXT("[Frost] Failed to parse JSON response for %s"), *ReferenceTime);
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* DataArray = nullptr;
            if (RootObject->TryGetArrayField(TEXT("data"), DataArray) && DataArray)
            {
                AccumulateDataArray(*DataArray, ElementLookup, Selections, Aggregated);
            }
            return true;
        }

        if (Response.StatusCode == EHttpResponseCodes::NotFound)
        {
            UE_LOG(LogTemp, Display, TEXT("[Frost] No observations returned for %s"), *ReferenceTime);
            return true;
        }

        const FFrostErrorInfo ErrorInfo = ParseErrorInfo(Response.Body);

        if (Response.StatusCode == EHttpResponseCodes::Forbidden)
        {
            const FTimespan RangeSpan = RangeEnd - RangeStart;
            const bool bCanSplit = RemainingSplitDepth > 0 && RangeSpan.GetTicks() > 0;
            if (bCanSplit && ShouldSplitRequest(ErrorInfo))
            {
                const FTimespan HalfSpan(RangeSpan.GetTicks() / 2);
                const FDateTime MidPoint = RangeStart + HalfSpan;
                if (MidPoint > RangeStart && MidPoint < RangeEnd)
                {
                    UE_LOG(LogTemp, Display, TEXT("[Frost] Splitting request %s due to API limits (reason: %s, message: %s)"),
                        *ReferenceTime, *ErrorInfo.Reason, *ErrorInfo.Message);
                    return FetchFrostRange(ClientId, ClientSecret, Sources, Elements, ElementLookup, Selections, RangeStart, MidPoint, RemainingSplitDepth - 1, Aggregated, OutRequestCount)
                        && FetchFrostRange(ClientId, ClientSecret, Sources, Elements, ElementLookup, Selections, MidPoint, RangeEnd, RemainingSplitDepth - 1, Aggregated, OutRequestCount);
                }
            }

            UE_LOG(LogTemp, Warning, TEXT("[Frost] Request forbidden for %s (reason: %s, message: %s)"),
                *ReferenceTime, *ErrorInfo.Reason, *ErrorInfo.Message);
            return false;
        }

        UE_LOG(LogTemp, Warning, TEXT("[Frost] HTTP error %d for %s. %s (%s)"),
            Response.StatusCode, *ReferenceTime, *ErrorInfo.Message, *ErrorInfo.Reason);
        return false;
    }

    FFrostPullOutcome PerformFrostPull(const FFrostPullConfig& Config)
    {
        FFrostPullOutcome Outcome;

        const int32 EffectiveHoursBack = Config.HoursBack > 0 ? Config.HoursBack : 24;
        const bool bHasExplicitRange = Config.bUseExplicitRange && (Config.RangeEndUtc > Config.RangeStartUtc);

        FDateTime RangeEndUtc = bHasExplicitRange ? Config.RangeEndUtc : FDateTime::UtcNow();
        FDateTime RangeStartUtc = bHasExplicitRange ? Config.RangeStartUtc : RangeEndUtc - FTimespan::FromHours(EffectiveHoursBack);

        if (RangeEndUtc <= RangeStartUtc)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost] Invalid reference range %s -> %s. Falling back to HoursBack."),
                *RangeStartUtc.ToIso8601(), *RangeEndUtc.ToIso8601());
            RangeEndUtc = FDateTime::UtcNow();
            RangeStartUtc = RangeEndUtc - FTimespan::FromHours(EffectiveHoursBack);
        }

        if (bHasExplicitRange)
        {
            RangeEndUtc += FTimespan::FromHours(1); // treat as end-exclusive
        }

        TArray<FFrostElementSelection> Selections;
        Selections.Reserve(10);

        auto SanitizeElementId = [](FString ElementId)
        {
            ElementId.TrimStartAndEndInline();
            return ElementId;
        };

        auto AddNumericSelection = [&Selections, &SanitizeElementId](const FString& ElementId, const TCHAR* FieldName, double FFrostAccum::* SumPtr, int32 FFrostAccum::* CountPtr)
        {
            FString CleanId = SanitizeElementId(ElementId);
            if (CleanId.IsEmpty())
            {
                return;
            }

            FFrostElementSelection Selection;
            Selection.ElementId = MoveTemp(CleanId);
            Selection.ElementIdLower = Selection.ElementId;
            Selection.ElementIdLower.ToLowerInline();
            Selection.Kind = EFrostElementKind::NumericAverage;
            Selection.OutputFieldName = FieldName;
            Selection.SumPtr = SumPtr;
            Selection.CountPtr = CountPtr;
            Selections.Add(MoveTemp(Selection));
        };

        auto AddSnowSelection = [&Selections, &SanitizeElementId](const FString& ElementId)
        {
            FString CleanId = SanitizeElementId(ElementId);
            if (CleanId.IsEmpty())
            {
                return;
            }

            FFrostElementSelection Selection;
            Selection.ElementId = MoveTemp(CleanId);
            Selection.ElementIdLower = Selection.ElementId;
            Selection.ElementIdLower.ToLowerInline();
            Selection.Kind = EFrostElementKind::SnowFlag;
            Selection.OutputFieldName = TEXT("SnowFrac");
            Selections.Add(MoveTemp(Selection));
        };

        AddNumericSelection(Config.AirTemperatureElementId, TEXT("T_C"), &FFrostAccum::TempSum, &FFrostAccum::TempCount);
        AddNumericSelection(Config.RelativeHumidityElementId, TEXT("RH_pct"), &FFrostAccum::RHSum, &FFrostAccum::RHCount);
        AddNumericSelection(Config.WindSpeedElementId, TEXT("Wind_mps"), &FFrostAccum::WindSum, &FFrostAccum::WindCount);
        AddNumericSelection(Config.WindDirectionElementId, TEXT("WindDir_deg"), &FFrostAccum::WindDirSum, &FFrostAccum::WindDirCount);
        AddNumericSelection(Config.PrecipitationAmountElementId, TEXT("Precip_mmph"), &FFrostAccum::PrecipSum, &FFrostAccum::PrecipCount);
        AddNumericSelection(Config.ShortwaveFluxElementId, TEXT("SWdown_Wm2"), &FFrostAccum::SWSum, &FFrostAccum::SWCount);
        AddNumericSelection(Config.DiffuseShortwaveFluxElementId, TEXT("DiffuseSWdown_Wm2"), &FFrostAccum::DiffuseSWSum, &FFrostAccum::DiffuseSWCount);
        AddNumericSelection(Config.LongwaveFluxElementId, TEXT("LWdown_Wm2"), &FFrostAccum::LWSum, &FFrostAccum::LWCount);
        AddNumericSelection(Config.SurfacePressureElementId, TEXT("Pressure_hPa"), &FFrostAccum::PressureSum, &FFrostAccum::PressureCount);
        AddSnowSelection(Config.PrecipitationTypeElementId);

        if (Selections.Num() == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost] No element identifiers configured. Aborting pull."));
            return Outcome;
        }

        TMap<FString, int32> ElementLookup;
        ElementLookup.Reserve(Selections.Num());

        TSet<FString> UniqueElementIds;
        TArray<FString> ElementList;
        ElementList.Reserve(Selections.Num());

        for (int32 Index = 0; Index < Selections.Num(); ++Index)
        {
            FFrostElementSelection& Selection = Selections[Index];
            if (Selection.ElementIdLower.IsEmpty())
            {
                continue;
            }

            if (!ElementLookup.Contains(Selection.ElementIdLower))
            {
                ElementLookup.Add(Selection.ElementIdLower, Index);
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("[Frost] Duplicate element id '%s' configured; ignoring later occurrence."), *Selection.ElementId);
                continue;
            }

            if (!UniqueElementIds.Contains(Selection.ElementId))
            {
                UniqueElementIds.Add(Selection.ElementId);
                ElementList.Add(Selection.ElementId);
            }
        }

        if (ElementList.Num() == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost] No valid element identifiers after deduplication."));
            return Outcome;
        }

        const FString ElementsCsv = FString::Join(ElementList, TEXT(","));
        UE_LOG(LogTemp, Display, TEXT("[Frost] Using request window %s -> %s (end exclusive)"),
            *RangeStartUtc.ToIso8601(), *RangeEndUtc.ToIso8601());
        UE_LOG(LogTemp, Display, TEXT("[Frost] Requesting elements: %s"), *ElementsCsv);

        TMap<FDateTime, FFrostAccum> Aggregated;
        int32 RequestCount = 0;
        if (!FetchFrostRange(Config.ClientId, Config.ClientSecret, Config.Sources, ElementsCsv, ElementLookup, Selections, RangeStartUtc, RangeEndUtc, GFrostMaxSplitDepth, Aggregated, RequestCount))
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost] Failed to retrieve observations."));
            return Outcome;
        }

        UE_LOG(LogTemp, Display, TEXT("[Frost] Completed Frost pull with %d request(s)."), RequestCount);

        for (const FFrostElementSelection& Selection : Selections)
        {
            if (Selection.Kind == EFrostElementKind::NumericAverage && Selection.ObservationCount == 0)
            {
                UE_LOG(LogTemp, Warning, TEXT("[Frost] No observations returned for element '%s' (field %s)."), *Selection.ElementId, *Selection.OutputFieldName.ToString());
            }
            else if (Selection.Kind == EFrostElementKind::SnowFlag && !Selection.bSnowObservation)
            {
                UE_LOG(LogTemp, Warning, TEXT("[Frost] No snow detections found using element '%s'."), *Selection.ElementId);
            }
        }

        Aggregated.KeySort([](const FDateTime& A, const FDateTime& B)
        {
            return A < B;
        });

        TSharedPtr<FJsonObject> OutputRoot = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
        Meta->SetStringField(TEXT("created_utc"), FDateTime::UtcNow().ToIso8601());
        Meta->SetStringField(TEXT("sources"), Config.Sources);
        Meta->SetStringField(TEXT("range_start_utc"), RangeStartUtc.ToIso8601());
        Meta->SetStringField(TEXT("range_end_utc_exclusive"), RangeEndUtc.ToIso8601());
        OutputRoot->SetObjectField(TEXT("meta"), Meta);

        TArray<TSharedPtr<FJsonValue>> SeriesArray;
        SeriesArray.Reserve(Aggregated.Num());

        for (const TPair<FDateTime, FFrostAccum>& Pair : Aggregated)
        {
            const FFrostAccum& Accum = Pair.Value;
            TSharedPtr<FJsonObject> SampleObj = MakeShared<FJsonObject>();
            SampleObj->SetStringField(TEXT("time_utc"), Accum.TimeUTC.ToIso8601());

            for (const FFrostElementSelection& Selection : Selections)
            {
                if (Selection.Kind != EFrostElementKind::NumericAverage)
                {
                    continue;
                }

                const double Sum = Accum.*(Selection.SumPtr);
                const int32 Count = Accum.*(Selection.CountPtr);
                SampleObj->SetField(Selection.OutputFieldName.ToString(), MakeNumericOrNull(Sum, Count));
            }

            if (Accum.bSnowObserved)
            {
                SampleObj->SetNumberField(TEXT("SnowFrac"), 1.0);
            }

            SeriesArray.Add(MakeShared<FJsonValueObject>(SampleObj));
        }

        OutputRoot->SetArrayField(TEXT("series"), SeriesArray);

        FString OutputString;
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
        FJsonSerializer::Serialize(OutputRoot.ToSharedRef(), Writer);

        FString OutputPath = ResolveFrostPath(Config.OutputPath);
        if (OutputPath.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost] Output path is not configured."));
            return Outcome;
        }

        EnsureOutputDirectory(OutputPath);

        if (FFileHelper::SaveStringToFile(OutputString, *OutputPath))
        {
            UE_LOG(LogTemp, Display, TEXT("[Frost] Wrote %d samples to %s"), SeriesArray.Num(), *OutputPath);
            Outcome.bSuccess = true;
            Outcome.SampleCount = SeriesArray.Num();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[Frost] Failed to write %s"), *OutputPath);
        }

        return Outcome;
    }
}

using namespace FrostEditorFetcherInternal;

void UFrostEditorFetcher::PullFrostNow()
{
    if (bPullInProgress)
    {
        UE_LOG(LogTemp, Verbose, TEXT("[Frost] Pull already in progress."));
        return;
    }

    bPullInProgress = true;

    const UFrostSettings* Settings = GetDefault<UFrostSettings>();
    if (!Settings)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Frost] Settings not found."));
        bPullInProgress = false;
        OnFrostPullFinished.Broadcast(false, 0);
        return;
    }

    FFrostPullConfig Config;
    Config.ClientId = Settings->ClientId;
    Config.ClientSecret = Settings->ClientSecret;
    Config.Sources = Settings->Sources.Replace(TEXT(" "), TEXT(""));
    Config.OutputPath = Settings->OutputJsonPath.FilePath;
    Config.HoursBack = Settings->HoursBack;
    Config.AirTemperatureElementId = Settings->AirTemperatureElementId;
    Config.RelativeHumidityElementId = Settings->RelativeHumidityElementId;
    Config.WindSpeedElementId = Settings->WindSpeedElementId;
    Config.WindDirectionElementId = Settings->WindDirectionElementId;
    Config.PrecipitationAmountElementId = Settings->PrecipitationAmountElementId;
    Config.PrecipitationTypeElementId = Settings->PrecipitationTypeElementId;
    Config.ShortwaveFluxElementId = Settings->ShortwaveFluxElementId;
    Config.DiffuseShortwaveFluxElementId = Settings->DiffuseShortwaveFluxElementId;
    Config.LongwaveFluxElementId = Settings->LongwaveFluxElementId;
    Config.SurfacePressureElementId = Settings->SurfacePressureElementId;

    if (Config.ClientId.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("[Frost] ClientId is empty. Configure Project Settings -> Frost (Weather)."));
        bPullInProgress = false;
        OnFrostPullFinished.Broadcast(false, 0);
        return;
    }

    if (Config.Sources.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("[Frost] Sources are empty."));
        bPullInProgress = false;
        OnFrostPullFinished.Broadcast(false, 0);
        return;
    }

    if (GEditor)
    {
        UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
        if (EditorWorld)
        {
            FDateTime RangeStart = FDateTime::MaxValue();
            FDateTime RangeEnd = FDateTime::MinValue();
            bool bFoundRange = false;

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
                        if (RunStart < RangeStart)
                        {
                            RangeStart = RunStart;
                        }
                        if (RunEnd > RangeEnd)
                        {
                            RangeEnd = RunEnd;
                        }
                    }
                }
            }

            if (bFoundRange)
            {
                const UFrostSettings* FrostSettings = GetDefault<UFrostSettings>();
                FDateTime EffectiveStart = RangeStart;
                FDateTime EffectiveEnd = RangeEnd;
                if (FrostSettings && FrostSettings->bTreatSimulationTimesAsLocal)
                {
                    // Convert local simulation times to UTC using a fixed offset
                    const FTimespan Offset = FTimespan::FromHours(FrostSettings->SimulationUtcOffsetHours);
                    EffectiveStart -= Offset;
                    EffectiveEnd -= Offset;
                }

                Config.bUseExplicitRange = true;
                Config.RangeStartUtc = EffectiveStart;
                Config.RangeEndUtc = EffectiveEnd;
                UE_LOG(LogTemp, Display, TEXT("[Frost] Using simulation span %s -> %s (treated as %s -> %s UTC)"),
                    *RangeStart.ToString(), *RangeEnd.ToString(), *EffectiveStart.ToIso8601(), *EffectiveEnd.ToIso8601());
            }
        }
    }

    TWeakObjectPtr<UFrostEditorFetcher> WeakThis(this);
    Async(EAsyncExecution::ThreadPool, [WeakThis, Config]()
    {
        const FFrostPullOutcome Outcome = PerformFrostPull(Config);

        AsyncTask(ENamedThreads::GameThread, [WeakThis, Outcome]()
        {
            if (UFrostEditorFetcher* StrongPtr = WeakThis.Get())
            {
                StrongPtr->HandlePullFinished(Outcome.bSuccess, Outcome.SampleCount);
            }
        });
    });
}

void UFrostEditorFetcher::HandlePullFinished(bool bSuccess, int32 NumSamples)
{
    bPullInProgress = false;
    OnFrostPullFinished.Broadcast(bSuccess, NumSamples);
}

#endif // WITH_EDITOR
