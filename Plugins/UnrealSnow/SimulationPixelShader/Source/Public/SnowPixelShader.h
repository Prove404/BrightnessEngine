

#pragma once

#include "Private/Snow/SnowPixelShaderDeclaration.h"
#include "RWStructuredBuffer.h"

/***************************************************************************/
/* This class demonstrates how to use the pixel shader we have declared.   */
/* Most importantly which RHI functions are needed to call and how to get  */
/* some interesting output.                                                */
/***************************************************************************/
class SIMULATIONPIXELSHADER_API FSnowPixelShader
{
public:
	FSnowPixelShader(ERHIFeatureLevel::Type ShaderFeatureLevel);
	~FSnowPixelShader();

	/**
	* Let the user change render target during runtime if they want to.
	* @param RenderTarget - This is the output render target
	*/
	void ExecutePixelShader(UTextureRenderTarget2D* RenderTarget, bool SaveSnowMap);

	/**
	* Only execute this from the render thread
	*/
	void ExecutePixelShaderInternal(bool SaveSnowMap);

	/** Initializes the simulation with the correct input data. */
	void Initialize(FRWStructuredBuffer* SnowBuffer, FRWStructuredBuffer* MaxSnowBuffer, int32 InCellsDimensionX, int32 InCellsDimensionY);

private:
	bool bIsPixelShaderExecuting;
	bool bMustRegenerateSRV;
	bool bIsUnloading;
	bool bSave;

	int32 ClimateDataDimension;
	int32 CellsDimensionX;
	int32 CellsDimensionY;
	ERHIFeatureLevel::Type FeatureLevel;

	/** Main texture */
	FTextureRHIRef CurrentTexture;
	UTextureRenderTarget2D* CurrentRenderTarget;

	/** Input snow map buffer. */
	FRWStructuredBuffer* SnowInputBuffer;

	/** Input snow max buffer. */
	FRWStructuredBuffer* MaxSnowInputBuffer;
};
