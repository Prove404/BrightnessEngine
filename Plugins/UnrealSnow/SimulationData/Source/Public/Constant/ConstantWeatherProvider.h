#pragma once

#include "SimulationWeatherDataProviderBase.h"
#include "ConstantWeatherProvider.generated.h"

/**
* Constant weather provider that provides fixed weather conditions.
* Useful for testing and baseline scenarios.
*/
UCLASS(Blueprintable, BlueprintType)
class SIMULATIONDATA_API UConstantWeatherProvider : public USimulationWeatherDataProviderBase
{
	GENERATED_BODY()

public:
	UConstantWeatherProvider();

	/** Temperature in Celsius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float Temperature_C = -5.0f;

	/** Relative humidity percentage (0-100) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float RH_Percent = 60.0f;

	/** Wind speed in m/s */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float Wind_mps = 2.0f;

	/** Shortwave downward radiation in W/m² */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float SWdown_Wm2 = 230.0f;

	/** Longwave downward radiation in W/m² */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float LWdown_Wm2 = 210.0f;

	/** Precipitation rate in mm/h */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float Precipitation_mmph = 0.0f;

	/** Snow fraction (0-1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float SnowFraction = 0.0f;

	/** Surface pressure in hPa */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float SurfacePressure_hPa = 900.0f;

	virtual void Initialize(FDateTime StartTime, FDateTime EndTime) override;

	virtual float GetMeasurementAltitude() override { return 1000.0f; }

	virtual TResourceArray<FClimateData>* CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime) override;

	virtual FWeatherForcingData GetWeatherForcing(FDateTime Time, int32 GridX = 0, int32 GridY = 0) override;
};
