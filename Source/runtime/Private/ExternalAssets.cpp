#include "ExternalAssets.h"
#include "common.h"
#include "Async/Async.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "CubemapUnwrapUtils.h"

// Update a texture with a new BGRA image. Note that this has to be called on the game thread, or it will cause a crash
void UpdateTextureBGRA(UTexture2D *tex, uint8 *bgra, int32 width, int32 height)
{
    void *textureData = tex->PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(textureData, bgra, width * height * 4);
    tex->PlatformData->Mips[0].BulkData.Unlock();
    tex->UpdateResource();
}

// Create a texture from a 32-bit BGRA impage
UTexture2D *TextureFromBGRA(uint8 *bgra, int32 width, int32 height)
{
    UTexture2D *tex = UTexture2D::CreateTransient(width, height, PF_B8G8R8A8);
    if (tex)
    {
        UpdateTextureBGRA(tex, bgra, width, height);
        return tex;
    }
    LWARN("Failed to create texture");
    return nullptr;
}

// Reads an image file from disk and decompresses it into the given buffer, passing out the data and the
// image dimensions. Returns true on success.
bool LoadAndDecompressImage(IImageWrapperModule& imageWrapperModule, FString path, TArray64<uint8>& data, int& w, int& h)
{
    // load the file's data from disk
    if (!FPaths::FileExists(path))
    {
        LERROR("File not found: %s", *path);
        return false;
    }

    TArray<uint8> fileData;
    if (!FFileHelper::LoadFileToArray(fileData, *path))
    {
        LERROR("Failed to load file %s", *path);
        return false;
    }

    // Detect the image format and create an imageWrapper with its data
    EImageFormat format = imageWrapperModule.DetectImageFormat(fileData.GetData(), fileData.Num());
    if (format == EImageFormat::Invalid)
    {
        LERROR("Unknown image format for %s", *path);
        return false;
    }

    TSharedPtr<IImageWrapper> imageWrapper = imageWrapperModule.CreateImageWrapper(format);
    if (!imageWrapper.IsValid() || !imageWrapper->SetCompressed(fileData.GetData(), fileData.Num()))
    {
        LERROR("Failed to create image wrapper for %s", *path);
        return false;
    }

    // Decompress the data into a raw buffer
    w = imageWrapper->GetWidth();
    h = imageWrapper->GetHeight();
    return imageWrapper->GetRaw(ERGBFormat::BGRA, 8, data);
}

// Note that this is the blocking API, so you should use UExternalTextureLoader most of the time
UTexture2D *LoadTextureFromFile(FString path)
{
    TArray64<uint8> uncompressedData;
    int w,h;
    IImageWrapperModule& iwm = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    if (!LoadAndDecompressImage(iwm, path, uncompressedData, w, h))
        return nullptr;
    return TextureFromBGRA(uncompressedData.GetData(), w, h);
}

void UExternalTextureLoader::Start(FString path, py::object& callback)
{
    Setup(callback);
    imagePath = path;
    IImageWrapperModule& iwm = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")); // we can't load this in a background thread
    AsyncTask(ENamedThreads::AnyHiPriThreadNormalTask, [&]
    {
        // do as much of the heavy lifting as we can on this background thread, but don't create
        // a texture yet - it will often work, but will sometimes blow up because you can't create
        // UObjects during garbage collection, and from this thread we have no control over when
        // that happens (at least as far as I could find).
        loadedOk = LoadAndDecompressImage(iwm, imagePath, uncompressedData, w, h);

        // now move back to the game thread to finish the job
        AsyncTask(ENamedThreads::GameThread, [this]()
        {
            UTexture2D* tex = nullptr;
            if (loadedOk)
                tex = TextureFromBGRA(uncompressedData.GetData(), w, h);
            TheEvent.Broadcast(imagePath, tex);
            Cleanup();
        });
    });
}

// given a cube texture render target, saves its contents to the given file. Returns true on success.
bool SaveCubeRenderTargetToFile(UTextureRenderTargetCube* target, FString fullPathPrefix)
{
    if (!target || !target->IsValidLowLevel())
    {
        LERROR("Invalid render target");
        return false;
    }

    UTextureCube* tCube = target->ConstructTextureCube(target, TEXT("what"), RF_Transient);
    TArray64<uint8> RawData;
    FIntPoint size;
    EPixelFormat format;
    bool bUnwrapSuccess = CubemapHelpers::GenerateLongLatUnwrap(tCube, RawData, size, format);
    if (!bUnwrapSuccess)
        return false;

    TArray<FColor> colorData;
    colorData.AddUninitialized(size.X*size.Y);
    memcpy(colorData.GetData(), RawData.GetData(), size.X*size.Y*4);
    for (FColor& c : colorData)
        c.A = 255;

    TArray<uint8> pngData;
    FImageUtils::CompressImageArray(size.X, size.Y, colorData, pngData);
    return FFileHelper::SaveArrayToFile(pngData, *fullPathPrefix);
}

bool SaveRenderTargetToFile(UTextureRenderTarget* target, FString fullPath)
{
    if (!target || !target->IsValidLowLevel())
    {
        LERROR("Invalid render target");
        return false;
    }

    FTextureRenderTargetResource *resource = target->GameThread_GetRenderTargetResource();

    TArray<FColor> pixelData;
    pixelData.AddUninitialized(target->GetSurfaceWidth() * target->GetSurfaceHeight());
    resource->ReadPixels(pixelData);
    int32 pixelCount = pixelData.Num();

    for (int i=0; i < pixelCount; i++)
        pixelData[i].A = 255; // picture is washed out without this

     FIntPoint destSize(target->GetSurfaceWidth(), target->GetSurfaceHeight());
     TArray<uint8> pngData;
     FImageUtils::CompressImageArray(destSize.X, destSize.Y, pixelData, pngData);
     bool imageSavedOk = FFileHelper::SaveArrayToFile(pngData, *fullPath);
     LOG("Saved ok? %d %s", imageSavedOk, *fullPath);
     return imageSavedOk;
}


