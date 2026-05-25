#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// RGB to Luminance conversion shader
class SIMULATIONPIXELSHADER_API FRGBToLuminanceCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRGBToLuminanceCS);
	SHADER_USE_PARAMETER_STRUCT(FRGBToLuminanceCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Texture dimensions
		SHADER_PARAMETER(FUintVector2, TextureDimensions)

		// Input texture (RGBA16f)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InputTexture)

		// Output texture (R16f single-channel)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
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
