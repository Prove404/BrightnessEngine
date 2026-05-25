#pragma once

#include "SimulationWeatherDataProviderBase.h"
#include "Frost/FrostTypes.h"
#include "CsvWeatherProvider.generated.h"

/**
* CSV weather provider that loads weather data from a CSV file.
* Supports time interpolation and per-cell data.
*/
UCLASS(Blueprintable, BlueprintType)
class SIMULATIONDATA_API UCsvWeatherProvider : public USimulationWeatherDataProviderBase
{
	GENERATED_BODY()

public:
	UCsvWeatherProvider();

	/** Path to the CSV file */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data")
	FFilePath CsvFilePath;

	/** Time format string for parsing timestamps */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data")
	FString TimeFormat = TEXT("yyyy-MM-dd HH:mm");

	/** Whether the CSV contains uniform data for the entire grid */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data")
	bool bUniformGrid = true;

    /** If true, approximate radiation fluxes when the dataset is missing them. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSV")
    bool UseDerivedRadiationIfMissing = true;

    /** Site metadata used by the radiation estimator. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSV")
    FSiteMeta Site;

    /** Precipitation unit scaling factor (multiplier applied to CSV values before time conversion) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSV", meta = (ClampMin = "0.0001", UIMin = "0.0001"))
    float PrecipitationScaleFactor = 1.0f;

	/** Weather station/measurement elevation in meters above sea level (or datum).
	 * This should match the elevation at which the weather data was measured.
	 * For ERA5: use the grid point elevation from the ERA5 data.
	 * For station data: use the station elevation from metadata. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSV", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float MeasurementAltitudeMeters = 1000.0f;

	/** Expected CSV header (case-insensitive):
	* time, T2m_C, RH_pct, Wind_mps, SWdown_Wm2, LWdown_Wm2, Precip_mmph, SnowFrac_0_1
	* Optional columns for per-cell data: i, j
	*/

	virtual void Initialize(FDateTime StartTime, FDateTime EndTime) override;

	virtual float GetMeasurementAltitude() override { return MeasurementAltitudeMeters * 100.0f; }  // Convert m to cm

	virtual TResourceArray<FClimateData>* CreateRawClimateDataResourceArray(FDateTime StartTime, FDateTime EndTime) override;

	virtual FWeatherForcingData GetWeatherForcing(FDateTime Time, int32 GridX = 0, int32 GridY = 0) override;

private:
	/** Parsed weather records */
	TArray<FWeatherForcingData> WeatherRecords;

	/** Load and parse the CSV file */
	bool LoadCsvData();


	/** Find weather data records bracketing the given time */
	void FindBracketingRecords(FDateTime Time, int32& OutIndex1, int32& OutIndex2, float& OutAlpha);

	/** Interpolate between two weather records */
	FWeatherForcingData InterpolateRecords(const FWeatherForcingData& Record1, const FWeatherForcingData& Record2, float Alpha);
};


