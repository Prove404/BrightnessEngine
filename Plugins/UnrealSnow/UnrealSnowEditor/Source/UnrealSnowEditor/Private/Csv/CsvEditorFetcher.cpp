#if WITH_EDITOR

#include "Csv/CsvEditorFetcher.h"

#include "Async/Async.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Csv/CsvSettings.h"
#include "SnowSimulationActor.h"
#include "Util/WeatherCsvUtils.h"

#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace CsvEditorFetcherInternal
{
    static FString ResolvePathTokens(const FString& InPath)
    {
        FString Resolved = InPath;
        if (Resolved.IsEmpty()) return Resolved;
        const TPair<FString, FString> Tokens[] = {
            { TEXT("{ProjectDir}"), FPaths::ProjectDir() },
            { TEXT("{ProjectContentDir}"), FPaths::ProjectContentDir() },
            { TEXT("{ProjectSavedDir}"), FPaths::ProjectSavedDir() }
        };
        for (const TPair<FString,FString>& T : Tokens)
        {
            Resolved.ReplaceInline(*T.Key, *T.Value, ESearchCase::CaseSensitive);
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
}

using namespace CsvEditorFetcherInternal;

void UCsvEditorFetcher::PullCsvNow()
{
    if (bPullInProgress)
    {
        UE_LOG(LogTemp, Verbose, TEXT("[CSV] Pull already in progress."));
        return;
    }

    const UCsvSettings* Settings = GetDefault<UCsvSettings>();
    if (!Settings)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CSV] Settings not found."));
        OnCsvPullFinished.Broadcast(false, 0);
        return;
    }

    FString InputPath = ResolvePathTokens(Settings->InputCsvPath.FilePath);
    FString OutputPath = ResolvePathTokens(Settings->OutputJsonPath.FilePath);
    if (InputPath.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("[CSV] Input CSV path is not configured."));
        OnCsvPullFinished.Broadcast(false, 0);
        return;
    }
    if (OutputPath.IsEmpty())
    {
        OutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("analysis_results/csv_weather.json"));
    }

    FDateTime RangeStart = FDateTime::MaxValue();
    FDateTime RangeEnd = FDateTime::MinValue();
    bool bFoundRange = false;

    if (GEditor)
    {
        if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
        {
            for (TActorIterator<ASnowSimulationActor> It(EditorWorld); It; ++It)
            {
                const FDateTime RunStart = It->GetRunStartTime();
                const FDateTime RunEnd = It->GetRunEndTime();
                if (RunEnd > RunStart)
                {
                    if (!bFoundRange)
                    {
                        RangeStart = RunStart; RangeEnd = RunEnd; bFoundRange = true;
                    }
                    else
                    {
                        RangeStart = FMath::Min(RangeStart, RunStart);
                        RangeEnd = FMath::Max(RangeEnd, RunEnd);
                    }
                }
            }
        }
    }

    if (!bFoundRange)
    {
        // Fallback to 168h back if no actor present
        RangeEnd = FDateTime::UtcNow();
        RangeStart = RangeEnd - FTimespan::FromHours(168);
    }

    if (Settings->bTreatSimulationTimesAsLocal)
    {
        const FTimespan Offset = FTimespan::FromHours(Settings->SimulationUtcOffsetHours);
        RangeStart -= Offset;
        RangeEnd -= Offset;
    }

    bPullInProgress = true;

    TWeakObjectPtr<UCsvEditorFetcher> WeakThis(this);
    Async(EAsyncExecution::ThreadPool, [WeakThis, InputPath, OutputPath, RangeStart, RangeEnd]()
    {
        TArray<FWeatherForcingData> Records;
        int32 NumKept = 0;
        if (FPaths::FileExists(InputPath))
        {
            TArray<FWeatherForcingData> All;
            if (FWeatherCsvUtilities::ParseWeatherCsvFile(InputPath, All))
            {
                for (const FWeatherForcingData& R : All)
                {
                    if (R.Timestamp >= RangeStart && R.Timestamp < RangeEnd)
                    {
                        Records.Add(R);
                    }
                }
                NumKept = Records.Num();
            }
        }

        bool bSuccess = false;
        if (NumKept > 0)
        {
            TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
            TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
            Meta->SetStringField(TEXT("created_utc"), FDateTime::UtcNow().ToIso8601());
            Meta->SetStringField(TEXT("range_start_utc"), RangeStart.ToIso8601());
            Meta->SetStringField(TEXT("range_end_utc_exclusive"), RangeEnd.ToIso8601());
            Root->SetObjectField(TEXT("meta"), Meta);

            TArray<TSharedPtr<FJsonValue>> Series;
            Series.Reserve(Records.Num());
            for (const FWeatherForcingData& R : Records)
            {
                TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
                S->SetStringField(TEXT("time_utc"), R.Timestamp.ToIso8601());
                S->SetNumberField(TEXT("T_C"), R.Temperature_K - 273.15f);
                S->SetNumberField(TEXT("RH_pct"), R.RH_01 * 100.0f);
                S->SetNumberField(TEXT("Wind_mps"), R.Wind_mps);
                S->SetNumberField(TEXT("SWdown_Wm2"), R.SWdown_Wm2);
                S->SetNumberField(TEXT("LWdown_Wm2"), R.LWdown_Wm2);
                S->SetNumberField(TEXT("Precip_mmph"), R.PrecipRate_kgm2s * 3600.0f);
                S->SetNumberField(TEXT("SnowFrac"), R.SnowFrac_01);
                Series.Add(MakeShared<FJsonValueObject>(S));
            }
            Root->SetArrayField(TEXT("series"), Series);

            FString JsonText;
            TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonText);
            FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

            EnsureOutputDirectory(OutputPath);
            bSuccess = FFileHelper::SaveStringToFile(JsonText, *OutputPath);
            if (bSuccess)
            {
                UE_LOG(LogTemp, Display, TEXT("[CSV] Wrote %d samples to %s"), Records.Num(), *OutputPath);
            }
        }

        AsyncTask(ENamedThreads::GameThread, [WeakThis, bSuccess, NumKept]()
        {
            if (UCsvEditorFetcher* Strong = WeakThis.Get())
            {
                Strong->HandlePullFinished(bSuccess, NumKept);
            }
        });
    });
}

void UCsvEditorFetcher::HandlePullFinished(bool bSuccess, int32 NumSamples)
{
    bPullInProgress = false;
    OnCsvPullFinished.Broadcast(bSuccess, NumSamples);
}

#endif // WITH_EDITOR


