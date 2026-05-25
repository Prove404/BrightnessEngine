#include "RGBToLuminanceShader.h"
#include "ShaderCompilerCore.h"

// Implement shader registration
// Maps to /Plugin/SimulationPixelShader/RGBToLuminance.usf
IMPLEMENT_GLOBAL_SHADER(FRGBToLuminanceCS, "/Plugin/SimulationPixelShader/RGBToLuminance.usf", "RGBToLuminanceCS", SF_Compute);
