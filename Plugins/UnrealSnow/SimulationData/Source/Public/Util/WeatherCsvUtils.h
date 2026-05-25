#pragma once

#include "CoreMinimal.h"
#include "ClimateData.h"

namespace FWeatherCsvUtilities
{
    /** Clamp/partition snow fraction using air temperature so warm hours stay rain-only. */
    SIMULATIONDATA_API void ApplyTemperatureSnowPartition(FWeatherForcingData& Forcing);

	/** Parse a single CSV line (matching the SimulationData weather schema) into forcing data. */
	SIMULATIONDATA_API bool ParseWeatherCsvLine(const FString& Line, FWeatherForcingData& OutData, float PrecipitationScaleFactor = 1.0f);

	/** Parse CSV content into ordered weather forcing records (skips header, sorts by timestamp). */
    SIMULATIONDATA_API bool ParseWeatherCsvContent(const FString& CsvContent, TArray<FWeatherForcingData>& OutRecords, float PrecipitationScaleFactor = 1.0f);

	/** Read and parse a CSV file into ordered weather forcing records. */
	SIMULATIONDATA_API bool ParseWeatherCsvFile(const FString& FilePath, TArray<FWeatherForcingData>& OutRecords, float PrecipitationScaleFactor = 1.0f);
}
