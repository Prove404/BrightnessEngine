#include "SnowPixelShader.h"
#include "../PixelShaderPrivatePCH.h"
#include "SnowPixelShaderDeclaration.h"
#include "PipelineStateCache.h"
#include "SceneUtils.h"
#include "RHIResourceUtils.h"

#define WRITE_SNOW_MAP false

//It seems to be the convention to expose all vertex declarations as globals, and then reference them as externs in the headers where they are needed.
//It kind of makes sense since they do not contain any parameters that change and are purely used as their names suggest, as declarations :)
TGlobalResource<FTextureVertexDeclaration> GSnowTextureVertexDeclaration;

FSnowPixelShader::FSnowPixelShader(ERHIFeatureLevel::Type ShaderFeatureLevel)
{
	FeatureLevel = ShaderFeatureLevel;

	ClimateDataDimension = 0;
	CellsDimensionX = 0;
	CellsDimensionY = 0;
	
	bMustRegenerateSRV = false;
	bIsPixelShaderExecuting = false;
	bIsUnloading = false;
	bSave = false;

	CurrentTexture = NULL;
	CurrentRenderTarget = NULL;
}

void FSnowPixelShader::Initialize(FRWStructuredBuffer* SnowBuffer, FRWStructuredBuffer* MaxSnowBuffer, int32 InCellsDimensionX, int32 InCellsDimensionY)
{
	this->SnowInputBuffer = SnowBuffer;
	this->MaxSnowInputBuffer = MaxSnowBuffer;

	this->CellsDimensionX = InCellsDimensionX;
	this->CellsDimensionY = InCellsDimensionY;
}

FSnowPixelShader::~FSnowPixelShader()
{
	bIsUnloading = true;
}

void FSnowPixelShader::ExecutePixelShader(UTextureRenderTarget2D* RenderTarget, bool SaveSnowMap)
{
	if (bIsUnloading || bIsPixelShaderExecuting) //Skip this execution round if we are already executing
	{
		return;
	}

	bIsPixelShaderExecuting = true;

	CurrentRenderTarget = RenderTarget;

	ENQUEUE_RENDER_COMMAND(ExecuteSnowPixelShader)(
		[this, SaveSnowMap](FRHICommandListImmediate& RHICmdList)
		{
			this->ExecutePixelShaderInternal(SaveSnowMap);
		}
	);
}

void FSnowPixelShader::ExecutePixelShaderInternal(bool SaveSnowMap)
{
	check(IsInRenderingThread());

	// Only cleanup
	if (bIsUnloading) 
	{
		return;
	}

	FRHICommandListImmediate& RHICmdList = GRHICommandList.GetImmediateCommandList();

	CurrentTexture = CurrentRenderTarget->GetRenderTargetResource()->GetRenderTargetTexture();

	// Set up render targets
	FRHIRenderPassInfo RPInfo(CurrentTexture, ERenderTargetActions::Load_Store);
	RHICmdList.BeginRenderPass(RPInfo, TEXT("SnowPixelShader"));

	// Set graphics pipeline state
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
	GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
	
	TShaderMapRef<FSnowVertexShader> VertexShader(GetGlobalShaderMap(FeatureLevel));
	// Note: In UE 5.6, custom pixel shaders need to be handled differently
	// For now, we'll use a simplified approach
	auto PixelShader = GetGlobalShaderMap(FeatureLevel)->GetShader<FSnowPixelShaderDeclaration>();

	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GSnowTextureVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

	// Bind parameters using parameter struct
	FSnowPixelShaderDeclaration::FParameters Parameters;
	Parameters.ClimateDataDimension = ClimateDataDimension;
	Parameters.CellsDimensionX = CellsDimensionX;
	Parameters.CellsDimensionY = CellsDimensionY;
	Parameters.SnowInputBuffer = SnowInputBuffer ? SnowInputBuffer->SRV : nullptr;
	Parameters.MaxSnowInputBuffer = MaxSnowInputBuffer ? MaxSnowInputBuffer->SRV : nullptr;

	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), Parameters);

	// Draw a fullscreen quad that we can run our pixel shader on
	TResourceArray<FTextureVertex> Vertices;
	Vertices.AddUninitialized(4);
	Vertices[0].Position = FVector4(-1.0f, 1.0f, 0, 1.0f);
	Vertices[1].Position = FVector4(1.0f, 1.0f, 0, 1.0f);
	Vertices[2].Position = FVector4(-1.0f, -1.0f, 0, 1.0f);
	Vertices[3].Position = FVector4(1.0f, -1.0f, 0, 1.0f);
	Vertices[0].UV = FVector2D(0, 0);
	Vertices[1].UV = FVector2D(CellsDimensionX, 0);
	Vertices[2].UV = FVector2D(0, CellsDimensionY);
	Vertices[3].UV = FVector2D(CellsDimensionX, CellsDimensionY);

	// Draw the fullscreen quad using immediate mode
	FRHIBufferCreateDesc CreateDesc;
	CreateDesc.DebugName = TEXT("SnowQuadVB");
	CreateDesc.Size = Vertices.GetResourceDataSize();
	CreateDesc.Usage = BUF_Volatile;
	CreateDesc.Stride = sizeof(FTextureVertex);
	CreateDesc.SetInitActionResourceArray(&Vertices);
	FBufferRHIRef VertexBufferRHI = RHICmdList.CreateBuffer(CreateDesc);

	RHICmdList.SetStreamSource(0, VertexBufferRHI, 0);
	RHICmdList.DrawPrimitive(0, 2, 1);

	// End render pass
	RHICmdList.EndRenderPass();
	 
	bIsPixelShaderExecuting = false;
	
	if (SaveSnowMap) 
	{
		TArray<FColor> Bitmap;

		FReadSurfaceDataFlags ReadDataFlags;
		ReadDataFlags.SetLinearToGamma(false);
		ReadDataFlags.SetOutputStencil(false);
		ReadDataFlags.SetMip(0);

		//This is pretty straight forward. Since we are using a standard format, we can use this convenience function instead of having to lock rect.
		RHICmdList.ReadSurfaceData(CurrentTexture, FIntRect(0, 0, CurrentTexture->GetSizeX(), CurrentTexture->GetSizeY()), Bitmap, ReadDataFlags);

		// if the format and texture type is supported
		if (Bitmap.Num())
		{
			// Create screenshot folder if not already present.
			IFileManager::Get().MakeDirectory(*FPaths::ScreenShotDir(), true);

			const FString ScreenFileName(FPaths::ScreenShotDir() / TEXT("SnowMap"));

			uint32 ExtendXWithMSAA = Bitmap.Num() / CurrentTexture->GetSizeY();

			// Save the contents of the array to a bitmap file. (24bit only so alpha channel is dropped)
			FFileHelper::CreateBitmap(*ScreenFileName, ExtendXWithMSAA, CurrentTexture->GetSizeY(), Bitmap.GetData());

		}
		else
		{
			UE_LOG(LogConsoleResponse, Error, TEXT("Failed to save BMP, format or texture type is not supported"));
		}
	}
}



