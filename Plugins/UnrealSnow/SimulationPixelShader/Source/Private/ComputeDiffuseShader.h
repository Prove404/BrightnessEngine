#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// Diffuse computation shader: RTY_Diffuse = max(RTY_Total - RTY_Direct, epsilon)
class SIMULATIONPIXELSHADER_API FComputeDiffuseCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeDiffuseCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeDiffuseCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Texture dimensions
		SHADER_PARAMETER(FUintVector2, TextureDimensions)

		// Enable blur flag
		SHADER_PARAMETER(uint32, bEnableBlur)

		// Solar zenith angle (for nighttime check)
		SHADER_PARAMETER(float, CosSolarZenith)

		// Input textures (R16f single-channel)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTY_Total)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTY_Direct)

		// Output textures (R16f single-channel)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RTY_Diffuse)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RTY_DiffuseTemp)
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

// Blur pass shader (optional, for speckle suppression)
class SIMULATIONPIXELSHADER_API FBlurDiffuseCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBlurDiffuseCS);
	SHADER_USE_PARAMETER_STRUCT(FBlurDiffuseCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Texture dimensions
		SHADER_PARAMETER(FUintVector2, TextureDimensions)

		// Input: temporary diffuse buffer
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTY_DiffuseTemp)

		// Output: final blurred diffuse
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RTY_Diffuse)
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
