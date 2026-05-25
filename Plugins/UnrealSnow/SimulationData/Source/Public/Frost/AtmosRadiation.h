#pragma once

#include "CoreMinimal.h"
#include "FrostTypes.h"
#include "AtmosRadiation.generated.h"

UCLASS()
class SIMULATIONDATA_API UAtmosRadiation : public UObject
{
	GENERATED_BODY()

public:
	/** Estimate downward shortwave and longwave radiation when observations are unavailable. */
	UFUNCTION(BlueprintCallable, Category="Frost|Radiation")
	static void EstimateSWLW(const FDateTime& TimeUTC, const FSiteMeta& Site,
		float T_C, float RH_pct,
		float& SW_Wm2, float& LW_Wm2, float CloudCover_pct = -1.0f);
};
