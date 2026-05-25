#pragma once

#include "CoreMinimal.h"
#include "ClimateData.generated.h"

/** Weather data (precipitation and temperature). */
USTRUCT(BlueprintType)
struct FClimateData
{
	GENERATED_USTRUCT_BODY()

	float Temperature;
	float Precipitation;

	FClimateData(float InPrecipitation, float InTemperature)
		: Temperature(InTemperature), Precipitation(InPrecipitation)
	{
	}

	FClimateData() : Temperature(0.0f), Precipitation(0.0f)
	{
	}
};

/** Comprehensive weather forcing data for snow simulation. */
USTRUCT(BlueprintType)
struct FWeatherForcingData
{
	GENERATED_USTRUCT_BODY()

public:
	/** Temperature in Kelvin */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float Temperature_K = 273.15f;  // 0°C in Kelvin

	/** Shortwave downward radiation in W/m² */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float SWdown_Wm2 = 0.0f;

	/** Longwave downward radiation in W/m² */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float LWdown_Wm2 = 200.0f;

	/** Wind speed in m/s */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float Wind_mps = 2.0f;

	/** Relative humidity (0-1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float RH_01 = 0.6f;

	/** Surface pressure in Pascals */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float Pressure_Pa = 101325.0f;

	/** Precipitation rate in kg/m²/s */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float PrecipRate_kgm2s = 0.0f;

	/** Snow fraction (0-1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float SnowFrac_01 = 0.0f;

	/** Explicit rain rate in kg/m²/s (optional, overrides SnowFrac if present) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float RainRate_kgm2s = 0.0f;

	/** Explicit snow rate in kg/m²/s (optional, overrides SnowFrac if present) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float SnowRate_kgm2s = 0.0f;

	/** Cloud cover fraction (0-1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float CloudCover_01 = 0.0f;

	/** Direct shortwave radiation component in W/m² (optional, for when source provides DNI/DHI split) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float DirectSWdown_Wm2 = -1.0f;  // -1 indicates not available

	/** Diffuse shortwave radiation component in W/m² (optional, for when source provides DNI/DHI split) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float DiffuseSWdown_Wm2 = -1.0f;  // -1 indicates not available

	/** Whether explicit rain/snow rates are available */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool bHasExplicitPrecipitation = false;

	/** Timestamp for this forcing data */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	FDateTime Timestamp;

	FWeatherForcingData() {}

	FWeatherForcingData(FDateTime InTimestamp, float TempK, float SWdown, float LWdown, float Wind, float RH, float Precip, float SnowFrac, float PressurePa = 101325.0f)
		: Temperature_K(TempK), SWdown_Wm2(SWdown), LWdown_Wm2(LWdown), Wind_mps(Wind), RH_01(RH),
		  Pressure_Pa(PressurePa), PrecipRate_kgm2s(Precip), SnowFrac_01(SnowFrac), Timestamp(InTimestamp)
	{
	}

	FWeatherForcingData(FDateTime InTimestamp, float TempK, float SWdown, float LWdown, float Wind, float RH, float RainRate, float SnowRate, bool bExplicit, float PressurePa = 101325.0f)
		: Temperature_K(TempK), SWdown_Wm2(SWdown), LWdown_Wm2(LWdown), Wind_mps(Wind), RH_01(RH),
		  Pressure_Pa(PressurePa), PrecipRate_kgm2s(RainRate + SnowRate), SnowFrac_01(0.0f), 
		  RainRate_kgm2s(RainRate), SnowRate_kgm2s(SnowRate), bHasExplicitPrecipitation(bExplicit),
		  Timestamp(InTimestamp)
	{
		if (PrecipRate_kgm2s > 0.0f)
		{
			SnowFrac_01 = SnowRate / PrecipRate_kgm2s;
		}
	}
};
