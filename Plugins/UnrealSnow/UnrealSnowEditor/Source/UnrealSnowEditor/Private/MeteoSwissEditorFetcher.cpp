#if WITH_EDITOR

#include "MeteoSwiss/MeteoSwissEditorFetcher.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "MeteoSwiss/MeteoSwissSettings.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SnowSimulationActor.h"

#include <limits>

namespace MeteoSwissEditorFetcherInternal
{
	double MakeNaN()
	{
		return std::numeric_limits<double>::quiet_NaN();
	}

	struct FMeteoSwissPullConfig
	{
		FString StacItemsBaseUrl;
		FString StationIdLower;
		FString StationIdUpper;
		FString OutputPath;
		int32 HoursBack = 0;
		bool bUseExplicitRange = false;
		FDateTime RangeStartUtc = FDateTime::MinValue();
		FDateTime RangeEndUtc = FDateTime::MinValue();
		float ElevationM = 0.0f;
		FMeteoSwissVariableSource TemperatureSource;
		FMeteoSwissVariableSource RelativeHumiditySource;
		FMeteoSwissVariableSource WindSpeedSource;
		FMeteoSwissVariableSource WindDirectionSource;
		FMeteoSwissVariableSource PrecipitationSource;
		FMeteoSwissVariableSource ShortwaveSource;
		FMeteoSwissVariableSource DiffuseShortwaveSource;
		FMeteoSwissVariableSource LongwaveSource;
		FMeteoSwissVariableSource PressureSource;
	};

	struct FMeteoSwissPullOutcome
	{
		bool bSuccess = false;
		int32 SampleCount = 0;
	};

	struct FMeteoSwissStacItem
	{
		FString StationIdLower;
		FString StationName;
		double Latitude = 0.0;
		double Longitude = 0.0;
		TMap<FString, FString> AssetUrls;
	};

	struct FMeteoSwissRow
	{
		FDateTime TimeUtc;
		double TemperatureC = MakeNaN();
		double RelativeHumidityPct = MakeNaN();
		double WindSpeedMps = MakeNaN();
		double WindDirectionDeg = MakeNaN();
		double PrecipitationMm = MakeNaN();
		double ShortwaveWm2 = MakeNaN();
		double DiffuseShortwaveWm2 = MakeNaN();
		double LongwaveWm2 = MakeNaN();
		double PressureHpa = MakeNaN();
	};

	struct FMeteoSwissStationDataset
	{
		FMeteoSwissStacItem StacItem;
		TArray<FString> UsedAssetUrls;
		TMap<FDateTime, FMeteoSwissRow> RowsByTime;
	};

	struct FMeteoSwissVariablePullConfig
	{
		bool bInclude = true;
		FString StationIdLower;
		FString StationIdUpper;
	};

	FString ResolveMeteoSwissPath(const FString& InPath)
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

	void EnsureOutputDirectory(const FString& FilePath)
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
		Request->SetHeader(TEXT("Accept"), TEXT("application/json,text/csv;q=0.9,*/*;q=0.1"));

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

	FString NormalizeConfiguredStationId(const FString& PrimaryStationId, const FString& OverrideStationId, bool bUppercase)
	{
		FString Value = OverrideStationId;
		Value.TrimStartAndEndInline();
		if (Value.IsEmpty())
		{
			Value = PrimaryStationId;
		}

		if (bUppercase)
		{
			Value.ToUpperInline();
		}
		else
		{
			Value.ToLowerInline();
		}

		return Value;
	}

	FMeteoSwissVariablePullConfig BuildVariableConfig(const FString& PrimaryStationIdUpper, const FMeteoSwissVariableSource& Source)
	{
		FMeteoSwissVariablePullConfig Config;
		Config.bInclude = Source.bInclude;
		Config.StationIdUpper = NormalizeConfiguredStationId(PrimaryStationIdUpper, Source.StationIdOverride, true);
		Config.StationIdLower = NormalizeConfiguredStationId(PrimaryStationIdUpper, Source.StationIdOverride, false);
		return Config;
	}

	void AddRequiredStation(const FMeteoSwissVariablePullConfig& VariableConfig, TSet<FString>& OutStations)
	{
		if (VariableConfig.bInclude && !VariableConfig.StationIdLower.IsEmpty())
		{
			OutStations.Add(VariableConfig.StationIdLower);
		}
	}

	bool ParseSwissTimestamp(const FString& Timestamp, FDateTime& OutTimeUtc)
	{
		TArray<FString> Parts;
		Timestamp.ParseIntoArray(Parts, TEXT(" "), true);
		if (Parts.Num() != 2)
		{
			return false;
		}

		TArray<FString> DateParts;
		TArray<FString> TimeParts;
		Parts[0].ParseIntoArray(DateParts, TEXT("."), true);
		Parts[1].ParseIntoArray(TimeParts, TEXT(":"), true);
		if (DateParts.Num() != 3 || TimeParts.Num() != 2)
		{
			return false;
		}

		int32 Day = 0;
		int32 Month = 0;
		int32 Year = 0;
		int32 Hour = 0;
		int32 Minute = 0;
		if (!LexTryParseString(Day, *DateParts[0]) ||
			!LexTryParseString(Month, *DateParts[1]) ||
			!LexTryParseString(Year, *DateParts[2]) ||
			!LexTryParseString(Hour, *TimeParts[0]) ||
			!LexTryParseString(Minute, *TimeParts[1]))
		{
			return false;
		}

		OutTimeUtc = FDateTime(Year, Month, Day, Hour, Minute, 0);
		return true;
	}

	bool TryParseOptionalDouble(const FString& Text, double& OutValue)
	{
		FString Clean = Text;
		Clean.ReplaceInline(TEXT("\r"), TEXT(""));
		Clean.TrimStartAndEndInline();
		if (Clean.IsEmpty())
		{
			return false;
		}

		return LexTryParseString(OutValue, *Clean);
	}

	bool ParseStacItem(const FString& JsonPayload, FMeteoSwissStacItem& OutItem)
	{
		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] Failed to parse STAC item response."));
			return false;
		}

		RootObject->TryGetStringField(TEXT("id"), OutItem.StationIdLower);

		const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
		if (RootObject->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj && PropertiesObj->IsValid())
		{
			(*PropertiesObj)->TryGetStringField(TEXT("title"), OutItem.StationName);
		}

		const TSharedPtr<FJsonObject>* GeometryObj = nullptr;
		if (RootObject->TryGetObjectField(TEXT("geometry"), GeometryObj) && GeometryObj && GeometryObj->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Coordinates = nullptr;
			if ((*GeometryObj)->TryGetArrayField(TEXT("coordinates"), Coordinates) && Coordinates && Coordinates->Num() >= 2)
			{
				OutItem.Longitude = (*Coordinates)[0]->AsNumber();
				OutItem.Latitude = (*Coordinates)[1]->AsNumber();
			}
		}

		const TSharedPtr<FJsonObject>* AssetsObj = nullptr;
		if (RootObject->TryGetObjectField(TEXT("assets"), AssetsObj) && AssetsObj && AssetsObj->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*AssetsObj)->Values)
			{
				const TSharedPtr<FJsonObject>* AssetObject = nullptr;
				if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(AssetObject) || !AssetObject || !AssetObject->IsValid())
				{
					continue;
				}

				FString Href;
				if ((*AssetObject)->TryGetStringField(TEXT("href"), Href) && !Href.IsEmpty())
				{
					OutItem.AssetUrls.Add(Pair.Key, Href);
				}
			}
		}

		return !OutItem.AssetUrls.IsEmpty();
	}

	void BuildCandidateAssetKeys(const FMeteoSwissPullConfig& Config, const FDateTime& RangeStartUtc, const FDateTime& RangeEndUtc, TArray<FString>& OutKeys)
	{
		OutKeys.Reset();
		if (RangeEndUtc <= RangeStartUtc)
		{
			return;
		}

		const FDateTime InclusiveEnd = FMath::Max(RangeStartUtc, RangeEndUtc - FTimespan::FromMinutes(1));
		const int32 StartDecade = (RangeStartUtc.GetYear() / 10) * 10;
		const int32 EndDecade = (InclusiveEnd.GetYear() / 10) * 10;

		for (int32 Decade = StartDecade; Decade <= EndDecade; Decade += 10)
		{
			OutKeys.Add(FString::Printf(TEXT("ogd-smn_%s_h_historical_%d-%d.csv"), *Config.StationIdLower, Decade, Decade + 9));
		}

		const int32 CurrentDecade = (FDateTime::UtcNow().GetYear() / 10) * 10;
		if (EndDecade >= CurrentDecade)
		{
			OutKeys.Add(FString::Printf(TEXT("ogd-smn_%s_h_recent.csv"), *Config.StationIdLower));
			OutKeys.Add(FString::Printf(TEXT("ogd-smn_%s_h_now.csv"), *Config.StationIdLower));
		}
	}

	void ParseCsvLine(const FString& Line, TArray<FString>& OutColumns)
	{
		OutColumns.Reset();
		Line.ParseIntoArray(OutColumns, TEXT(";"), false);
		for (FString& Column : OutColumns)
		{
			Column.ReplaceInline(TEXT("\r"), TEXT(""));
		}
	}

	bool ParseCsvPayload(const FString& CsvPayload, const FMeteoSwissPullConfig& Config, const FDateTime& RangeStartUtc, const FDateTime& RangeEndUtc, TArray<FMeteoSwissRow>& OutRows)
	{
		TArray<FString> Lines;
		CsvPayload.ParseIntoArrayLines(Lines, false);
		if (Lines.Num() < 2)
		{
			return false;
		}

		TArray<FString> HeaderColumns;
		ParseCsvLine(Lines[0], HeaderColumns);
		if (HeaderColumns.Num() == 0)
		{
			return false;
		}

		HeaderColumns[0].ReplaceInline(TEXT("\ufeff"), TEXT(""));

		TMap<FString, int32> ColumnIndex;
		for (int32 Index = 0; Index < HeaderColumns.Num(); ++Index)
		{
			ColumnIndex.Add(HeaderColumns[Index], Index);
		}

		auto GetColumnValue = [&ColumnIndex](const TArray<FString>& Columns, const TCHAR* Name, double& OutValue)
		{
			const int32* Index = ColumnIndex.Find(Name);
			return Index && Columns.IsValidIndex(*Index) && TryParseOptionalDouble(Columns[*Index], OutValue);
		};

		for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
		{
			if (Lines[LineIndex].TrimStartAndEnd().IsEmpty())
			{
				continue;
			}

			TArray<FString> Columns;
			ParseCsvLine(Lines[LineIndex], Columns);
			if (Columns.Num() < HeaderColumns.Num())
			{
				Columns.SetNum(HeaderColumns.Num());
			}

			const int32* TimestampIndex = ColumnIndex.Find(TEXT("reference_timestamp"));
			if (!TimestampIndex || !Columns.IsValidIndex(*TimestampIndex))
			{
				continue;
			}

			FDateTime SampleTimeUtc;
			if (!ParseSwissTimestamp(Columns[*TimestampIndex], SampleTimeUtc))
			{
				continue;
			}

			if (SampleTimeUtc < RangeStartUtc || SampleTimeUtc >= RangeEndUtc)
			{
				continue;
			}

			const int32* StationIndex = ColumnIndex.Find(TEXT("station_abbr"));
			if (StationIndex && Columns.IsValidIndex(*StationIndex))
			{
				FString StationCode = Columns[*StationIndex];
				StationCode.TrimStartAndEndInline();
				StationCode.ToUpperInline();
				if (!Config.StationIdUpper.IsEmpty() && StationCode != Config.StationIdUpper)
				{
					continue;
				}
			}

			FMeteoSwissRow Row;
			Row.TimeUtc = SampleTimeUtc;
			GetColumnValue(Columns, TEXT("tre200h0"), Row.TemperatureC);
			GetColumnValue(Columns, TEXT("ure200h0"), Row.RelativeHumidityPct);
			GetColumnValue(Columns, TEXT("fkl010h0"), Row.WindSpeedMps);
			GetColumnValue(Columns, TEXT("dkl010h0"), Row.WindDirectionDeg);
			GetColumnValue(Columns, TEXT("rre150h0"), Row.PrecipitationMm);
			GetColumnValue(Columns, TEXT("gre000h0"), Row.ShortwaveWm2);
			GetColumnValue(Columns, TEXT("ods000h0"), Row.DiffuseShortwaveWm2);
			GetColumnValue(Columns, TEXT("oli000h0"), Row.LongwaveWm2);
			GetColumnValue(Columns, TEXT("prestah0"), Row.PressureHpa);
			OutRows.Add(MoveTemp(Row));
		}

		return true;
	}

	void DeduplicateAndSortRows(TArray<FMeteoSwissRow>& Rows)
	{
		Rows.Sort([](const FMeteoSwissRow& A, const FMeteoSwissRow& B)
		{
			return A.TimeUtc < B.TimeUtc;
		});

		TArray<FMeteoSwissRow> UniqueRows;
		UniqueRows.Reserve(Rows.Num());
		for (const FMeteoSwissRow& Row : Rows)
		{
			if (UniqueRows.Num() > 0 && UniqueRows.Last().TimeUtc == Row.TimeUtc)
			{
				UniqueRows.Last() = Row;
			}
			else
			{
				UniqueRows.Add(Row);
			}
		}

		Rows = MoveTemp(UniqueRows);
	}

	bool FetchStationDataset(const FMeteoSwissPullConfig& Config,
		const FString& StationIdLower,
		const FString& StationIdUpper,
		const FDateTime& RangeStartUtc,
		const FDateTime& RangeEndUtc,
		FMeteoSwissStationDataset& OutDataset)
	{
		const FString StacItemUrl = Config.StacItemsBaseUrl.EndsWith(TEXT("/"))
			? Config.StacItemsBaseUrl + StationIdLower
			: Config.StacItemsBaseUrl + TEXT("/") + StationIdLower;

		FString StacPayload;
		FString StacError;
		if (!ExecuteGetRequest(StacItemUrl, StacPayload, StacError))
		{
			UE_LOG(LogTemp, Error, TEXT("[MeteoSwiss] Failed to fetch STAC item %s: %s"), *StacItemUrl, *StacError);
			return false;
		}

		if (!ParseStacItem(StacPayload, OutDataset.StacItem))
		{
			UE_LOG(LogTemp, Error, TEXT("[MeteoSwiss] STAC item for station %s does not expose assets."), *StationIdUpper);
			return false;
		}

		TArray<FString> AssetKeys;
		BuildCandidateAssetKeys(Config, RangeStartUtc, RangeEndUtc, AssetKeys);
		if (AssetKeys.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] No asset keys selected for range %s -> %s."), *RangeStartUtc.ToIso8601(), *RangeEndUtc.ToIso8601());
			return false;
		}

		TArray<FMeteoSwissRow> Rows;
		for (const FString& AssetKey : AssetKeys)
		{
			const FString* AssetUrl = OutDataset.StacItem.AssetUrls.Find(AssetKey);
			if (!AssetUrl)
			{
				continue;
			}

			FString CsvPayload;
			FString CsvError;
			if (!ExecuteGetRequest(*AssetUrl, CsvPayload, CsvError))
			{
				UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] Failed to download %s: %s"), **AssetUrl, *CsvError);
				continue;
			}

			const int32 BeforeCount = Rows.Num();
			FMeteoSwissPullConfig StationSpecificConfig = Config;
			StationSpecificConfig.StationIdUpper = StationIdUpper;
			StationSpecificConfig.StationIdLower = StationIdLower;
			if (!ParseCsvPayload(CsvPayload, StationSpecificConfig, RangeStartUtc, RangeEndUtc, Rows))
			{
				UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] Failed to parse CSV asset %s"), *AssetKey);
				continue;
			}

			if (Rows.Num() > BeforeCount)
			{
				OutDataset.UsedAssetUrls.AddUnique(*AssetUrl);
			}
		}

		if (Rows.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] No rows found for station %s inside %s -> %s."), *StationIdUpper, *RangeStartUtc.ToIso8601(), *RangeEndUtc.ToIso8601());
			return false;
		}

		DeduplicateAndSortRows(Rows);
		for (const FMeteoSwissRow& Row : Rows)
		{
			OutDataset.RowsByTime.Add(Row.TimeUtc, Row);
		}

		return true;
	}

	void CollectTimestamps(const FMeteoSwissStationDataset& Dataset, TSet<FDateTime>& OutTimestamps)
	{
		for (const TPair<FDateTime, FMeteoSwissRow>& Pair : Dataset.RowsByTime)
		{
			OutTimestamps.Add(Pair.Key);
		}
	}

	void ApplyVariableValue(const FMeteoSwissVariablePullConfig& VariableConfig,
		const TMap<FString, FMeteoSwissStationDataset>& DatasetsByStation,
		const FDateTime& Timestamp,
		double FMeteoSwissRow::* ValueMember,
		double& OutValue)
	{
		OutValue = MakeNaN();
		if (!VariableConfig.bInclude)
		{
			return;
		}

		const FMeteoSwissStationDataset* Dataset = DatasetsByStation.Find(VariableConfig.StationIdLower);
		if (!Dataset)
		{
			return;
		}

		const FMeteoSwissRow* Row = Dataset->RowsByTime.Find(Timestamp);
		if (!Row)
		{
			return;
		}

		OutValue = Row->*ValueMember;
	}

	TSharedPtr<FJsonObject> BuildVariableSourceMeta(const TCHAR* VariableName, const FMeteoSwissVariablePullConfig& VariableConfig)
	{
		TSharedPtr<FJsonObject> VariableMeta = MakeShared<FJsonObject>();
		VariableMeta->SetBoolField(TEXT("included"), VariableConfig.bInclude);
		VariableMeta->SetStringField(TEXT("station_id"), VariableConfig.StationIdUpper);
		VariableMeta->SetStringField(TEXT("variable"), VariableName);
		return VariableMeta;
	}

	FMeteoSwissPullOutcome PerformMeteoSwissPull(const FMeteoSwissPullConfig& Config)
	{
		FMeteoSwissPullOutcome Outcome;

		if (Config.StacItemsBaseUrl.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] STAC base URL is not configured."));
			return Outcome;
		}

		if (Config.StationIdLower.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] StationId is not configured."));
			return Outcome;
		}

		const FString OutputPath = ResolveMeteoSwissPath(Config.OutputPath);
		if (OutputPath.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] Output path is not configured."));
			return Outcome;
		}

		FDateTime RangeStartUtc = Config.RangeStartUtc;
		FDateTime RangeEndUtc = Config.RangeEndUtc;
		if (!Config.bUseExplicitRange || RangeEndUtc <= RangeStartUtc)
		{
			RangeEndUtc = FDateTime::UtcNow();
			RangeStartUtc = RangeEndUtc - FTimespan::FromHours(FMath::Max(Config.HoursBack, 1));
		}

		UE_LOG(LogTemp, Display, TEXT("[MeteoSwiss] Using request window %s -> %s (end exclusive)"), *RangeStartUtc.ToIso8601(), *RangeEndUtc.ToIso8601());

		const FMeteoSwissVariablePullConfig TemperatureConfig = BuildVariableConfig(Config.StationIdUpper, Config.TemperatureSource);
		const FMeteoSwissVariablePullConfig RelativeHumidityConfig = BuildVariableConfig(Config.StationIdUpper, Config.RelativeHumiditySource);
		const FMeteoSwissVariablePullConfig WindSpeedConfig = BuildVariableConfig(Config.StationIdUpper, Config.WindSpeedSource);
		const FMeteoSwissVariablePullConfig WindDirectionConfig = BuildVariableConfig(Config.StationIdUpper, Config.WindDirectionSource);
		const FMeteoSwissVariablePullConfig PrecipitationConfig = BuildVariableConfig(Config.StationIdUpper, Config.PrecipitationSource);
		const FMeteoSwissVariablePullConfig ShortwaveConfig = BuildVariableConfig(Config.StationIdUpper, Config.ShortwaveSource);
		const FMeteoSwissVariablePullConfig DiffuseShortwaveConfig = BuildVariableConfig(Config.StationIdUpper, Config.DiffuseShortwaveSource);
		const FMeteoSwissVariablePullConfig LongwaveConfig = BuildVariableConfig(Config.StationIdUpper, Config.LongwaveSource);
		const FMeteoSwissVariablePullConfig PressureConfig = BuildVariableConfig(Config.StationIdUpper, Config.PressureSource);

		TSet<FString> RequiredStations;
		RequiredStations.Add(Config.StationIdLower);
		AddRequiredStation(TemperatureConfig, RequiredStations);
		AddRequiredStation(RelativeHumidityConfig, RequiredStations);
		AddRequiredStation(WindSpeedConfig, RequiredStations);
		AddRequiredStation(WindDirectionConfig, RequiredStations);
		AddRequiredStation(PrecipitationConfig, RequiredStations);
		AddRequiredStation(ShortwaveConfig, RequiredStations);
		AddRequiredStation(DiffuseShortwaveConfig, RequiredStations);
		AddRequiredStation(LongwaveConfig, RequiredStations);
		AddRequiredStation(PressureConfig, RequiredStations);

		TMap<FString, FMeteoSwissStationDataset> DatasetsByStation;
		TArray<FString> RequiredStationList = RequiredStations.Array();
		RequiredStationList.Sort();
		for (const FString& StationIdLower : RequiredStationList)
		{
			FString StationIdUpper = StationIdLower;
			StationIdUpper.ToUpperInline();
			FMeteoSwissStationDataset Dataset;
			if (FetchStationDataset(Config, StationIdLower, StationIdUpper, RangeStartUtc, RangeEndUtc, Dataset))
			{
				DatasetsByStation.Add(StationIdLower, MoveTemp(Dataset));
			}
		}

		const FMeteoSwissStationDataset* PrimaryDataset = DatasetsByStation.Find(Config.StationIdLower);
		if (!PrimaryDataset)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] Primary station dataset %s could not be loaded."), *Config.StationIdUpper);
			return Outcome;
		}

		TSet<FDateTime> TimestampSet;
		for (const TPair<FString, FMeteoSwissStationDataset>& Pair : DatasetsByStation)
		{
			CollectTimestamps(Pair.Value, TimestampSet);
		}

		TArray<FDateTime> Timestamps = TimestampSet.Array();
		Timestamps.Sort();
		if (Timestamps.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] No timestamps found after station merge."));
			return Outcome;
		}

		TArray<FMeteoSwissRow> MergedRows;
		MergedRows.Reserve(Timestamps.Num());
		int32 DiffuseCount = 0;
		for (const FDateTime& Timestamp : Timestamps)
		{
			FMeteoSwissRow Row;
			Row.TimeUtc = Timestamp;
			ApplyVariableValue(TemperatureConfig, DatasetsByStation, Timestamp, &FMeteoSwissRow::TemperatureC, Row.TemperatureC);
			ApplyVariableValue(RelativeHumidityConfig, DatasetsByStation, Timestamp, &FMeteoSwissRow::RelativeHumidityPct, Row.RelativeHumidityPct);
			ApplyVariableValue(WindSpeedConfig, DatasetsByStation, Timestamp, &FMeteoSwissRow::WindSpeedMps, Row.WindSpeedMps);
			ApplyVariableValue(WindDirectionConfig, DatasetsByStation, Timestamp, &FMeteoSwissRow::WindDirectionDeg, Row.WindDirectionDeg);
			ApplyVariableValue(PrecipitationConfig, DatasetsByStation, Timestamp, &FMeteoSwissRow::PrecipitationMm, Row.PrecipitationMm);
			ApplyVariableValue(ShortwaveConfig, DatasetsByStation, Timestamp, &FMeteoSwissRow::ShortwaveWm2, Row.ShortwaveWm2);
			ApplyVariableValue(DiffuseShortwaveConfig, DatasetsByStation, Timestamp, &FMeteoSwissRow::DiffuseShortwaveWm2, Row.DiffuseShortwaveWm2);
			ApplyVariableValue(LongwaveConfig, DatasetsByStation, Timestamp, &FMeteoSwissRow::LongwaveWm2, Row.LongwaveWm2);
			ApplyVariableValue(PressureConfig, DatasetsByStation, Timestamp, &FMeteoSwissRow::PressureHpa, Row.PressureHpa);
			if (FMath::IsFinite(Row.DiffuseShortwaveWm2))
			{
				++DiffuseCount;
			}
			MergedRows.Add(MoveTemp(Row));
		}

		const FDateTime ObservedStartUtc = MergedRows[0].TimeUtc;
		const FDateTime ObservedEndUtc = MergedRows.Last().TimeUtc + FTimespan::FromHours(1);

		if (ObservedStartUtc > RangeStartUtc || ObservedEndUtc < RangeEndUtc)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] Dataset does not fully cover requested window %s -> %s. Observed %s -> %s."),
				*RangeStartUtc.ToIso8601(), *RangeEndUtc.ToIso8601(), *ObservedStartUtc.ToIso8601(), *ObservedEndUtc.ToIso8601());
		}

		if (DiffuseCount == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] No non-empty ods000h0 values found in the requested window. Diffuse shortwave will be omitted so the simulation can derive the split from global shortwave."));
		}

		TSharedPtr<FJsonObject> OutputRoot = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
		Meta->SetStringField(TEXT("created_utc"), FDateTime::UtcNow().ToIso8601());
		Meta->SetStringField(TEXT("sources"), PrimaryDataset->StacItem.StationName.IsEmpty() ? Config.StationIdUpper : PrimaryDataset->StacItem.StationName);
		Meta->SetStringField(TEXT("data_source"), TEXT("MeteoSwiss"));
		Meta->SetStringField(TEXT("primary_station_id"), Config.StationIdUpper);
		Meta->SetStringField(TEXT("primary_station_name"), PrimaryDataset->StacItem.StationName);
		Meta->SetStringField(TEXT("range_start_utc"), RangeStartUtc.ToIso8601());
		Meta->SetStringField(TEXT("range_end_utc_exclusive"), RangeEndUtc.ToIso8601());
		Meta->SetStringField(TEXT("observed_range_start_utc"), ObservedStartUtc.ToIso8601());
		Meta->SetStringField(TEXT("observed_range_end_utc_exclusive"), ObservedEndUtc.ToIso8601());
		Meta->SetNumberField(TEXT("latitude_deg"), PrimaryDataset->StacItem.Latitude);
		Meta->SetNumberField(TEXT("longitude_deg"), PrimaryDataset->StacItem.Longitude);
		Meta->SetNumberField(TEXT("site_elevation_m"), Config.ElevationM);

		TArray<FString> UsedAssetUrls;
		for (const TPair<FString, FMeteoSwissStationDataset>& Pair : DatasetsByStation)
		{
			for (const FString& Url : Pair.Value.UsedAssetUrls)
			{
				UsedAssetUrls.AddUnique(Url);
			}
		}

		TArray<TSharedPtr<FJsonValue>> AssetArray;
		AssetArray.Reserve(UsedAssetUrls.Num());
		for (const FString& Url : UsedAssetUrls)
		{
			AssetArray.Add(MakeShared<FJsonValueString>(Url));
		}
		Meta->SetArrayField(TEXT("asset_urls"), AssetArray);

		TSharedPtr<FJsonObject> VariableSources = MakeShared<FJsonObject>();
		VariableSources->SetObjectField(TEXT("temperature"), BuildVariableSourceMeta(TEXT("tre200h0"), TemperatureConfig));
		VariableSources->SetObjectField(TEXT("relative_humidity"), BuildVariableSourceMeta(TEXT("ure200h0"), RelativeHumidityConfig));
		VariableSources->SetObjectField(TEXT("wind_speed"), BuildVariableSourceMeta(TEXT("fkl010h0"), WindSpeedConfig));
		VariableSources->SetObjectField(TEXT("wind_direction"), BuildVariableSourceMeta(TEXT("dkl010h0"), WindDirectionConfig));
		VariableSources->SetObjectField(TEXT("precipitation"), BuildVariableSourceMeta(TEXT("rre150h0"), PrecipitationConfig));
		VariableSources->SetObjectField(TEXT("shortwave"), BuildVariableSourceMeta(TEXT("gre000h0"), ShortwaveConfig));
		VariableSources->SetObjectField(TEXT("diffuse_shortwave"), BuildVariableSourceMeta(TEXT("ods000h0"), DiffuseShortwaveConfig));
		VariableSources->SetObjectField(TEXT("longwave"), BuildVariableSourceMeta(TEXT("oli000h0"), LongwaveConfig));
		VariableSources->SetObjectField(TEXT("pressure"), BuildVariableSourceMeta(TEXT("prestah0"), PressureConfig));
		Meta->SetObjectField(TEXT("variable_sources"), VariableSources);

		OutputRoot->SetObjectField(TEXT("meta"), Meta);

		TArray<TSharedPtr<FJsonValue>> SeriesArray;
		SeriesArray.Reserve(MergedRows.Num());
		for (const FMeteoSwissRow& Row : MergedRows)
		{
			TSharedPtr<FJsonObject> SampleObject = MakeShared<FJsonObject>();
			SampleObject->SetStringField(TEXT("time_utc"), Row.TimeUtc.ToIso8601());
			SetOptionalNumber(SampleObject, TEXT("T_C"), Row.TemperatureC);
			SetOptionalNumber(SampleObject, TEXT("RH_pct"), Row.RelativeHumidityPct);
			SetOptionalNumber(SampleObject, TEXT("Wind_mps"), Row.WindSpeedMps);
			SetOptionalNumber(SampleObject, TEXT("WindDir_deg"), Row.WindDirectionDeg);
			SetOptionalNumber(SampleObject, TEXT("Precip_mmph"), Row.PrecipitationMm);
			SetOptionalNumber(SampleObject, TEXT("SWdown_Wm2"), Row.ShortwaveWm2);
			SetOptionalNumber(SampleObject, TEXT("DiffuseSWdown_Wm2"), Row.DiffuseShortwaveWm2);
			SetOptionalNumber(SampleObject, TEXT("LWdown_Wm2"), Row.LongwaveWm2);
			SetOptionalNumber(SampleObject, TEXT("Pressure_hPa"), Row.PressureHpa);
			SeriesArray.Add(MakeShared<FJsonValueObject>(SampleObject));
		}
		OutputRoot->SetArrayField(TEXT("series"), SeriesArray);

		FString OutputString;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
		FJsonSerializer::Serialize(OutputRoot.ToSharedRef(), Writer);

		EnsureOutputDirectory(OutputPath);
		if (FFileHelper::SaveStringToFile(OutputString, *OutputPath))
		{
			UE_LOG(LogTemp, Display, TEXT("[MeteoSwiss] Wrote %d samples to %s"), SeriesArray.Num(), *OutputPath);
			Outcome.bSuccess = true;
			Outcome.SampleCount = SeriesArray.Num();
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] Failed to write %s"), *OutputPath);
		}

		return Outcome;
	}
}

using namespace MeteoSwissEditorFetcherInternal;

void UMeteoSwissEditorFetcher::PullMeteoSwissNow()
{
	if (bPullInProgress)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[MeteoSwiss] Pull already in progress."));
		return;
	}

	const UMeteoSwissSettings* Settings = GetDefault<UMeteoSwissSettings>();
	if (!Settings)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] Settings not found."));
		OnMeteoSwissPullFinished.Broadcast(false, 0);
		return;
	}

	FMeteoSwissPullConfig Config;
	bool bUsedDefaultBaseUrl = false;
	Config.StacItemsBaseUrl = Settings->GetResolvedStacItemsBaseUrl(bUsedDefaultBaseUrl);
	if (bUsedDefaultBaseUrl)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] STAC base URL missing host. Using default %s."), *Config.StacItemsBaseUrl);
	}

	Config.StationIdLower = Settings->GetNormalizedStationIdLower();
	Config.StationIdUpper = Settings->GetNormalizedStationIdUpper();
	Config.OutputPath = Settings->OutputJsonPath.FilePath;
	Config.HoursBack = Settings->HoursBack;
	Config.ElevationM = Settings->ElevationM;
	Config.TemperatureSource = Settings->TemperatureSource;
	Config.RelativeHumiditySource = Settings->RelativeHumiditySource;
	Config.WindSpeedSource = Settings->WindSpeedSource;
	Config.WindDirectionSource = Settings->WindDirectionSource;
	Config.PrecipitationSource = Settings->PrecipitationSource;
	Config.ShortwaveSource = Settings->ShortwaveSource;
	Config.DiffuseShortwaveSource = Settings->DiffuseShortwaveSource;
	Config.LongwaveSource = Settings->LongwaveSource;
	Config.PressureSource = Settings->PressureSource;

	if (Config.StationIdLower.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] StationId is empty."));
		OnMeteoSwissPullFinished.Broadcast(false, 0);
		return;
	}

	if (GEditor)
	{
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
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
						RangeStart = FMath::Min(RangeStart, RunStart);
						RangeEnd = FMath::Max(RangeEnd, RunEnd);
					}
				}
			}

			if (bFoundRange)
			{
				Config.bUseExplicitRange = true;
				Config.RangeStartUtc = RangeStart;
				Config.RangeEndUtc = RangeEnd;
				UE_LOG(LogTemp, Display, TEXT("[MeteoSwiss] Using simulation span %s -> %s"),
					*RangeStart.ToIso8601(), *RangeEnd.ToIso8601());
			}
		}
	}

	bPullInProgress = true;

	TWeakObjectPtr<UMeteoSwissEditorFetcher> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Config]()
	{
		const FMeteoSwissPullOutcome Outcome = PerformMeteoSwissPull(Config);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Outcome]()
		{
			if (UMeteoSwissEditorFetcher* StrongPtr = WeakThis.Get())
			{
				StrongPtr->HandlePullFinished(Outcome.bSuccess, Outcome.SampleCount);
			}
		});
	});
}

void UMeteoSwissEditorFetcher::HandlePullFinished(bool bSuccess, int32 NumSamples)
{
	bPullInProgress = false;
	OnMeteoSwissPullFinished.Broadcast(bSuccess, NumSamples);
}

#endif // WITH_EDITOR
