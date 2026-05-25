// ExtractRadiationShader.cpp
// Shader implementation for efficient per-cell radiation index extraction

#include "ExtractRadiationShader.h"
#include "ShaderCompilerCore.h"

// Register the global shader with Unreal's shader system
// Maps C++ class to USF file
IMPLEMENT_GLOBAL_SHADER(FExtractRadiationIndicesCS, "/Plugin/SimulationPixelShader/ExtractRadiationIndices.usf", "ExtractRadiationIndicesCS", SF_Compute);
