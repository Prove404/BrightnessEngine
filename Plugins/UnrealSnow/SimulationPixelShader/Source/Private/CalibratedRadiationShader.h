#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

/**
 * GPU shader that scales UE luminance buffers to calibrated irradiance.
 * Inputs: RTY_Direct/RTY_Diffuse (R16f SRV)
 * Outputs: RTF_Etot (R32f UAV), RTF_rUE_cal (R16f UAV)
 */
class SIMULATIONPIXELSHADER_API FCalibratedRadiationCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCalibratedRadiationCS);
	SHADER_USE_PARAMETER_STRUCT(FCalibratedRadiationCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Render target dimensions
		SHADER_PARAMETER(FUintVector2, TextureDimensions)

		// Scalar calibration factors computed on CPU
		SHADER_PARAMETER(float, DirectScale)
		SHADER_PARAMETER(float, DiffuseScale)

		// Reciprocal global irradiance (avoids division in the shader)
		SHADER_PARAMETER(float, InvGlobalIrradiance)

		// Input luminance buffers (R16f)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTY_Direct)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTY_Diffuse)

		// Output irradiance/index buffers
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RTF_Etot)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RTF_rUE_cal)
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
