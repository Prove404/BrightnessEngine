#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// Shader for extracting per-cell radiation indices from luminance texture
class SIMULATIONPIXELSHADER_API FExtractRadiationIndicesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FExtractRadiationIndicesCS);
	SHADER_USE_PARAMETER_STRUCT(FExtractRadiationIndicesCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Grid dimensions
		SHADER_PARAMETER(uint32, GridDimX)
		SHADER_PARAMETER(uint32, GridDimY)
		SHADER_PARAMETER(uint32, NumCells)

		// Reference luminance and configuration
		SHADER_PARAMETER(float, ReferenceMean)
		SHADER_PARAMETER(uint32, BorderRows)
		SHADER_PARAMETER(float, MaxRadiationIndex)
		SHADER_PARAMETER(float, SunVisibility)

		// Hock Model 3 parameters for low sun angle fallback
		// MeasuredGHI: measured global horizontal irradiance (I) [W/m²]
		// ClearSkyReference: potential clear-sky direct on horizontal (Is) [W/m²]
		// RTF_Etot contains: UE scene radiation on slope (Gs) [W/m²]
		// Formula: RadiationFactor = I * (Gs/Is)
		SHADER_PARAMETER(float, MeasuredGHI_Wm2)
		SHADER_PARAMETER(float, ClearSkyReference_Wm2)

		// Input source selection
		// If 1, read from RTF_Etot (physical irradiance W/m²); if 0, read from RTY_Total (luminance cd/m²)
		SHADER_PARAMETER(uint32, bUsePhysicalIrradiance)

		// Input textures
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTY_Total)  // Luminance [cd/m²]
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, RTF_Etot)   // Physical irradiance [W/m²]

		// Per-cell pixel coordinate mapping
		SHADER_PARAMETER_SRV(StructuredBuffer<uint2>, CellPixelCoords)

		// Output buffer (per-cell radiation indices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RadiationIndexBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Params)
	{
		return IsFeatureLevelSupported(Params.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		// Use 256 threads per group for efficient linear cell processing
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 256);
	}
};
