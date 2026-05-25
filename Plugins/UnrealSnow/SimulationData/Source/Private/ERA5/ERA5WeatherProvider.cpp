#include "ERA5/ERA5WeatherProvider.h"

#include "Components/SceneComponent.h"
#include "ERA5/ERA5Settings.h"
#include "Frost/FrostWeatherProvider.h"
#include "GeoReferencingSystem.h"
#include "GeographicCoordinates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_EDITOR
#include "Kismet/KismetSystemLibrary.h"
#endif

namespace
{
    float NormalizeElevationMeters(float InElevation, const TCHAR* SourceTag)
    {
        float ElevMeters = InElevation;
        // Guard against cm values ending up in meter fields.
        if (FMath::Abs(ElevMeters) > 10000.0f)
        {
            UE_LOG(LogTemp, Warning, TEXT("[ERA5] %s elevation appears too large (%.2f). Assuming centimeters and converting to meters."), SourceTag, ElevMeters);
            ElevMeters /= 100.0f;
        }
        return ElevMeters;
    }

    void CopySamples(const TArray<FWeatherSample>& From, TArray<FWeatherSample>& To)
    {
        To.Reset(From.Num());
        To.Append(From);
    }

    bool TryGetGeospatialOrigin(UWorld* World, FSiteMeta& OutSite)
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
                OutSite.LatDeg = static_cast<float>(GeoCoords.Latitude);
                OutSite.LonDeg = static_cast<float>(GeoCoords.Longitude);
                OutSite.ElevM = NormalizeElevationMeters(static_cast<float>(GeoCoords.Altitude), TEXT("Georeferencing projected"));
            }
            else
            {
                OutSite.LatDeg = static_cast<float>(Geo->OriginLatitude);
                OutSite.LonDeg = static_cast<float>(Geo->OriginLongitude);
                OutSite.ElevM = NormalizeElevationMeters(static_cast<float>(Geo->OriginAltitude), TEXT("Georeferencing geographic"));
            }

            return true;
        }

        return false;
    }

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
}

UERA5WeatherProvider::UERA5WeatherProvider()
{
    Backend = CreateDefaultSubobject<UFrostWeatherProvider>(TEXT("ERA5Backend"));
    if (Backend)
    {
        Backend->SetFlags(RF_Transactional);
    }

    if (const UERA5Settings* Settings = UERA5Settings::Get())
    {
        JsonPath.FilePath = Settings->OutputJsonPath.FilePath;
        Site = Settings->BuildSiteMeta();
    }

    SyncSettingsToBackend();
}

void UERA5WeatherProvider::Initialize(FDateTime StartTime, FDateTime EndTime)
{
    SyncSettingsToBackend();
    if (Backend)
    {
        Backend->Initialize(StartTime, EndTime);
    }

    // Ensure ERA5 JSON metadata (including site elevation) is always applied on startup.
    LoadFromJson();
}

TResourceArray<FClimateData>* UERA5WeatherProvider::CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime)
{
    SyncSettingsToBackend();
    return Backend ? Backend->CreateRawClimateDataResourceArray(StartTime, EndTime) : nullptr;
}

FWeatherForcingData UERA5WeatherProvider::GetWeatherForcing(FDateTime Time, int32 GridX, int32 GridY)
{
    SyncSettingsToBackend();
    return Backend ? Backend->GetWeatherForcing(Time, GridX, GridY) : FWeatherForcingData();
}

int32 UERA5WeatherProvider::LoadFromJson()
{
    SyncSettingsToBackend();
    int32 Result = Backend ? Backend->LoadFromJson() : 0;
    if (Backend)
    {
        Site = Backend->Site;
    }

    FString FullPath = ResolveEra5Path(JsonPath.FilePath);
    if (FullPath.IsEmpty())
    {
        if (const UERA5Settings* Settings = UERA5Settings::Get())
        {
            FullPath = ResolveEra5Path(Settings->OutputJsonPath.FilePath);
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
                        const float ElevMeters = NormalizeElevationMeters(static_cast<float>(Value), TEXT("JSON meta site_elevation_m"));
                        Site.ElevM = ElevMeters;
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
        UE_LOG(LogTemp, Display, TEXT("[ERA5] Site metadata applied from JSON (lat=%.6f lon=%.6f elev=%.2f m)."), Site.LatDeg, Site.LonDeg, Site.ElevM);
    }

    SyncCacheFromBackend();
    return Result;
}

bool UERA5WeatherProvider::GetWeatherAtUTC(const FDateTime& TimeUTC, FWeatherSample& OutSample) const
{
    return Backend ? Backend->GetWeatherAtUTC(TimeUTC, OutSample) : false;
}

void UERA5WeatherProvider::ApplySiteOverride(const FSiteMeta& InSite)
{
    Site = InSite;
    SyncSettingsToBackend();
}

void UERA5WeatherProvider::SyncSettingsToBackend()
{
    if (!Backend)
    {
        return;
    }

    Backend->JsonPath = JsonPath;
    Backend->UseDerivedRadiationIfMissing = UseDerivedRadiationIfMissing;
    Backend->Site = Site;
}

void UERA5WeatherProvider::SyncCacheFromBackend()
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

    CopySamples(Backend->Samples, Samples);
    RequestedStartUtc = Backend->RequestedStartUtc;
    RequestedEndUtc = Backend->RequestedEndUtc;
    DatasetStartUtc = Backend->DatasetStartUtc;
    DatasetEndUtc = Backend->DatasetEndUtc;
}

AERA5WeatherProviderActor::AERA5WeatherProviderActor()
{
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    RootComponent = SceneRoot;

    Provider = CreateDefaultSubobject<UERA5WeatherProvider>(TEXT("ERA5Provider"));

    if (Provider)
    {
        JsonPath = Provider->JsonPath;
        UseDerivedRadiationIfMissing = Provider->UseDerivedRadiationIfMissing;
        Site = Provider->Site;
    }
}

void AERA5WeatherProviderActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    SyncComponentSettings();
}

void AERA5WeatherProviderActor::BeginPlay()
{
    Super::BeginPlay();

    if (Provider)
    {
        SyncComponentSettings();
        Provider->LoadFromJson();
    }
}

void AERA5WeatherProviderActor::RefreshFromJson()
{
    if (Provider)
    {
        SyncComponentSettings();
        Provider->LoadFromJson();
    }
}

void AERA5WeatherProviderActor::SyncComponentSettings()
{
    if (!Provider)
    {
        return;
    }

    FSiteMeta EffectiveSite = Site;
    if (TryGetGeospatialOrigin(GetWorld(), EffectiveSite))
    {
        Site = EffectiveSite;
    }

    Provider->JsonPath = JsonPath;
    Provider->UseDerivedRadiationIfMissing = UseDerivedRadiationIfMissing;
    Provider->ApplySiteOverride(EffectiveSite);
}

#if WITH_EDITOR
void AERA5WeatherProviderActor::PullERA5Now()
{
    if (UWorld* World = GetWorld())
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(World, TEXT("PullERA5Now"));
    }
}
#endif
