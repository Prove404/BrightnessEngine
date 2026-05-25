#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// UE5-compliant compute shader class
class FComputeShaderDeclaration : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeShaderDeclaration);
	SHADER_USE_PARAMETER_STRUCT(FComputeShaderDeclaration, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Constant parameters
		SHADER_PARAMETER(int32, TotalSimulationHours)
		SHADER_PARAMETER(int32, CellsDimensionX)
		SHADER_PARAMETER(float, ThreadGroupCountX)
		SHADER_PARAMETER(float, ThreadGroupCountY)
		SHADER_PARAMETER(float, TSnowA)
		SHADER_PARAMETER(float, TSnowB)
		SHADER_PARAMETER(float, TMeltA)
		SHADER_PARAMETER(float, TMeltB)
		SHADER_PARAMETER(float, k_e)
		SHADER_PARAMETER(float, k_m)
		SHADER_PARAMETER(float, MeasurementAltitude)
		SHADER_PARAMETER(float, SlopeThreshold)
		SHADER_PARAMETER(float, FreshSnowAlbedo)
		SHADER_PARAMETER(float, OldSnowAlbedo)
		SHADER_PARAMETER(uint32, bUseWeatherSnowFraction)
		SHADER_PARAMETER(uint32, bDisableLapseRateAdjustments)
		// Variable parameters
		SHADER_PARAMETER(int32, CurrentSimulationStep)
		SHADER_PARAMETER(int32, Timesteps)
		SHADER_PARAMETER(int32, DayOfYear)
		SHADER_PARAMETER(int32, HourOfDay)
		SHADER_PARAMETER(float, WeatherSnowFrac)
		SHADER_PARAMETER(float, PrecipRate_kgm2s)
		// UAVs
		SHADER_PARAMETER_UAV(RWTexture2D<uint>, OutputSurface)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<SimulationCell>, SimulationCellsBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<WeatherData>, WeatherDataBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, MaxSnowBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, SnowOutputBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Params)
	{
		return IsFeatureLevelSupported(Params.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};
