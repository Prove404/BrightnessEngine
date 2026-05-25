#pragma once
#include "LandscapeProxy.h"
#include "Engine/Engine.h"

// Inline functions for setting material parameters on landscape
inline void SetVectorParameterValue(ALandscapeProxy* Landscape, FName ParameterName, FLinearColor Value)
{
	if (Landscape)
	{
		Landscape->SetLandscapeMaterialVectorParameterValue(ParameterName, Value);
	}
}

inline void SetTextureParameterValue(ALandscapeProxy* Landscape, FName ParameterName, UTexture* Value, UEngine* Engine)
{
	if (Landscape)
	{
		Landscape->SetLandscapeMaterialTextureParameterValue(ParameterName, Value);
	}
}

inline void SetScalarParameterValue(ALandscapeProxy* Landscape, FName ParameterName, float Value)
{
	if (Landscape)
	{
		Landscape->SetLandscapeMaterialScalarParameterValue(ParameterName, Value);
	}
}

/**
 * Validates that a material has all required parameters for snow simulation.
 * @param Material - The material interface to validate
 * @return true if all required parameters are present, false otherwise
 */
bool CheckMaterialParamsValid(UMaterialInterface* Material);
