#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

class SIMULATIONPIXELSHADER_API FTextureAccumulateCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTextureAccumulateCS);
	SHADER_USE_PARAMETER_STRUCT(FTextureAccumulateCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FUintVector2, TextureDimensions)
		SHADER_PARAMETER(float, Weight)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SourceTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, AccumulatorTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), 8);
	}
};


