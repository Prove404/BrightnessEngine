#pragma once

#include "../VertexShader.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"


/**
* A simple passthrough vertexshader that we will use.
*/
class FSnowVertexShader : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSnowVertexShader);
public:

	static bool ShouldCache(EShaderPlatform Platform) { return true; }

	FSnowVertexShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FGlobalShader(Initializer)
	{}
	FSnowVertexShader() {}
};

/**
* This class is what encapsulates the shader in the engine.
* It is the main bridge between the HLSL located in the engine directory
* and the engine itself.
*/
class FSnowPixelShaderDeclaration : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSnowPixelShaderDeclaration);
	SHADER_USE_PARAMETER_STRUCT(FSnowPixelShaderDeclaration, FGlobalShader);

public:

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, ClimateDataDimension)
		SHADER_PARAMETER(int32, CellsDimensionX)
		SHADER_PARAMETER(int32, CellsDimensionY)
		SHADER_PARAMETER_SRV(StructuredBuffer<float>, SnowInputBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, MaxSnowInputBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Params) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

