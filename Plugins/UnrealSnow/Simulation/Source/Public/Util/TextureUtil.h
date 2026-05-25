#pragma once
#include "Engine/Texture2D.h"

inline void UpdateTexture(UTexture2D* Texture, TArray<FColor>& TextureData)
{
	FUpdateTextureRegion2D* RegionData = new FUpdateTextureRegion2D(0, 0, 0, 0, Texture->GetSizeX(), Texture->GetSizeY());

	auto CleanupFunction = [](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
	{
		delete Regions;
	};

	// Update the texture
	Texture->UpdateTextureRegions(
		0, 1,
		RegionData, Texture->GetSizeX() * 4, 4,
		(uint8*)TextureData.GetData(),
		CleanupFunction
		);
}

// Updates a PF_R16F texture from float meters by converting to half and using correct strides
inline void UpdateTextureR16FFromFloat(UTexture2D* Texture, int32 Width, int32 Height, const TArray<float>& DepthMeters)
{
    if (!Texture || Width <= 0 || Height <= 0) return;

    FUpdateTextureRegion2D* RegionData = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);

    // Allocate heap buffer so render thread can free it after upload
    const int32 Count = Width * Height;
    FFloat16* HalfData = new FFloat16[Count];
    const int32 SrcCount = DepthMeters.Num();
    for (int32 i = 0; i < Count; ++i)
    {
        const float v = (i < SrcCount) ? DepthMeters[i] : 0.0f;
        HalfData[i] = FFloat16(v);
    }

    auto CleanupFunction = [](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
    {
        delete[] reinterpret_cast<FFloat16*>(SrcData);
        delete Regions;
    };

    const uint32 SrcBpp = sizeof(FFloat16);
    const uint32 SrcPitch = Width * sizeof(FFloat16);

    Texture->UpdateTextureRegions(
        0, 1,
        RegionData, SrcPitch, SrcBpp,
        reinterpret_cast<uint8*>(HalfData),
        CleanupFunction
    );
}

// Updates a PF_R16F texture directly from half-float meters using correct strides
inline void UpdateTextureR16FFromHalf(UTexture2D* Texture, int32 Width, int32 Height, const TArray<FFloat16>& DepthMeters)
{
    if (!Texture || Width <= 0 || Height <= 0) return;

    FUpdateTextureRegion2D* RegionData = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);

    // Allocate and copy so we can free after render thread upload
    const int32 Count = Width * Height;
    FFloat16* HalfData = new FFloat16[Count];
    const int32 SrcCount = DepthMeters.Num();
    for (int32 i = 0; i < Count; ++i)
    {
        HalfData[i] = (i < SrcCount) ? DepthMeters[i] : FFloat16(0.0f);
    }

    auto CleanupFunction = [](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
    {
        delete[] reinterpret_cast<FFloat16*>(SrcData);
        delete Regions;
    };

    const uint32 SrcBpp = sizeof(FFloat16);
    const uint32 SrcPitch = Width * sizeof(FFloat16);

    Texture->UpdateTextureRegions(
        0, 1,
        RegionData, SrcPitch, SrcBpp,
        reinterpret_cast<uint8*>(HalfData),
        CleanupFunction
    );
}