#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "ClimateData.h"

namespace FWeatherLegacyConversion
{
	/** Allocate and populate a legacy climate data resource array from forcing data. */
	SIMULATIONDATA_API TResourceArray<FClimateData>* CreateLegacyClimateArray(const TArray<FWeatherForcingData>& ForcingSeries);
}
