#include "CalibratedRadiationShader.h"

// Maps to /Plugin/SimulationPixelShader/CalibratedRadiation.usf
IMPLEMENT_GLOBAL_SHADER(FCalibratedRadiationCS, "/Plugin/SimulationPixelShader/CalibratedRadiation.usf", "CalibratedRadiationCS", SF_Compute);
