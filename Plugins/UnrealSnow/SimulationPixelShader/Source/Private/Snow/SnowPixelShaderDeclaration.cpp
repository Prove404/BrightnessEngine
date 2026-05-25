
#include "SnowPixelShaderDeclaration.h"
#include "../PixelShaderPrivatePCH.h"
#include "ShaderParameterStruct.h"

void FSnowPixelShaderDeclaration::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
}

// Register shaders
IMPLEMENT_GLOBAL_SHADER(FSnowVertexShader, 
    	"/Project/UnrealSnow/SnowPixelShader.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FSnowPixelShaderDeclaration, 
    	"/Project/UnrealSnow/SnowPixelShader.usf", "MainPS", SF_Pixel);