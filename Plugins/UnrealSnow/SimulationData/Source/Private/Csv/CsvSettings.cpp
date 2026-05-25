#include "Csv/CsvSettings.h"

UCsvSettings::UCsvSettings()
{
    CategoryName = TEXT("CSV (Weather)");
    InputCsvPath.FilePath = TEXT("{ProjectDir}/ForcingData/CSV/meteoAlptal2004_2005.csv");
    OutputJsonPath.FilePath = TEXT("{ProjectDir}/analysis_results/csv_weather.json");
    bTreatSimulationTimesAsLocal = false;
    SimulationUtcOffsetHours = 0;
    Schema = ECsvSchema::Auto;
    TimeColumnName = TEXT("");
}

FName UCsvSettings::GetCategoryName() const
{
    return FName(TEXT("Project"));
}


