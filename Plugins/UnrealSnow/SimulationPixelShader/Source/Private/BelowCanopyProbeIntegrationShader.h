#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// ---------------------------------------------------------------------------
// Pass 1: per-face luminance statistics → used to compute SkyLuminanceThreshold
// ---------------------------------------------------------------------------
class SIMULATIONPIXELSHADER_API FBelowCanopyProbeFaceStatsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBelowCanopyProbeFaceStatsCS);
	SHADER_USE_PARAMETER_STRUCT(FBelowCanopyProbeFaceStatsCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FUintVector2, TextureDimensions)
		SHADER_PARAMETER(uint32, NumThreadGroupsX)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InputTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, OutStats)
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

// ---------------------------------------------------------------------------
// Pass 2: single-thread threshold reduction (stats → one float)
// ---------------------------------------------------------------------------
class SIMULATIONPIXELSHADER_API FBelowCanopyProbeThresholdCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBelowCanopyProbeThresholdCS);
	SHADER_USE_PARAMETER_STRUCT(FBelowCanopyProbeThresholdCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumStats)
		SHADER_PARAMETER(float, ThresholdFraction)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, InStats)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, OutThreshold)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

// ---------------------------------------------------------------------------
// Pass 3: cosine-weighted irradiance integration with HPEval-style sky masking
// ---------------------------------------------------------------------------
class SIMULATIONPIXELSHADER_API FBelowCanopyProbeIntegrateCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBelowCanopyProbeIntegrateCS);
	SHADER_USE_PARAMETER_STRUCT(FBelowCanopyProbeIntegrateCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FUintVector2, TextureDimensions)
		SHADER_PARAMETER(uint32, NumThreadGroupsX)
		SHADER_PARAMETER(float, HorizontalFovDegrees)
		SHADER_PARAMETER(float, FaceForwardX)
		SHADER_PARAMETER(float, FaceForwardY)
		SHADER_PARAMETER(float, FaceForwardZ)
		SHADER_PARAMETER(float, FaceUpX)
		SHADER_PARAMETER(float, FaceUpY)
		SHADER_PARAMETER(float, FaceUpZ)
		SHADER_PARAMETER(float, ReceiverNormalX)
		SHADER_PARAMETER(float, ReceiverNormalY)
		SHADER_PARAMETER(float, ReceiverNormalZ)
		SHADER_PARAMETER(uint32, bUseSkyMask)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, SkyThresholdBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InputTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, PartialSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, TerrainPartialSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, SkyFractionPartialSums)
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
