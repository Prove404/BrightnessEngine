#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// Radiation index computation shader
class SIMULATIONPIXELSHADER_API FComputeRadiationIndexCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeRadiationIndexCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeRadiationIndexCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Texture dimensions
		SHADER_PARAMETER(FUintVector2, TextureDimensions)

		// Reference means (from reference patch)
		SHADER_PARAMETER(float, ReferenceMean_Direct)
		SHADER_PARAMETER(float, ReferenceMean_Diffuse)

		// Border columns (left edge strip)
		SHADER_PARAMETER(uint32, BorderRows)

		// Scalar mask (0-1) describing whether the sun is above the visibility threshold
		SHADER_PARAMETER(float, SunVisibility)

		// Input textures (R16f single-channel luminance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTY_Direct)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTY_Diffuse)

		// Output textures (R16f single-channel dimensionless indices)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RTF_DirIndex)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RTF_DiffIndex)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Params)
	{
		return IsFeatureLevelSupported(Params.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), 8);
	}
};

// Reference mean computation shader
class SIMULATIONPIXELSHADER_API FComputeReferenceMeanCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeReferenceMeanCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeReferenceMeanCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Texture dimensions
		SHADER_PARAMETER(FUintVector2, TextureDimensions)

		// Input textures (R16f single-channel luminance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTY_Direct)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTY_Diffuse)

		// Partial sum output buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, PartialSums_Direct)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, PartialSums_Diffuse)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Params)
	{
		return IsFeatureLevelSupported(Params.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), 8);
	}
};
