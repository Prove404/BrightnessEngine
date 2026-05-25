#include "ComputeDiffuseShader.h"
#include "ShaderCompilerCore.h"

// Implement shader registration
// Maps to /Plugin/SimulationPixelShader/ComputeDiffuse.usf
IMPLEMENT_GLOBAL_SHADER(FComputeDiffuseCS, "/Plugin/SimulationPixelShader/ComputeDiffuse.usf", "ComputeDiffuseCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBlurDiffuseCS, "/Plugin/SimulationPixelShader/ComputeDiffuse.usf", "BlurDiffuseCS", SF_Compute);
