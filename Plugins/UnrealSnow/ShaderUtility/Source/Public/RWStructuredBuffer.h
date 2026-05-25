#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "RHICommandList.h"
#include "RHIDefinitions.h"
#include "RHIBufferInitializer.h"
#include "PixelFormat.h"

/** Encapsulates a GPU read/write structured buffer with its UAV and SRV. */
struct FRWStructuredBuffer
{
	FRHIBuffer* Buffer;
	FUnorderedAccessViewRHIRef UAV;
	FShaderResourceViewRHIRef SRV;
	uint32 NumBytes;
	FRWStructuredBuffer() : Buffer(nullptr), NumBytes(0) {}

	void Initialize(FRHICommandListBase& RHICmdList, uint32 BytesPerElement, uint32 NumElements, FResourceArrayInterface* Data = nullptr, uint32 AdditionalUsage = 0, bool bUseUavCounter = false, bool bAppendBuffer = false)
	{
		check(GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5);
		NumBytes = BytesPerElement * NumElements;

		// Create buffer description
		FRHIBufferCreateDesc Desc;
		Desc.DebugName = TEXT("RWStructuredBuffer");
		Desc.Size = NumBytes;
		Desc.Usage = EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource;
		if ((AdditionalUsage & static_cast<uint32>(EBufferUsageFlags::Static)) != 0)
		{
			Desc.Usage |= EBufferUsageFlags::Static;
		}
		if ((AdditionalUsage & static_cast<uint32>(EBufferUsageFlags::Dynamic)) != 0)
		{
			Desc.Usage |= EBufferUsageFlags::Dynamic;
		}
		Desc.Stride = BytesPerElement;

		// Create buffer using UE 5.6 API - UPDATED FOR UE 5.6 COMPATIBILITY
		if (Data)
		{
			Desc.SetInitActionResourceArray(Data);
		}

		Buffer = RHICmdList.CreateBuffer(Desc);

		// Create UAV
		UAV = RHICmdList.CreateUnorderedAccessView(Buffer,
			FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(BytesPerElement)
				.SetNumElements(NumElements));

		// Create SRV
		SRV = RHICmdList.CreateShaderResourceView(Buffer,
			FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(BytesPerElement)
				.SetNumElements(NumElements));
	}

	void Release()
	{
		NumBytes = 0;
		Buffer = nullptr;
		UAV.SafeRelease();
		SRV.SafeRelease();
	}
};