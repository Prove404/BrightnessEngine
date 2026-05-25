#include "RuntimeMaterialChange.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"

// Required material parameter names for snow simulation
static const TArray<FName> RequiredTextureParams = {
	TEXT("SnowDepthTex")
};

static const TArray<FName> RequiredScalarParams = {
	TEXT("SnowDisplacementScale")
};

static const TArray<FName> RequiredVectorParams = {
	TEXT("SnowOriginMeters"),
	TEXT("SnowInvSizePerMeter")
};

// Optional material parameter names (warn if missing but don't fail)
static const TArray<FName> OptionalScalarParams = {
	TEXT("Albedo_WSA"),
	TEXT("Albedo_BSA"),
	TEXT("SnowRoughness"),
	TEXT("SparkleIntensity"),
	TEXT("SparkleScale"),
	TEXT("SnowAgeDays"),
	TEXT("GrainSize_um"),
	TEXT("Impurity_ppm")
};

bool CheckMaterialParamsValid(UMaterialInterface* Material)
{
	if (!Material)
	{
		UE_LOG(LogTemp, Error, TEXT("[Snow] Material validation failed: Material is null"));
		return false;
	}

	bool bAllRequiredParamsFound = true;

	// Check texture parameters
	TArray<FMaterialParameterInfo> TextureInfos;
	TArray<FGuid> TextureIds;
	Material->GetAllTextureParameterInfo(TextureInfos, TextureIds);
	
	for (const FName& RequiredParam : RequiredTextureParams)
	{
		bool bFound = false;
		for (const FMaterialParameterInfo& Info : TextureInfos)
		{
			if (Info.Name == RequiredParam)
			{
				bFound = true;
				break;
			}
		}
		
		if (!bFound)
		{
			UE_LOG(LogTemp, Error, TEXT("[Snow] Material validation failed: Missing required texture parameter '%s'"), *RequiredParam.ToString());
			bAllRequiredParamsFound = false;
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("[Snow] Material validation: Found required texture parameter '%s'"), *RequiredParam.ToString());
		}
	}

	// Check scalar parameters
	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid> ScalarIds;
	Material->GetAllScalarParameterInfo(ScalarInfos, ScalarIds);
	
	for (const FName& RequiredParam : RequiredScalarParams)
	{
		bool bFound = false;
		for (const FMaterialParameterInfo& Info : ScalarInfos)
		{
			if (Info.Name == RequiredParam)
			{
				bFound = true;
				break;
			}
		}
		
		if (!bFound)
		{
			UE_LOG(LogTemp, Error, TEXT("[Snow] Material validation failed: Missing required scalar parameter '%s'"), *RequiredParam.ToString());
			bAllRequiredParamsFound = false;
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("[Snow] Material validation: Found required scalar parameter '%s'"), *RequiredParam.ToString());
		}
	}

	// Check vector parameters
	TArray<FMaterialParameterInfo> VectorInfos;
	TArray<FGuid> VectorIds;
	Material->GetAllVectorParameterInfo(VectorInfos, VectorIds);
	
	for (const FName& RequiredParam : RequiredVectorParams)
	{
		bool bFound = false;
		for (const FMaterialParameterInfo& Info : VectorInfos)
		{
			if (Info.Name == RequiredParam)
			{
				bFound = true;
				break;
			}
		}
		
		if (!bFound)
		{
			UE_LOG(LogTemp, Error, TEXT("[Snow] Material validation failed: Missing required vector parameter '%s'"), *RequiredParam.ToString());
			bAllRequiredParamsFound = false;
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("[Snow] Material validation: Found required vector parameter '%s'"), *RequiredParam.ToString());
		}
	}

	// Check optional parameters (warn only)
	for (const FName& OptionalParam : OptionalScalarParams)
	{
		bool bFound = false;
		for (const FMaterialParameterInfo& Info : ScalarInfos)
		{
			if (Info.Name == OptionalParam)
			{
				bFound = true;
				break;
			}
		}
		
		if (!bFound)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Snow] Material validation: Missing optional scalar parameter '%s'"), *OptionalParam.ToString());
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("[Snow] Material validation: Found optional scalar parameter '%s'"), *OptionalParam.ToString());
		}
	}

	if (bAllRequiredParamsFound)
	{
		UE_LOG(LogTemp, Display, TEXT("[Snow] Material validation: All required parameters found"));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[Snow] Material validation: Some required parameters are missing - simulation may not work correctly"));
	}

	return bAllRequiredParamsFound;
}
