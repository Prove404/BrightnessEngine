#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformMath.h"
#include <limits>
#include "FrostTypes.generated.h"

USTRUCT(BlueprintType)
struct SIMULATIONDATA_API FSiteMeta
{
	GENERATED_BODY()

	FSiteMeta()
		: LatDeg(60.0f)
		, LonDeg(8.0f)
		, ElevM(1222.0f)
	{}

	// Latitude in degrees (+N).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost")
	float LatDeg;

	// Longitude in degrees (+E).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost")
	float LonDeg;

	// Site elevation (meters above sea level).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Frost")
	float ElevM;
};

USTRUCT(BlueprintType)
struct SIMULATIONDATA_API FWeatherSample
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	FDateTime TimeUTC;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float T_C = 0.0f;

	// True only when T_C was actually present in the source data (not a default zero).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	bool bTemperatureValid = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float RH_pct = 80.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float Wind_mps = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float WindDir_deg = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float Precip_mmph = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float SWdown_Wm2 = std::numeric_limits<float>::quiet_NaN();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float DiffuseSWdown_Wm2 = std::numeric_limits<float>::quiet_NaN();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float LWdown_Wm2 = std::numeric_limits<float>::quiet_NaN();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float Pressure_hPa = std::numeric_limits<float>::quiet_NaN();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float SnowFrac_0_1 = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float Rain_mmph = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float Snow_mmph = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Frost")
	float CloudCover_pct = std::numeric_limits<float>::quiet_NaN();

	bool HasTemperature() const { return bTemperatureValid; }
	bool HasExplicitPrecip() const { return Rain_mmph > 0.0f || Snow_mmph > 0.0f || (Precip_mmph == 0.0f && Rain_mmph == 0.0f && Snow_mmph == 0.0f); }
	bool HasCloudCover() const { return FMath::IsFinite(CloudCover_pct); }

	bool HasSW() const { return FMath::IsFinite(SWdown_Wm2); }
	bool HasDiffuseSW() const { return FMath::IsFinite(DiffuseSWdown_Wm2); }
	bool HasLW() const { return FMath::IsFinite(LWdown_Wm2); }
	bool HasPressure() const { return FMath::IsFinite(Pressure_hPa); }
};

