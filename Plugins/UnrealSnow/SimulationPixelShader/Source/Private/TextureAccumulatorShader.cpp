#include "TextureAccumulatorShader.h"
#include "RenderGraphUtils.h"

IMPLEMENT_GLOBAL_SHADER(FTextureAccumulateCS, "/Plugin/SimulationPixelShader/TextureAccumulator.usf", "TextureAccumulateCS", SF_Compute);

