#include "ComputeShaderDeclaration.h"
#include "Simulation.h"
#include "ShaderParameterStruct.h"

void FComputeShaderDeclaration::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
}

// Register compute shader (source file lives under Shaders/UnrealSnow in the project)
IMPLEMENT_GLOBAL_SHADER(FComputeShaderDeclaration,
	"/Project/UnrealSnow/DegreeDaySimulationComputeShader.usf", "MainComputeShader", SF_Compute);

// This is required for the plugin to build
IMPLEMENT_MODULE(FDefaultModuleImpl, Simulation)
