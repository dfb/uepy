// sets up the main 'uepy' builtin module
#include "uepy.h"
#include "uepy_netrep.h"
#include "AIController.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "common.h"
#include "Camera/CameraComponent.h"
#include "CineCameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/DecalComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Components/SkyLightComponent.h"
#include "Components/SphereComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/WidgetInteractionComponent.h"
#include "CubemapUnwrapUtils.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/PackageMapClient.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTargetCube.h"
#include "EngineUtils.h"
#include "FileMediaSource.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameMode.h"
#include "GameFramework/GameState.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Haptics/HapticFeedbackEffect_Base.h"
#include "Haptics/HapticFeedbackEffect_Curve.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "HighResScreenshot.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "IXRTrackingSystem.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/ImportanceSamplingLibrary.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "MediaPlayer.h"
#include "MediaSoundComponent.h"
#include "MediaTexture.h"
#include "Misc/FileHelper.h"
#include "MotionControllerComponent.h"
#include "MotionTrackedDeviceFunctionLibrary.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "Paper2D/Classes/PaperSprite.h"
#include "PaperSprite.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "UObject/ConstructorHelpers.h"
#include "AudioCaptureComponent.h"

#include <map>

#pragma warning(push)

// the compiler complains about stuff like FVector overloaing the mult operator with floats. The behavior we get is right, though, so we disable that warning.
#pragma warning (disable : 4686)

//#pragma optimize("", off)

// NOTE: GetPyWrapperForEngineObject is no longer used - it seems just a little too hacky. Or maybe we should just not log the warning message because
// it shows up all the time. :)
// given a UObject, returns the first pybind11 wrapper for it thatwe can find. This shouldn't be needed very often, but for the SetMaterial
// stuff we needed to do py::cast(UMeshComp pointer) but doing so always returned a new pybind wrapper, but the whole point of the cast
// was to get access to the existing wrapper so that we could access the Python state, but a new wrapper instance meant we'd get fresh state
// each time. In theory this shouldn't be needed, but it seems like we were getting a new wrapper because we have our own polymorphic_type_hook
// and because UE4 doesn't have RTTI enabled everywhere, or something. Possibly it's because UObject doesn't have a virtual destructor.
// See https://github.com/pybind/pybind11/issues/1148 and https://github.com/pybind/pybind11/issues/645 for others who ran into the same problem.
py::handle GetPyWrapperForEngineObject(UObject* obj)
{
    // iterate over all the pybind wrappers for this engine object
    auto it_instances = py::detail::get_internals().registered_instances.equal_range(obj);

    // because of the cost of the more involved stuff below, it's to our advantage to iterate over the matches twice because
    // if there is only a single match then we can just return it without additional checking
    if (std::distance(it_instances.first, it_instances.second) == 1)
        return py::handle((PyObject*)it_instances.first->second);

    for (auto it_i = it_instances.first; it_i != it_instances.second; ++it_i)
    {
        // in theory we should have one py wrapper, however occasionally (and so far, only in PIE, and after a code reload)
        // we have more than one, which is bad. To make matters worse, some wrappers are for engine objects of completely
        // different types, suggesting that the wrapper exists and points to an engine object whose slot has been reclaimed
        // and is now actually a different engine object. A further weirdness is that if we ask the wrapper to call GetClass
        // on the engine object it wraps, we get back the *original* engine class, instead of the engine class the wrapper now
        // points to (or something - I'm not entirely sure what's going on). However, the type info extracted from the wrapper
        // somehow has the cpp type info for the new class, so we can use it to detect that the wrapper and the engine object
        // are out of sync. I'm 99% sure this mess is just a symptom of a problem elsewhere, but here is where it manifests, so
        // here is where we deal with it. Oh, and to add insult to injury, the type information we can get out of pybind11 isn't
        // the actual engine class, so the only way I've found is to compare their names, and those don't even match completely
        // either (the wrapper typeinfo will have a decorated name like '_uepy.<className>').
        py::handle h((PyObject*)(it_i->second));
        py::object pyInstKlass = h.attr("GetClass")();
        UClass* instClass = py::cast<UClass*>(pyInstKlass);
        const char *tpName = it_i->second->ob_base.ob_type->tp_name;
        FString engineClassName = FString(UTF8_TO_TCHAR(tpName)); // engineClassName will be like "_uepy.UMeshComponent"
        if (engineClassName.Find(instClass->GetName()) == -1) // instClass->GetName will be like "UMeshComponent"
        {
            LOG("WARNING: ignoring wrapper mismatch  %s %s %d", *instClass->GetName(), *FString(UTF8_TO_TCHAR(tpName)), obj->IsA(instClass));
            continue;
        }

        // once we get here, this should always be true
        if (obj->IsA(instClass))
            return h;
    }
    return py::none(); // and we should never get here
}

// for some reason, passing back FSoundAttenuationSettings structs causes a crash in pybind11. Until we can figure it out, we use a proxy struct
// that has only certain members in it (i.e. add more here as needed, and also expose it in the py APIs).
struct FHackyAttenuationSettings
{
    FHackyAttenuationSettings(FSoundAttenuationSettings& in)
    {
        attenuationShape = (int)in.AttenuationShape;
        attenuationShapeExtents = in.AttenuationShapeExtents;
        binauralRadius = in.BinauralRadius;
        enablePriorityAttenuation = in.bEnablePriorityAttenuation;
        enableSubmixSends = in.bEnableSubmixSends;
        falloffDistance = in.FalloffDistance;
        falloffMode = (int)in.FalloffMode;
        manualPriorityAttenuation = in.ManualPriorityAttenuation;
        priorityAttenuationMax = in.PriorityAttenuationMax;
        priorityAttenuationMethod = (int)in.PriorityAttenuationMethod;
        priorityAttenuationMin = in.PriorityAttenuationMin;
        attenuate = in.bAttenuate;
        spatialize = in.bSpatialize;
        attenuateWithLPF = in.bAttenuateWithLPF;
        coneOffset = in.ConeOffset;
        distanceAlgorithm = (int)in.DistanceAlgorithm;
    }

    void ApplyTo(FSoundAttenuationSettings& in)
    {
        in.AttenuationShape = (EAttenuationShape::Type)attenuationShape;
        in.AttenuationShapeExtents = attenuationShapeExtents;
        in.BinauralRadius = binauralRadius;
        in.FalloffDistance = falloffDistance;
        in.FalloffMode = (ENaturalSoundFalloffMode)falloffMode;
        in.ManualPriorityAttenuation = manualPriorityAttenuation;
        in.PriorityAttenuationMax = priorityAttenuationMax;
        in.PriorityAttenuationMethod = (EPriorityAttenuationMethod)priorityAttenuationMethod;
        in.PriorityAttenuationMin = priorityAttenuationMin;
        in.bEnablePriorityAttenuation = enablePriorityAttenuation;
        in.bEnableSubmixSends = enableSubmixSends;
        in.bAttenuate = attenuate;
        in.bSpatialize = spatialize;
        in.bAttenuateWithLPF = attenuateWithLPF;
        in.ConeOffset = coneOffset;
        in.DistanceAlgorithm =  (EAttenuationDistanceModel)distanceAlgorithm;
    }

    FVector attenuationShapeExtents;
    bool enablePriorityAttenuation;
    bool enableSubmixSends;
    float binauralRadius;
    float falloffDistance;
    float manualPriorityAttenuation;
    float priorityAttenuationMax;
    float priorityAttenuationMin;
    int attenuationShape;
    int falloffMode;
    int priorityAttenuationMethod;
    bool attenuate;
    bool spatialize;
    bool attenuateWithLPF;
    float coneOffset;
    int distanceAlgorithm;
};

static std::map<FString, py::object> pyClassMap; // class name --> python class
static py::dict spawnArgs; // see RegisterPythonSubclass, SpawnActor_, and NewObject
void SetInternalSpawnArgs(py::dict& kwargs)
{
    if (py::len(kwargs) > 0) // only set them if they have a value; sometimes we have cases where an outer object sets them, e.g. CreateWidget -> NewObject
        spawnArgs = kwargs;
}

void ClearInternalSpawnArgs()
{
    spawnArgs.clear();
}

py::object GetPyClassFromName(FString& name)
{
    auto entry = pyClassMap.find(name);
    if (entry == pyClassMap.end())
        return py::none();
    return entry->second;
}

/* Update a texture with a new BGRA image. Note that this has to be called
 * on the game thread, or it will cause a crash
 */
void UpdateTextureBGRA(UTexture2D *tex, uint8 *bgra, int32 width, int32 height)
{
    void *textureData = tex->PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(textureData, bgra, width * height * 4);
    tex->PlatformData->Mips[0].BulkData.Unlock();
    tex->UpdateResource();
}

/* Create a texture from a 32-bit BGRA impage
 */
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

UTexture2D *LoadTextureFromFile(FString path)
{
    if (!FPaths::FileExists(path))
    {
        LWARN("File not found: %s", *path);
        return nullptr;;
    }

    EImageFormat format;
    if (path.EndsWith(TEXT(".png")))
        format = EImageFormat::PNG;
    else if (path.EndsWith(TEXT(".jpg")))
        format = EImageFormat::JPEG;
    else
    {
        LWARN("Unhandled file type for %s", *path);
        return nullptr;;
    }

    IImageWrapperModule& imageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> imageWrapper = imageWrapperModule.CreateImageWrapper(format);

    TArray<uint8> fileData;
    if (!FFileHelper::LoadFileToArray(fileData, *path) || !imageWrapper.IsValid() || !imageWrapper->SetCompressed(fileData.GetData(), fileData.Num()))
    {
        LWARN("Failed to load file %s", *path);
        return nullptr;
    }

    TArray64<uint8> uncompressedData;
    if (imageWrapper->GetRaw(ERGBFormat::BGRA, 8, uncompressedData))
    {
        return TextureFromBGRA(uncompressedData.GetData(), imageWrapper->GetWidth(), imageWrapper->GetHeight());
    }
    LWARN("Failed to read image data for %s", *path);
    return nullptr;
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

using namespace pybind11::literals;

// this module is automagically loaded by virtual of the global declaration and the use of the embedded module macro
// other builtin modules get added via FUEPythonDelegates::LaunchInit
PYBIND11_EMBEDDED_MODULE(_uepy, m) { // note the _ prefix, the builtin module uses _<name> and then we provide a <name> .py wrapper for additional stuffs
    // ALL Python-subclassable classes should live here (i.e. all C++ _CGLUE classes should be exposed via this submodule)
    py::module glueclasses = m.def_submodule("glueclasses");

    // note that WITH_EDITOR does not necessarily mean that the uepyEditor module will be loaded
#if WITH_EDITOR
    m.attr("WITH_EDITOR") = true;
#else
    m.attr("WITH_EDITOR") = false;
#endif

    m.attr("commandLineRaw") = FCommandLine::Get();

    // tells what mode we're in right now - PIE (4), editor (3), source from the command line (2), or a build (1).
    // NOTE: currently PIE vs editor doesn't work - it always returns 3 (but src CLI and build modes *are* properly detected)
    // returns EEngineMode from enums.py
    m.def("GetEngineMode", []()
    {
#if WITH_EDITOR
        if (GIsEditor) // we can't check "if (GEditor)" here because this can be called even before GEditor is set up
        {
            if (GWorld->HasBegunPlay())
                return 4; // PIE
            return 3; // Editor
        }
        else
            return 2; // SrcCLI
#else
        return 1; // Build
#endif
    });

    m.def("IsInGameThread", []() { return IsInGameThread(); });
    m.def("IsInSlateThread", []() { return IsInSlateThread(); });

    // Returns true if VR is enabled for reals: there's an HMD, it's connected, *and* stereo rendering is enabled. This
    // function is able to detect PIE VR Preview mode.
    m.def("IsVREnabled", []()
    {
        if (!UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled()) return false;
        if (!UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayConnected()) return false;
        if (!GEngine->XRSystem.IsValid()) return false;
        auto stereo = GEngine->XRSystem->GetStereoRenderingDevice();
        if (!stereo) return false;
        return stereo->IsStereoEnabled();
    });

    // dumps the render target cube to a series of images with the given path prefix; returns true on success
    m.def("SaveCubeRenderTargetToFile", [](UTextureRenderTargetCube* target, std::string& fullPath) { return SaveCubeRenderTargetToFile(target, FSTR(fullPath)); });
    m.def("SaveRenderTargetToFile", [](UTextureRenderTarget* target, std::string& fullPath) { return SaveRenderTargetToFile(target, FSTR(fullPath)); });

    // given an engine obj that you know to actually be a Python-implemented, get the associated Python instance from it (or None if you were wrong)
    m.def("PyInst", [](UObject *obj)
    {
        py::object ret = py::none();
        if (obj && obj->IsValidLowLevel() && !obj->IsPendingKillOrUnreachable())
        {
            IUEPYGlueMixin *p = Cast<IUEPYGlueMixin>(obj);
            if (p)
                ret = p->pyInst;
        }
        return ret;
    });

    // get VR hardware platform and version strings from the XRSystem component
    // TODO: replace with UHeadMountedDisplayFunctionLibrary API once we are on 4.26+  { 4.25 is missing ::GetHMDData(..) }
    m.def("GetVRHardwareInfo", []()
    {
        if (GEngine->XRSystem.IsValid())
        {
            std::string platform, hmd, controllers;
            IXRTrackingSystem* vrSystem = GEngine->XRSystem.Get();
            platform = PYSTR(vrSystem->GetSystemName().ToString());
            hmd = PYSTR(vrSystem->GetVersionString());
            controllers = PYSTR(vrSystem->GetVersionString()); // TODO: enhance this to read the controller info from OpenVR
            return py::make_tuple(true, platform, hmd, controllers);
        }
        else
        {
            return py::make_tuple(false, "Unkown", "Unkown", "Unkown");
        }
    });

    m.def("SetConsoleVarFloat", [](std::string& name, float v)
    {
        // Set the initial mic threshold - the default is too high and too much talking doesn't get picked up, so
        // we need a lower default. Previously we lowered the default in the engine build, but doing it here is one
        // less patch to make to the engine. Also, I forgot last time we moved to a new engine. :)
        FString varName = FSTR(name);
        IConsoleVariable* var = IConsoleManager::Get().FindConsoleVariable(*varName);
        if (var != nullptr)
            var->Set(v, ECVF_SetByGameSetting);
        else
            LWARN("Failed to find console variable %s", *varName);
    });

    m.def("TakeScreenshot", [](int w, int h, bool captureHDR, std::string& outPath)
    {
        FHighResScreenshotConfig& cfg = GetHighResScreenshotConfig(); // I guess this is some global struct we modify in place?
        cfg.bCaptureHDR = captureHDR;
        cfg.SetResolution(w, h, 1.0F);
        FString filename = FSTR(outPath);
        cfg.SetFilename(filename.Replace(TEXT("\\"), TEXT("/")));
        return GEngine->GameViewport->Viewport->TakeHighResScreenShot();
    });

    py::class_<FVector2D>(m, "FVector2D")
        .def(py::init<FVector2D>())
        .def(py::init([](float n) { return FVector2D(n,n); })) // note this special case of FVector(a) === FVector(a,a,a)
        .def(py::init<float,float>(), "x"_a=0.0f, "y"_a=0.0f)
        .def_readwrite("x", &FVector2D::X)
        .def_readwrite("X", &FVector2D::X)
        .def_readwrite("y", &FVector2D::Y)
        .def_readwrite("Y", &FVector2D::Y)
        .def(py::self == py::self)
        .def(py::self != py::self)
        .def(py::self + py::self)
        .def(py::self - py::self)
        .def(py::self * py::self)
        .def(py::self / py::self)
        .def(py::self | py::self)
        .def(py::self ^ py::self)
        .def(-py::self)
        .def(float() * py::self)
        .def(py::self * float())
        .def(py::self / float())
        .def("__iter__", [](FVector2D& self) { return py::make_tuple(self.X, self.Y).attr("__iter__")(); })
        .def("__getitem__", [](FVector2D& self, int i) { return self[i]; })
        .def("__setitem__", [](FVector2D& self, int i, float v) { self[i] = v; })
        .def("GetSafeNormal", [](FVector2D& self) { return self.GetSafeNormal(); })
        .def("Equals", [](FVector2D& self, FVector2D &other, float tolerance) { return self.Equals(other, tolerance); }, py::arg("other"), py::arg("tolerance")=KINDA_SMALL_NUMBER)
        ;

    py::class_<FVector>(m, "FVector")
        .def(py::init<FVector>())
        .def(py::init([]() { return FVector(0,0,0); }))
        .def(py::init([](float n) { return FVector(n,n,n); })) // note this special case of FVector(a) === FVector(a,a,a)
        .def(py::init([](float x, float y) { return FVector(x,y,0); }))
        .def(py::init([](float x, float y, float z) { return FVector(x,y,z); }))
        .def_readwrite("x", &FVector::X)
        .def_readwrite("X", &FVector::X)
        .def_readwrite("y", &FVector::Y)
        .def_readwrite("Y", &FVector::Y)
        .def_readwrite("z", &FVector::Z)
        .def_readwrite("Z", &FVector::Z)
        .def(py::self == py::self)
        .def(py::self != py::self)
        .def(py::self + py::self)
        .def(py::self - py::self)
        .def(py::self * py::self)
        .def(py::self / py::self)
        .def(-py::self)
        .def(int() * py::self)
        .def(py::self * int())
        .def(float() * py::self)
        .def(py::self * float())
        .def(py::self / float())
        .def("__iter__", [](FVector& self) { return py::make_tuple(self.X, self.Y, self.Z).attr("__iter__")(); })
        .def("__getitem__", [](FVector& self, int i) { return self[i]; })
        .def("__setitem__", [](FVector& self, int i, float v) { self[i] = v; })
        .def("Rotation", [](FVector& self) { return self.Rotation(); })
        .def("ToOrientationQuat", [](FVector& self) { return self.ToOrientationQuat(); })
        .def("ToOrientationRotator", [](FVector& self) { return self.ToOrientationRotator(); })
        .def_readonly_static("ZeroVector", &FVector::ZeroVector)
        .def_readonly_static("OneVector", &FVector::OneVector)
        .def_readonly_static("UpVector", &FVector::UpVector)
        .def_readonly_static("DownVector", &FVector::DownVector)
        .def_readonly_static("ForwardVector", &FVector::ForwardVector)
        .def_readonly_static("BackwardVector", &FVector::BackwardVector)
        .def_readonly_static("RightVector", &FVector::RightVector)
        .def_readonly_static("LeftVector", &FVector::LeftVector)
        .def("Size", [](FVector& self) { return self.Size(); })
        .def("SizeSquared", [](FVector& self) { return self.SizeSquared(); })
        .def("IsNearlyZero", [](FVector& self) { return self.IsNearlyZero(); })
        //.def("Normalize", [](FVector& self) { self.Normalize(); }) // Don't use this - it returns bool if it did something, false if it decided not to do anything at all
        .def("GetSafeNormal", [](FVector& self) { return self.GetSafeNormal(); })
        .def("IsNormalized", [](FVector& self) { return self.IsNormalized(); })
        .def("Equals", [](FVector& self, FVector &other, float tolerance) { return self.Equals(other, tolerance); }, py::arg("other"), py::arg("tolerance")=KINDA_SMALL_NUMBER)
        .def_static("Parallel", [](FVector& normal1, FVector& normal2, float threshold) { return FVector::Parallel(normal1, normal2, threshold); }, py::arg("normal1"), py::arg("normal2"), py::arg("threshold")=THRESH_NORMALS_ARE_PARALLEL)
        .def_static("Orthogonal", [](FVector& normal1, FVector& normal2, float threshold) { return FVector::Orthogonal(normal1, normal2, threshold); }, py::arg("normal1"), py::arg("normal2"), py::arg("threshold")=THRESH_NORMALS_ARE_ORTHOGONAL)
        .def_static("Coincident", [](FVector& normal1, FVector& normal2) { return FVector::Coincident(normal1, normal2); })
        .def_static("DotProduct", [](FVector& a, FVector& b) { return FVector::DotProduct(a, b); })
        .def_static("CrossProduct", [](FVector& a, FVector& b) { return FVector::CrossProduct(a, b); })
        .def_static("DistXY", [](FVector& a, FVector& b) { return FVector::DistXY(a, b); })
        .def("GetAbs", [](FVector &self) { return self.GetAbs(); })
        .def("ToString", [](FVector& self) { return PYSTR(self.ToString()); })
        .def_static("PointPlaneProject", [](FVector& pt, FPlane& plane) { return FVector::PointPlaneProject(pt, plane); })
        ;

    py::class_<FRotator>(m, "FRotator")
        .def(py::init<FRotator>())
        .def(py::init([]() { return FRotator(0,0,0); }))
        .def(py::init([](float n) { return FRotator(n,n,n); })) // note this special case of FRotator(a) === FRotator(a,a,a)
        .def(py::init([](float roll=0, float pitch=0, float yaw=0) { FRotator r; r.Roll=roll; r.Pitch=pitch; r.Yaw=yaw; return r; })) //<float, float, float>(), "roll"_a=0.0f, "pitch"_a=0.0f, "yaw"_a=0.0f) // weird order, but matches UnrealEnginePython
        .def_readwrite("roll", &FRotator::Roll)
        .def_readwrite("Roll", &FRotator::Roll)
        .def_readwrite("pitch", &FRotator::Pitch)
        .def_readwrite("Pitch", &FRotator::Pitch)
        .def_readwrite("yaw", &FRotator::Yaw)
        .def_readwrite("Yaw", &FRotator::Yaw)
        .def("RotateVector", [](FRotator& self, FVector& v) { return self.RotateVector(v); })
        .def("UnrotateVector", [](FRotator& self, FVector& v) { return self.UnrotateVector(v); })
        .def("Quaternion", [](FRotator& self) { return self.Quaternion(); })
        .def("__iter__", [](FRotator& self) { return py::make_tuple(self.Roll, self.Pitch, self.Yaw).attr("__iter__")(); })
        .def(float() * py::self)
        .def(py::self * float())
        .def(py::self + py::self)
        .def("Equals", [](FRotator& self, FRotator &other, float tolerance) { return self.Equals(other, tolerance); }, py::arg("other"), py::arg("tolerance")=KINDA_SMALL_NUMBER)
        .def("ToString", [](FRotator& self) { return PYSTR(self.ToString()); })
        ;

    py::class_<FQuat>(m, "FQuat")
        .def(py::init([](bool init) { if (init) return FQuat(EForceInit::ForceInit); else return FQuat(); }))
        .def(py::init<FQuat>())
        .def(py::init<FRotator>())
        .def(py::init<float,float,float,float>())
        .def(py::init<FVector,float>()) // axis/angle
        .def_readwrite("X", &FQuat::X)
        .def_readwrite("Y", &FQuat::Y)
        .def_readwrite("Z", &FQuat::Z)
        .def_readwrite("W", &FQuat::W)
        .def_readwrite("x", &FQuat::X)
        .def_readwrite("y", &FQuat::Y)
        .def_readwrite("z", &FQuat::Z)
        .def_readwrite("w", &FQuat::W)
        .def("__iter__", [](FQuat& self) { return py::make_tuple(self.X, self.Y, self.Z, self.W).attr("__iter__")(); })
        .def_static("FindBetweenVectors", [](FVector& a, FVector& b) { return FQuat::FindBetweenVectors(a,b); })
        .def("Inverse", [](FQuat& self) { return self.Inverse(); })
        .def("Rotator", [](FQuat& self) { return self.Rotator(); })
        .def("RotateVector", [](FQuat& self, FVector v) { return self.RotateVector(v); })
        .def("UnrotateVector", [](FQuat& self, FVector v) { return self.UnrotateVector(v); })
        .def("GetTwistAngle", [](FQuat& self, FVector& axis) { return self.GetTwistAngle(axis); })
        .def("ToAxisAndAngle", [](FQuat& self)
        {
            FVector axis;
            float angle;
            self.ToAxisAndAngle(axis, angle);
            return py::make_tuple(axis, angle);
        })
        .def("ToString", [](FQuat& self) { return PYSTR(self.ToString()); })
        .def(py::self * py::self)
        .def(py::self + py::self)
        .def(py::self | py::self)
        .def(py::self += py::self)
        .def(py::self - py::self)
        .def(py::self -= py::self)
        .def(py::self == py::self)
        .def(py::self * FVector())
        .def(py::self * FQuat())
        .def(py::self * float())
        .def(py::self / float())
        .def(py::self *= float())
        .def(py::self /= float())
        .def("GetNormalized", [](FQuat& self) { return self.GetNormalized(); })
        .def("AngularDistance", [](FQuat& self, FQuat& other) { return self.AngularDistance(other); })
        .def("GetForwardVector", [](FQuat& self) { return self.GetForwardVector(); })
        .def("GetRightVector", [](FQuat& self) { return self.GetRightVector(); })
        .def("GetUpVector", [](FQuat& self) { return self.GetUpVector(); })
        ;

    py::class_<FTransform>(m, "FTransform")
        .def(py::init<FTransform>())
        .def_readonly_static("Identity", &FTransform::Identity)
        .def(py::init<>())
        .def(py::init([](FVector& loc, FRotator& rot, FVector& scale) { return FTransform(rot, loc, scale); }))
        .def(py::init([](FVector& loc) { return FTransform(FRotator(0), loc, FVector(1)); }))
        .def(py::init([](FTransform& t) { return FTransform(t); }))
        .def(py::self * py::self)
        .def("Inverse", [](FTransform& self) { return self.Inverse(); })
        .def("Rotator", [](FTransform& self) { return self.Rotator(); })
        .def("GetRotation", [](FTransform& self) { return self.GetRotation(); })
        .def("SetRotation", [](FTransform& self, FRotator& r) { FQuat q(r); self.SetRotation(q); })
        .def("SetRotation", [](FTransform& self, FQuat& q) { self.SetRotation(q); })
        .def("GetTranslation", [](FTransform& self) { return self.GetTranslation(); })
        .def("GetLocation", [](FTransform& self) { return self.GetLocation(); })
        .def("SetTranslation", [](FTransform& self, FVector& t) { self.SetTranslation(t); })
        .def("SetLocation", [](FTransform& self, FVector& t) { self.SetLocation(t); })
        .def("GetScale3D", [](FTransform& self) { return self.GetScale3D(); })
        .def("SetScale3D", [](FTransform& self, FVector& v) { self.SetScale3D(v); })
        .def("GetRelativeTransform", [](FTransform& self, FTransform& other) { return self.GetRelativeTransform(other); })
        .def("TransformPosition", [](FTransform& self, FVector& pos) { return self.TransformPosition(pos); })
        .def("InverseTransformPosition", [](FTransform& self, FVector& pos) { return self.InverseTransformPosition(pos); })
        .def("TransformRotation", [](FTransform& self, FQuat& q) { return self.TransformRotation(q); })
        .def("InverseTransformRotation", [](FTransform& self, FQuat& q) { return self.InverseTransformRotation(q); })
        .def("TransformVector", [](FTransform& self, FVector& v) { return self.TransformVector(v); })
        .def("InverseTransformVector", [](FTransform& self, FVector& v) { return self.InverseTransformVector(v); })
        .def("EqualsNoScale", [](FTransform& self, FTransform& other) { return self.EqualsNoScale(other); })
        .def_property("translation", [](FTransform& self) { return self.GetTranslation(); }, [](FTransform& self, FVector& v) { self.SetTranslation(v); })
        .def_property("scale", [](FTransform& self) { return self.GetScale3D(); }, [](FTransform& self, FVector& v) { self.SetScale3D(v); })
        .def_property("rotation", [](FTransform& self) { return self.Rotator(); }, [](FTransform& self, FRotator& r) { FQuat q(r); self.SetRotation(q); })
        ;

    py::class_<FMath>(m, "FMath")
        .def_static("RayPlaneIntersection", [](FVector& origin, FVector& direction, FPlane& plane) { return FMath::RayPlaneIntersection(origin, direction, plane); })
        .def_static("ClosestPointOnInfiniteLine", [](FVector& lineStart, FVector& lineEnd, FVector& pt) { return FMath::ClosestPointOnInfiniteLine(lineStart, lineEnd, pt); })
        .def_static("VInterpTo", [](FVector& cur, FVector& target, float delta, float speed) { return FMath::VInterpTo(cur, target, delta, speed); })
        .def_static("Vector2DInterpTo", [](FVector2D& cur, FVector2D& target, float delta, float speed) { return FMath::Vector2DInterpTo(cur, target, delta, speed); })
        .def_static("RInterpTo", [](FRotator& cur, FRotator& target, float delta, float speed) { return FMath::RInterpTo(cur, target, delta, speed); })
        .def_static("FInterpTo", [](float cur, float target, float delta, float speed) { return FMath::FInterpTo(cur, target, delta, speed); })
        .def_static("CInterpTo", [](FLinearColor& cur, FLinearColor& target, float delta, float speed) { return FMath::CInterpTo(cur, target, delta, speed); })
        .def_static("QInterpTo", [](FQuat& cur, FQuat& target, float delta, float speed) { return FMath::QInterpTo(cur, target, delta, speed); })
        .def_static("PointDistToSegment", [](FVector& point, FVector& startPoint, FVector& endPoint) { return FMath::PointDistToSegment(point, startPoint, endPoint); })
        .def_static("GetTForSegmentPlaneIntersect", [](FVector& start, FVector& end, FPlane& plane) { return FMath::GetTForSegmentPlaneIntersect(start, end, plane); })
        ;

    py::class_<FPlane>(m, "FPlane")
        .def(py::init([](bool init) { if (init) return FPlane(EForceInit::ForceInit); else return FPlane(); }))
        .def(py::init<FVector,FVector>()) // point in plane, plane normal vector
        .def("PlaneDot", [](FPlane& self, FVector& v) { return self.PlaneDot(v); })
        ;

    py::class_<FColor>(m, "FColor")
        .def(py::init<FColor>())
        .def(py::init<int, int, int, int>(), "r"_a=0, "g"_a=0, "b"_a=0, "a"_a=0)
        .def_readwrite("R", &FColor::R)
        .def_readwrite("r", &FColor::R)
        .def_readwrite("G", &FColor::G)
        .def_readwrite("g", &FColor::G)
        .def_readwrite("B", &FColor::B)
        .def_readwrite("b", &FColor::B)
        .def_readwrite("A", &FColor::A)
        .def_readwrite("a", &FColor::A)
        .def("__iter__", [](FColor& self) { return py::make_tuple(self.R, self.G, self.B, self.A).attr("__iter__")(); })
        ;

    py::class_<FLinearColor>(m, "FLinearColor")
        .def(py::init<FLinearColor>())
        .def(py::init<float, float, float, float>(), "r"_a=0.0f, "g"_a=0.0f, "b"_a=0.0f, "a"_a=1.0f)
        .def(py::init<FVector>())
        .def_readwrite("R", &FLinearColor::R)
        .def_readwrite("r", &FLinearColor::R)
        .def_readwrite("G", &FLinearColor::G)
        .def_readwrite("g", &FLinearColor::G)
        .def_readwrite("B", &FLinearColor::B)
        .def_readwrite("b", &FLinearColor::B)
        .def_readwrite("A", &FLinearColor::A)
        .def_readwrite("a", &FLinearColor::A)
        .def("__iter__", [](FLinearColor& self) { return py::make_tuple(self.R, self.G, self.B, self.A).attr("__iter__")(); })
        .def_readonly_static("White", &FLinearColor::White)
        .def_readonly_static("Gray", &FLinearColor::Gray)
        .def_readonly_static("Black", &FLinearColor::Black)
        .def_readonly_static("Transparent", &FLinearColor::Transparent)
        .def_readonly_static("Red", &FLinearColor::Red)
        .def_readonly_static("Green", &FLinearColor::Green)
        .def_readonly_static("Blue", &FLinearColor::Blue)
        .def_readonly_static("Yellow", &FLinearColor::Yellow)
        ;

    py::class_<FMargin>(m, "FMargin")
        .def(py::init<FMargin>())
        .def(py::init<>())
        .def(py::init<float>())
        .def(py::init<float,float>())
        .def(py::init<float,float,float,float>())
        .def_readwrite("Left", &FMargin::Left)
        .def_readwrite("Top", &FMargin::Top)
        .def_readwrite("Right", &FMargin::Right)
        .def_readwrite("Bottom", &FMargin::Bottom)
        .def_readwrite("left", &FMargin::Left)
        .def_readwrite("top", &FMargin::Top)
        .def_readwrite("right", &FMargin::Right)
        .def_readwrite("bottom", &FMargin::Bottom)
        ;

    // Given a UObject, returns its address as a number
    m.def("AddressOf", [](UObject* obj) { return (unsigned long long)obj; });

    // Force garbage collection to happen
    m.def("ForceGC", []() { if (GEngine) GEngine->ForceGarbageCollection(true); });

    // ideally called by all objects that call UObject.Bind(event, self.OnSomethingOrOther) to force bindings to
    // be removed if Unbind wasn't called for them already.
    m.def("UnbindDelegatesOn", [](py::object obj) { FPyObjectTracker::Get()->UnbindDelegatesOn(obj); });

    // Enables a telnet-ish remote REPL
    m.def("EnableRemoteConsole", [](std::string& host, int port, float processInterval, py::object& env)
    {
        LOG("Enabling remote console on %d (%.1f)", port, processInterval);
        try {
            py::object rrepl = py::module::import("uepy.rrepl").attr("RemoteREPL")(host, port, env);
            FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([rrepl](float dt)
            {
                try {
                    rrepl.attr("Process")();
                } catchpy;
                return true; // true = repeat
            }), processInterval);
        } catchpy;
    });

    py::class_<FPaths>(m, "FPaths")
        .def_static("ProjectDir", []() { return PYSTR(FPaths::ProjectDir()); })
        .def_static("ProjectContentDir", []() { return PYSTR(FPaths::ProjectContentDir()); })
        .def_static("ProjectPluginsDir", []() { return PYSTR(FPaths::ProjectPluginsDir()); })
        ;

    py::class_<UObject, UnrealTracker<UObject>>(m, "UObject")
        .def_static("StaticClass", []() { return UObject::StaticClass(); }, py::return_value_policy::reference)
        .def("GetClass", [](UObject& self) { return self.GetClass(); }, py::return_value_policy::reference)
        .def("GetName", [](UObject& self) { return PYSTR(self.GetName()); })
        .def("GetPathName", [](UObject& self) { return PYSTR(self.GetPathName()); })
        .def("GetOuter", [](UObject& self) { return self.GetOuter(); }, py::return_value_policy::reference)
        .def("ConditionalBeginDestroy", [](UObject* self) { if (self->IsValidLowLevel()) self->ConditionalBeginDestroy(); })
        .def("IsValid", [](UObject* self) { return self->IsValidLowLevel() && !self->IsPendingKillOrUnreachable(); })
        .def("IsDefaultObject", [](UObject& self) { return self.HasAnyFlags(RF_ClassDefaultObject); })

        .def("IsA", [](UObject& self, py::object& _klass)
        {
            if (!self.IsValidLowLevel())
                return false;
            UClass *klass = PyObjectToUClass(_klass);
            return self.IsA(klass);
        })

        // TODO: we could create a generic CreateDefaultSubobject utility func that takes a UClass of what to create
        // and just return it as a UObject, though the caller would then need to also cast it, e.g.
        // self.foo = uepy.AsUStaticMeshComponent(self.CreateDefaultSubObject(uepy.UStaticMeshComponent, 'mymesh'))
        // which seems like a lot of typing, so for now I'm creating class-specific versions.
        // TODO: I think we want to get rid of this in favor of NewObject
        .def("CreateUStaticMeshComponent", [](UObject& self, py::str name)
        {
            std::string sname = name;
            return self.CreateDefaultSubobject<UStaticMeshComponent>(UTF8_TO_TCHAR(sname.c_str()));
        }, py::return_value_policy::reference)

        // methods for accessing stuff via the UE4 reflection system (e.g. to interact with Blueprints, UPROPERTYs, etc)
        .def("Set", [](UObject* self, std::string k, py::object& value) { SetObjectProperty(self, k, value); })
        .def("Get", [](UObject* self, std::string k) { return GetObjectProperty(self, k); }, py::return_value_policy::reference)
        .def("Call", [](UObject* self, std::string funcName, py::args& args){ return CallObjectUFunction(self, funcName, args); }, py::return_value_policy::reference)
        .def("Bind", [](UObject* self, std::string eventName, py::object callback) { BindDelegateCallback(self, eventName, callback); })
        .def("Unbind", [](UObject* self, std::string eventName, py::object callback) { UnbindDelegateCallback(self, eventName, callback); })
        .def("Broadcast", [](UObject* self, std::string eventName, py::args& args) { BroadcastEvent(self, eventName, args); })
        ;

    UEPY_EXPOSE_CLASS(UClass, UObject, m) // TODO: UClass --> UStruct --> UField --> UObject
        .def("GetDefaultObject", [](UClass& self) { return self.GetDefaultObject(); }, py::return_value_policy::reference)
        .def("GetSuperClass", [](UClass& self) { return self.GetSuperClass(); }, py::return_value_policy::reference)
        .def("ImplementsInterface", [](UClass& self, py::object interfaceClass)
        {
            UClass *k = PyObjectToUClass(interfaceClass);
            return k ? self.ImplementsInterface(k) : false;
        })
        ;

    UEPY_EXPOSE_CLASS(UEngineTypes, UObject, m)
        .def("ConvertToTraceType", [](int collisionChan) { return (int)UEngineTypes::ConvertToTraceType((ECollisionChannel)collisionChan); })
        ;

    UEPY_EXPOSE_CLASS(UBlueprintGeneratedClass, UClass, m)
        ;

    UEPY_EXPOSE_CLASS(UInterface, UObject, m)
        ;

    UEPY_EXPOSE_CLASS(UCurveBase, UObject, m)
        .def("CreateCurveFromCSVString", [](UCurveBase& self, std::string s) { self.CreateCurveFromCSVString(FSTR(s)); })
        .def("ResetCurve", [](UCurveBase& self) { self.ResetCurve(); })
        .def("GetTimeRange", [](UCurveBase& self)
        {
            float min, max;
            self.GetTimeRange(min, max);
            return py::make_tuple(min, max);
        })
        .def("GetValueRange", [](UCurveBase& self)
        {
            float min, max;
            self.GetValueRange(min, max);
            return py::make_tuple(min, max);
        })
        ;

    UEPY_EXPOSE_CLASS(UCurveFloat, UCurveBase, m)
        .def("GetFloatValue", [](UCurveFloat& self, float f) { return self.GetFloatValue(f); })
        ;

    UEPY_EXPOSE_CLASS(UCurveVector, UCurveBase, m)
        .def("GetVectorValue", [](UCurveVector& self, float f) { return self.GetVectorValue(f); })
        ;

    UEPY_EXPOSE_CLASS(UFont, UObject, m)
        ;

    UEPY_EXPOSE_CLASS(UStaticMesh, UObject, m)
        .def("GetBounds", [](UStaticMesh& self) { return self.GetBounds(); })
        .def("GetBoundingBox", [](UStaticMesh& self) { return self.GetBoundingBox(); })
        .def("GetMaterial", [](UStaticMesh& self, int i) { return self.GetMaterial(i); }, py::return_value_policy::reference)
        .def("GetSize", [](UStaticMesh& self) { return self.GetBounds().BoxExtent * 2; })
        .def("FindSocket", [](UStaticMesh& self, std::string& name) { return self.FindSocket(FSTR(name)); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(UStaticMeshSocket, UObject, m)
        .def_property_readonly("SocketName", [](UStaticMeshSocket& self) { std::string s = TCHAR_TO_UTF8(*self.SocketName.ToString()); return s; })
        .def_readwrite("RelativeLocation", &UStaticMeshSocket::RelativeLocation)
        .def_readwrite("RelativeRotation", &UStaticMeshSocket::RelativeRotation)
        .def_readwrite("RelativeScale", &UStaticMeshSocket::RelativeScale)
        ;

    UEPY_EXPOSE_CLASS(UActorComponent, UObject, m)
        .def("GetReadableName", [](UActorComponent& self) { return PYSTR(self.GetReadableName()); })
        .def("GetOwner", [](UActorComponent& self) { return self.GetOwner(); }, py::return_value_policy::reference)
        .def("SetActive", [](UActorComponent& self, bool a) { self.SetActive(a); })
        .def("SetIsReplicated", [](UActorComponent& self, bool b) { self.SetIsReplicated(b); })
        .def("IsRegistered", [](UActorComponent& self) { return self.IsRegistered(); })
        .def("SetComponentTickEnabled", [](UActorComponent& self, bool enabled) { self.SetComponentTickEnabled(enabled); })
        .def("RegisterComponent", [](UActorComponent& self) { self.RegisterComponent(); })
        .def("UnregisterComponent", [](UActorComponent& self) { self.UnregisterComponent(); })
        .def("DestroyComponent", [](UActorComponent& self) { self.DestroyComponent(); })
        BIT_PROP(bAutoActivate, UActorComponent)
        .def("IsActive", [](UActorComponent& self) { return self.IsActive(); })
        .def("Activate", [](UActorComponent& self, bool reset) { self.Activate(reset); }, "reset"_a=false)
        .def("Deactivate", [](UActorComponent& self) { self.Deactivate(); })
        .def("SetActivated", [](UActorComponent& self, bool a) { if (a) self.Activate(); else self.Deactivate(); })
        .def("ComponentHasTag", [](UActorComponent& self, std::string& tag) { return self.ComponentHasTag(FSTR(tag)); })
        .def("HasAnyTags", [](UActorComponent& self, py::list& pytags) // returns true if any of the given tags are present
        {
            for (const py::handle pytag : pytags)
            {
                if (self.ComponentHasTag(pytag.cast<std::string>().c_str()))
                    return true;
            }
            return false;
        })
        .def("AddTag", [](UActorComponent& self, std::string& tag) { self.ComponentTags.AddUnique(FName(FSTR(tag))); })
        .def("RemoveTag", [](UActorComponent& self, std::string& tag) { self.ComponentTags.Remove(FName(FSTR(tag))); })
        .def_property("ComponentTags", [](UActorComponent& self)
            {
                py::list ret;
                for (FName& tag : self.ComponentTags)
                    ret.append(PYSTR(tag.ToString()));
                return ret;
            },
            [](UActorComponent& self, py::list pytags)
            {
                self.ComponentTags.Empty();
                for (const py::handle pytag : pytags)
                    self.ComponentTags.Emplace(pytag.cast<std::string>().c_str());
            })
        ;

    py::class_<FKey>(m, "FKey")
        .def("IsValid", [](FKey& self) { return self.IsValid(); })
        .def("IsModifierKey", [](FKey& self) { return self.IsModifierKey(); })
        .def("IsGamepadKey", [](FKey& self) { return self.IsGamepadKey(); })
        .def("IsTouch", [](FKey& self) { return self.IsTouch(); })
        .def("IsMouseButton", [](FKey& self) { return self.IsMouseButton(); })
        .def("IsAxis1D", [](FKey& self) { return self.IsAxis1D(); })
        .def("IsAxis2D", [](FKey& self) { return self.IsAxis2D(); })
        .def("IsAxis3D", [](FKey& self) { return self.IsAxis3D(); })
        .def("ToString", [](FKey& self) { return PYSTR(self.ToString()); })
        ;

    UEPY_EXPOSE_CLASS(UInputComponent, UObject, m)
        .def("BindAction", [](UInputComponent& self, std::string actionName, int keyEvent, py::object callback)
        {
            UBasePythonDelegate *delegate = FPyObjectTracker::Get()->CreateDelegate(&self, "Ignore", "On", callback);
            FInputActionBinding binding(FSTR(actionName), (EInputEvent)keyEvent);
            binding.ActionDelegate.BindDelegate(delegate, &UBasePythonDelegate::On);
            self.AddActionBinding(binding);
        })
        .def("BindKeyAction", [](UInputComponent& self, std::string actionName, int keyEvent, py::object callback) // for cases where an FKey is passed back, e.g. AnyKey
        {
            UBasePythonDelegate *delegate = FPyObjectTracker::Get()->CreateDelegate(&self, "Ignore", "UInputComponent_OnKeyAction", callback);
            FInputActionBinding binding(FSTR(actionName), (EInputEvent)keyEvent);
            binding.ActionDelegate.BindDelegate(delegate, &UBasePythonDelegate::UInputComponent_OnKeyAction);
            self.AddActionBinding(binding);
        })
        .def("BindAxis", [](UInputComponent& self, std::string axisName, py::object callback) // callback can be None to indicate that we want the input but will poll for it
        {
            if (!callback.is(py::none()))
            {
                UBasePythonDelegate *delegate = FPyObjectTracker::Get()->CreateDelegate(&self, "Ignore", "UInputComponent_OnAxis", callback);
                FInputAxisBinding binding(FSTR(axisName));
                binding.AxisDelegate.BindDelegate(delegate, &UBasePythonDelegate::UInputComponent_OnAxis);
                self.AxisBindings.Emplace(MoveTemp(binding));
            }
            else
                self.BindAxis(FSTR(axisName));
        })
        .def("GetAxisValue", [](UInputComponent& self, std::string& axisName) { return self.GetAxisValue(FSTR(axisName)); })
        ;

    UEPY_EXPOSE_CLASS(USceneComponent, UActorComponent, m)
        .def("GetRelativeLocation", [](USceneComponent& self) { return self.GetRelativeLocation(); })
        .def("SetRelativeLocation", [](USceneComponent& self, FVector v) { self.SetRelativeLocation(v); })
        .def("GetRelativeRotation", [](USceneComponent& self) { return self.GetRelativeRotation(); })
        .def("SetRelativeRotation", [](USceneComponent& self, FRotator r) { self.SetRelativeRotation(r); })
        .def("GetRelativeScale3D", [](USceneComponent& self) { return self.GetRelativeScale3D(); })
        .def("SetRelativeScale3D", [](USceneComponent& self, FVector v) { self.SetRelativeScale3D(v); })
        .def("GetRelativeTransform", [](USceneComponent& self) { return self.GetRelativeTransform(); })
        .def("SetRelativeTransform", [](USceneComponent& self, FTransform& t) { self.SetRelativeTransform(t); })
        .def("ResetRelativeTransform", [](USceneComponent& self) { self.ResetRelativeTransform(); })
        .def("AttachToComponent", [](USceneComponent& self, USceneComponent *parent, std::string& socket, int attachmentRule)
        {
            FName socketName = NAME_None;
            FAttachmentTransformRules rules = attachmentRule == 0 ? FAttachmentTransformRules::KeepRelativeTransform : FAttachmentTransformRules::KeepWorldTransform;
            if (socket.length() > 0)
            {
                socketName = FSTR(socket);
                rules = FAttachmentTransformRules::SnapToTargetNotIncludingScale;
            }
            return self.AttachToComponent(parent, rules, socketName);
        }, py::arg("parent"), py::arg("socket")="", py::arg("attachmentRule")=0)

        .def("SetupAttachment", [](USceneComponent& self, USceneComponent *parent, std::string socketName)
        {
            if (socketName.length())
                self.SetupAttachment(parent, FSTR(socketName));
            else
                self.SetupAttachment(parent);
        }, py::arg("parent"), py::arg("socketName")="")

        .def("DetachFromComponent", [](USceneComponent& self) { self.DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform); })
        .def("SetRelativeLocationAndRotation", [](USceneComponent& self, FVector& loc, FRotator& rot) { self.SetRelativeLocationAndRotation(loc, rot); })
        .def("SetWorldLocationAndRotation", [](USceneComponent& self, FVector& loc, FRotator& rot) { self.SetWorldLocationAndRotation(loc, rot); })
        .def("SetWorldLocationAndRotation", [](USceneComponent& self, FVector& loc, FQuat& rot) { self.SetWorldLocationAndRotation(loc, rot); })
        .def("AddRelativeLocation", [](USceneComponent& self, FVector& deltaLoc) { self.AddRelativeLocation(deltaLoc); })
        .def("AddLocalOffset", [](USceneComponent& self, FVector& deltaLoc, bool sweep) { self.AddLocalOffset(deltaLoc, sweep); }, py::arg("deltaLoc"), py::arg("sweep")=false)
        .def("AddLocalRotation", [](USceneComponent& self, FRotator& deltaRot) { self.AddLocalRotation(deltaRot); })
        .def("AddLocalRotation", [](USceneComponent& self, FQuat& deltaRot) { self.AddLocalRotation(deltaRot); })
        .def("SetVisibility", [](USceneComponent& self, bool visible, bool propagate) { self.SetVisibility(visible, propagate); }, py::arg("visible"), py::arg("propagate")=true)
        .def("GetHiddenInGame", [](USceneComponent& self) { return self.bHiddenInGame; })
        .def("IsHidden", [](USceneComponent& self) { return self.bHiddenInGame; })
        .def("SetHiddenInGame", [](USceneComponent& self, bool hidden, bool propagate) { self.SetHiddenInGame(hidden, propagate); }, py::arg("hidden"), py::arg("propagate")=true)
        .def("IsVisible", [](USceneComponent& self) { return self.IsVisible(); })
        .def_property_readonly("bVisible", [](USceneComponent& self) { return (int)self.GetVisibleFlag(); }) // TODO: get rid of this
        .def("GetForwardVector", [](USceneComponent& self) { return self.GetForwardVector(); })
        .def("GetRightVector", [](USceneComponent& self) { return self.GetRightVector(); })
        .def("GetUpVector", [](USceneComponent& self) { return self.GetUpVector(); })
        .def("GetComponentLocation", [](USceneComponent& self) { return self.GetComponentLocation(); })
        .def("GetComponentRotation", [](USceneComponent& self) { return self.GetComponentRotation(); })
        .def("GetComponentQuat", [](USceneComponent& self) { return self.GetComponentQuat(); })
        .def("GetComponentScale", [](USceneComponent& self) { return self.GetComponentScale(); })
        .def("GetComponentToWorld", [](USceneComponent& self) { return self.GetComponentToWorld(); })
        .def("GetComponentTransform", [](USceneComponent& self) { return self.GetComponentTransform(); })
        .def("SetWorldLocation", [](USceneComponent& self, FVector& loc) { self.SetWorldLocation(loc); })
        .def("SetWorldRotation", [](USceneComponent& self, FRotator& rot) { self.SetWorldRotation(rot); })
        .def("SetWorldRotation", [](USceneComponent& self, FQuat& rot) { self.SetWorldRotation(rot); })
        .def("SetWorldTransform", [](USceneComponent& self, FTransform& t) { self.SetWorldTransform(t); })
        .def("SetWorldScale3D", [](USceneComponent& self, FVector& s) { self.SetWorldScale3D(s); })
        .def("GetSocketTransform", [](USceneComponent& self, std::string& name, int transformSpace) { return self.GetSocketTransform(FSTR(name), (ERelativeTransformSpace)transformSpace); }, py::arg("name"), py::arg("transformSpace")=(int)ERelativeTransformSpace::RTS_World)
        .def("GetSocketLocation", [](USceneComponent& self, std::string& name) { return self.GetSocketLocation(FSTR(name)); })
        .def("GetSocketRotation", [](USceneComponent& self, std::string& name) { return self.GetSocketRotation(FSTR(name)); })
        .def("DoesSocketExist", [](USceneComponent& self, std::string& name) { return self.DoesSocketExist(FSTR(name)); })
        .def("CalcBounds", [](USceneComponent& self, FTransform& locToWorld) { return self.CalcBounds(locToWorld); })
        .def("GetAttachParent", [](USceneComponent& self) { return self.GetAttachParent(); }, py::return_value_policy::reference)
        .def("GetChildrenComponents", [](USceneComponent& self, bool incAllDescendents)
        {
            TArray<USceneComponent*> kids;
            self.GetChildrenComponents(incAllDescendents, kids);
            py::list ret;
            for (auto kid : kids)
                ret.append(kid);
            return ret;
        }, py::return_value_policy::reference)
        .def("SetMobility", [](USceneComponent& self, int mobility) { self.SetMobility((EComponentMobility::Type)mobility); })
        .def("Show", [](USceneComponent& self, bool visible, bool propagate, bool updateCollision)
        {   // this exists because we added UPrimitiveComponent.Show, which is really handy, but we don't want devs to have to switch back to
            // SetVisibility for other types of components (and/or have to constantly ask themselves if Show() is available or not)
            self.SetVisibility(visible, propagate);
        }, "visible"_a, "propagate"_a=true, "updateCollision"_a=false)
        ;

    UEPY_EXPOSE_CLASS(UDecalComponent, USceneComponent, m)
        .def_readwrite("DecalSize", &UDecalComponent::DecalSize)
        .def("SetFadeIn", [](UDecalComponent& self, float startDelay, float dur) { self.SetFadeIn(startDelay, dur); })
        .def("SetFadeOut", [](UDecalComponent& self, float startDelay, float dur) { self.SetFadeOut(startDelay, dur); })
        .def("SetFadeScreenSize", [](UDecalComponent& self, float size) { self.SetFadeScreenSize(size); })
        .def("SetDecalMaterial", [](UDecalComponent& self, UMaterialInterface* mat) { self.SetDecalMaterial(mat); })
        BIT_PROP(bDestroyOwnerAfterFade, UDecalComponent)
        ;

    UEPY_EXPOSE_CLASS(UPrimitiveComponent, USceneComponent, m)
        BIT_PROP(bReceivesDecals, UPrimitiveComponent)
        .def("GetNumMaterials", [](UPrimitiveComponent& self) { return self.GetNumMaterials(); })
        .def("SetMaterial", [](UPrimitiveComponent& self, int index, UMaterialInterface* mat) { self.SetMaterial(index, mat); })
        .def("GetMaterial", [](UPrimitiveComponent& self, int elementIndex) { return self.GetMaterial(elementIndex); }, py::return_value_policy::reference)
        .def("SetCollisionEnabled", [](UPrimitiveComponent& self, int c) { self.SetCollisionEnabled((ECollisionEnabled::Type)c); })
        .def("Show", [](UPrimitiveComponent& self, bool visible, bool propagate, bool updateCollision)
        {
            self.SetVisibility(visible, propagate);
            if (updateCollision)
                self.SetCollisionEnabled(visible ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
        }, "visible"_a, "propagate"_a=true, "updateCollision"_a=true)
        .def("SetCollisionObjectType", [](UPrimitiveComponent& self, int c) { self.SetCollisionObjectType((ECollisionChannel)c); })
        .def("SetCollisionProfileName", [](UPrimitiveComponent& self, std::string& name, bool updateOverlaps) { self.SetCollisionProfileName(FSTR(name), updateOverlaps); })
        .def("SetCollisionResponseToAllChannels", [](UPrimitiveComponent& self, int r) { self.SetCollisionResponseToAllChannels((ECollisionResponse)r); })
        .def("SetCollisionResponseToChannel", [](UPrimitiveComponent& self, int c, int r) { self.SetCollisionResponseToChannel((ECollisionChannel)c, (ECollisionResponse)r); })
        .def("SetRenderCustomDepth", [](UPrimitiveComponent& self, bool b) { self.SetRenderCustomDepth(b); })
        .def("SetCustomDepthStencilValue", [](UPrimitiveComponent& self, int v) { self.SetCustomDepthStencilValue(v); })
        .def_readonly("CustomDepthStencilValue", &UPrimitiveComponent::CustomDepthStencilValue)
        .def("SetCastShadow", [](UPrimitiveComponent& self, bool s) { self.SetCastShadow(s); })
        .def_property_readonly("bRenderCustomDepth", [](UPrimitiveComponent& self) { return (bool)self.bRenderCustomDepth; })
        .def("SetPhysMaterialOverride", [](UPrimitiveComponent& self, UPhysicalMaterial* mat) { self.SetPhysMaterialOverride(mat); })
        .def("GetGenerateOverlapEvents", [](UPrimitiveComponent& self) { return self.GetGenerateOverlapEvents(); })
        .def("SetGenerateOverlapEvents", [](UPrimitiveComponent& self, bool gens) { self.SetGenerateOverlapEvents(gens); })
        .def("GetClosestPointOnCollision", [](UPrimitiveComponent& self, FVector& pt)
        {   // returns (point on the collision closest to pt, >0 success, <= comp has no collision, ==0 if it is either not convex or inside of the point
            FVector outPt;
            float success = self.GetClosestPointOnCollision(pt, outPt);
            return py::make_tuple(outPt, success);
        })
        .def_readwrite("TranslucencySortPriority", &UPrimitiveComponent::TranslucencySortPriority)
        .def("SetCustomPrimitiveDataFloat", [](UPrimitiveComponent& self, int index, float data) { self.SetCustomPrimitiveDataFloat(index, data); })
        ;

    UEPY_EXPOSE_CLASS(UMotionControllerComponent, UPrimitiveComponent, m)
        .def("SetAssociatedPlayerIndex", [](UMotionControllerComponent& self, int player) { self.SetAssociatedPlayerIndex(player); })
        .def("SetCustomDisplayMesh", [](UMotionControllerComponent& self, UStaticMesh* mesh) { self.SetCustomDisplayMesh(mesh); })
        .def("SetTrackingSource", [](UMotionControllerComponent& self, int hand) { self.SetTrackingSource((EControllerHand)hand); })
        .def("SetTrackingMotionSource", [](UMotionControllerComponent& self, std::string& name) { self.SetTrackingMotionSource(FName(FSTR(name))); })
        .def_property("bDisableLowLatencyUpdate", [](UMotionControllerComponent& self) { return (bool)self.bDisableLowLatencyUpdate; }, [](UMotionControllerComponent& self, bool b) { self.bDisableLowLatencyUpdate = b; })
        .def_property_readonly("MotionSource", [](UMotionControllerComponent& self) { return PYSTR(self.MotionSource.ToString()); })
        .def_property_readonly("DisplayModelSource", [](UMotionControllerComponent& self) { return PYSTR(self.DisplayModelSource.ToString()); })
        .def("SetShowDeviceModel", [](UMotionControllerComponent& self, bool show) { self.SetShowDeviceModel(show); })
        ;

    UEPY_EXPOSE_CLASS(UFXSystemComponent, UPrimitiveComponent, m)
        .def("SetFloatParameter", [](UFXSystemComponent& self, std::string name, float v) { self.SetFloatParameter(FSTR(name), v); })
        .def("SetVectorParameter", [](UFXSystemComponent& self, std::string name, FVector& v) { self.SetVectorParameter(FSTR(name), v); })
        .def("SetColorParameter", [](UFXSystemComponent& self, std::string name, FLinearColor& v) { self.SetColorParameter(FSTR(name), v); })
        .def("SetActorParameter", [](UFXSystemComponent& self, std::string name, AActor* v) { self.SetActorParameter(FSTR(name), v); })
        ;

    py::class_<UNiagaraFunctionLibrary, UObject, UnrealTracker<UNiagaraFunctionLibrary>>(m, "UNiagaraFunctionLibrary")
        .def_static("OverrideSystemUserVariableStaticMeshComponent", [](UNiagaraComponent* obj, std::string& o, UStaticMeshComponent* comp) { UNiagaraFunctionLibrary::OverrideSystemUserVariableStaticMeshComponent(obj, FSTR(o), comp); })
        ;

    UEPY_EXPOSE_CLASS(UNiagaraComponent, UFXSystemComponent, m)
        .def("SetNiagaraVariableFloat", [](UNiagaraComponent& self, std::string inName, float inValue) { self.SetNiagaraVariableFloat(FSTR(inName), inValue); })
        .def("SetAsset", [](UNiagaraComponent& self, UNiagaraSystem* inAsset, bool reset) { self.SetAsset(inAsset, reset); })
        .def("DeactivateImmediate", [](UNiagaraComponent& self) { self.DeactivateImmediate(); })
        ;

    UEPY_EXPOSE_CLASS(UParticleSystemComponent, UFXSystemComponent, m)
        .def("SetTemplate", [](UParticleSystemComponent& self, UParticleSystem* sys) { self.SetTemplate(sys); })
        .def("SetBeamSourcePoint", [](UParticleSystemComponent& self, int emitter, FVector& source, int sourceIndex) { self.SetBeamSourcePoint(emitter, source, sourceIndex); })
        .def("SetBeamTargetPoint", [](UParticleSystemComponent& self, int emitter, FVector& target, int targetIndex) { self.SetBeamTargetPoint(emitter, target, targetIndex); })
        .def("SetBeamEndPoint", [](UParticleSystemComponent& self, int emitter, FVector& target) { self.SetBeamEndPoint(emitter, target); })
        .def("SetEmitterMaterials", [](UParticleSystemComponent& self, py::list& _mats)
        {
            TArray<UMaterialInterface*> mats;
            for (py::handle h : _mats)
                mats.Emplace(h.cast<UMaterialInterface*>());
            self.EmitterMaterials = mats;
        })
        ;

    UEPY_EXPOSE_CLASS(UTextRenderComponent, UPrimitiveComponent, m)
        .def("SetText", [](UTextRenderComponent& self, std::string s) { self.SetText(FText::FromString(FSTR(s))); })
        .def("GetTextLocalSize", [](UTextRenderComponent& self) { return self.GetTextLocalSize(); })
        .def("GetTextWorldSize", [](UTextRenderComponent& self) { return self.GetTextWorldSize(); })
        .def("SetWorldSize", [](UTextRenderComponent& self, float size) { self.SetWorldSize(size); })
        .def("SetHorizontalAlignment", [](UTextRenderComponent& self, int a) { self.SetHorizontalAlignment((EHorizTextAligment)a); })
        .def("SetVerticalAlignment", [](UTextRenderComponent& self, int a) { self.SetVerticalAlignment((EVerticalTextAligment)a); })
        .def("SetTextMaterial", [](UTextRenderComponent& self, UMaterialInterface* mat) { self.SetTextMaterial(mat); })
        .def("SetTextRenderColor", [](UTextRenderComponent& self, FColor& c) { self.SetTextRenderColor(c); })
        .def("SetFont", [](UTextRenderComponent& self, UFont* font) { self.SetFont(font); })
        .def("SetHorizSpacingAdjust", [](UTextRenderComponent& self, float v) { self.SetHorizSpacingAdjust(v); })
        .def("SetVertSpacingAdjust", [](UTextRenderComponent& self, float v) { self.SetVertSpacingAdjust(v); })
        ;

    UEPY_EXPOSE_CLASS(UShapeComponent, UPrimitiveComponent, m)
        ;

    UEPY_EXPOSE_CLASS(USphereComponent, UShapeComponent, m)
        .def("SetSphereRadius", [](USphereComponent& self, float r) { self.SetSphereRadius(r); })
        ;

    UEPY_EXPOSE_CLASS(UBoxComponent, UShapeComponent, m)
        .def("SetBoxExtent", [](UBoxComponent& self, FVector& e) { self.SetBoxExtent(e); })
        .def("GetUnscaledBoxExtent", [](UBoxComponent& self) { return self.GetUnscaledBoxExtent(); }) // this is the same as boxComp.BoxExtent
        .def("IgnoreActorWhenMoving", [](UBoxComponent& self, AActor *actor, bool shouldIgnore) { self.IgnoreActorWhenMoving(actor, shouldIgnore); })
        ;

    UEPY_EXPOSE_CLASS(UCapsuleComponent, UShapeComponent, m)
        .def("SetCapsuleSize", [](UCapsuleComponent& self, float radius, float halfHeight) { self.SetCapsuleSize(radius, halfHeight); })
        .def("SetCapsuleRadius", [](UCapsuleComponent& self, float radius) { self.SetCapsuleRadius(radius); })
        .def("SetCapsuleHalfHeight", [](UCapsuleComponent& self, float halfHeight) { self.SetCapsuleHalfHeight(halfHeight); })
        ;

    UEPY_EXPOSE_CLASS(UBoxComponent_CGLUE, UBoxComponent, glueclasses)
        .def("SuperBeginPlay", [](UBoxComponent_CGLUE& self) { self.SuperBeginPlay(); })
        .def("SuperEndPlay", [](UBoxComponent_CGLUE& self, int reason) { self.SuperEndPlay((EEndPlayReason::Type)reason); })
        .def("SuperOnRegister", [](UBoxComponent_CGLUE& self) { self.SuperOnRegister(); })
        .def("OverrideTickAllowed", [](UBoxComponent_CGLUE& self, bool allowed) { self.tickAllowed = allowed; })
        ;

    UEPY_EXPOSE_CLASS(UMeshComponent, UPrimitiveComponent, m)
        .def("SetMaterial", [](UMeshComponent* self, int index, UMaterialInterface* mat)
        {   // we intercept calls to SetMaterial so that we can support temporary replacement of mesh materials for things
            // like object selection mode (where we 'gray out' objects unless they are of a certain type).
            MaterialArray* origMats = FPyObjectTracker::Get()->matOverrideMeshComps.Find(self);
            if (origMats)
            {   // we are currently in material override mode, so accept the SetMaterial call, but just tuck this mat
                // away for later when we leave override mode
                origMats->EmplaceAt(index, mat);
            }
            else // not in override mode, so set the material normally
                self->SetMaterial(index, mat);
        })
        .def("GetMaterial", [](UMeshComponent* self, int index)
        {   // since we intercept SetMaterial calls, we also intercept GetMaterial calls
            MaterialArray* origMats = FPyObjectTracker::Get()->matOverrideMeshComps.Find(self);
            if (origMats)
                return origMats->GetData()[index]; // we are in override mode, so return the original
            return self->GetMaterial(index);
        }, py::return_value_policy::reference)
        .def("SetOverrideMaterial", [](UMeshComponent* self, UMaterialInterface* newMat) // TODO: I think override mode is broken for SMC when we dynamically switch meshes
        {   // this enters/exits material override mode - mat=None to exit
            int matCount = self->GetNumMaterials();
            FPyObjectTracker* tracker = FPyObjectTracker::Get();
            MaterialArray* origMats = tracker->matOverrideMeshComps.Find(self);
            if (newMat)
            {   // caller passed a material, so they want to begin overriding
                if (!origMats) // if we're already overriding, we've already saved the original materials, so we don't want to stomp them
                {
                    MaterialArray mats;
                    mats.SetNumZeroed(matCount);
                    for (int i=0; i < matCount; i++)
                    {
                        mats[i] = self->GetMaterial(i); // remember the old
                        self->SetMaterial(i, newMat); // use the override
                    }
                    tracker->matOverrideMeshComps.Emplace(self, mats);
                }
            }
            else if (origMats)
            {   // leaving override mode, so restore the original mats
                for (int i=0; i < matCount; i++)
                    self->SetMaterial(i, origMats->GetData()[i]);
                tracker->matOverrideMeshComps.Remove(self);
            }
        })
        ;

    UEPY_EXPOSE_CLASS(UStaticMeshComponent, UMeshComponent, m)
        .def("GetStaticMesh", [](UStaticMeshComponent& self) { return self.GetStaticMesh(); }, py::return_value_policy::reference)
        .def("SetStaticMesh", [](UStaticMeshComponent& self, UStaticMesh *newMesh) -> bool { return self.SetStaticMesh(newMesh); })
        .def_readwrite("StreamingDistanceMultiplier", &UStaticMeshComponent::StreamingDistanceMultiplier)
        ;

    UEPY_EXPOSE_CLASS(UInstancedStaticMeshComponent, UStaticMeshComponent, m)
        .def("AddInstance", [](UInstancedStaticMeshComponent& self, FTransform& t) { return self.AddInstance(t); })
        .def("RemoveInstance", [](UInstancedStaticMeshComponent& self, int index) { return self.RemoveInstance(index); })
        .def("ClearInstances", [](UInstancedStaticMeshComponent& self) { self.ClearInstances(); })
        .def("GetInstanceCount", [](UInstancedStaticMeshComponent& self) { return self.GetInstanceCount(); })
        .def("SetCustomDataValue", [](UInstancedStaticMeshComponent& self, int32 instanceIndex, int32 customDataIndex, float customDataValue, bool bMarkRenderStateDirty) { return self.SetCustomDataValue(instanceIndex, customDataIndex, customDataValue, bMarkRenderStateDirty); })
        .def("BatchUpdateInstancesTransforms", [](UInstancedStaticMeshComponent& self, int32 startInstanceIndex, py::list &transforms, bool bWorldSpace, bool bMarkRenderStateDirty, bool bTeleport)
        {

            TArray<FTransform> newInstanceTransforms;
            for (py::handle h : transforms)
                newInstanceTransforms.Emplace(h.cast<FTransform>());
            return self.BatchUpdateInstancesTransforms(startInstanceIndex, newInstanceTransforms, bWorldSpace, bMarkRenderStateDirty, bTeleport);

        })
        .def_readwrite("InstancingRandomSeed", &UInstancedStaticMeshComponent::InstancingRandomSeed)
        .def_readwrite("NumCustomDataFloats", &UInstancedStaticMeshComponent::NumCustomDataFloats)
        ;

    UEPY_EXPOSE_CLASS(UWidgetComponent, UMeshComponent, m)
        .def("SetWidgetSpace", [](UWidgetComponent& self, int newSpace) { self.SetWidgetSpace((EWidgetSpace)newSpace); })
        .def("SetWidgetClass", [](UWidgetComponent& self, py::object& klass) { self.SetWidgetClass(PyObjectToUClass(klass)); })
        .def("SetWidget", [](UWidgetComponent& self, UUserWidget* w) { self.SetWidget(w); })
        .def("GetUserWidgetObject", [](UWidgetComponent& self) { return self.GetUserWidgetObject(); }, py::return_value_policy::reference)
        .def("SetDrawSize", [](UWidgetComponent& self, FVector2D& size) { self.SetDrawSize(size); })
        .def("SetGeometryMode", [](UWidgetComponent& self, int mode) { self.SetGeometryMode((EWidgetGeometryMode)mode); })
        .def("SetTwoSided", [](UWidgetComponent& self, bool two) { self.SetTwoSided(two); })
        ;

    UEPY_EXPOSE_CLASS(UWorld, UObject, m)
        .def_property_readonly("WorldType", [](UWorld& self) { return (int)self.WorldType; })
        BIT_PROP(bIsTearingDown, UWorld)
        .def("IsClient", [](UWorld& self) { return self.IsClient(); })
        .def("IsServer", [](UWorld& self) { return self.IsServer(); })
        .def("GetParameterCollectionInstance", [](UWorld& self, UMaterialParameterCollection* collection) { return self.GetParameterCollectionInstance(collection); }, py::return_value_policy::reference)
        .def("GetAllActors", [](UWorld* self)
        {
            py::list ret;
            for (TActorIterator<AActor> it(self); it ; ++it)
            {
                AActor *actor = *it;
                if (actor->IsValidLowLevel() && !actor->IsPendingKillOrUnreachable())
                    ret.append(actor);
            }
            return ret;
        }, py::return_value_policy::reference)
        .def("GetAllPlayerControllers", [](UWorld* self)
        {
            py::list ret;
            for(FConstPlayerControllerIterator it = self->GetPlayerControllerIterator(); it; ++it)
            {
                APlayerController* pc = Cast<APlayerController>(*it);
                if (pc != NULL && pc->IsValidLowLevel() && !pc->IsPendingKillOrUnreachable())
                    ret.append(pc);
            }
            return ret;
        }, py::return_value_policy::reference)
        ;

    // for use with engine-replicated actors only
    m.def("GetOrAssignNetGUID", [](UWorld* world, UObject* obj)
    {
        UNetDriver* driver = world->GetNetDriver();
        if (!driver) return -1;
        int32 value = driver->GuidCache->GetOrAssignNetGUID(obj).Value;
        return value;
    });

    m.def("GetAllWorlds", []()
    {
        py::list ret;
        for (TObjectIterator<UWorld> iter; iter; ++iter)
            ret.append(*iter);
        return ret;
    }, py::return_value_policy::reference);

    py::class_<UGameplayStatics, UObject, UnrealTracker<UGameplayStatics>>(m, "UGameplayStatics") // not sure that it makes sense to really expose this fully
        .def_static("GetGameInstance", [](UWorld *world) { return UGameplayStatics::GetGameInstance(world); }, py::return_value_policy::reference)
        .def_static("GetGameState", [](UWorld *world) { return UGameplayStatics::GetGameState(world); }, py::return_value_policy::reference)
        .def_static("GetAllActorsOfClass", [](UWorld *world, py::object& _klass)
        {
            TArray<AActor*> actors;
            py::list ret;
            UClass *klass = PyObjectToUClass(_klass);
            if (klass)
            {
                UGameplayStatics::GetAllActorsOfClass(world, klass, actors);
                for (AActor *a : actors)
                    ret.append(a);
            }
            return ret;
        }, py::return_value_policy::reference)
        .def_static("GetPlayerController", [](UObject *worldCtx, int playerIndex) { return UGameplayStatics::GetPlayerController(worldCtx, playerIndex); }, py::return_value_policy::reference)
        .def("SpawnEmitterAttached", [](UParticleSystem* emitterTemplate, USceneComponent* attachTo, std::string& socketName, bool autoDestroy)
        {
            FName attachName = NAME_None;
            EAttachLocation::Type loc = EAttachLocation::KeepRelativeOffset;
            if (socketName.length() > 0)
            {
                attachName = FSTR(socketName);
                loc = EAttachLocation::SnapToTarget;
            }
            return UGameplayStatics::SpawnEmitterAttached(emitterTemplate, attachTo, attachName, FVector(ForceInit), FRotator::ZeroRotator, FVector(1.f), loc, autoDestroy);
        }, py::arg("attachTo"), py::arg("socketName")="", py::arg("autoDestroy")=true, py::return_value_policy::reference)
        .def("SpawnEmitterAtLocation", [](UObject* worldCtx, UParticleSystem* emitterTemplate, FVector& loc, FRotator& rot, FVector& scale, bool autoDestroy) { return UGameplayStatics::SpawnEmitterAtLocation(worldCtx, emitterTemplate, loc, rot, scale, autoDestroy); }, py::return_value_policy::reference)
        .def_static("Blueprint_PredictProjectilePath_ByTraceChannel", [](UObject *worldCtx, FVector &start, FVector &launchVelocity, bool tracePath, float projectileRadius,
                    int channel, bool bTraceComplex, py::list& _ignore, int DrawDebugType, float DrawDebugTime, float SimFrequency, float MaxSimTime, float OverrideGravityZ)
        {
            TArray<AActor*> ignore;
            for (py::handle h : _ignore)
                ignore.Emplace(h.cast<AActor*>());
            FHitResult hitResult;
            TArray<FVector> pathPositions;
            FVector dest;
            bool hit = UGameplayStatics::Blueprint_PredictProjectilePath_ByTraceChannel(worldCtx, hitResult, pathPositions, dest, start, launchVelocity, tracePath, projectileRadius,
                    TEnumAsByte<ECollisionChannel>(channel), bTraceComplex, ignore, (EDrawDebugTrace::Type)DrawDebugType, DrawDebugTime, SimFrequency, MaxSimTime, OverrideGravityZ);
            //Return hitResult, pathPositions, finalDestination, bHit
            py::list ret;
            ret.append(hitResult);
            py::list path;
            for (auto &pos : pathPositions) path.append(pos);
            ret.append(path);
            ret.append(dest);
            ret.append(hit);
            return ret;
        }, py::return_value_policy::reference, py::arg("worldCtx"), py::arg("start"), py::arg("launchVelocity"), py::arg("tracePath"), py::arg("projectileRadius"), py::arg("channel"),
            py::arg("complex"), py::arg("_ignore"), py::arg("type")=(int)EDrawDebugTrace::None, py::arg("drawDebugTime")=0, py::arg("simFrequency")=15.f, py::arg("maxSimTime")=2.f,
            py::arg("overrideGravityZ")=0)
        .def_static("SetSoundMixClassOverride", [](UObject* worldCtx, USoundMix* soundMixModifier, USoundClass* soundClass, float volume, float pitch, float fadeInTime, bool applyToChildren)
        {
            UGameplayStatics::SetSoundMixClassOverride(worldCtx, soundMixModifier, soundClass, volume, pitch, fadeInTime, applyToChildren);
        }, py::arg("worldCtx"), py::arg("soundMixModifier"), py::arg("soundClass"), "volume"_a=1.0f, "pitch"_a=1.0f, "fadeInTime"_a=1.0f, "applyToChildren"_a=true)
        .def_static("PushSoundMixModifier", [](UObject* worldCtx, USoundMix* m) { UGameplayStatics::PushSoundMixModifier(worldCtx, m); })
        .def_static("PopSoundMixModifier", [](UObject* worldCtx, USoundMix* m) { UGameplayStatics::PopSoundMixModifier(worldCtx, m); })
        .def_static("ClearSoundMixModifiers", [](UObject* worldCtx) { UGameplayStatics::ClearSoundMixModifiers(worldCtx); })
        .def_static("GetPlayerCameraManager", [](UObject* worldCtx, int player) { return UGameplayStatics::GetPlayerCameraManager(worldCtx, player); }, py::return_value_policy::reference)
        .def_static("GetWorldDeltaSeconds", [](UObject* worldCtx) { return UGameplayStatics::GetWorldDeltaSeconds(worldCtx); })
        .def_static("GetTimeSeconds", [](UObject* worldCtx) { return UGameplayStatics::GetTimeSeconds(worldCtx); })
        .def_static("OpenLevel", [](UObject* worldCtx, std::string& levelName, bool absolute, std::string& options) { UGameplayStatics::OpenLevel(worldCtx, FName(FSTR(levelName)), absolute, FSTR(options)); })
        .def_static("PlaySound2D", [](UObject* worldCtx, USoundBase* sound, float volMult, float pitchMult, float startTime) { UGameplayStatics::PlaySound2D(worldCtx, sound, volMult, pitchMult, startTime); }, "worldCtx"_a, "sound"_a, "volMult"_a=1.0f, "pitchMult"_a=1.0f, "startTime"_a=0.0f)
        ;

    py::class_<FHitResult>(m, "FHitResult")
        .def_property_readonly("Normal", [](FHitResult& self) { FVector v = self.Normal; return v; }) // in FHitResult, these aren't FVector but are FVector_NetQuantize instead :(
        .def_property_readonly("Location", [](FHitResult& self) { FVector v = self.Location; return v; })
        .def_property_readonly("ImpactPoint", [](FHitResult& self) { FVector v = self.ImpactPoint; return v; })
        .def_property_readonly("ImpactNormal", [](FHitResult& self) { FVector v = self.ImpactNormal; return v; })
        .def_property_readonly("PhysMaterial", [](FHitResult& self)
        {
            UPhysicalMaterial* m = nullptr;
            if (self.PhysMaterial.IsValid())
            m = self.PhysMaterial.Get();
            return m;
        }, py::return_value_policy::reference)
        .def_property("Actor", [](FHitResult& self)
        {
            AActor* a = nullptr;
            if (self.Actor.IsValid())
                a = self.Actor.Get();
            return a;
        }, [](FHitResult& self, AActor* a) { self.Actor = a; }, py::return_value_policy::reference)
        .def_property("Component", [](FHitResult& self)
        {
            UPrimitiveComponent* c = nullptr;
            if (self.Component.IsValid())
                c = self.Component.Get();
            return c;
        }, [](FHitResult& self, UPrimitiveComponent* c) { self.Component = c; }, py::return_value_policy::reference)
        .def_readonly("Time", &FHitResult::Time)
        .def_readonly("Distance", &FHitResult::Distance)
        ;

    py::class_<UKismetRenderingLibrary, UObject, UnrealTracker<UKismetRenderingLibrary>>(m, "UKismetRenderingLibrary")
        .def_static("CreateRenderTarget2D", [](UObject* worldCtx, int w, int h, int format) { return UKismetRenderingLibrary::CreateRenderTarget2D(worldCtx, w, h, (ETextureRenderTargetFormat)format); }, py::return_value_policy::reference)
        .def_static("ReleaseRenderTarget2D", [](UTextureRenderTarget2D* target) { UKismetRenderingLibrary::ReleaseRenderTarget2D(target); })
        .def_static("ExportRenderTarget", [](UObject* worldCtx, UTextureRenderTarget2D* target, std::string& filePath, std::string& fileName) { UKismetRenderingLibrary::ExportRenderTarget(worldCtx, target, FSTR(filePath), FSTR(fileName)); })
        ;

    py::class_<UKismetSystemLibrary, UObject, UnrealTracker<UKismetSystemLibrary>>(m, "UKismetSystemLibrary")
        .def_static("ExecuteConsoleCommand", [](UObject* worldCtx, std::string& cmd) { UKismetSystemLibrary::ExecuteConsoleCommand(worldCtx, FSTR(cmd)); })
        .def_static("GetPathName", [](UObject* obj) { return PYSTR(UKismetSystemLibrary::GetPathName(obj)); })
        .def_static("GetDisplayName", [](UObject* obj) { return PYSTR(UKismetSystemLibrary::GetDisplayName(obj)); })
        .def_static("GetObjectName", [](UObject* obj) { return PYSTR(UKismetSystemLibrary::GetObjectName(obj)); })
        .def_static("IsValid", [](UObject* obj) { return UKismetSystemLibrary::IsValid(obj); })
        .def_static("DrawDebugLine", [](UObject *worldCtx, FVector& start, FVector& end, FLinearColor& color, float duration, float thickness) { UKismetSystemLibrary::DrawDebugLine(worldCtx, start, end, color, duration, thickness); })
        .def_static("DrawDebugBox", [](UObject *worldCtx, FVector& center, FVector& extent, FLinearColor& color, FRotator& rot, float duration, float thickness) { UKismetSystemLibrary::DrawDebugBox(worldCtx, center, extent, color, rot, duration, thickness); })
        .def_static("DrawDebugConeInDegrees", [](UObject *worldCtx, FVector& origin, FVector& dir, float len, float angleW, float angleH, int numSides, FLinearColor& color, float dur, float thickness) { UKismetSystemLibrary::DrawDebugConeInDegrees(worldCtx, origin, dir, len, angleW, angleH, numSides, color, dur, thickness); })
        .def_static("DrawDebugPlane", [](UObject* worldCtx, FPlane& plane, FVector& loc, float size, FLinearColor& color, float duration) { UKismetSystemLibrary::DrawDebugPlane(worldCtx, plane, loc, size, color, duration); })
        .def_static("DrawDebugSphere", [](UObject *worldCtx, FVector& center, float radius, int segs, FLinearColor& color, float duration, float thickness) { UKismetSystemLibrary::DrawDebugSphere(worldCtx, center, radius, segs, color, duration, thickness); })
        .def_static("DrawDebugArrow", [](UObject* worldCtx, FVector& start, FVector& end, float size, FLinearColor& color, float duration, float thickness) { UKismetSystemLibrary::DrawDebugArrow(worldCtx, start, end, size, color, duration, thickness); })
        .def_static("LineTraceSingle", [](UObject *worldCtx, FVector& start, FVector& end, int channel, bool isComplex, py::list& _ignore, int debugType, bool ignoreSelf, FLinearColor& traceColor, FLinearColor& hitColor, float drawTime)
        {
            TArray<AActor*> ignore;
            for (py::handle h : _ignore)
                ignore.Emplace(h.cast<AActor*>());
            FHitResult hitResult;
            bool hit = UKismetSystemLibrary::LineTraceSingle(worldCtx, start, end, (ETraceTypeQuery)channel, isComplex, ignore, (EDrawDebugTrace::Type)debugType, hitResult, ignoreSelf, traceColor, hitColor, drawTime);
            return py::make_tuple(hitResult, hit);
        }, py::return_value_policy::reference, py::arg("worldCtx"), py::arg("start"), py::arg("end"), py::arg("channel"), py::arg("isComplex"), py::arg("_ignore"), py::arg("type")=(int)EDrawDebugTrace::None, py::arg("ignoreSelf")=true, py::arg("traceColor")=FLinearColor::Red, py::arg("hitColor")=FLinearColor::Green, py::arg("drawTime")=5.0f)
        .def_static("LineTraceMulti", [](UObject* worldCtx, FVector& start, FVector& end, int channel, bool isComplex, py::list& _ignore, int debugType, bool ignoreSelf, FLinearColor& traceColor, FLinearColor& hitColor, float drawTime)
        {
            TArray<AActor*> ignore;
            for (py::handle h : _ignore)
                ignore.Emplace(h.cast<AActor*>());
            TArray<FHitResult> hits;
            UKismetSystemLibrary::LineTraceMulti(worldCtx, start, end, (ETraceTypeQuery)channel, isComplex, ignore, (EDrawDebugTrace::Type)debugType, hits, ignoreSelf, traceColor, hitColor, drawTime);
            py::list retHits;
            for (auto h : hits)
                retHits.append(h);
            return retHits; // if retHits is empty, it means didHit is false, so no need to send it to the caller
        }, py::return_value_policy::reference, py::arg("worldCtx"), py::arg("start"), py::arg("end"), py::arg("channel"), py::arg("isComplex"), py::arg("_ignore"), py::arg("debugType")=(int)EDrawDebugTrace::None, py::arg("ignoreSelf")=true, py::arg("traceColor")=FLinearColor::Red, py::arg("hitColor")=FLinearColor::Green, py::arg("drawTime")=5.0f)
        .def_static("LineTraceMultiForObjects", [](UObject* worldCtx, FVector& start, FVector& end, py::list& _objectTypes, bool isComplex, py::list& _ignore, int debugType, bool ignoreSelf, FLinearColor& traceColor, FLinearColor& hitColor, float drawTime)
        {
            TArray<TEnumAsByte<EObjectTypeQuery>> objectTypes;
            for (py::handle h: _objectTypes)
                objectTypes.Emplace(EObjectTypeQuery(h.cast<int>()));
            TArray<AActor*> ignore;
            for (py::handle h : _ignore)
                ignore.Emplace(h.cast<AActor*>());
            TArray<FHitResult> hits;
            UKismetSystemLibrary::LineTraceMultiForObjects(worldCtx, start, end, objectTypes, isComplex, ignore, (EDrawDebugTrace::Type)debugType, hits, ignoreSelf, traceColor, hitColor, drawTime);
            py::list retHits;
            for (auto h : hits)
                retHits.append(h);
            return retHits; // if retHits is empty, it means didHit is false, so no need to send it to the caller
        }, py::return_value_policy::reference, py::arg("worldCtx"), py::arg("start"), py::arg("end"), py::arg("objectTypes"), py::arg("isComplex"), py::arg("_ignore"), py::arg("debugType") = (int)EDrawDebugTrace::None, py::arg("ignoreSelf") = true, py::arg("traceColor") = FLinearColor::Red, py::arg("hitColor") = FLinearColor::Green, py::arg("drawTime") = 5.0f)
        .def_static("BoxTraceSingle", [](UObject* worldCtx, FVector& start, FVector& end, FVector& halfSize, FRotator& orientation, int channel, bool isComplex, py::list& _ignore, int debugType, bool ignoreSelf, FLinearColor& traceColor, FLinearColor& hitColor, float drawTime)
        {
            TArray<AActor*> ignore;
            for (py::handle h : _ignore)
                ignore.Emplace(h.cast<AActor*>());
            FHitResult hitResult;
            bool hit = UKismetSystemLibrary::BoxTraceSingle(worldCtx, start, end, halfSize, orientation, (ETraceTypeQuery)channel, isComplex, ignore, (EDrawDebugTrace::Type)debugType, hitResult, ignoreSelf, traceColor, hitColor, drawTime);
            return py::make_tuple(hitResult, hit);
        }, py::return_value_policy::reference, py::arg("worldCtx"), py::arg("start"), py::arg("end"), py::arg("halfSize"), py::arg("orientation"), py::arg("channel"), py::arg("isComplex"), py::arg("_ignore"), py::arg("debugType"), py::arg("ignoreSelf"), py::arg("traceColor")=FLinearColor::Red, py::arg("hitColor")=FLinearColor::Green, py::arg("drawTime")=5.0f)
        .def_static("QuitGame", [](UObject* worldCtx) { UKismetSystemLibrary::QuitGame(worldCtx, nullptr, EQuitPreference::Quit, false); })
        .def_static("GetSupportedFullscreenResolutions", []()
        { // the engine API is a little messy, so clean it up
            TArray<FIntPoint> resolutions;
            UKismetSystemLibrary::GetSupportedFullscreenResolutions(resolutions);
            py::list ret;
            for (auto res : resolutions)
                ret.append(py::make_tuple(res.X, res.Y));
            return ret;
        })
        ;

    py::class_<UImportanceSamplingLibrary, UObject, UnrealTracker<UImportanceSamplingLibrary>>(m, "UImportanceSamplingLibrary")
        .def_static("RandomSobolCell2D", [](int index, int numCells, FVector2D& cell, FVector2D& seed) { return UImportanceSamplingLibrary::RandomSobolCell2D(index, numCells, cell, seed); })
        ;

    py::class_<FBox>(m, "FBox")
        .def(py::init<FVector,FVector>())
        .def(py::init([](bool init) { if (init) return FBox(EForceInit::ForceInit); else return FBox(); }))
        .def_readwrite("Min", &FBox::Min)
        .def_readwrite("Max", &FBox::Max)
        .def_readwrite("min", &FBox::Min)
        .def_readwrite("max", &FBox::Max)
        .def_readwrite("IsValid", &FBox::IsValid)
        .def("ExpandBy", [](FBox& self, float W) { return self.ExpandBy(W); })
        .def("GetCenter", [](FBox& self) { return self.GetCenter(); })
        .def("GetExtent", [](FBox& self) { return self.GetExtent(); })
        .def("GetSize", [](FBox& self) { return self.GetSize(); })
        .def("Intersect", [](FBox &self, FBox &other) { return self.Intersect(other); })
        .def("IsInsideOrOn", [](FBox &self, FVector &in) { return self.IsInsideOrOn(in); })
        .def("TransformBy", [](FBox& self, const FTransform &M) { return self.TransformBy(M); })
        .def("ToString", [](FBox& self) { return PYSTR(self.ToString()); })
        .def(py::self + py::self)
        .def(py::self += py::self)
        ;

    py::class_<FBoxSphereBounds>(m, "FBoxSphereBounds")
        .def(py::init([](bool init) { if (init) return FBoxSphereBounds(EForceInit::ForceInit); else return FBoxSphereBounds(); }))
        .def(py::init<FBoxSphereBounds>())
        .def(py::init<FVector&,FVector&,float>())
        .def(py::init<FBox&,FSphere&>())
        .def(py::init<FBox&>())
        .def(py::self + py::self)
        .def(py::self == py::self)
        .def_readwrite("Origin", &FBoxSphereBounds::Origin)
        .def_readwrite("BoxExtent", &FBoxSphereBounds::BoxExtent)
        .def_readwrite("SphereRadius", &FBoxSphereBounds::SphereRadius)
        .def("GetBox", [](FBoxSphereBounds& self) { return self.GetBox(); })
        .def("GetSphere", [](FBoxSphereBounds& self) { return self.GetSphere(); })
        .def("TransformBy", [](FBoxSphereBounds& self, FTransform& t) { return self.TransformBy(t); })
        ;

    py::class_<UKismetMathLibrary, UObject, UnrealTracker<UKismetMathLibrary>>(m, "UKismetMathLibrary")
        .def_static("DegSin", [](float& a) { return UKismetMathLibrary::DegSin(a); })
        .def_static("DegAsin", [](float& a) { return UKismetMathLibrary::DegAsin(a); })
        .def_static("DegCos", [](float& a) { return UKismetMathLibrary::DegCos(a); })
        .def_static("DegAcos", [](float& a) { return UKismetMathLibrary::DegAcos(a); })
        .def_static("DegTan", [](float& a) { return UKismetMathLibrary::DegTan(a); })
        .def_static("DegAtan", [](float& a) { return UKismetMathLibrary::DegAtan(a); })
        .def_static("Ease", [](float& a,float& b, float alpha, int easingFunc, float blend, int steps) { return UKismetMathLibrary::Ease(a, b, alpha, (EEasingFunc::Type)easingFunc, blend, steps); }, "a"_a, "b"_a, "alpha"_a, "easingFunc"_a=7, "blend"_a=2, "steps"_a=2) // 7=easeInOut
        .def_static("TEase", [](FTransform& a, FTransform& b, float alpha, int easingFunc, float blend, int steps) { return UKismetMathLibrary::TEase(a, b, alpha, (EEasingFunc::Type)easingFunc, blend, steps); }, "a"_a, "b"_a, "alpha"_a, "easingFunc"_a=7, "blend"_a=2, "steps"_a=2)
        .def_static("VEase", [](FVector& a, FVector& b, float alpha, int easingFunc, float blend, int steps) { return UKismetMathLibrary::VEase(a, b, alpha, (EEasingFunc::Type)easingFunc, blend, steps); }, "a"_a, "b"_a, "alpha"_a, "easingFunc"_a=7, "blend"_a=2, "steps"_a=2)
        .def_static("REase", [](FRotator& a, FRotator& b, float alpha, bool shortestPath, int easingFunc, float blend, int steps) { return UKismetMathLibrary::REase(a, b, alpha, shortestPath, (EEasingFunc::Type)easingFunc, blend, steps); }, "a"_a, "b"_a, "alpha"_a, "shortestPath"_a=true, "easingFunc"_a=7, "blend"_a=2, "steps"_a=2)
        .def_static("EqualEqual_VectorVector", [](FVector& a, FVector& b, float error) { return UKismetMathLibrary::EqualEqual_VectorVector(a, b, error); }, py::arg("a"), py::arg("b"), py::arg("error")=1.e-4f)
        .def_static("EqualEqual_Vector2DVector2D", [](FVector2D& a, FVector2D& b, float error) { return UKismetMathLibrary::EqualEqual_Vector2DVector2D(a, b, error); }, py::arg("a"), py::arg("b"), py::arg("error")=1.e-4f)
        .def_static("LinearColor_IsNearEqual", [](FLinearColor& a, FLinearColor& b, float error) { return UKismetMathLibrary::LinearColor_IsNearEqual(a, b, error); }, py::arg("a"), py::arg("b"), py::arg("error")=1.e-4f)
        .def_static("EqualEqual_QuatQuat", [](FQuat& a, FQuat& b, float error) { return UKismetMathLibrary::EqualEqual_QuatQuat(a, b, error); }, py::arg("a"), py::arg("b"), py::arg("error")=1.e-4f)
        .def_static("EqualEqual_RotatorRotator", [](FRotator& a, FRotator& b, float error) { return UKismetMathLibrary::EqualEqual_RotatorRotator(a, b, error); }, py::arg("a"), py::arg("b"), py::arg("error")=1.e-4f)
        .def_static("NearlyEqual_FloatFloat", [](float a, float b, float error) { return UKismetMathLibrary::NearlyEqual_FloatFloat(a, b, error); }, py::arg("a"), py::arg("b"), py::arg("error")=1.e-4f)
        .def_static("FClamp", [](float v, float min, float max) { return UKismetMathLibrary::FClamp(v, min, max); })
        .def_static("FindLookAtRotation", [](FVector& start, FVector& target) { return UKismetMathLibrary::FindLookAtRotation(start, target); })
        .def_static("GetForwardVector", [](FRotator& rot) { return UKismetMathLibrary::GetForwardVector(rot); })
        .def_static("GetRightVector", [](FRotator& rot) { return UKismetMathLibrary::GetRightVector(rot); })
        .def_static("GetUpVector", [](FRotator& rot) { return UKismetMathLibrary::GetUpVector(rot); })
        .def_static("Hypotenuse", [](float w, float h) { return UKismetMathLibrary::Hypotenuse(w, h); })
        .def_static("InverseTransformRotation", [](FTransform& t, FRotator& r) { return UKismetMathLibrary::InverseTransformRotation(t, r); })
        .def_static("InverseTransformLocation", [](FTransform& t, FVector& l) { return UKismetMathLibrary::InverseTransformLocation(t, l); })
        .def_static("MirrorVectorByNormal", [](FVector& v, FVector& normal) { return UKismetMathLibrary::MirrorVectorByNormal(v, normal); })
        .def_static("Normal", [](FVector& a, float tolerance) { return UKismetMathLibrary::Normal(a, tolerance); }, py::arg("a"), py::arg("tolerance")=1.e-4f)
        .def_static("NormalizeToRange", [](float v, float min, float max) { return UKismetMathLibrary::NormalizeToRange(v, min, max); })
        .def_static("MakeRotFromX", [](FVector& x) { return UKismetMathLibrary::MakeRotFromX(x); })
        .def_static("MakeRotFromY", [](FVector& y) { return UKismetMathLibrary::MakeRotFromY(y); })
        .def_static("MakeRotFromZ", [](FVector& z) { return UKismetMathLibrary::MakeRotFromZ(z); })
        .def_static("MakeRotFromXY", [](FVector& x, FVector& y) { return UKismetMathLibrary::MakeRotFromXY(x, y); })
        .def_static("MakeRotFromXZ", [](FVector& x, FVector& z) { return UKismetMathLibrary::MakeRotFromXZ(x, z); })
        .def_static("MakeRotFromYX", [](FVector& y, FVector& x) { return UKismetMathLibrary::MakeRotFromYX(y, x); })
        .def_static("MakeRotFromYZ", [](FVector& y, FVector& z) { return UKismetMathLibrary::MakeRotFromYZ(y, z); })
        .def_static("MakeRotFromZX", [](FVector& z, FVector& x) { return UKismetMathLibrary::MakeRotFromZX(z, x); })
        .def_static("MakeRotFromZY", [](FVector& z, FVector& y) { return UKismetMathLibrary::MakeRotFromZY(z, y); })
        .def_static("NearlyEqual_FloatFloat", [](float a, float b, float tolerance) { return UKismetMathLibrary::NearlyEqual_FloatFloat(a, b, tolerance); })
        .def_static("ProjectVectorOnToPlane", [](FVector& v, FVector& planeNormal) { return UKismetMathLibrary::ProjectVectorOnToPlane(v, planeNormal); })
        .def_static("RandomPointInBoundingBox", [](FVector origin, FVector boxExtent) { return UKismetMathLibrary::RandomPointInBoundingBox(origin, boxExtent); })
        .def_static("RandomUnitVectorInConeInDegrees", [](FVector& coneDir, float coneHalfAngle) { return UKismetMathLibrary::RandomUnitVectorInConeInDegrees(coneDir, coneHalfAngle); })
        .def_static("InRange_FloatFloat", [](float v, float min, float max, bool inclusiveMin, bool inclusiveMax) { return UKismetMathLibrary::InRange_FloatFloat(v, min, max, inclusiveMin, inclusiveMax); }, py::arg("v"), py::arg("min"), py::arg("max"), py::arg("inclusiveMin")=true, py::arg("inclusiveMax")=true)
        .def_static("Lerp", [](float a, float b, float alpha) { return UKismetMathLibrary::Lerp(a, b, alpha); })
        .def_static("RLerp", [](FRotator a, FRotator b, float alpha, bool shortestPath) { return UKismetMathLibrary::RLerp(a,b,alpha,shortestPath); })
        .def_static("VLerp", [](FVector a, FVector b, float alpha) { return UKismetMathLibrary::VLerp(a,b,alpha); })
        .def_static("VSize", [](FVector& a) { return UKismetMathLibrary::VSize(a); })
        .def_static("FInterpTo", [](float cur, float target, float deltaTime, float speed) { return UKismetMathLibrary::FInterpTo(cur, target, deltaTime, speed); })
        .def_static("RInterpTo", [](FRotator& cur, FRotator& target, float deltaTime, float speed) { return UKismetMathLibrary::RInterpTo(cur, target, deltaTime, speed); })
        .def_static("VInterpTo", [](FVector& cur, FVector& target, float deltaTime, float speed) { return UKismetMathLibrary::VInterpTo(cur, target, deltaTime, speed); })
        .def_static("TInterpTo", [](FTransform& cur, FTransform& target, float deltaTime, float speed) { return UKismetMathLibrary::TInterpTo(cur, target, deltaTime, speed); })
        .def_static("RotateAngleAxis", [](FVector& v, float angleDeg, FVector& axis) { return UKismetMathLibrary::RotateAngleAxis(v, angleDeg, axis); })
        .def_static("RGBToHSV", [](FLinearColor& c)
        {
            float h,s,v,a;
            UKismetMathLibrary::RGBToHSV(c, h, s, v, a);
            py::list ret;
            ret.append(h);
            ret.append(s);
            ret.append(v);
            ret.append(a);
            return ret;
        })
        .def_static("HSVToRGB", [](float h, float s, float v, float a) { return UKismetMathLibrary::HSVToRGB(h,s,v,a); })
        .def_static("TransformRotation", [](FTransform& t, FRotator& r) { return UKismetMathLibrary::TransformRotation(t, r); })
        .def_static("TransformLocation", [](FTransform& t, FVector& l) { return UKismetMathLibrary::TransformLocation(t, l); })
        .def_static("LinePlaneIntersection_OriginNormal", [](FVector& lineStart, FVector& lineEnd, FVector& planeOrigin, FVector& planeNormal)
        {   // returns (intersected, point of intersection, "time" along ray of intersection)
            FVector intersectPt;
            float t;
            bool intersected = UKismetMathLibrary::LinePlaneIntersection_OriginNormal(lineStart, lineEnd, planeOrigin, planeNormal, t, intersectPt);
            return py::make_tuple(intersected, intersectPt, t);
        })
        ;

    UEPY_EXPOSE_CLASS(UMaterialInterface, UObject, m)
        .def("GetScalarParameterValue", [](UMaterialInterface& self, std::string name) { float f; self.GetScalarParameterValue(FSTR(name), f); return f; })
        .def("GetVectorParameterValue", [](UMaterialInterface& self, std::string name) { FLinearColor c; self.GetVectorParameterValue(FSTR(name), c); return c; })
        ;

    UEPY_EXPOSE_CLASS(UMaterial, UMaterialInterface, m)
        ;

    UEPY_EXPOSE_CLASS(UMaterialInstance, UMaterialInterface, m)
        .def_readwrite("Parent", &UMaterialInstance::Parent, py::return_value_policy::reference)
        .def_readwrite("PhysMaterial", &UMaterialInstance::PhysMaterial, py::return_value_policy::reference)
        .def("GetPhysicalMaterial", [](UMaterialInstance& self) { return self.GetPhysicalMaterial(); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(UMaterialInstanceConstant, UMaterialInstance, m)
        ;

    UEPY_EXPOSE_CLASS(UMaterialInstanceDynamic, UMaterialInstance, m)
        .def_static("Create", [](UMaterialInterface* material, UObject* outer)
        {
            UMaterialInstanceDynamic* mat = UMaterialInstanceDynamic::Create(material, outer);
            if (outer == nullptr)
                mat->SetFlags(RF_Transient);
            return mat;
        }, py::return_value_policy::reference, py::arg("material"), py::arg("outer")=nullptr)
        .def("SetTextureParameterValue", [](UMaterialInstanceDynamic& self, std::string name, UTexture* value) -> void { self.SetTextureParameterValue(UTF8_TO_TCHAR(name.c_str()), value); })
        .def("SetScalarParameterValue", [](UMaterialInstanceDynamic& self, std::string name, float v) { self.SetScalarParameterValue(FSTR(name), v); })
        .def("SetVectorParameterValue", [](UMaterialInstanceDynamic& self, std::string name, FLinearColor& v) { self.SetVectorParameterValue(FSTR(name), v); })
        ;

    UEPY_EXPOSE_CLASS(UMaterialParameterCollection, UObject, m)
        .def("GetParameterNames", [](UMaterialParameterCollection& self) // returns (list of scalar param names, list of vector param names)
        {
            TArray<FName> names;
            self.GetParameterNames(names, false);
            py::list scalarNames, vectorNames;
            for (auto fname : names)
                scalarNames.append(PYSTR(fname.ToString()));
            names.Empty();
            self.GetParameterNames(names, true);
            for (auto fname : names)
                vectorNames.append(PYSTR(fname.ToString()));
            return py::make_tuple(scalarNames, vectorNames);
        })
        ;

    UEPY_EXPOSE_CLASS(UMaterialParameterCollectionInstance, UObject, m)
        .def("SetScalarParameterValue", [](UMaterialParameterCollectionInstance& self, std::string name, float value) { return self.SetScalarParameterValue(FSTR(name), value); })
        .def("SetVectorParameterValue", [](UMaterialParameterCollectionInstance& self, std::string name, FLinearColor& value) { return self.SetVectorParameterValue(FSTR(name), value); })
        .def("GetScalarParameterValue", [](UMaterialParameterCollectionInstance& self, std::string name) { float v=0; self.GetScalarParameterValue(FSTR(name), v); return v; })
        .def("GetVectorParameterValue", [](UMaterialParameterCollectionInstance& self, std::string name) { FLinearColor v; self.GetVectorParameterValue(FSTR(name), v); return v; })
        ;

    py::class_<UFXSystemAsset, UObject, UnrealTracker<UFXSystemAsset>>(m, "UFXSystemAsset")
        .def_static("StaticClass", []() { return UFXSystemAsset::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UFXSystemAsset>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UNiagaraSystem, UFXSystemAsset, UnrealTracker<UNiagaraSystem>>(m, "UNiagaraSystem")
        .def_static("StaticClass", []() { return UNiagaraSystem::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UNiagaraSystem>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UParticleSystem, UFXSystemAsset, UnrealTracker<UParticleSystem>>(m, "UParticleSystem")
        .def_static("StaticClass", []() { return UParticleSystem::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UParticleSystem>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UKismetMaterialLibrary, UObject, UnrealTracker<UKismetMaterialLibrary>>(m, "UKismetMaterialLibrary")
        .def_static("CreateDynamicMaterialInstance", [](UObject *worldCtx, UMaterialInterface *parent) { return UKismetMaterialLibrary::CreateDynamicMaterialInstance(worldCtx, parent); }, py::return_value_policy::reference)
        .def_static("GetVectorParameterValue", [](UObject* worldCtx, UMaterialParameterCollection* coll, std::string name) { return UKismetMaterialLibrary::GetVectorParameterValue(worldCtx, coll, FSTR(name)); })
        ;

    py::class_<UTexture, UObject, UnrealTracker<UTexture>>(m, "UTexture")
        .def_static("StaticClass", []() { return UTexture::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UTexture>(w); }, py::return_value_policy::reference)
        BIT_PROP(SRGB, UTexture)
        ;

    py::class_<UTexture2D, UTexture, UnrealTracker<UTexture2D>>(m, "UTexture2D")
        .def_static("StaticClass", []() { return UTexture2D::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UTexture2D>(w); }, py::return_value_policy::reference)
        .def("GetSizeX", [](UTexture2D& self) { return self.GetSizeX(); })
        .def("GetSizeY", [](UTexture2D& self) { return self.GetSizeY(); })
        ;

    py::class_<UTextureRenderTarget, UTexture, UnrealTracker<UTextureRenderTarget>>(m, "UTextureRenderTarget")
        .def_static("StaticClass", []() { return UTextureRenderTarget::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UTextureRenderTarget>(w); }, py::return_value_policy::reference)
        .def_readwrite("TargetGamma", &UTextureRenderTarget::TargetGamma)
        ;

    py::class_<UTextureRenderTarget2D, UTextureRenderTarget, UnrealTracker<UTextureRenderTarget2D>>(m, "UTextureRenderTarget2D")
        .def_static("StaticClass", []() { return UTextureRenderTarget2D::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UTextureRenderTarget2D>(w); }, py::return_value_policy::reference)
        .def_readonly("SizeX", &UTextureRenderTarget2D::SizeX)
        .def_readonly("SizeY", &UTextureRenderTarget2D::SizeY)
        .def_readonly("ClearColor", &UTextureRenderTarget2D::ClearColor)
        ;

    UEPY_EXPOSE_CLASS(UTextureRenderTargetCube, UTextureRenderTarget, m)
        .def("Init", [](UTextureRenderTargetCube& self, int sizeX, int format) { self.Init(sizeX, (EPixelFormat)format); })
        .def("InitAutoFormat", [](UTextureRenderTargetCube& self, int sizeX) { self.InitAutoFormat(sizeX); })
        .def_readwrite("SizeX", &UTextureRenderTargetCube::SizeX)
        BIT_PROP(bHDR, UTextureRenderTargetCube)
        BIT_PROP(bForceLinearGamma, UTextureRenderTargetCube)
        ;

    py::class_<UCanvasRenderTarget2D, UTextureRenderTarget2D, UnrealTracker<UCanvasRenderTarget2D>>(m, "UCanvasRenderTarget2D")
        .def_static("StaticClass", []() { return UCanvasRenderTarget2D::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UCanvasRenderTarget2D>(w); }, py::return_value_policy::reference)
        .def_static("CreateCanvasRenderTarget2D", [](UObject* worldCtx, py::object& _subclass, int w, int h)
        {
            UClass *subclass = PyObjectToUClass(_subclass);
            if (!subclass)
            {
                LERROR("Cannot convert class param to a subclass of CanvasRenderTarget2D");
                return (UCanvasRenderTarget2D*)nullptr;
            }
            return UCanvasRenderTarget2D::CreateCanvasRenderTarget2D(worldCtx, subclass, w, h);
        }, py::return_value_policy::reference)
        ;

    py::class_<UMediaTexture, UTexture, UnrealTracker<UMediaTexture>>(m, "UMediaTexture")
        .def_static("StaticClass", []() { return UMediaTexture::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UMediaTexture>(w); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(UTextureCube, UTexture, m)
        ;

    UEPY_EXPOSE_CLASS(UHapticFeedbackEffect_Base, UObject, m)
        .def("GetDuration", [](UHapticFeedbackEffect_Base& self) { return self.GetDuration(); })
        ;
    UEPY_EXPOSE_CLASS(UHapticFeedbackEffect_Curve, UHapticFeedbackEffect_Base, m)
        ;

    py::class_<UGameInstance, UObject, UnrealTracker<UGameInstance>>(m, "UGameInstance")
        .def_static("StaticClass", []() { return UGameInstance::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UGameInstance>(w); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(AActor, UObject, m)
        //.def("HasLocalNetOwner", [](AActor& self) { return self.HasLocalNetOwner(); }) <--- don't use this, doesn't work with NR
        .def("SetReplicates", [](AActor& self, bool b) { self.SetReplicates(b); })
        //.def("GetIsReplicated", [](AActor& self) { return self.GetIsReplicated(); }) <--- doesn't work with NR
        .def("SetCanBeDamaged", [](AActor& self, bool b) { self.SetCanBeDamaged(b); })
        .def_readwrite("InputComponent", &AActor::InputComponent)
        BIT_PROP(bAlwaysRelevant, AActor)
        ENUM_PROP(SpawnCollisionHandlingMethod, ESpawnActorCollisionHandlingMethod, AActor)
        .def("GetWorld", [](AActor& self) { return self.GetWorld(); }, py::return_value_policy::reference)
        .def("GetActorLocation", [](AActor& self) { return self.GetActorLocation(); })
        .def("SetActorLocation", [](AActor& self, FVector& v) { return self.SetActorLocation(v); })
        .def("GetActorRotation", [](AActor& self) { return self.GetActorRotation(); })
        .def("SetActorRotation", [](AActor& self, FRotator& r) { self.SetActorRotation(r); })
        .def("SetActorRotation", [](AActor& self, FQuat& q) { self.SetActorRotation(q); })
        .def("SetActorLocationAndRotation", [](AActor& self, FVector& loc, FRotator& rot) { self.SetActorLocationAndRotation(loc, rot); })
        .def("SetActorTransform", [](AActor& self, FTransform& t) { self.SetActorTransform(t); })
        .def("GetActorTransform", [](AActor& self) { return self.GetActorTransform(); })
        .def("GetTransform", [](AActor& self) { return self.GetTransform(); })
        .def("GetActorForwardVector", [](AActor& self) { return self.GetActorForwardVector(); })
        .def("GetActorUpVector", [](AActor& self) { return self.GetActorUpVector(); })
        .def("GetActorRightVector", [](AActor& self) { return self.GetActorRightVector(); })
        .def("SetRootComponent", [](AActor& self, USceneComponent *s) { self.SetRootComponent(s); })
        .def("GetRootComponent", [](AActor& self) { return self.GetRootComponent(); }, py::return_value_policy::reference)
        .def("SetActorScale3D", [](AActor& self, FVector& v) { self.SetActorScale3D(v); })
        .def("GetActorScale3D", [](AActor& self) { return self.GetActorScale3D(); })
        .def("Destroy", [](AActor& self) { self.Destroy(); })
        .def("IsPendingKillPending", [](AActor& self) { return self.IsPendingKillPending(); })
        .def("IsActorTickEnabled", [](AActor& self) { return self.IsActorTickEnabled(); })
        .def("SetTickGroup", [](AActor& self, int group) { self.SetTickGroup((ETickingGroup)group); })
        .def("SetActorTickEnabled", [](AActor& self, bool enabled) { self.SetActorTickEnabled(enabled); })
        .def("SetActorTickInterval", [](AActor& self, float interval) { self.SetActorTickInterval(interval); })
        .def("GetActorTickInterval", [](AActor& self) { return self.GetActorTickInterval(); })
        .def("SetReplicateMovement", [](AActor& self, bool b) { self.SetReplicateMovement(b); })
        .def("IsHidden", [](AActor& self) { return self.IsHidden(); })
        .def("SetActorHiddenInGame", [](AActor& self, bool b) { self.SetActorHiddenInGame(b); })
        .def("HasAuthority", [](AActor& self) { return self.HasAuthority(); })
        .def("GetOwner", [](AActor& self) { return self.GetOwner(); }, py::return_value_policy::reference)
        .def("SetOwner", [](AActor& self, AActor *newOwner) { self.SetOwner(newOwner); })
        .def("GetInputAxisValue", [](AActor& self, std::string& axisName) { return self.GetInputAxisValue(FSTR(axisName)); })
        .def("ActorHasTag", [](AActor& self, std::string& tag) { return self.ActorHasTag(FSTR(tag)); })
        .def("AddTag", [](AActor& self, std::string& tag) { self.Tags.AddUnique(FName(FSTR(tag))); })
        .def("RemoveTag", [](AActor& self, std::string& tag) { self.Tags.Remove(FName(FSTR(tag))); })
        .def("EnableInput", [](AActor& self, APlayerController* pc) { self.EnableInput(pc); })
        .def("DisableInput", [](AActor& self, APlayerController* pc) { self.DisableInput(pc); })
        .def_property("Tags", [](AActor& self)
            {
                py::list ret;
                for (FName& tag : self.Tags)
                    ret.append(PYSTR(tag.ToString()));
                return ret;
            },
            [](AActor& self, py::list pytags)
            {
                self.Tags.Empty();
                for (const py::handle pytag : pytags)
                    self.Tags.Emplace(pytag.cast<std::string>().c_str());
            })
        .def("AttachToActor", [](AActor& self, AActor* parent, std::string& socket)
        {
            FName socketName = NAME_None;
            FAttachmentTransformRules rules = FAttachmentTransformRules::KeepRelativeTransform;
            if (socket.length() > 0)
            {
                socketName = FSTR(socket);
                rules = FAttachmentTransformRules::SnapToTargetNotIncludingScale;
            }
            self.AttachToActor(parent, rules, socketName);
        }, py::arg("parent"), py::arg("socket")="")
        .def("GetActorBounds", [](AActor& self, bool bOnlyCollidingComps, bool bIncludeFromChildActors) // returns FVector origin, FVector boxExtent
        {
            FVector origin, boxExtent;
            self.GetActorBounds(bOnlyCollidingComps, origin, boxExtent, bIncludeFromChildActors);
            return py::make_tuple(origin, boxExtent);
        })
        .def("GetComponentsBoundingBox", [](AActor& self, bool bNonColliding, bool bIncludeFromChildActors) { return self.GetComponentsBoundingBox(bNonColliding, bIncludeFromChildActors); })
        .def("CalculateComponentsBoundingBoxInLocalSpace", [](AActor& self, bool bNonColliding, bool bIncludeFromChildActors) { return self.CalculateComponentsBoundingBoxInLocalSpace(bNonColliding, bIncludeFromChildActors); })
        .def("GetComponentByName", [](AActor& self, std::string& _name, bool incAllDescendents, std::string& suffixSeparator) -> USceneComponent*
        {
            FString suffixSep = FSTR(suffixSeparator); // see uepy/__init__.py COMPONENT_NAME_SUFFIX_SEPARATOR
            TArray<USceneComponent*> kids;
            USceneComponent* root = self.GetRootComponent();
            if (!root)
                return nullptr;

            FString name = FSTR(_name);
            if (root->GetName() == name)
                return root;

            root->GetChildrenComponents(incAllDescendents, kids);
            for (auto kid : kids)
            {
                FString kidName = kid->GetName();
                if (suffixSep.Len())
                {   // if a suffix separator was provided, caller wants us to strip off the end of the name and just compare against the first part
                    FString left, right;
                    if (kidName.Split(suffixSep, &left, &right) && left == name)
                        return kid;
                }
                else if (kidName == name)
                    return kid;
            }
            return nullptr;
        }, py::return_value_policy::reference)
        ;

    py::class_<AController, AActor, UnrealTracker<AController>>(m, "AController")
        .def_static("StaticClass", []() { return AController::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<AController>(obj); }, py::return_value_policy::reference)
        .def("GetPawn", [](AController& self) { return self.GetPawn(); }, py::return_value_policy::reference)
        .def("Possess", [](AController& self, APawn* p) { self.Possess(p); })
        .def("SetControlRotation", [](AController& self, FRotator& r) { self.SetControlRotation(r); })
        .def("SetIgnoreMoveInput", [](AController& self, bool i) { self.SetIgnoreMoveInput(i); })
        .def("ResetIgnoreMoveInput", [](AController& self) { self.ResetIgnoreMoveInput(); })
        .def("IsMoveInputIgnored", [](AController& self) { return self.IsMoveInputIgnored(); })
        .def("SetIgnoreLookInput", [](AController& self, bool i) { self.SetIgnoreLookInput(i); })
        .def("ResetIgnoreLookInput", [](AController& self) { self.ResetIgnoreLookInput(); })
        .def("IsLookInputIgnored", [](AController& self) { return self.IsLookInputIgnored(); })
        .def("IsLocalController", [](AController& self) { return self.IsLocalController(); })
        .def("IsLocalPlayerController", [](AController& self) { return self.IsLocalPlayerController(); })
        ;

    UEPY_EXPOSE_CLASS(AAIController, AController, m)
        ;

    UEPY_EXPOSE_CLASS(UBlueprintFunctionLibrary, UObject, m);

    UEPY_EXPOSE_CLASS(UWidgetBlueprintLibrary, UBlueprintFunctionLibrary, m)
        .def_static("SetInputMode_UIOnlyEx", [](APlayerController* pc, UWidget* focusWidget, int mouseLockMode)
            { UWidgetBlueprintLibrary::SetInputMode_UIOnlyEx(pc, focusWidget, (EMouseLockMode)mouseLockMode); },
            "pc"_a, "focusWidget"_a=nullptr, "mouseLockMode"_a=(int)EMouseLockMode::DoNotLock)
        .def_static("SetInputMode_GameAndUIEx", [](APlayerController* pc, UWidget* focusWidget, int mouseLockMode, bool hideCursorDuringCapture)
            { UWidgetBlueprintLibrary::SetInputMode_GameAndUIEx(pc, focusWidget, (EMouseLockMode)mouseLockMode, hideCursorDuringCapture); },
            "pc"_a, "focusWidget"_a=nullptr, "mouseLockMode"_a=(int)EMouseLockMode::DoNotLock, "hideCursorDuringCapture"_a=true)
        .def_static("SetInputMode_GameOnly", [](APlayerController* pc) { UWidgetBlueprintLibrary::SetInputMode_GameOnly(pc); })
        .def_static("SetFocusToGameViewport", []() { UWidgetBlueprintLibrary::SetFocusToGameViewport(); })
        ;

    UEPY_EXPOSE_CLASS(UMotionTrackedDeviceFunctionLibrary, UBlueprintFunctionLibrary, m)
        .def_static("EnumerateMotionSources", []()
        {
            py::list ret;
            for (auto name : UMotionTrackedDeviceFunctionLibrary::EnumerateMotionSources())
                ret.append(PYSTR(name.ToString()));
            return ret;
        })
        ;

    py::class_<APlayerController, AController, UnrealTracker<APlayerController>>(m, "APlayerController")
        .def_static("StaticClass", []() { return APlayerController::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<APlayerController>(obj); }, py::return_value_policy::reference)
        .def_property_readonly("PlayerCameraManager", [](APlayerController& self) { return self.PlayerCameraManager; }, py::return_value_policy::reference)
        .def("ConsoleCommand", [](APlayerController& self, std::string& cmd) { FString ret = self.ConsoleCommand(FSTR(cmd)); return PYSTR(ret); })
        .def("PlayHapticEffect", [](APlayerController& self, UHapticFeedbackEffect_Base* effect, int hand, float scale, bool loop) { self.PlayHapticEffect(effect, (EControllerHand)hand, scale, loop); }, "effect"_a, "hand"_a, "scale"_a=1.0, "loop"_a=false)
        .def("StopHapticEffect", [](APlayerController& self, int hand) { self.StopHapticEffect((EControllerHand)hand); })
        BIT_PROP(bShowMouseCursor, APlayerController)
        .def_readwrite("InputYawScale", &APlayerController::InputYawScale)
        .def_readwrite("InputPitchScale", &APlayerController::InputPitchScale)
        .def_readwrite("InputRollScale", &APlayerController::InputRollScale)
        ;

    py::class_<AGameModeBase, AActor, UnrealTracker<AGameModeBase>>(m, "AGameModeBase") // technically this subclasses AInfo
        .def_static("StaticClass", []() { return AGameModeBase::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<AGameModeBase>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<AGameStateBase, AActor, UnrealTracker<AGameStateBase>>(m, "AGameStateBase")
        .def_static("StaticClass", []() { return AGameStateBase::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<AGameStateBase>(obj); }, py::return_value_policy::reference)
        ;
    py::class_<AGameState, AGameStateBase, UnrealTracker<AGameState>>(m, "AGameState")
        .def_static("StaticClass", []() { return AGameState::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<AGameState>(obj); }, py::return_value_policy::reference)
        ;
    UEPY_EXPOSE_CLASS(APlayerCameraManager, AActor, m)
        .def("StartCameraFade", [](APlayerCameraManager& self, float fromAlpha, float toAlpha, float duration, FLinearColor Color, bool bShouldFadeAudio, bool bHoldWhenFinished)
                { self.StartCameraFade(fromAlpha, toAlpha, duration, Color, bShouldFadeAudio, bHoldWhenFinished); })
        .def("GetCameraLocation", [](APlayerCameraManager& self) { return self.GetCameraLocation(); })
        .def_readwrite("FadeAmount", &APlayerCameraManager::FadeAmount)
        ;

    UEPY_EXPOSE_CLASS(USplineComponent, UPrimitiveComponent, m)
        .def("ClearSplinePoints", [](USplineComponent& self, bool bUpdateSpline) { self.ClearSplinePoints(bUpdateSpline); })
        .def("AddSplinePoint", [](USplineComponent& self, const FVector &Position, int CoordinateSpace, bool bUpdateSpline) { self.AddSplinePoint(Position, (ESplineCoordinateSpace::Type)CoordinateSpace, bUpdateSpline); })
        .def("UpdateSpline", [](USplineComponent& self) { self.UpdateSpline(); })
        .def("GetLocationAtSplinePoint", [](USplineComponent& self, int32 PointIndex, int CoordinateSpace) { return self.GetLocationAtSplinePoint(PointIndex, (ESplineCoordinateSpace::Type)CoordinateSpace); })
        .def("GetTangentAtSplinePoint", [](USplineComponent& self, int32 PointIndex, int CoordinateSpace) { return self.GetTangentAtSplinePoint(PointIndex, (ESplineCoordinateSpace::Type)CoordinateSpace); })
        .def("SetSplinePointType", [](USplineComponent& self, int32 PointIndex, int Type, bool bUpdateSpline) { return self.SetSplinePointType(PointIndex, (ESplinePointType::Type)Type, bUpdateSpline); })
        .def("GetNumberOfSplinePoints", [](USplineComponent& self) { return self.GetNumberOfSplinePoints(); })
        ;

    UEPY_EXPOSE_CLASS(USplineMeshComponent, UStaticMeshComponent, m)
        .def("SetStartAndEnd", [](USplineMeshComponent &self, FVector StartPos, FVector StartTangent, FVector EndPos, FVector EndTangent, bool bUpdateMesh)
                { self.SetStartAndEnd(StartPos, StartTangent, EndPos, EndTangent, bUpdateMesh); })
        .def("SetStartScale", [](USplineMeshComponent& self, FVector2D scale, bool update) { self.SetStartScale(scale, update); })
        .def("SetEndScale", [](USplineMeshComponent& self, FVector2D scale, bool update) { self.SetEndScale(scale, update); })
        ;

    UEPY_EXPOSE_CLASS(APlayerState, AActor, m) // technically this subclasses AInfo
        .def("GetPawn", [](APlayerState& self) { return self.GetPawn(); }, py::return_value_policy::reference)
        ;

    m.def("log", [](py::args args) -> void
    {
        TArray<FString> parts;
        for (int i=0; i < args.size(); i++)
        {
            std::string s = py::str(args[i]);
            parts.Emplace(UTF8_TO_TCHAR(s.c_str()));
        }
        FString s = FString::Join(parts, TEXT(" "));
        UE_LOG(UEPY, Log, TEXT("%s"), *s); // using UE_LOG instead of LOG because the latter includes the function, but it's always this lambda
    });

    m.def("logTB", []() -> void
    {
        static py::module traceback = py::module::import("traceback");
        std::string s = py::str(traceback.attr("format_exc")());
        UE_LOG(UEPY, Error, TEXT("%s"), UTF8_TO_TCHAR(s.c_str()));
    });

    m.def("LoadMesh", [](py::str path) -> UStaticMesh*
    {
        std::string spath = path;
        return (UStaticMesh*)StaticLoadObject(UStaticMesh::StaticClass(), NULL, UTF8_TO_TCHAR(spath.c_str()));
    }, py::return_value_policy::reference);

    m.def("LoadMaterial", [](py::str path) -> UMaterial*
    {
        std::string spath = path;
        return (UMaterial*)StaticLoadObject(UMaterial::StaticClass(), NULL, UTF8_TO_TCHAR(spath.c_str()));
    }, py::return_value_policy::reference);

    m.def("LoadTexture2D", [](py::str path) -> UTexture2D*
    {
        std::string spath = path;
        return (UTexture2D*)StaticLoadObject(UTexture2D::StaticClass(), NULL, UTF8_TO_TCHAR(spath.c_str()));
    }, py::return_value_policy::reference);

    m.def("LoadTextureFromFile", [](py::str path) -> UTexture2D*
    {
        std::string spath = path;
        return LoadTextureFromFile(UTF8_TO_TCHAR(spath.c_str()));
    }, py::return_value_policy::reference);

    m.def("TextureFromBGRA", [](const char *bgra, int width, int height) -> UTexture2D*
    {
        return TextureFromBGRA((uint8 *)bgra, width, height);
    }, py::return_value_policy::reference);

    m.def("UpdateTextureBGRA", [](UTexture2D *tex, const char *bgra, int width, int height) -> void
    {
        UpdateTextureBGRA(tex, (uint8 *)bgra, width, height);
    });

    m.def("RegisterPythonSubclass", [](py::str fqClassName, UClass *engineParentClass, py::object& pyClass, py::list& interfaceClasses) -> UClass*
    {
        std::string sname = fqClassName;
        //UClass *engineParentClass = FindObject<UClass>(ANY_PACKAGE, UTF8_TO_TCHAR(((std::string)engineParentClassPath).c_str()));
        if (!engineParentClass->ImplementsInterface(UUEPYGlueMixin::StaticClass()))
        {
            LERROR("Class does not implement IUEPYGlueMixin");
            return nullptr;
        }

        FString name(UTF8_TO_TCHAR(sname.c_str()));
        pyClassMap[name] = pyClass; // GRR: saving the class to a map because I can't get the lambda below to work with captured arguments

        UClass *engineClass = FindObject<UClass>(ANY_PACKAGE, *name);
        if (!engineClass)
            engineClass = NewObject<UClass>(engineParentClass->GetOuter(), *name, RF_Public | RF_Transient | RF_MarkAsNative);

        engineClass->ClassAddReferencedObjects = engineParentClass->ClassAddReferencedObjects;
        engineClass->SetSuperStruct(engineParentClass);
        //LOG("FULL PATH: %s", *engineClass->GetPathName());
        engineClass->PropertiesSize = engineParentClass->PropertiesSize;
        engineClass->ClassFlags |= CLASS_Native;
        engineClass->ClassFlags |= (engineParentClass->ClassFlags & (CLASS_Inherit | CLASS_ScriptInherit));
        engineClass->Children = engineParentClass->Children;
        engineClass->PropertyLink = engineParentClass->PropertyLink;
        engineClass->ClassWithin = engineParentClass->ClassWithin;
        engineClass->ClassConfigName = engineParentClass->ClassConfigName;
        engineClass->ClassCastFlags = engineParentClass->ClassCastFlags;
        engineClass->ClassConstructor = [](const FObjectInitializer& objInitializer)
        {
            UObject *engineObj = objInitializer.GetObj();
            UClass *parentClass = engineObj->GetClass()->GetSuperClass();
            if (parentClass && parentClass->ClassConstructor)
                parentClass->ClassConstructor(objInitializer);
            if (!engineObj->HasAnyFlags(RF_ClassDefaultObject)) // during CDO creation, we don't want to create a pyinst
            {
                try {
                    IUEPYGlueMixin *p = Cast<IUEPYGlueMixin>(engineObj);
                    FString className = engineObj->GetClass()->GetName();
                    py::object& pyClass = pyClassMap[className];
                    pyClass(engineObj, **spawnArgs); // the metaclass in uepy.__init__ requires engineObj to be passed as the first param; it gobbles it up and auto-sets self.engineObj on the new instance
                    ClearInternalSpawnArgs();
                }
                catchpy;
            }
        };

        // add in any interfaces that this class implements, both from the parent class as well as the python subclass itself
        // Note that (for now at least) there isn't really any way for the Python class to /actually/ implement a BP interface,
        // only a c++ interface. It can declare that it implements it, but we don't yet support exposing UFunctions (and ideally
        // we never will, but for modus there are a bunch of cases where we filter objects based on which interfaces the objects
        // say they implement)
        for (FImplementedInterface& info : engineParentClass->Interfaces)
            engineClass->Interfaces.Add(info);
        for (auto h : interfaceClasses)
        {
            py::object _interfaceClass = h.cast<py::object>();
            UClass *interfaceClass = PyObjectToUClass(_interfaceClass);
            if (!interfaceClass || !interfaceClass->HasAnyClassFlags(CLASS_Interface))
            {
                LERROR("Class %s created with invalid interface class %s", *name, REPR(_interfaceClass));
                continue;
            }

            FImplementedInterface info;
            info.Class = interfaceClass;
            info.PointerOffset = 0;
            info.bImplementedByK2 = false;
            engineClass->Interfaces.Emplace(info);
        }

        engineClass->ClearFunctionMapsCaches();
        engineClass->Bind();
        engineClass->StaticLink(true);
        engineClass->AssembleReferenceTokenStream();
        engineClass->GetDefaultObject();
        return engineClass;
    }, py::return_value_policy::reference);

    // used during class construction to set pyInst
    m.def("InternalSetPyInst", [](UObject* self, py::object& inst)
    {
        IUEPYGlueMixin *p = Cast<IUEPYGlueMixin>(self);
        p->pyInst = inst;
    });

    m.def("StaticLoadObject", [](py::object& _typeClass, std::string refPath) {
        UClass *typeClass = PyObjectToUClass(_typeClass);
        return StaticLoadObject(typeClass, NULL, UTF8_TO_TCHAR(refPath.c_str()));
        //return Cast<UClass>(obj);
    }, py::return_value_policy::reference);

    m.def("SpawnActor_", [](UWorld *world, py::object& _actorClass, FVector& location, FRotator& rotation, py::dict kwargs)
    {
        UClass *actorClass = PyObjectToUClass(_actorClass);
        if (!actorClass)
            return (AActor*)nullptr;

        if (py::len(kwargs) == 0)
        {   // one-shot spawn because no extra params were passed
            FActorSpawnParameters info;
            info.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            return world->SpawnActor(actorClass, &location, &rotation, info);
        }
        else
        {   // caller also wants to pass in some params so we need to do a multi-step spawn. If the class is actually a Python subclass of an engine class,
            // we pass the kwargs directly to the constructor via a bit of hackery. This is both more efficient but, more importantly, it allows us to do
            // some earlier initialization that is pretty much impossible any other way (assuming you want replication to work right).
            bool directInit = false;
            if (pyClassMap.count(actorClass->GetName()))
            {
                directInit = true;
                SetInternalSpawnArgs(kwargs);
            }

            FTransform transform(rotation, location);
            AActor *actor = world->SpawnActorDeferred<AActor>(actorClass, transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
            if (!actor)
            {
                LERROR("Failed to spawn actor");
                ClearInternalSpawnArgs();
                return (AActor*)nullptr;
            }

            if (!directInit)
            {   // fall back to the engine way of two-step init
                for (auto item : kwargs)
                {
                    std::string k = item.first.cast<std::string>();
                    py::object v = py::cast<py::object>(item.second);
                    SetObjectProperty(actor, k, v);
                }
            }

            UGameplayStatics::FinishSpawningActor(actor, transform);
            return actor;
        }
    }, py::arg("world"), py::arg("actorClass"), py::arg("location")=FVector(0,0,0), py::arg("rotation")=FRotator(0,0,0), py::arg("kwargs"), py::return_value_policy::reference);

    m.def("NewObject_", [](py::object& _class, UObject *owner, std::string& name, py::dict kwargs) // underscore suffix because there is a Python NewObject function that calls it
    {
        UClass *klass = PyObjectToUClass(_class);
        if (!owner)
            owner = GetTransientPackage();
        FName instName = NAME_None;
        if (name.length())
            instName = FSTR(name);
        SetInternalSpawnArgs(kwargs);

        UObject *obj = NewObject<UObject>(owner, klass, instName);
        if (obj)
            obj->PostLoad(); // ? is this right ?
        return obj;
    }, py::return_value_policy::reference);

    py::class_<AActor_CGLUE, AActor, UnrealTracker<AActor_CGLUE>>(glueclasses, "AActor_CGLUE")
        .def_static("StaticClass", []() { return AActor_CGLUE::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<AActor_CGLUE>(obj); }, py::return_value_policy::reference)
        .def("SuperBeginPlay", [](AActor_CGLUE& self) { self.SuperBeginPlay(); })
        .def("SuperEndPlay", [](AActor_CGLUE& self, int reason) { self.SuperEndPlay((EEndPlayReason::Type)reason); })
        .def("SuperPostInitializeComponents", [](AActor_CGLUE& self) { self.SuperPostInitializeComponents(); })
        .def("SuperTick", [](AActor_CGLUE& self, float dt) { self.SuperTick(dt); })
        .def("OverrideTickAllowed", [](AActor_CGLUE& self, bool allowed) { self.tickAllowed = allowed; })
        .def("UpdateTickSettings", [](AActor_CGLUE& self, bool canEverTick, bool startWithTickEnabled) { self.PrimaryActorTick.bCanEverTick = canEverTick; self.PrimaryActorTick.bStartWithTickEnabled = startWithTickEnabled; })
        ;

    py::class_<APawn, AActor, UnrealTracker<APawn>>(m, "APawn")
        .def_static("StaticClass", []() { return APawn::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<APawn>(w); }, py::return_value_policy::reference)
        .def_readwrite("BaseEyeHeight", &APawn::BaseEyeHeight)
        .def_readonly("Controller", &APawn::Controller, py::return_value_policy::reference)
        ENUM_PROP(AutoPossessPlayer, EAutoReceiveInput::Type, APawn)
        ENUM_PROP(AutoPossessAI, EAutoPossessAI, APawn)
        BIT_PROP(bUseControllerRotationPitch, APawn)
        BIT_PROP(bUseControllerRotationYaw, APawn)
        BIT_PROP(bUseControllerRotationRoll, APawn)
        .def("AddMovementInput", [](APawn& self, FVector& worldDir, float scale, bool force) { self.AddMovementInput(worldDir, scale, force); })
        .def("AddControllerPitchInput", [](APawn& self, float v) { self.AddControllerPitchInput(v); })
        .def("AddControllerYawInput", [](APawn& self, float v) { self.AddControllerYawInput(v); })
        .def("AddControllerRollInput", [](APawn& self, float v) { self.AddControllerRollInput(v); })
        .def("IsLocallyControlled", [](APawn& self) { return self.IsLocallyControlled(); })
        .def("GetPlayerState", [](APawn& self) { return self.GetPlayerState(); }, py::return_value_policy::reference)
        .def("GetController", [](APawn& self) { return self.GetController(); }, py::return_value_policy::reference)
        .def("GetUserID", [](APawn& self)
        {
            const AActor* owner = self.GetNetOwner();
            if (owner)
            {
                const APlayerController* pc = Cast<APlayerController>(owner);
                if (pc && pc->NetConnection)
                {
                    for (UChannel* chan : pc->NetConnection->OpenChannels)
                    {
                        UNRChannel *repChan = Cast<UNRChannel>(chan);
                        if (VALID(repChan))
                            return repChan->channelID;
                    }
                }
            }
            return 0;
        })
        .def_property("AIControllerClass", [](APawn& self) { return self.AIControllerClass; }, [](APawn& self, py::object& _klass) { self.AIControllerClass = PyObjectToUClass(_klass); }, py::return_value_policy::reference)
        .def("SpawnDefaultController", [](APawn& self) { self.SpawnDefaultController(); })
        ;

    UEPY_EXPOSE_CLASS(UMovementComponent, UActorComponent, m)
        .def_readwrite("Velocity", &UMovementComponent::Velocity)
        ;

    UEPY_EXPOSE_CLASS(UNavMovementComponent, UMovementComponent, m);
    UEPY_EXPOSE_CLASS(UPawnMovementComponent, UNavMovementComponent, m);

    UEPY_EXPOSE_CLASS(UCharacterMovementComponent, UPawnMovementComponent, m)
        ENUM_PROP(MovementMode, EMovementMode, UCharacterMovementComponent)
        ENUM_PROP(DefaultLandMovementMode, EMovementMode, UCharacterMovementComponent)
        ENUM_PROP(DefaultWaterMovementMode, EMovementMode, UCharacterMovementComponent)
        .def_readwrite("MaxFlySpeed", &UCharacterMovementComponent::MaxFlySpeed)
        .def_readwrite("MaxAcceleration", &UCharacterMovementComponent::MaxFlySpeed)
        .def_readwrite("BrakingDecelerationFlying", &UCharacterMovementComponent::BrakingDecelerationFlying)
        .def("SetMovementMode", [](UCharacterMovementComponent& self, int m) { self.SetMovementMode((EMovementMode)m); })
        .def("AddImpulse", [](UCharacterMovementComponent& self, FVector& i, bool bVelocityChange) { self.AddImpulse(i, bVelocityChange); }, "i"_a, "bVelocityChange"_a=false)
        .def("AddForce", [](UCharacterMovementComponent& self, FVector& f) { self.AddForce(f); })
        ;

    py::class_<APawn_CGLUE, APawn, UnrealTracker<APawn_CGLUE>>(glueclasses, "APawn_CGLUE")
        .def_static("StaticClass", []() { return APawn_CGLUE::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<APawn_CGLUE>(obj); }, py::return_value_policy::reference)
        .def("SuperBeginPlay", [](APawn_CGLUE& self) { self.SuperBeginPlay(); })
        .def("SuperEndPlay", [](APawn_CGLUE& self, int reason) { self.SuperEndPlay((EEndPlayReason::Type)reason); })
        .def("SuperPostInitializeComponents", [](APawn_CGLUE& self) { self.SuperPostInitializeComponents(); })
        .def("SuperTick", [](APawn_CGLUE& self, float dt) { self.SuperTick(dt); })
        .def("OverrideTickAllowed", [](APawn_CGLUE& self, bool allowed) { self.tickAllowed = allowed; })
        .def("SuperSetupPlayerInputComponent", [](APawn_CGLUE& self, UInputComponent* comp) { self.SuperSetupPlayerInputComponent(comp); })
        ;

    UEPY_EXPOSE_CLASS(ACharacter, APawn, m)
        .def("OverrideTickAllowed", [](ACharacter_CGLUE& self, bool allowed) { self.tickAllowed = allowed; })
        .def("GetCharacterMovement", [](ACharacter_CGLUE& self) { return self.GetCharacterMovement(); }, py::return_value_policy::reference)
        .def("GetCapsuleComponent", [](ACharacter_CGLUE& self) { return self.GetCapsuleComponent(); }, py::return_value_policy::reference)
        .def("SetReplicateMovement", [](ACharacter_CGLUE& self, bool b) { self.SetReplicateMovement(b); })
        ;

    UEPY_EXPOSE_CLASS(ACharacter_CGLUE, ACharacter, glueclasses)
        .def("SuperBeginPlay", [](ACharacter_CGLUE& self) { self.SuperBeginPlay(); })
        .def("SuperEndPlay", [](ACharacter_CGLUE& self, int reason) { self.SuperEndPlay((EEndPlayReason::Type)reason); })
        .def("SuperPostInitializeComponents", [](ACharacter_CGLUE& self) { self.SuperPostInitializeComponents(); })
        .def("SuperTick", [](ACharacter_CGLUE& self, float dt) { self.SuperTick(dt); })
        .def("OverrideTickAllowed", [](ACharacter_CGLUE& self, bool allowed) { self.tickAllowed = allowed; })
        .def("SuperSetupPlayerInputComponent", [](ACharacter_CGLUE& self, UInputComponent* comp) { self.SuperSetupPlayerInputComponent(comp); })
        ;

    UEPY_EXPOSE_CLASS(USceneComponent_CGLUE, USceneComponent, glueclasses)
        .def("SuperBeginPlay", [](USceneComponent_CGLUE& self) { self.SuperBeginPlay(); })
        .def("SuperEndPlay", [](USceneComponent_CGLUE& self, int reason) { self.SuperEndPlay((EEndPlayReason::Type)reason); })
        .def("SuperOnRegister", [](USceneComponent_CGLUE& self) { self.SuperOnRegister(); })
        .def("OverrideTickAllowed", [](USceneComponent_CGLUE& self, bool allowed) { self.tickAllowed = allowed; })
        ;

    py::class_<USoundClass, UObject, UnrealTracker<USoundClass>>(m, "USoundClass")
        .def_static("StaticClass", []() { return USoundClass::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<USoundClass>(obj); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(USoundMix, UObject, m)
        ;

    py::class_<UMediaPlayer, UObject, UnrealTracker<UMediaPlayer>>(m, "UMediaPlayer")
        .def_static("StaticClass", []() { return UMediaPlayer::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UMediaPlayer>(obj); }, py::return_value_policy::reference)
        .def("OpenSource", [](UMediaPlayer& self, UMediaSource* source) { return self.OpenSource(source); })
        .def("SetRate", [](UMediaPlayer& self, float rate) { return self.SetRate(rate); })
        .def("GetDuration", [](UMediaPlayer& self) { return self.GetDuration().GetTotalSeconds(); })
        .def("GetTime", [](UMediaPlayer& self) { return self.GetTime().GetTotalSeconds(); })
        .def("OpenFile", [](UMediaPlayer& self, std::string& path) { return self.OpenFile(FSTR(path)); })
        .def("IsPlaying", [](UMediaPlayer& self) { return self.IsPlaying(); })
        .def("Close", [](UMediaPlayer& self) { self.Close(); })
        .def("GetVideoTrackDimensions", [](UMediaPlayer& self, int track, int fmt) { FIntPoint p = self.GetVideoTrackDimensions(track, fmt); return py::make_tuple(p.X, p.Y); })
        .def("GetVideoTrackAspectRatio", [](UMediaPlayer& self, int track, int fmt) { return self.GetVideoTrackAspectRatio(track, fmt); })
        .def("Play", [](UMediaPlayer& self) { return self.Play(); })
        .def("Seek", [](UMediaPlayer& self, float pos) { self.Seek(FTimespan::FromSeconds(pos)); })
        .def("Pause", [](UMediaPlayer& self) { return self.Pause(); })
        .def("IsPaused", [](UMediaPlayer& self) { return self.IsPaused(); })
        .def("SetLooping", [](UMediaPlayer& self, bool loop) { return self.SetLooping(loop); })
        .def("IsLooping", [](UMediaPlayer& self) { return self.IsLooping(); })
        ;

    py::class_<UMediaSource, UObject, UnrealTracker<UMediaSource>>(m, "UMediaSource")
        .def_static("StaticClass", []() { return UMediaSource::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UMediaSource>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UFileMediaSource, UMediaSource, UnrealTracker<UFileMediaSource>>(m, "UFileMediaSource") // TODO: actually it's UFileMediaSource<--UBaseMediaSource<--UMediaSource
        .def_static("StaticClass", []() { return UFileMediaSource::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UFileMediaSource>(obj); }, py::return_value_policy::reference)
        .def("SetFilePath", [](UFileMediaSource& self, py::str path) { std::string p = path; self.SetFilePath(p.c_str()); })
        ;

    py::class_<UAudioComponent, USceneComponent, UnrealTracker<UAudioComponent>>(m, "UAudioComponent")
        .def_static("StaticClass", []() { return UAudioComponent::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UAudioComponent>(obj); }, py::return_value_policy::reference)
        BIT_PROP(bAllowSpatialization, UAudioComponent)
        .def("GetAttenuationOverrides", [](UAudioComponent& self) { return FHackyAttenuationSettings(self.AttenuationOverrides); }, py::return_value_policy::reference)
        .def("AdjustAttenuation", [](UAudioComponent& self, FHackyAttenuationSettings& hs) { FSoundAttenuationSettings s = self.AttenuationOverrides; hs.ApplyTo(s); self.AdjustAttenuation(s); })
        .def_readwrite("VolumeMultiplier", &UAudioComponent::VolumeMultiplier)
        .def("SetSound", [](UAudioComponent& self, USoundBase* s) { self.SetSound(s); })
        BIT_PROP(bOverrideAttenuation, UAudioComponent)
        .def("SetFloatParameter", [](UAudioComponent& self, std::string name, float v) { self.SetFloatParameter(FSTR(name), v); })
        .def("SetBoolParameter", [](UAudioComponent& self, std::string name, bool v) { self.SetBoolParameter(FSTR(name), v); })
        .def("SetIntParameter", [](UAudioComponent& self, std::string name, int v) { self.SetIntParameter(FSTR(name), v); })
        .def("Play", [](UAudioComponent& self) { self.Play(); })
        .def("Stop", [](UAudioComponent& self) { self.Stop(); })
        .def("SetPaused", [](UAudioComponent& self, bool p) { self.SetPaused(p); })
        .def("IsPlaying", [](UAudioComponent& self) { return self.IsPlaying(); })
        ;

    UEPY_EXPOSE_CLASS(USoundBase, UObject, m)
        .def("GetDuration", [](USoundBase& self) { return self.GetDuration(); })
        ;

    UEPY_EXPOSE_CLASS(USoundCue, USoundBase, m)
        ;

    UEPY_EXPOSE_CLASS(USoundWave, USoundBase, m)
        BIT_PROP(bLooping, USoundWave)
        ;

    py::class_<FHackyAttenuationSettings>(m, "FHackyAttenuationSettings")
        .def_readwrite("enablePriorityAttenuation", &FHackyAttenuationSettings::enablePriorityAttenuation)
        .def_readwrite("falloffDistance", &FHackyAttenuationSettings::falloffDistance)
        .def_readwrite("falloffMode", &FHackyAttenuationSettings::falloffMode)
        .def_readwrite("attenuationShapeExtents", &FHackyAttenuationSettings::attenuationShapeExtents)
        .def_readwrite("enableSubmixSends", &FHackyAttenuationSettings::enableSubmixSends)
        .def_readwrite("binauralRadius", &FHackyAttenuationSettings::binauralRadius)
        .def_readwrite("manualPriorityAttenuation", &FHackyAttenuationSettings::manualPriorityAttenuation)
        .def_readwrite("priorityAttenuationMax", &FHackyAttenuationSettings::priorityAttenuationMin)
        .def_readwrite("priorityAttenuationMin", &FHackyAttenuationSettings::priorityAttenuationMax)
        .def_readwrite("attenuationShape", &FHackyAttenuationSettings::attenuationShape)
        .def_readwrite("priorityAttenuationMethod", &FHackyAttenuationSettings::priorityAttenuationMethod)
        .def_readwrite("attenuate", &FHackyAttenuationSettings::attenuate)
        .def_readwrite("spatialize", &FHackyAttenuationSettings::spatialize)
        .def_readwrite("attenuateWithLPF", &FHackyAttenuationSettings::attenuateWithLPF)
        .def_readwrite("coneOffset", &FHackyAttenuationSettings::coneOffset)
        .def_readwrite("distanceAlgorithm", &FHackyAttenuationSettings::distanceAlgorithm)
        ;
    /* See notes in FHackyAttenuationSettings; instead use FHackyAttenuationSettings (above)
    py::class_<FBaseAttenuationSettings>(m, "FBaseAttenuationSettings");
    py::class_<FSoundAttenuationSettings, FBaseAttenuationSettings>(m, "FSoundAttenuationSettings");
    */

    py::class_<USynthComponent, USceneComponent, UnrealTracker<USynthComponent>>(m, "USynthComponent")
        .def_static("StaticClass", []() { return USynthComponent::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<USynthComponent>(obj); }, py::return_value_policy::reference)
        .def_readwrite("SoundClass", &USynthComponent::SoundClass, py::return_value_policy::reference)
        .def_property("bEnableBaseSubmix", [](USynthComponent& self) { return self.bEnableBaseSubmix; }, [](USynthComponent& self, bool b) { self.bEnableBaseSubmix = b; })
        .def_property("bIsUISound", [](USynthComponent& self) { return self.bIsUISound; }, [](USynthComponent& self, bool b) { self.bIsUISound = b; }) // for some reason I couldn't bind this prop directly. Maybe because it's a uint8?
        .def_property("bAllowSpatialization", [](USynthComponent& self) { return self.bAllowSpatialization; }, [](USynthComponent& self, bool b) { self.bAllowSpatialization = b; }) // ditto
        .def_property("bOverrideAttenuation", [](USynthComponent& self) { return self.bOverrideAttenuation; }, [](USynthComponent& self, bool b) { self.bOverrideAttenuation = b; }) // ditto
        // Note: I always get a crash when trying to return this struct, so until we can figure it out I'm just adding accessors for each FSoundAttenuationSettings property we care to access
        //.def_readwrite("AttenuationOverrides", &USynthComponent::AttenuationOverrides)
        // TODO: don't add any new items here, but instead refactor this to instead use FHackyAttenuationSettings
        .def_property("StereoSpread", [](USynthComponent& self) { return self.AttenuationOverrides.StereoSpread; }, [](USynthComponent&self, float v) { self.AttenuationOverrides.StereoSpread = v; })
        .def_property("LPFRadiusMin", [](USynthComponent& self) { return self.AttenuationOverrides.LPFRadiusMin; }, [](USynthComponent&self, float v) { self.AttenuationOverrides.LPFRadiusMin = v; })
        .def_property("LPFRadiusMax", [](USynthComponent& self) { return self.AttenuationOverrides.LPFRadiusMax; }, [](USynthComponent&self, float v) { self.AttenuationOverrides.LPFRadiusMax = v; })
        .def_property("bApplyNormalizationToStereoSounds", [](USynthComponent& self) { return self.AttenuationOverrides.bApplyNormalizationToStereoSounds; }, [](USynthComponent&self, bool v) { self.AttenuationOverrides.bApplyNormalizationToStereoSounds = v; })
        .def_property("bAttenuateWithLPF", [](USynthComponent& self) { return self.AttenuationOverrides.bAttenuateWithLPF; }, [](USynthComponent&self, bool v) { self.AttenuationOverrides.bAttenuateWithLPF = v; })
        .def_property("bEnableLogFrequencyScaling", [](USynthComponent& self) { return self.AttenuationOverrides.bEnableLogFrequencyScaling; }, [](USynthComponent&self, bool v) { self.AttenuationOverrides.bEnableLogFrequencyScaling = v; })
        .def_property("bEnableListenerFocus", [](USynthComponent& self) { return self.AttenuationOverrides.bEnableListenerFocus; }, [](USynthComponent&self, bool v) { self.AttenuationOverrides.bEnableListenerFocus = v; })
        .def_property("bEnableOcclusion", [](USynthComponent& self) { return self.AttenuationOverrides.bEnableOcclusion; }, [](USynthComponent&self, bool v) { self.AttenuationOverrides.bEnableOcclusion = v; })
        .def("IsPlaying", [](USynthComponent& self) { return self.IsPlaying(); })
        .def("Start", [](USynthComponent& self) { self.Start(); })
        .def("Stop", [](USynthComponent& self) { self.Stop(); })
        .def("SetVolumeMultiplier", [](USynthComponent& self, float m) { self.SetVolumeMultiplier(m); })
        .def("SetStarted", [](USynthComponent& self, bool go)
        {
            if (go)
            {
                if (!self.IsPlaying())
                    self.Start();
            }
            else
            {
                if (self.IsPlaying())
                    self.Stop();
            }
        });
        ;

    py::class_<UMediaSoundComponent, USynthComponent, UnrealTracker<UMediaSoundComponent>>(m, "UMediaSoundComponent")
        .def_static("StaticClass", []() { return UMediaSoundComponent::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UMediaSoundComponent>(obj); }, py::return_value_policy::reference)
        .def("SetMediaPlayer", [](UMediaSoundComponent& self, UMediaPlayer* player) { self.SetMediaPlayer(player); })
        .def("GetAudioComponent", [](UMediaSoundComponent& self) { return self.GetAudioComponent(); }, py::return_value_policy::reference)
        ;

    py::class_<ULightComponentBase, USceneComponent, UnrealTracker<ULightComponentBase>>(m, "ULightComponentBase")
        .def_static("StaticClass", []() { return ULightComponentBase::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<ULightComponentBase>(obj); }, py::return_value_policy::reference)
        .def_readwrite("Intensity", &ULightComponentBase::Intensity)
        .def("SetCastStaticShadows", [](ULightComponentBase& self, bool b) { self.CastStaticShadows = b; })
        .def("SetCastDynamicShadows", [](ULightComponentBase& self, bool b) { self.CastDynamicShadows = b; })
        .def("SetTransmission", [](ULightComponentBase& self, bool b) { self.bTransmission = b; })
        .def_readwrite("IndirectLightingIntensity", &ULightComponentBase::IndirectLightingIntensity)
        .def_readwrite("LightColor", &ULightComponentBase::LightColor)
        .def_property("bAffectsWorld", [](ULightComponentBase& self) { return (bool)self.bAffectsWorld; }, [](ULightComponentBase& self, bool b) { self.bAffectsWorld=(uint32)b; })
        .def("SetCastShadows", [](ULightComponentBase& self, bool b) { self.SetCastShadows(b); })
        .def("SetCastVolumetricShadow", [](ULightComponentBase& self, bool b) { self.SetCastVolumetricShadow(b); })
        .def("SetAffectReflection", [](ULightComponentBase& self, bool b) { self.SetAffectReflection(b); })
        .def("SetAffectGlobalIllumination", [](ULightComponentBase& self, bool b) { self.SetAffectGlobalIllumination(b); })
        .def("SetCastRaytracedShadow", [](ULightComponentBase& self, bool b) { self.SetCastRaytracedShadow(b); })
        .def("SetSamplesPerPixel", [](ULightComponentBase& self, int i) { self.SetSamplesPerPixel(i); })
        ;

    py::class_<ULightComponent, ULightComponentBase, UnrealTracker<ULightComponent>>(m, "ULightComponent")
        .def_static("StaticClass", []() { return ULightComponent::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<ULightComponent>(obj); }, py::return_value_policy::reference)
        .def_property("bUseTemperature", [](ULightComponent& self) { return (bool)self.bUseTemperature; }, [](ULightComponent& self, bool b) { self.bUseTemperature=(int32)b; })
        .def("GetBoundingBox", [](ULightComponent& self) { return self.GetBoundingBox(); })
        .def("GetBoundingSphere", [](ULightComponent& self) { return self.GetBoundingSphere(); })
        .def("GetDirection", [](ULightComponent& self) { return self.GetDirection(); })
        .def("GetMaterial", [](ULightComponent& self, int index) { return self.GetMaterial(index); }, py::return_value_policy::reference)
        .def("GetNumMaterials", [](ULightComponent& self) { return self.GetNumMaterials(); })
        .def("SetAffectDynamicIndirectLighting", [](ULightComponent& self, bool b) { self.SetAffectDynamicIndirectLighting(b); })
        .def("SetAffectTranslucentLighting", [](ULightComponent& self, bool b) { self.SetAffectTranslucentLighting(b); })
        .def("SetBloomScale", [](ULightComponent& self, float f) { self.SetBloomScale(f); })
        .def("SetBloomThreshold", [](ULightComponent& self, float f) { self.SetBloomThreshold(f); })
        .def("SetBloomTint", [](ULightComponent& self, FColor& c) { self.SetBloomTint(c); })
        .def("SetEnableLightShaftBloom", [](ULightComponent& self, bool b) { self.SetEnableLightShaftBloom(b); })
        .def("SetForceCachedShadowsForMovablePrimitives", [](ULightComponent& self, bool b) { self.SetForceCachedShadowsForMovablePrimitives(b); })
        .def("SetIndirectLightingIntensity", [](ULightComponent& self, float f) { self.SetIndirectLightingIntensity(f); })
        .def("SetIntensity", [](ULightComponent& self, float f) { self.SetIntensity(f); })
        .def("SetLightColor", [](ULightComponent& self, FLinearColor& c, bool bSRGB) { self.SetLightColor(c, bSRGB); }, py::arg("c"), py::arg("bSRGB")=true)
        .def("SetLightFunctionDisabledBrightness", [](ULightComponent& self, float f) { self.SetLightFunctionDisabledBrightness(f); })
        .def("SetLightFunctionFadeDistance", [](ULightComponent& self, float f) { self.SetLightFunctionFadeDistance(f); })
        .def("SetLightFunctionMaterial", [](ULightComponent& self, UMaterialInterface* m) { self.SetLightFunctionMaterial(m); })
        .def("SetLightFunctionScale", [](ULightComponent& self, FVector& v) { self.SetLightFunctionScale(v); })
        .def("SetMaterial", [](ULightComponent& self, int index, UMaterialInterface* m) { self.SetMaterial(index, m); })
        .def("SetShadowBias", [](ULightComponent& self, float f) { self.SetShadowBias(f); })
        .def("SetShadowSlopeBias", [](ULightComponent& self, float f) { self.SetShadowSlopeBias(f); })
        .def("SetSpecularScale", [](ULightComponent& self, float f) { self.SetSpecularScale(f); })
        .def("SetTemperature", [](ULightComponent& self, float f) { self.SetTemperature(f); })
        .def("SetTransmission", [](ULightComponent& self, bool b) { self.SetTransmission(b); })
        .def("SetVolumetricScatteringIntensity", [](ULightComponent& self, float f) { self.SetVolumetricScatteringIntensity(f); })
        ;

    py::class_<ULocalLightComponent, ULightComponent, UnrealTracker<ULocalLightComponent>>(m, "ULocalLightComponent")
        .def_static("StaticClass", []() { return ULocalLightComponent::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<ULocalLightComponent>(obj); }, py::return_value_policy::reference)
        .def("SetAttenuationRadius", [](ULocalLightComponent& self, float r) { self.SetAttenuationRadius(r); })
        .def_readonly("AttenuationRadius", &ULocalLightComponent::AttenuationRadius)
        .def("SetIntensityUnits", [](ULocalLightComponent& self, int u) { self.SetIntensityUnits((ELightUnits)u); })
        ;

    py::class_<UPointLightComponent, ULocalLightComponent, UnrealTracker<UPointLightComponent>>(m, "UPointLightComponent")
        .def_static("StaticClass", []() { return UPointLightComponent::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UPointLightComponent>(obj); }, py::return_value_policy::reference)
        .def("SetLightFalloffExponent", [](UPointLightComponent& self, float f) { self.SetLightFalloffExponent(f); })
        .def("SetSourceRadius", [](UPointLightComponent& self, float f) { self.SetSourceRadius(f); })
        .def("SetSoftSourceRadius", [](UPointLightComponent& self, float f) { self.SetSoftSourceRadius(f); })
        .def("SetSourceLength", [](UPointLightComponent& self, float f) { self.SetSourceLength(f); })
        .def_property("bUseInverseSquaredFalloff", [](UPointLightComponent& self) { return (bool)self.bUseInverseSquaredFalloff; }, [](UPointLightComponent& self, bool b) { self.bUseInverseSquaredFalloff=(uint32)b; })
        ;

    py::class_<USpotLightComponent, UPointLightComponent, UnrealTracker<USpotLightComponent>>(m, "USpotLightComponent")
        .def_static("StaticClass", []() { return USpotLightComponent::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<USpotLightComponent>(obj); }, py::return_value_policy::reference)
        .def("SetInnerConeAngle", [](USpotLightComponent& self, float f) { self.SetInnerConeAngle(f); })
        .def("SetOuterConeAngle", [](USpotLightComponent& self, float f) { self.SetOuterConeAngle(f); })
        .def_readonly("InnerConeAngle", &USpotLightComponent::InnerConeAngle)
        .def_readonly("OuterConeAngle", &USpotLightComponent::OuterConeAngle)
        ;

    UEPY_EXPOSE_CLASS(USkyLightComponent, ULightComponentBase, m)
        .def("SetCubemap", [](USkyLightComponent& self, UTextureCube *c) { self.SetCubemap(c); })
        .def("SetLightColor", [](USkyLightComponent& self, FLinearColor& c) { self.SetLightColor(c); })
        .def("SetIntensity", [](USkyLightComponent& self, float f) { self.SetIntensity(f); })
        ENUM_PROP(SourceType, ESkyLightSourceType, USkyLightComponent)
        .def_readwrite("SkyDistanceThreshold", &USkyLightComponent::SkyDistanceThreshold)
        .def("SetIndirectLightingIntensity", [](USkyLightComponent& self, float i) { self.SetIndirectLightingIntensity(i); })
        .def("SetVolumetricScatteringIntensity", [](USkyLightComponent& self, float i) { self.SetVolumetricScatteringIntensity(i); })
        .def_readwrite("bLowerHemisphereIsBlack", &USkyLightComponent::bLowerHemisphereIsBlack)
        ;

    py::class_<USceneCaptureComponent, USceneComponent, UnrealTracker<USceneCaptureComponent>>(m, "USceneCaptureComponent")
        .def_static("StaticClass", []() { return USceneCaptureComponent::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<USceneCaptureComponent>(obj); }, py::return_value_policy::reference)
        .def_property("bCaptureEveryFrame", [](USceneCaptureComponent& self) { return self.bCaptureEveryFrame; }, [](USceneCaptureComponent& self, bool b) { self.bCaptureEveryFrame = b; })
        .def_readwrite("bAlwaysPersistRenderingState", &USceneCaptureComponent::bAlwaysPersistRenderingState)
        ENUM_PROP(CaptureSource, ESceneCaptureSource, USceneCaptureComponent)
        ENUM_PROP(PrimitiveRenderMode, ESceneCapturePrimitiveRenderMode, USceneCaptureComponent)
        LIST_PROP(HiddenActors, AActor*, USceneCaptureComponent2D)
        BIT_PROP(bCaptureOnMovement, USceneCaptureComponent)
        .def("HideComponent", [](USceneCaptureComponent& self, UPrimitiveComponent* comp) { self.HideComponent(comp); })
        ;

    py::class_<USceneCaptureComponent2D, USceneCaptureComponent, UnrealTracker<USceneCaptureComponent2D>>(m, "USceneCaptureComponent2D")
        .def_static("StaticClass", []() { return USceneCaptureComponent2D::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<USceneCaptureComponent2D>(obj); }, py::return_value_policy::reference)
        .def_readwrite("FOVAngle", &USceneCaptureComponent2D::FOVAngle)
        .def_readwrite("TextureTarget", &USceneCaptureComponent2D::TextureTarget, py::return_value_policy::reference)
        .def("CaptureScene", [](USceneCaptureComponent2D& self) { self.CaptureScene(); })
        BIT_PROP(bOverride_CustomNearClippingPlane, USceneCaptureComponent2D)
        .def_readwrite("CustomNearClippingPlane", &USceneCaptureComponent2D::CustomNearClippingPlane)
        ;

    UEPY_EXPOSE_CLASS(USceneCaptureComponentCube, USceneCaptureComponent, m)
        .def_readwrite("TextureTarget", &USceneCaptureComponentCube::TextureTarget, py::return_value_policy::reference)
        .def_readwrite("bCaptureRotation", &USceneCaptureComponentCube::bCaptureRotation)
        .def("CaptureScene", [](USceneCaptureComponentCube& self) { self.CaptureScene(); })
        ;

    py::class_<FPostProcessSettings>(m ,"FPostProcessSettings")
        .def(py::init<>())
        BIT_PROP(bOverride_AutoExposureBias, FPostProcessSettings)
        .def_readwrite("AutoExposureBias", &FPostProcessSettings::AutoExposureBias) // 'Exposure Compensation'
        BIT_PROP(bOverride_ScreenPercentage, FPostProcessSettings)
        .def_readwrite("ScreenPercentage", &FPostProcessSettings::ScreenPercentage)
        BIT_PROP(bOverride_DepthOfFieldNearBlurSize, FPostProcessSettings)
        .def_readwrite("DepthOfFieldNearBlurSize", &FPostProcessSettings::DepthOfFieldNearBlurSize)
        BIT_PROP(bOverride_DepthOfFieldFarBlurSize, FPostProcessSettings)
        .def_readwrite("DepthOfFieldFarBlurSize", &FPostProcessSettings::DepthOfFieldFarBlurSize)
        BIT_PROP(bOverride_AutoExposureMinBrightness, FPostProcessSettings)
        .def_readwrite("AutoExposureMinBrightness", &FPostProcessSettings::AutoExposureMinBrightness)
        BIT_PROP(bOverride_AutoExposureMaxBrightness, FPostProcessSettings)
        .def_readwrite("AutoExposureMaxBrightness", &FPostProcessSettings::AutoExposureMaxBrightness)
        ;

    UEPY_EXPOSE_CLASS(UCameraComponent, USceneComponent, m)
        .def("SetFieldOfView", [](UCameraComponent& self, float fov) { self.SetFieldOfView(fov); })
        BIT_PROP(bLockToHmd, UCameraComponent)
        .def("SetConstraintAspectRatio", [](UCameraComponent& self, bool c) { self.SetConstraintAspectRatio(c); })
        .def("SetConstrainAspectRatio", [](UCameraComponent& self, bool c) { self.SetConstraintAspectRatio(c); }) // the above appears to be a UE4 misspelling, so provide a good version too
        .def_readonly("AspectRatio", &UCameraComponent::AspectRatio)
        .def("SetFieldOfView", [](UCameraComponent& self, float f) { self.SetFieldOfView(f); })
        .def_readonly("FieldOfView", &UCameraComponent::FieldOfView)
        .def("SetAspectRatio", [](UCameraComponent& self, float ar) { self.SetAspectRatio(ar); })
        .def("SetPostProcessBlendWeight", [](UCameraComponent& self, float w) { self.SetPostProcessBlendWeight(w); })
        .def("SetProjectionMode", [](UCameraComponent& self, int mode) { self.SetProjectionMode((ECameraProjectionMode::Type)mode); })
        .def_readwrite("PostProcessSettings", &UCameraComponent::PostProcessSettings)
        ;

    py::class_<FCameraFilmbackSettings>(m, "FCameraFilmbackSettings")
        .def(py::init<>())
        .def_readwrite("SensorWidth", &FCameraFilmbackSettings::SensorWidth)
        .def_readwrite("SensorHeight", &FCameraFilmbackSettings::SensorHeight)
        .def_readwrite("SensorAspectRatio", &FCameraFilmbackSettings::SensorAspectRatio)
        ;

    py::class_<FCameraLensSettings>(m, "FCameraLensSettings")
        .def(py::init<>())
        .def_readwrite("MinFocalLength", &FCameraLensSettings::MinFocalLength)
        .def_readwrite("MaxFocalLength", &FCameraLensSettings::MaxFocalLength)
        .def_readwrite("MinFStop", &FCameraLensSettings::MinFStop)
        .def_readwrite("MaxFStop", &FCameraLensSettings::MaxFStop)
        .def_readwrite("MinimumFocusDistance", &FCameraLensSettings::MinimumFocusDistance)
        .def_readwrite("DiaphragmBladeCount", &FCameraLensSettings::DiaphragmBladeCount)
        ;

    py::class_<FCameraTrackingFocusSettings>(m, "FCameraTrackingFocusSettings")
        .def_property("ActorToTrack", [](FCameraTrackingFocusSettings& self) { return self.ActorToTrack.Get(); }, [](FCameraTrackingFocusSettings& self, AActor* a) { self.ActorToTrack = a; }, py::return_value_policy::reference)
        .def_readwrite("RelativeOffset", &FCameraTrackingFocusSettings::RelativeOffset)
        BIT_PROP(bDrawDebugTrackingFocusPoint, FCameraTrackingFocusSettings)
        ;

    py::class_<FCameraFocusSettings>(m, "FCameraFocusSettings")
        .def(py::init<>())
        .def_property("FocusMethod", [](FCameraFocusSettings& self) { return (int)self.FocusMethod; }, [](FCameraFocusSettings& self, int m) { self.FocusMethod = (ECameraFocusMethod)m; })
        .def_readwrite("ManualFocusDistance", &FCameraFocusSettings::ManualFocusDistance)
        .def_readwrite("TrackingFocusSettings", &FCameraFocusSettings::TrackingFocusSettings)
        BIT_PROP(bSmoothFocusChanges, FCameraFocusSettings)
        .def_readwrite("FocusSmoothingInterpSpeed", &FCameraFocusSettings::FocusSmoothingInterpSpeed)
        .def_readwrite("FocusOffset", &FCameraFocusSettings::FocusOffset)
        ;

    UEPY_EXPOSE_CLASS(UCineCameraComponent, UCameraComponent, m)
        .def("SetCurrentFocalLength", [](UCineCameraComponent& self, float f) { self.SetCurrentFocalLength(f); })
        .def("GetHorizontalFieldOfView", [](UCineCameraComponent& self) { return self.GetHorizontalFieldOfView(); })
        .def("GetVerticalFieldOfView", [](UCineCameraComponent& self) { return self.GetVerticalFieldOfView(); })
        .def("SetFilmbackPresetByName", [](UCineCameraComponent& self, std::string& s) { self.SetFilmbackPresetByName(FSTR(s)); })
        .def("SetLensPresetByName", [](UCineCameraComponent& self, std::string& s) { self.SetLensPresetByName(FSTR(s)); })
        .def_readwrite("CurrentFocalLength", &UCineCameraComponent::CurrentFocalLength)
        .def_readwrite("CurrentAperture", &UCineCameraComponent::CurrentAperture)
        .def_readwrite("CurrentFocusDistance", &UCineCameraComponent::CurrentFocusDistance)
        .def_readwrite("Filmback", &UCineCameraComponent::Filmback)
        .def_readwrite("LensSettings", &UCineCameraComponent::LensSettings)
        .def_readwrite("FocusSettings", &UCineCameraComponent::FocusSettings)
        ;

    UEPY_EXPOSE_CLASS(UWidgetInteractionComponent, USceneComponent, m)
        .def_readwrite("VirtualUserIndex", &UWidgetInteractionComponent::VirtualUserIndex)
        .def_readwrite("PointerIndex", &UWidgetInteractionComponent::PointerIndex)
        ENUM_PROP(InteractionSource, EWidgetInteractionSource, UWidgetInteractionComponent)
        .def("SetCustomHitResult", [](UWidgetInteractionComponent& self, FHitResult& hr) { self.SetCustomHitResult(hr); })
        .def("PressPointerKey", [](UWidgetInteractionComponent& self, std::string& keyName) { FKey key(keyName.c_str()); self.PressPointerKey(key); })
        .def("ReleasePointerKey", [](UWidgetInteractionComponent& self, std::string& keyName) { FKey key(keyName.c_str()); self.ReleasePointerKey(key); })
        .def("ScrollWheel", [](UWidgetInteractionComponent& self, float scrollDelta) { self.ScrollWheel(scrollDelta); })
        ;

    UEPY_EXPOSE_CLASS(UPostProcessComponent, USceneComponent, m)
        // NOTE: the APIs here are pretty hacky - I added only what was needed for one very narrow use case
        .def("AddOrUpdateBlendable", [](UPostProcessComponent& self, UObject* blendable, float weight) { self.AddOrUpdateBlendable(blendable, weight); })
        BIT_PROP(bEnabled, UPostProcessComponent)
        BIT_PROP(bUnbound, UPostProcessComponent)
        .def("SetVignetteStuff", [](UPostProcessComponent& self, float vignetteSize, float vignetteIntensity)
        {
            FPostProcessSettings* settings = &self.Settings;
            settings->bOverride_VignetteIntensity = true;
            settings->bOverride_DepthOfFieldVignetteSize = true;
            settings->DepthOfFieldVignetteSize = vignetteSize;
            settings->VignetteIntensity = vignetteIntensity;
        })
        .def("ClearBlendables", [](UPostProcessComponent& self) { self.Settings.WeightedBlendables.Array.Empty(); })
        ;

    py::class_<FSlateAtlasData>(m, "FSlateAtlasData")
        .def(py::init<UTexture*,FVector2D,FVector2D>())
        .def_readwrite("AtlasTexture", &FSlateAtlasData::AtlasTexture, py::return_value_policy::reference)
        .def_readwrite("StartUV", &FSlateAtlasData::StartUV)
        .def_readwrite("SizeUV", &FSlateAtlasData::SizeUV)
        .def("GetSourceDimensions", [](FSlateAtlasData& self) { return self.GetSourceDimensions(); })
        ;

    py::class_<UPaperSprite, UObject, UnrealTracker<UPaperSprite>>(m, "UPaperSprite")
        .def_static("StaticClass", []() { return UPaperSprite::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UPaperSprite>(obj); }, py::return_value_policy::reference)
        .def("GetBakedTexture", [](UPaperSprite& self) { return self.GetBakedTexture(); }, py::return_value_policy::reference)
        .def("GetSlateAtlasData", [](UPaperSprite& self) { return self.GetSlateAtlasData(); }, py::return_value_policy::reference)
        ;

    py::class_<UPhysicalMaterial, UObject, UnrealTracker<UPhysicalMaterial>>(m, "UPhysicalMaterial")
        .def_static("StaticClass", []() { return UPhysicalMaterial::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UPhysicalMaterial>(obj); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(UHeadMountedDisplayFunctionLibrary, UObject, m)
        .def_static("IsHeadMountedDisplayEnabled", []() { return UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled(); })
        .def_static("IsHeadMountedDisplayConnected", []() { return UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayConnected(); })
        .def_static("EnableHMD", [](bool enable) { return UHeadMountedDisplayFunctionLibrary::EnableHMD(enable); })
        .def_static("GetHMDDeviceName", []() { return PYSTR(UHeadMountedDisplayFunctionLibrary::GetHMDDeviceName().ToString()); })
        .def_static("GetOrientationAndPosition", []()
        {
            FRotator rot;
            FVector loc;
            UHeadMountedDisplayFunctionLibrary::GetOrientationAndPosition(rot, loc);
            return py::make_tuple(rot, loc);
        })
        .def_static("SetTrackingOrigin", [](int oType) { UHeadMountedDisplayFunctionLibrary::SetTrackingOrigin((EHMDTrackingOrigin::Type)oType); })
        .def_static("GetVRFocusState", []()
        {
            bool useFocus, hasFocus;
            UHeadMountedDisplayFunctionLibrary::GetVRFocusState(useFocus, hasFocus);
            return py::make_tuple(useFocus, hasFocus);
        })
        .def_static("SetSpectatorScreenMode", [](int m) { UHeadMountedDisplayFunctionLibrary::SetSpectatorScreenMode((ESpectatorScreenMode)m); })
        .def_static("SetSpectatorScreenTexture", [](UTexture* t) { UHeadMountedDisplayFunctionLibrary::SetSpectatorScreenTexture(t); })
        .def_static("SetSpectatorScreenModeTexturePlusEyeLayout", [](FVector2D& eyeRectMin, FVector2D& eyeRectMax, FVector2D& texRectMin, FVector2D& texRectMax, bool drawEyeFirst, bool clearBlack, bool useAlpha) { UHeadMountedDisplayFunctionLibrary::SetSpectatorScreenModeTexturePlusEyeLayout(eyeRectMin, eyeRectMax, texRectMin, texRectMax, drawEyeFirst, clearBlack, useAlpha); })
        .def_static("GetMotionControllerData_DeviceName", [](UObject* worldCtx, int hand)
        {   // hacky helper to try and detect motion controller types since under OpenXR we are getting overly generic device names when SteamVR is the OpenXR runtime
            FXRMotionControllerData d;
            UHeadMountedDisplayFunctionLibrary::GetMotionControllerData(worldCtx, (EControllerHand)hand, d);
            return PYSTR(d.DeviceName.ToString());
        })
        ;

    UEPY_EXPOSE_CLASS(UVOIPTalker, UActorComponent, m)
        .def("RegisterWithPlayerState", [](UVOIPTalker &self, APlayerState *owningState) { self.RegisterWithPlayerState(owningState); })
        .def("GetVoiceLevel", [](UVOIPTalker &self) { return self.GetVoiceLevel(); })
        ;

    UEPY_EXPOSE_CLASS(UVOIPTalker_CGLUE, UVOIPTalker, glueclasses)
        ;

    UEPY_EXPOSE_CLASS(UAudioCaptureComponent, USynthComponent, m)
        ;

    UEPY_EXPOSE_CLASS(UGameUserSettings, UObject, m)
        // note that how we expose some of these is currently different than the UE4 API to make it less clunky
        .def_static("GetDesktopResolution", []() { FIntPoint p = UGameUserSettings::GetGameUserSettings()->GetDesktopResolution(); return py::make_tuple(p.X, p.Y); })
        .def_static("GetScreenResolution", []() { FIntPoint p = UGameUserSettings::GetGameUserSettings()->GetScreenResolution(); return py::make_tuple(p.X, p.Y); })
        .def_static("GetGameUserSettings", []() { return UGameUserSettings::GetGameUserSettings(); }, py::return_value_policy::reference)
        .def("ApplySettings", [](UGameUserSettings& self, bool checkCLIOverrides) { self.ApplySettings(checkCLIOverrides); }) // note that this persists them also
        .def("SetWindowPosition", [](UGameUserSettings& self, int x, int y) { self.SetWindowPosition(x, y); })
        .def("SetFrameRateLimit", [](UGameUserSettings& self, float fps) { self.SetFrameRateLimit(fps); })
        .def("SetFullscreenMode", [](UGameUserSettings& self, int mode) { self.SetFullscreenMode((EWindowMode::Type)mode); })
        .def("SetScreenResolution", [](UGameUserSettings& self, int w, int h) { self.SetScreenResolution(FIntPoint(w,h)); })
        ;

    // net rep stuff
    py::class_<UNRChannel, UObject, UnrealTracker<UNRChannel>>(m, "UNRChannel", py::dynamic_attr())
        .def_static("StaticClass", []() { return UNRChannel::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UNRChannel>(w); }, py::return_value_policy::reference)
        .def_static("SetAppBridge", [](py::object& bridge) { UNRChannel::SetAppBridge(bridge); })
        .def_readwrite("channelID", &UNRChannel::channelID)
        .def("AddMessage", [](UNRChannel& self, py::object pyPayload, bool reliable)
        {
            py::buffer buffer = pyPayload.cast<py::buffer>();
            py::buffer_info info = buffer.request();
            if (info.format != py::format_descriptor<uint8>::format() || info.ndim != 1)
            {
                LERROR("Not given a flat uint8 buffer");
                return;
            }
            TArray<uint8> payload;
            payload.AddUninitialized(info.size);
            memcpy(payload.GetData(), info.ptr, info.size);
            self.AddMessage(payload, reliable);
        })
        ;
}

//#pragma optimize("", on)

#pragma warning(pop)
