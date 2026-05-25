#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CsvSettings.generated.h"

UENUM(BlueprintType)
enum class ECsvSchema : uint8
{
    Auto         UMETA(DisplayName="Auto-detect"),
    Alptal       UMETA(DisplayName="Alptal (datetime, SW, LW, Sf, Rf, Ta[K], RH, Ua, Ps[Pa])"),
    FinseGeneric UMETA(DisplayName="Finse (Tair[C], Lin, Sin, wind, p[Pa], q, rainfall, snowfall, t_span)"),
    Simple8      UMETA(DisplayName="Simple 8-column (time, T_C, RH_pct, Wind_mps, SW, LW, Precip_mmph, SnowFrac)")
};

/** Project-wide settings for using CSV weather forcing. */
UCLASS(config=Engine, defaultconfig, meta=(DisplayName="CSV Weather Settings"))
class SIMULATIONDATA_API UCsvSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UCsvSettings();

    // Path to the input CSV file. Supports {ProjectDir}, {ProjectSavedDir}, {ProjectContentDir}.
    UPROPERTY(EditAnywhere, config, Category="Input")
    FFilePath InputCsvPath;

    // Optional: output JSON written by the editor pull for coverage checks.
    UPROPERTY(EditAnywhere, config, Category="Output")
    FFilePath OutputJsonPath;

    // If true, interpret SnowSimulationActor Start/End as local time and convert to UTC
    // using SimulationUtcOffsetHours when slicing the CSV during the editor pull.
    UPROPERTY(EditAnywhere, config, Category="Request")
    bool bTreatSimulationTimesAsLocal;

    // Fixed UTC offset (hours) to apply if bTreatSimulationTimesAsLocal is true.
    UPROPERTY(EditAnywhere, config, Category="Request", meta=(ClampMin="-12", ClampMax="14", EditCondition="bTreatSimulationTimesAsLocal"))
    int32 SimulationUtcOffsetHours;

    // Schema hint for the parser. Auto-detection will try to infer from header.
    UPROPERTY(EditAnywhere, config, Category="Parsing")
    ECsvSchema Schema;

    // Name of the timestamp column when Schema==Auto or Simple8 with headers (e.g. "datetime", "t_span").
    UPROPERTY(EditAnywhere, config, Category="Parsing", meta=(ToolTip="Optional override for the timestamp column name when auto-detecting."))
    FString TimeColumnName;

    static const UCsvSettings* Get()
    {
        return GetDefault<UCsvSettings>();
    }

    virtual FName GetCategoryName() const override;
};


