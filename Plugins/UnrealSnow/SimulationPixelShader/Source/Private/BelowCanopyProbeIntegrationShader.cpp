#include "BelowCanopyProbeIntegrationShader.h"

IMPLEMENT_GLOBAL_SHADER(FBelowCanopyProbeFaceStatsCS,   "/Plugin/SimulationPixelShader/BelowCanopyProbeFaceStats.usf",              "BelowCanopyProbeFaceStatsCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBelowCanopyProbeThresholdCS,   "/Plugin/SimulationPixelShader/BelowCanopyProbeThreshold.usf",              "BelowCanopyProbeThresholdCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBelowCanopyProbeIntegrateCS,   "/Plugin/SimulationPixelShader/IntegrateBelowCanopyProbeIrradiance.usf",    "BelowCanopyProbeIntegrateCS",   SF_Compute);
