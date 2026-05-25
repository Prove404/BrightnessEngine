#include "MeteoSwiss/MeteoSwissWeatherDataProvider.h"

#include "Components/SceneComponent.h"
#include "Frost/FrostWeatherProvider.h"
#include "MeteoSwiss/MeteoSwissSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_EDITOR
#include "Kismet/KismetSystemLibrary.h"
#endif

namespace
{
	float NormalizeMeteoSwissElevationMeters(float InElevation, const TCHAR* SourceTag)
	{
		float ElevMeters = InElevation;
		if (FMath::Abs(ElevMeters) > 10000.0f)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeteoSwiss] %s elevation appears too large (%.2f). Assuming centimeters and converting to meters."), SourceTag, ElevMeters);
			ElevMeters /= 100.0f;
		}
		return ElevMeters;
	}

	void CopyMeteoSwissSamples(const TArray<FWeatherSample>& From, TArray<FWeatherSample>& To)
	{
		To.Reset(From.Num());
		To.Append(From);
	}

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
}

UMeteoSwissWeatherDataProvider::UMeteoSwissWeatherDataProvider()
{
	Backend = CreateDefaultSubobject<UFrostWeatherProvider>(TEXT("MeteoSwissBackend"));
	if (Backend)
	{
		Backend->SetFlags(RF_Transactional);
	}

	if (const UMeteoSwissSettings* Settings = UMeteoSwissSettings::Get())
	{
		JsonPath.FilePath = Settings->OutputJsonPath.FilePath;
		Site = Settings->BuildSiteMeta();
	}

	SyncSettingsToBackend();
}

void UMeteoSwissWeatherDataProvider::Initialize(FDateTime StartTime, FDateTime EndTime)
{
	SyncSettingsToBackend();
	if (Backend)
	{
		Backend->Initialize(StartTime, EndTime);
	}

	LoadFromJson();
}

TResourceArray<FClimateData>* UMeteoSwissWeatherDataProvider::CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime)
{
	SyncSettingsToBackend();
	return Backend ? Backend->CreateRawClimateDataResourceArray(StartTime, EndTime) : nullptr;
}

FWeatherForcingData UMeteoSwissWeatherDataProvider::GetWeatherForcing(FDateTime Time, int32 GridX, int32 GridY)
{
	SyncSettingsToBackend();
	return Backend ? Backend->GetWeatherForcing(Time, GridX, GridY) : FWeatherForcingData();
}

int32 UMeteoSwissWeatherDataProvider::LoadFromJson()
{
	SyncSettingsToBackend();
	const int32 Result = Backend ? Backend->LoadFromJson() : 0;
	if (Backend)
	{
		Site = Backend->Site;
	}

	FString FullPath = ResolveMeteoSwissPath(JsonPath.FilePath);
	if (FullPath.IsEmpty())
	{
		if (const UMeteoSwissSettings* Settings = UMeteoSwissSettings::Get())
		{
			FullPath = ResolveMeteoSwissPath(Settings->OutputJsonPath.FilePath);
		}
	}

	bool bUpdatedSiteFromMeta = false;

	if (!FullPath.IsEmpty())
	{
		FString JsonText;
		if (FFileHelper::LoadFileToString(JsonText, *FullPath))
		{
			TSharedPtr<FJsonObject> RootObject;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			if (FJsonSerializer::Deserialize(Reader, RootObject) && RootObject.IsValid())
			{
				const TSharedPtr<FJsonObject>* MetaObj = nullptr;
				if (RootObject->TryGetObjectField(TEXT("meta"), MetaObj) && MetaObj && MetaObj->IsValid())
				{
					double Value = 0.0;
					if ((*MetaObj)->TryGetNumberField(TEXT("latitude_deg"), Value))
					{
						Site.LatDeg = static_cast<float>(Value);
						bUpdatedSiteFromMeta = true;
					}
					if ((*MetaObj)->TryGetNumberField(TEXT("longitude_deg"), Value))
					{
						Site.LonDeg = static_cast<float>(Value);
						bUpdatedSiteFromMeta = true;
					}
					if ((*MetaObj)->TryGetNumberField(TEXT("site_elevation_m"), Value))
					{
						Site.ElevM = NormalizeMeteoSwissElevationMeters(static_cast<float>(Value), TEXT("JSON meta site_elevation_m"));
						bUpdatedSiteFromMeta = true;
					}
				}
			}
		}
	}

	if (Backend)
	{
		Backend->Site = Site;
	}

	if (bUpdatedSiteFromMeta)
	{
		UE_LOG(LogTemp, Display, TEXT("[MeteoSwiss] Site metadata applied from JSON (lat=%.6f lon=%.6f elev=%.2f m)."), Site.LatDeg, Site.LonDeg, Site.ElevM);
	}

	SyncCacheFromBackend();
	return Result;
}

bool UMeteoSwissWeatherDataProvider::GetWeatherAtUTC(const FDateTime& TimeUTC, FWeatherSample& OutSample) const
{
	return Backend ? Backend->GetWeatherAtUTC(TimeUTC, OutSample) : false;
}

void UMeteoSwissWeatherDataProvider::ApplySiteOverride(const FSiteMeta& InSite)
{
	Site = InSite;
	SyncSettingsToBackend();
}

void UMeteoSwissWeatherDataProvider::SyncSettingsToBackend()
{
	if (!Backend)
	{
		return;
	}

	Backend->JsonPath = JsonPath;
	Backend->UseDerivedRadiationIfMissing = UseDerivedRadiationIfMissing;
	Backend->Site = Site;
	Backend->AuxiliaryProvider = AuxiliaryProviderActor
		? AuxiliaryProviderActor->FindComponentByClass<USimulationWeatherDataProviderBase>()
		: nullptr;
	Backend->AuxiliaryElevM = AuxiliaryElevM;
	Backend->BiasWindowHours = BiasWindowHours;
}

void UMeteoSwissWeatherDataProvider::SyncCacheFromBackend()
{
	if (!Backend)
	{
		Samples.Reset();
		RequestedStartUtc = FDateTime::MinValue();
		RequestedEndUtc = FDateTime::MinValue();
		DatasetStartUtc = FDateTime::MinValue();
		DatasetEndUtc = FDateTime::MinValue();
		return;
	}

	CopyMeteoSwissSamples(Backend->Samples, Samples);
	RequestedStartUtc = Backend->RequestedStartUtc;
	RequestedEndUtc = Backend->RequestedEndUtc;
	DatasetStartUtc = Backend->DatasetStartUtc;
	DatasetEndUtc = Backend->DatasetEndUtc;
}

AMeteoSwissWeatherProviderActor::AMeteoSwissWeatherProviderActor()
{
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	Provider = CreateDefaultSubobject<UMeteoSwissWeatherDataProvider>(TEXT("MeteoSwissProvider"));

	if (Provider)
	{
		JsonPath = Provider->JsonPath;
		UseDerivedRadiationIfMissing = Provider->UseDerivedRadiationIfMissing;
		Site = Provider->Site;
	}
}

void AMeteoSwissWeatherProviderActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	SyncComponentSettings();
}

void AMeteoSwissWeatherProviderActor::BeginPlay()
{
	Super::BeginPlay();

	if (Provider)
	{
		SyncComponentSettings();
		Provider->LoadFromJson();
	}
}

void AMeteoSwissWeatherProviderActor::RefreshFromJson()
{
	if (Provider)
	{
		SyncComponentSettings();
		Provider->LoadFromJson();
	}
}

void AMeteoSwissWeatherProviderActor::SyncComponentSettings()
{
	if (!Provider)
	{
		return;
	}

	Provider->JsonPath = JsonPath;
	Provider->UseDerivedRadiationIfMissing = UseDerivedRadiationIfMissing;
	Provider->ApplySiteOverride(Site);
	Provider->AuxiliaryProviderActor = AuxiliaryProviderActor;
	Provider->AuxiliaryElevM = AuxiliaryElevM;
	Provider->BiasWindowHours = BiasWindowHours;
}

#if WITH_EDITOR
void AMeteoSwissWeatherProviderActor::PullMeteoSwissNow()
{
	if (UWorld* World = GetWorld())
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(World, TEXT("PullMeteoSwissNow"));
	}
}
#endif
