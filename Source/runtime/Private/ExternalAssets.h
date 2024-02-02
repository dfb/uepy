// routines for dealing with external things like loading image files as textures, or saving textures to disk

#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTargetCube.h"
#include "uepy.h"
#include "ExternalAssets.generated.h"

UCLASS()
class UEPY_API UExternalTextureLoader : public UBackgroundWorker
{
    GENERATED_BODY()

    // info about the image
    FString imagePath;
    TArray64<uint8> uncompressedData;
    int w=-1,h=-1;
    bool loadedOk = false;

public:
    void Start(FString path, py::object& callback);

    DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTheEvent, FString, path, UTexture2D*, texture);
	UPROPERTY() FTheEvent TheEvent;
};

// Update a texture with a new BGRA image. Note that this has to be called on the game thread, or it will cause a crash
void UpdateTextureBGRA(UTexture2D *tex, uint8 *bgra, int32 width, int32 height);

// Create a texture from a 32-bit BGRA impage
UTexture2D *TextureFromBGRA(uint8 *bgra, int32 width, int32 height);

UTexture2D *LoadTextureFromFile(FString path); // DEPRECATED - use the async version!

bool ConvertPNGtoJPG(FString pngPath, FString jpgPath, int quality);

// given a cube texture render target, saves its contents to the given file. Returns true on success.
bool SaveCubeRenderTargetToFile(UTextureRenderTargetCube* target, FString fullPathPrefix);

bool SaveRenderTargetToFile(UTextureRenderTarget* target, FString fullPath);

