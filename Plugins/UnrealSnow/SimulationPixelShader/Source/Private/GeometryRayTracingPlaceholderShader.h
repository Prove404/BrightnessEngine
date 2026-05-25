#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

/**
 * Placeholder compute shader reserved for the future full-scene hardware ray-tracing
 * geometry backend used by SVF_UE/SWdir_UE exports. The actor-side mode and export
 * plumbing can target this module before the final ray-generation pass is wired.
 */
class SIMULATIONPIXELSHADER_API FGeometryRayTracingPlaceholderCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGeometryRayTracingPlaceholderCS);
	SHADER_USE_PARAMETER_STRUCT(FGeometryRayTracingPlaceholderCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FUintVector4, PlaceholderInfo)
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
