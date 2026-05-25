#include "RadiationIndexShader.h"
#include "ShaderCompilerCore.h"

// Implement shader registration
// Maps to /Plugin/SimulationPixelShader/ComputeRadiationIndex.usf
IMPLEMENT_GLOBAL_SHADER(FComputeRadiationIndexCS, "/Plugin/SimulationPixelShader/ComputeRadiationIndex.usf", "ComputeRadiationIndexCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FComputeReferenceMeanCS, "/Plugin/SimulationPixelShader/ComputeRadiationIndex.usf", "ComputeReferenceMeanCS", SF_Compute);
