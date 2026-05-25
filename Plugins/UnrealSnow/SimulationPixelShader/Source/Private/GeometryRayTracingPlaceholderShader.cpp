#include "GeometryRayTracingPlaceholderShader.h"

IMPLEMENT_GLOBAL_SHADER(
	FGeometryRayTracingPlaceholderCS,
	"/Plugin/SimulationPixelShader/GeometryRayTracingPlaceholder.usf",
	"GeometryRayTracingPlaceholderCS",
	SF_Compute);
