// sets up the main 'uepy' builtin module
#include "uepy.h"
#include "common.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/ImportanceSamplingLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameState.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "EngineUtils.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Components/BoxComponent.h"
#include "PaperSprite.h"
#include <map>

//#pragma optimize("", off)

static std::map<FString, py::object> pyClassMap; // class name --> python class

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

    // given an engine obj that you know to actually be a Python-implemented, get the associated Python instance from it (or None if you were wrong)
    m.def("PyInst", [](UObject *obj)
    {
        py::object ret = py::none();
        IUEPYGlueMixin *p = Cast<IUEPYGlueMixin>(obj);
        if (p)
            ret = p->pyInst;
        return ret;
    });

    py::class_<PyCheesyIterator<float>>(m, "CheesyFloatIterator")
        .def("__iter__", [](PyCheesyIterator<float> &it) -> PyCheesyIterator<float>& { return it; })
        .def("__next__", &PyCheesyIterator<float>::next)
        ;

    py::class_<PyCheesyIterator<int>>(m, "CheesyIntIterator")
        .def("__iter__", [](PyCheesyIterator<int> &it) -> PyCheesyIterator<int>& { return it; })
        .def("__next__", &PyCheesyIterator<int>::next)
        ;

    py::class_<FVector2D>(m, "FVector2D")
        .def(py::init<FVector2D>())
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
        .def(-py::self)
        .def(float() * py::self)
        .def(py::self * float())
        .def(py::self / float())
        .def("__iter__", [](FVector2D& self) { return PyCheesyIterator<float>({self.X, self.Y}); })
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
        .def("__iter__", [](FVector& self) { return PyCheesyIterator<float>({self.X, self.Y, self.Z}); })
        .def("Rotation", [](FVector& self) { return self.Rotation(); })
        .def("ToOrientationQuat", [](FVector& self) { return self.ToOrientationQuat(); })
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
        .def("Normalize", [](FVector& self) { return self.Normalize(); })
        .def("IsNormalized", [](FVector& self) { return self.IsNormalized(); })
        .def_static("DotProduct", [](FVector& a, FVector& b) { return FVector::DotProduct(a, b); })
        .def_static("CrossProduct", [](FVector& a, FVector& b) { return FVector::CrossProduct(a, b); })
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
        .def("__iter__", [](FRotator& self) { return PyCheesyIterator<float>({self.Roll, self.Pitch, self.Yaw}); })
        .def(float() * py::self)
        .def(py::self * float())
        ;

    py::class_<FQuat>(m, "FQuat")
        .def(py::init<FQuat>())
        .def(py::init<>())
        .def(py::init<float,float,float,float>())
        .def_readwrite("X", &FQuat::X)
        .def_readwrite("Y", &FQuat::Y)
        .def_readwrite("Z", &FQuat::Z)
        .def_readwrite("W", &FQuat::W)
        .def_readwrite("x", &FQuat::X)
        .def_readwrite("y", &FQuat::Y)
        .def_readwrite("z", &FQuat::Z)
        .def_readwrite("w", &FQuat::W)
        .def_static("FindBetweenVectors", [](FVector& a, FVector& b) { return FQuat::FindBetweenVectors(a,b); })
        .def("Rotator", [](FQuat& self) { return self.Rotator(); })
        .def(py::self * py::self)
        .def(py::self + py::self)
        .def(py::self | py::self)
        .def(py::self += py::self)
        .def(py::self - py::self)
        .def(py::self -= py::self)
        .def(py::self == py::self)
        .def(py::self * FVector())
        .def(py::self * float())
        .def(py::self / float())
        .def(py::self *= float())
        .def(py::self /= float())
        ;

    py::class_<FTransform>(m, "FTransform")
        .def(py::init<FTransform>())
        .def_readonly_static("Identity", &FTransform::Identity)
        .def(py::init<>())
        .def(py::init([](FVector& loc, FRotator& rot, FVector& scale) { return FTransform(rot, loc, scale); }))
        .def("Inverse", [](FTransform& self) { return self.Inverse(); })
        .def("Rotator", [](FTransform& self) { return self.Rotator(); })
        .def("GetRotation", [](FTransform& self) { return self.Rotator(); })
        .def("SetRotation", [](FTransform& self, FRotator& r) { FQuat q(r); self.SetRotation(q); })
        .def("GetTranslation", [](FTransform& self) { return self.GetTranslation(); })
        .def("GetLocation", [](FTransform& self) { return self.GetLocation(); })
        .def("SetTranslation", [](FTransform& self, FVector& t) { self.SetTranslation(t); })
        .def("SetLocation", [](FTransform& self, FVector& t) { self.SetLocation(t); })
        .def("GetScale3D", [](FTransform& self) { return self.GetScale3D(); })
        .def("SetScale3D", [](FTransform& self, FVector& v) { self.SetScale3D(v); })
        .def("TransformPosition", [](FTransform& self, FVector& pos) { return self.TransformPosition(pos); })
        .def_property("translation", [](FTransform& self) { return self.GetTranslation(); }, [](FTransform& self, FVector& v) { self.SetTranslation(v); })
        .def_property("scale", [](FTransform& self) { return self.GetScale3D(); }, [](FTransform& self, FVector& v) { self.SetScale3D(v); })
        .def_property("rotation", [](FTransform& self) { return self.Rotator(); }, [](FTransform& self, FRotator& r) { FQuat q(r); self.SetRotation(q); })
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
        .def("__iter__", [](FColor& self) { return PyCheesyIterator<int>({self.R, self.G, self.B, self.A}); })
        ;

    py::class_<FLinearColor>(m, "FLinearColor")
        .def(py::init<FLinearColor>())
        .def(py::init<float, float, float, float>(), "r"_a=0.0f, "g"_a=0.0f, "b"_a=0.0f, "a"_a=1.0f)
        .def_readwrite("R", &FLinearColor::R)
        .def_readwrite("r", &FLinearColor::R)
        .def_readwrite("G", &FLinearColor::G)
        .def_readwrite("g", &FLinearColor::G)
        .def_readwrite("B", &FLinearColor::B)
        .def_readwrite("b", &FLinearColor::B)
        .def_readwrite("A", &FLinearColor::A)
        .def_readwrite("a", &FLinearColor::A)
        .def("__iter__", [](FLinearColor& self) { return PyCheesyIterator<float>({self.R, self.G, self.B, self.A}); })
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

    py::class_<FAnchors>(m, "FAnchors")
        .def(py::init<FAnchors>())
        .def(py::init<>())
        .def(py::init<float>())
        .def(py::init<float,float>())
        .def(py::init<float,float,float,float>())
        .def_readwrite("Minimum", &FAnchors::Minimum)
        .def_readwrite("Maximum", &FAnchors::Maximum)
        ;

    // Given a UObject, returns its address as a number
    m.def("AddressOf", [](UObject* obj) { return (unsigned long long)obj; });

    // Force garbage collection to happen
    m.def("ForceGC", []() { if (GEngine) GEngine->ForceGarbageCollection(true); });

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
        .def_static("StaticClass", []() { return UObject::StaticClass(); })
        .def("GetClass", [](UObject& self) { return self.GetClass(); })
        .def("GetName", [](UObject& self) { return PYSTR(self.GetName()); })
        .def("GetPathName", [](UObject& self) { return PYSTR(self.GetPathName()); })
        .def("ConditionalBeginDestroy", [](UObject* self) { if (self->IsValidLowLevel()) self->ConditionalBeginDestroy(); })
        .def("IsValid", [](UObject* self) { return self->IsValidLowLevel() && !self->IsPendingKillOrUnreachable(); })
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
        })

        // methods for accessing stuff via the UE4 reflection system (e.g. to interact with Blueprints, UPROPERTYs, etc)
        .def("Set", [](UObject* self, std::string k, py::object& value) { SetObjectUProperty(self, k, value); })
        .def("Get", [](UObject* self, std::string k) { return GetObjectUProperty(self, k); })
        .def("Call", [](UObject* self, std::string funcName, py::args& args){ return CallObjectUFunction(self, funcName, args); }, py::return_value_policy::reference)
        .def("Bind", [](UObject* self, std::string eventName, py::object callback) { BindDelegateCallback(self, eventName, callback); })
        .def("Unbind", [](UObject* self, std::string eventName, py::object callback) { UnbindDelegateCallback(self, eventName, callback); })
        .def("Broadcast", [](UObject* self, std::string eventName, py::args& args) { BroadcastEvent(self, eventName, args); })
        ;

    py::class_<UClass, UObject, UnrealTracker<UClass>>(m, "UClass") // TODO: UClass --> UStruct --> UField --> UObject
        .def_static("Cast", [](UObject *obj) { return Cast<UClass>(obj); }, py::return_value_policy::reference)
        .def("GetDefaultObject", [](UClass& self) { return self.GetDefaultObject(); }, py::return_value_policy::reference)
        .def("ImplementsInterface", [](UClass& self, py::object interfaceClass)
        {
            UClass *k = PyObjectToUClass(interfaceClass);
            return k ? self.ImplementsInterface(k) : false;
        })
        ;

    py::class_<UBlueprintGeneratedClass, UClass, UnrealTracker<UBlueprintGeneratedClass>>(m, "UBlueprintGeneratedClass")
        .def_static("StaticClass", []() { return UBlueprintGeneratedClass::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UBlueprintGeneratedClass>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UInterface, UObject, UnrealTracker<UInterface>>(m, "UInterface")
        .def_static("StaticClass", []() { return UInterface::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UInterface>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UCurveBase, UObject, UnrealTracker<UCurveBase>>(m, "UCurveBase")
        .def_static("StaticClass", []() { return UCurveBase::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UCurveBase>(obj); }, py::return_value_policy::reference)
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

    py::class_<UCurveFloat, UCurveBase, UnrealTracker<UCurveFloat>>(m, "UCurveFloat")
        .def_static("StaticClass", []() { return UCurveFloat::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UCurveFloat>(obj); }, py::return_value_policy::reference)
        .def("GetFloatValue", [](UCurveFloat& self, float f) { return self.GetFloatValue(f); })
        ;

    py::class_<UCurveVector, UCurveBase, UnrealTracker<UCurveVector>>(m, "UCurveVector")
        .def_static("StaticClass", []() { return UCurveVector::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UCurveVector>(obj); }, py::return_value_policy::reference)
        .def("GetVectorValue", [](UCurveVector& self, float f) { return self.GetVectorValue(f); })
        ;

    py::class_<UStaticMesh, UObject, UnrealTracker<UStaticMesh>>(m, "UStaticMesh")
        .def_static("StaticClass", []() { return UStaticMesh::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UStaticMesh>(obj); }, py::return_value_policy::reference)
        .def("GetBounds", [](UStaticMesh& self) { return self.GetBounds(); })
        .def("GetBoundingBox", [](UStaticMesh& self) { return self.GetBoundingBox(); })
        .def("GetSize", [](UStaticMesh& self) { return self.GetBounds().BoxExtent * 2; })
        ;

    py::class_<UActorComponent, UObject, UnrealTracker<UActorComponent>>(m, "UActorComponent")
        .def_static("StaticClass", []() { return UActorComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UActorComponent>(obj); }, py::return_value_policy::reference)
        .def("ComponentHasTag", [](UActorComponent& self, std::string& tag) { return self.ComponentHasTag(FSTR(tag)); })
        .def("SetActive", [](UActorComponent& self, bool a) { self.SetActive(a); })
        .def("IsRegistered", [](UActorComponent& self) { return self.IsRegistered(); })
        .def("RegisterComponent", [](UActorComponent& self) { self.RegisterComponent(); })
        .def("UnregisterComponent", [](UActorComponent& self) { self.UnregisterComponent(); })
        .def("DestroyComponent", [](UActorComponent& self) { self.DestroyComponent(); })
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
        ;

    py::class_<USceneComponent, UActorComponent, UnrealTracker<USceneComponent>>(m, "USceneComponent")
        .def_static("StaticClass", []() { return USceneComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<USceneComponent>(obj); }, py::return_value_policy::reference)
        .def("GetRelativeLocation", [](USceneComponent& self) { return self.GetRelativeLocation(); })
        .def("SetRelativeLocation", [](USceneComponent& self, FVector v) { self.SetRelativeLocation(v); })
        .def("GetRelativeRotation", [](USceneComponent& self) { return self.GetRelativeRotation(); })
        .def("SetRelativeRotation", [](USceneComponent& self, FRotator r) { self.SetRelativeRotation(r); })
        .def("GetRelativeScale3D", [](USceneComponent& self) { return self.GetRelativeScale3D(); })
        .def("SetRelativeScale3D", [](USceneComponent& self, FVector v) { self.SetRelativeScale3D(v); })
        .def("GetRelativeTransform", [](USceneComponent& self) { return self.GetRelativeTransform(); })
        .def("SetRelativeTransform", [](USceneComponent& self, FTransform& t) { self.SetRelativeTransform(t); })
        .def("ResetRelativeTransform", [](USceneComponent& self) { self.ResetRelativeTransform(); })
        .def("AttachToComponent", [](USceneComponent& self, USceneComponent *parent) { return self.AttachToComponent(parent, FAttachmentTransformRules::KeepRelativeTransform); }) // TODO: AttachmentRules, socket
        .def("DetachFromComponent", [](USceneComponent& self) { self.DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform); })
        .def("SetRelativeLocationAndRotation", [](USceneComponent& self, FVector& loc, FRotator& rot) { self.SetRelativeLocationAndRotation(loc, rot); })
        .def("SetRelativeTransform", [](USceneComponent& self, FTransform& t) { self.SetRelativeTransform(t); })
        .def("AddRelativeLocation", [](USceneComponent& self, FVector& deltaLoc) { self.AddRelativeLocation(deltaLoc); })
        .def("AddLocalOffset", [](USceneComponent& self, FVector& deltaLoc) { self.AddLocalOffset(deltaLoc); })
        .def("AddLocalRotation", [](USceneComponent& self, FRotator& deltaRot) { self.AddLocalRotation(deltaRot); })
        .def("AddLocalRotation", [](USceneComponent& self, FQuat& deltaRot) { self.AddLocalRotation(deltaRot); })
        .def("SetVisibility", [](USceneComponent& self, bool visible, bool propagate) { self.SetVisibility(visible, propagate); }, py::arg("visible"), py::arg("propagate")=false)
        .def("GetHiddenInGame", [](USceneComponent& self) { return self.bHiddenInGame; })
        .def("SetHiddenInGame", [](USceneComponent& self, bool hidden, bool propagate) { self.SetHiddenInGame(hidden, propagate); }, py::arg("hidden"), py::arg("propagate")=false)
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
        .def("SetWorldLocation", [](USceneComponent& self, FVector& loc) { self.SetWorldLocation(loc); })
        .def("SetWorldRotation", [](USceneComponent& self, FRotator& rot) { self.SetWorldRotation(rot); })
        .def("SetWorldRotation", [](USceneComponent& self, FQuat& rot) { self.SetWorldRotation(rot); })
        .def("GetSocketTransform", [](USceneComponent& self, std::string& name) { return self.GetSocketTransform(FSTR(name)); })
        .def("GetSocketLocation", [](USceneComponent& self, std::string& name) { return self.GetSocketLocation(FSTR(name)); })
        .def("GetSocketRotation", [](USceneComponent& self, std::string& name) { return self.GetSocketRotation(FSTR(name)); })
        .def("CalcBounds", [](USceneComponent& self, FTransform& locToWorld) { return self.CalcBounds(locToWorld); })
        .def("GetAttachParent", [](USceneComponent& self) { return self.GetAttachParent(); })
        .def("GetChildrenComponents", [](USceneComponent& self, bool incAllDescendents)
        {
            TArray<USceneComponent*> kids;
            self.GetChildrenComponents(incAllDescendents, kids);
            py::list ret;
            for (auto kid : kids)
                ret.append(kid);
            return ret;
        }, py::return_value_policy::reference)
        ;

    py::class_<UDecalComponent, USceneComponent, UnrealTracker<UDecalComponent>>(m, "UDecalComponent")
        .def_static("StaticClass", []() { return UDecalComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UDecalComponent>(obj); }, py::return_value_policy::reference)
        .def_readwrite("DecalSize", &UDecalComponent::DecalSize)
        .def("SetFadeIn", [](UDecalComponent& self, float startDelay, float dur) { self.SetFadeIn(startDelay, dur); })
        .def("SetFadeOut", [](UDecalComponent& self, float startDelay, float dur) { self.SetFadeOut(startDelay, dur); })
        .def("SetFadeScreenSize", [](UDecalComponent& self, float size) { self.SetFadeScreenSize(size); })
        .def("SetDecalMaterial", [](UDecalComponent& self, UMaterialInterface* mat) { self.SetDecalMaterial(mat); })
        ;

    py::class_<UPrimitiveComponent, USceneComponent, UnrealTracker<UPrimitiveComponent>>(m, "UPrimitiveComponent")
        .def_static("StaticClass", []() { return UPrimitiveComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UPrimitiveComponent>(obj); }, py::return_value_policy::reference)
        .def("SetCollisionEnabled", [](UPrimitiveComponent& self, int c) { self.SetCollisionEnabled((ECollisionEnabled::Type)c); })
        .def("SetCollisionObjectType", [](UPrimitiveComponent& self, int c) { self.SetCollisionObjectType((ECollisionChannel)c); })
        .def("SetCollisionResponseToAllChannels", [](UPrimitiveComponent& self, int r) { self.SetCollisionResponseToAllChannels((ECollisionResponse)r); })
        .def("SetCollisionResponseToChannel", [](UPrimitiveComponent& self, int c, int r) { self.SetCollisionResponseToChannel((ECollisionChannel)c, (ECollisionResponse)r); })
        .def("SetRenderCustomDepth", [](UPrimitiveComponent& self, bool b) { self.SetRenderCustomDepth(b); })
        .def("SetCustomDepthStencilValue", [](UPrimitiveComponent& self, int v) { self.SetCustomDepthStencilValue(v); })
        .def_readonly("CustomDepthStencilValue", &UPrimitiveComponent::CustomDepthStencilValue)
        .def("SetCastShadow", [](UPrimitiveComponent& self, bool s) { self.SetCastShadow(s); })
        .def_property_readonly("bRenderCustomDepth", [](UPrimitiveComponent& self) { return (bool)self.bRenderCustomDepth; })
        ;

    py::class_<UFXSystemComponent, UPrimitiveComponent, UnrealTracker<UFXSystemComponent>>(m, "UFXSystemComponent")
        .def_static("StaticClass", []() { return UFXSystemComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UFXSystemComponent>(obj); }, py::return_value_policy::reference)
        .def("SetFloatParameter", [](UFXSystemComponent& self, std::string name, float v) { self.SetFloatParameter(FSTR(name), v); })
        .def("SetVectorParameter", [](UFXSystemComponent& self, std::string name, FVector& v) { self.SetVectorParameter(FSTR(name), v); })
        .def("SetColorParameter", [](UFXSystemComponent& self, std::string name, FLinearColor& v) { self.SetColorParameter(FSTR(name), v); })
        .def("SetActorParameter", [](UFXSystemComponent& self, std::string name, AActor* v) { self.SetActorParameter(FSTR(name), v); })
        ;

    py::class_<UParticleSystemComponent, UFXSystemComponent, UnrealTracker<UParticleSystemComponent>>(m, "UParticleSystemComponent")
        .def_static("StaticClass", []() { return UParticleSystemComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UParticleSystemComponent>(obj); }, py::return_value_policy::reference)
        .def("SetTemplate", [](UParticleSystemComponent& self, UParticleSystem* sys) { self.SetTemplate(sys); })
        ;

    py::class_<UShapeComponent, UPrimitiveComponent, UnrealTracker<UShapeComponent>>(m, "UShapeComponent")
        .def_static("StaticClass", []() { return UShapeComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UShapeComponent>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<USphereComponent, UShapeComponent, UnrealTracker<USphereComponent>>(m, "USphereComponent")
        .def_static("StaticClass", []() { return USphereComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<USphereComponent>(obj); }, py::return_value_policy::reference)
        .def("SetSphereRadius", [](USphereComponent& self, float r) { self.SetSphereRadius(r); })
        ;

    py::class_<UBoxComponent, UShapeComponent, UnrealTracker<UBoxComponent>>(m, "UBoxComponent")
        .def_static("StaticClass", []() { return UBoxComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UBoxComponent>(obj); }, py::return_value_policy::reference)
        .def("SetBoxExtent", [](UBoxComponent& self, FVector& e) { self.SetBoxExtent(e); })
        .def("GetUnscaledBoxExtent", [](UBoxComponent& self) { return self.GetUnscaledBoxExtent(); }) // this is the same as boxComp.BoxExtent
        ;

    py::class_<UMeshComponent, UPrimitiveComponent, UnrealTracker<UMeshComponent>>(m, "UMeshComponent")
        .def_static("StaticClass", []() { return UMeshComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UMeshComponent>(obj); }, py::return_value_policy::reference)
        .def("SetMaterial", [](UMeshComponent& self, int index, UMaterialInterface* mat) { self.SetMaterial(index, mat); })
        ;

    py::class_<UStaticMeshComponent, UMeshComponent, UnrealTracker<UStaticMeshComponent>>(m, "UStaticMeshComponent")
        .def_static("StaticClass", []() { return UStaticMeshComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UStaticMeshComponent>(obj); }, py::return_value_policy::reference)
        .def("GetStaticMesh", [](UStaticMeshComponent& self) { return self.GetStaticMesh(); })
        .def("SetStaticMesh", [](UStaticMeshComponent& self, UStaticMesh *newMesh) -> bool { return self.SetStaticMesh(newMesh); })
        .def("SetMaterial", [](UStaticMeshComponent& self, int index, UMaterialInterface *newMat) -> void { self.SetMaterial(index, newMat); }) // technically, UMaterialInterface
        .def_readwrite("StreamingDistanceMultiplier", &UStaticMeshComponent::StreamingDistanceMultiplier)
        ;

    py::class_<UInstancedStaticMeshComponent, UStaticMeshComponent, UnrealTracker<UInstancedStaticMeshComponent>>(m, "UInstancedStaticMeshComponent")
        .def_static("StaticClass", []() { return UInstancedStaticMeshComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UInstancedStaticMeshComponent>(obj); }, py::return_value_policy::reference)
        .def("AddInstance", [](UInstancedStaticMeshComponent& self, FTransform& t) { return self.AddInstance(t); })
        .def("RemoveInstance", [](UInstancedStaticMeshComponent& self, int index) { return self.RemoveInstance(index); })
        .def("ClearInstances", [](UInstancedStaticMeshComponent& self) { self.ClearInstances(); })
        .def("GetInstanceCount", [](UInstancedStaticMeshComponent& self) { return self.GetInstanceCount(); })
        .def_readwrite("InstancingRandomSeed", &UInstancedStaticMeshComponent::InstancingRandomSeed)
        ;

    py::class_<UWorld, UObject, UnrealTracker<UWorld>>(m, "UWorld")
        .def_static("StaticClass", []() { return UWorld::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UWorld>(obj); }, py::return_value_policy::reference)
        .def_property_readonly("WorldType", [](UWorld& self) { return (int)self.WorldType; })
        .def("GetAllActors", [](UWorld* self)
        {
            py::list ret;
            for (TActorIterator<AActor> it(self); it ; ++it)
                ret.append(*it);
            return ret;
        }, py::return_value_policy::reference)
        ;

    m.def("GetAllWorlds", []()
    {
        py::list ret;
        for (TObjectIterator<UWorld> iter; iter; ++iter)
            ret.append(*iter);
        return ret;
    }, py::return_value_policy::reference);

    py::class_<UGameplayStatics, UObject, UnrealTracker<UGameplayStatics>>(m, "UGameplayStatics") // not sure that it makes sense to really expose this fully
        .def_static("GetGameInstance", [](UWorld *world) { return UGameplayStatics::GetGameInstance(world); })
        .def_static("GetGameState", [](UWorld *world) { return UGameplayStatics::GetGameState(world); })
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
        .def_static("GetPlayerController", [](UObject *worldCtx, int playerIndex) { return UGameplayStatics::GetPlayerController(worldCtx, playerIndex); })
        ;

    py::class_<FHitResult>(m, "FHitResult")
        .def_property_readonly("Normal", [](FHitResult& self) { FVector v = self.Normal; return v; })
        .def_property_readonly("Location", [](FHitResult& self) { FVector v = self.Location; return v; })
        .def_property_readonly("ImpactPoint", [](FHitResult& self) { FVector v = self.ImpactPoint; return v; })
        .def_property_readonly("ImpactNormal", [](FHitResult& self) { FVector v = self.ImpactNormal; return v; })
        .def_property_readonly("Actor", [](FHitResult& self) { AActor* a = nullptr; if (self.Actor.IsValid()) a = self.Actor.Get(); return a; })
        .def_property_readonly("Component", [](FHitResult& self) { UPrimitiveComponent* c = nullptr; if (self.Component.IsValid()) c = self.Component.Get(); return c; })
        .def_readonly("Time", &FHitResult::Time)
        .def_readonly("Distance", &FHitResult::Distance)
        ;

    py::class_<UKismetSystemLibrary, UObject, UnrealTracker<UKismetSystemLibrary>>(m, "UKismetSystemLibrary")
        .def_static("GetPathName", [](UObject* obj) { return PYSTR(UKismetSystemLibrary::GetPathName(obj)); })
        .def_static("GetDisplayName", [](UObject* obj) { return PYSTR(UKismetSystemLibrary::GetDisplayName(obj)); })
        .def_static("GetObjectName", [](UObject* obj) { return PYSTR(UKismetSystemLibrary::GetObjectName(obj)); })
        .def_static("IsValid", [](UObject* obj) { return UKismetSystemLibrary::IsValid(obj); })
        .def_static("DrawDebugLine", [](UObject *worldCtx, FVector& start, FVector& end, FLinearColor& color, float duration, float thickness) { UKismetSystemLibrary::DrawDebugLine(worldCtx, start, end, color, duration, thickness); })
        .def_static("DrawDebugBox", [](UObject *worldCtx, FVector& center, FVector& extent, FLinearColor& color, FRotator& rot, float duration, float thickness) { UKismetSystemLibrary::DrawDebugBox(worldCtx, center, extent, color, rot, duration, thickness); })
        .def_static("DrawDebugSphere", [](UObject *worldCtx, FVector& center, float radius, int segs, FLinearColor& color, float duration, float thickness) { UKismetSystemLibrary::DrawDebugSphere(worldCtx, center, radius, segs, color, duration, thickness); })
        .def_static("LineTraceSingle", [](UObject *worldCtx, FVector& start, FVector& end, int channel, bool isComplex, py::list& _ignore, int type, bool ignoreSelf, FLinearColor& traceColor, FLinearColor& hitColor, float drawTime)
        {
            TArray<AActor*> ignore;
            for (py::handle h : _ignore)
                ignore.Emplace(h.cast<AActor*>());
            FHitResult hitResult;
            bool hit = UKismetSystemLibrary::LineTraceSingle(worldCtx, start, end, (ETraceTypeQuery)channel, isComplex, ignore, (EDrawDebugTrace::Type)type, hitResult, ignoreSelf, traceColor, hitColor, drawTime);
            py::list ret;
            ret.append(hitResult);
            ret.append(hit);
            return ret;
        }, py::arg("worldCtx"), py::arg("start"), py::arg("end"), py::arg("channel"), py::arg("isComplex"), py::arg("_ignore"), py::arg("type"), py::arg("ignoreSelf"), py::arg("traceColor")=FLinearColor::Red, py::arg("hitColor")=FLinearColor::Green, py::arg("drawTime")=5.0f)
        ;

    py::class_<UImportanceSamplingLibrary, UObject, UnrealTracker<UImportanceSamplingLibrary>>(m, "UImportanceSamplingLibrary")
        .def_static("RandomSobolCell2D", [](int index, int numCells, FVector2D& cell, FVector2D& seed) { return UImportanceSamplingLibrary::RandomSobolCell2D(index, numCells, cell, seed); })
        ;

    py::class_<FBox>(m, "FBox")
        .def(py::init<FBox>())
        .def(py::init([]() { FBox b(EForceInit::ForceInit); return b; }))
        .def(py::init<FVector,FVector>())
        .def_readwrite("Min", &FBox::Min)
        .def_readwrite("Max", &FBox::Max)
        .def_readwrite("min", &FBox::Min)
        .def_readwrite("max", &FBox::Max)
        .def_readwrite("IsValid", &FBox::IsValid)
        .def("GetCenter", [](FBox& self) { return self.GetCenter(); })
        .def("GetSize", [](FBox& self) { return self.GetSize(); })
        .def(py::self + py::self)
        .def(py::self += py::self)
        ;

    py::class_<FBoxSphereBounds>(m, "FBoxSphereBounds")
        .def(py::init<FBoxSphereBounds>())
        .def(py::init<>())
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
        ;

    py::class_<UKismetMathLibrary, UObject, UnrealTracker<UKismetMathLibrary>>(m, "UKismetMathLibrary")
        .def_static("DegSin", [](float& a) { return UKismetMathLibrary::DegSin(a); })
        .def_static("DegAsin", [](float& a) { return UKismetMathLibrary::DegAsin(a); })
        .def_static("DegCos", [](float& a) { return UKismetMathLibrary::DegCos(a); })
        .def_static("DegAcos", [](float& a) { return UKismetMathLibrary::DegAcos(a); })
        .def_static("DegTan", [](float& a) { return UKismetMathLibrary::DegTan(a); })
        .def_static("DegAtan", [](float& a) { return UKismetMathLibrary::DegAtan(a); })
        .def_static("TEase", [](FTransform& a, FTransform& b, float alpha, int easingFunc, float blend, int steps) { return UKismetMathLibrary::TEase(a, b, alpha, (EEasingFunc::Type)easingFunc, blend, steps); })
        .def_static("VEase", [](FVector& a, FVector& b, float alpha, int easingFunc, float blend, int steps) { return UKismetMathLibrary::VEase(a, b, alpha, (EEasingFunc::Type)easingFunc, blend, steps); })
        .def_static("REase", [](FRotator& a, FRotator& b, float alpha, bool shortestPath, int easingFunc, float blend, int steps) { return UKismetMathLibrary::REase(a, b, alpha, shortestPath, (EEasingFunc::Type)easingFunc, blend, steps); })
        .def_static("EqualEqual_VectorVector", [](FVector& a, FVector& b, float error) { return UKismetMathLibrary::EqualEqual_VectorVector(a, b, error); }, py::arg("a"), py::arg("b"), py::arg("error")=1.e-4f)
        .def_static("FClamp", [](float v, float min, float max) { return UKismetMathLibrary::FClamp(v, min, max); })
        .def_static("FindLookAtRotation", [](FVector& start, FVector& target) { return UKismetMathLibrary::FindLookAtRotation(start, target); })
        .def_static("GetForwardVector", [](FRotator& rot) { return UKismetMathLibrary::GetForwardVector(rot); })
        .def_static("GetRightVector", [](FRotator& rot) { return UKismetMathLibrary::GetRightVector(rot); })
        .def_static("GetUpVector", [](FRotator& rot) { return UKismetMathLibrary::GetUpVector(rot); })
        .def_static("InverseTransformRotation", [](FTransform& t, FRotator& r) { return UKismetMathLibrary::InverseTransformRotation(t, r); })
        .def_static("InverseTransformLocation", [](FTransform& t, FVector& l) { return UKismetMathLibrary::InverseTransformLocation(t, l); })
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
        .def_static("RandomUnitVectorInConeInDegrees", [](FVector& coneDir, float coneHalfAngle) { return UKismetMathLibrary::RandomUnitVectorInConeInDegrees(coneDir, coneHalfAngle); })
        .def_static("InRange_FloatFloat", [](float v, float min, float max, bool inclusiveMin, bool inclusiveMax) { return UKismetMathLibrary::InRange_FloatFloat(v, min, max, inclusiveMin, inclusiveMax); }, py::arg("v"), py::arg("min"), py::arg("max"), py::arg("inclusiveMin")=true, py::arg("inclusiveMax")=true)
        .def_static("Lerp", [](float a, float b, float alpha) { return UKismetMathLibrary::Lerp(a, b, alpha); })
        .def_static("RLerp", [](FRotator a, FRotator b, float alpha, bool shortestPath) { return UKismetMathLibrary::RLerp(a,b,alpha,shortestPath); })
        .def_static("VLerp", [](FVector a, FVector b, float alpha) { return UKismetMathLibrary::VLerp(a,b,alpha); })
        .def_static("VSize", [](FVector& a) { return UKismetMathLibrary::VSize(a); })
        .def_static("FInterpTo", [](float cur, float target, float deltaTime, float speed) { return UKismetMathLibrary::FInterpTo(cur, target, deltaTime, speed); })
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
        ;

    py::class_<UMaterialInterface, UObject, UnrealTracker<UMaterialInterface>>(m, "UMaterialInterface")
        .def_static("StaticClass", []() { return UMaterialInterface::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UMaterialInterface>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UMaterial, UMaterialInterface, UnrealTracker<UMaterial>>(m, "UMaterial")
        .def_static("StaticClass", []() { return UMaterial::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UMaterial>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UMaterialInstance, UMaterialInterface, UnrealTracker<UMaterialInstance>>(m, "UMaterialInstance")
        .def_static("StaticClass", []() { return UMaterialInstance::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UMaterialInstance>(obj); }, py::return_value_policy::reference)
        .def_readwrite("Parent", &UMaterialInstance::Parent)
        ;

    py::class_<UMaterialInstanceConstant, UMaterialInstance, UnrealTracker<UMaterialInstanceConstant>>(m, "UMaterialInstanceConstant")
        .def_static("StaticClass", []() { return UMaterialInstanceConstant::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UMaterialInstanceConstant>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UMaterialInstanceDynamic, UMaterialInstance, UnrealTracker<UMaterialInstanceDynamic>>(m, "UMaterialInstanceDynamic")
        .def_static("StaticClass", []() { return UMaterialInstanceDynamic::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UMaterialInstanceDynamic>(obj); }, py::return_value_policy::reference)
        .def("SetTextureParameterValue", [](UMaterialInstanceDynamic& self, std::string name, UTexture* value) -> void { self.SetTextureParameterValue(UTF8_TO_TCHAR(name.c_str()), value); })
        .def("SetScalarParameterValue", [](UMaterialInstanceDynamic& self, std::string name, float v) { self.SetScalarParameterValue(FSTR(name), v); })
        .def("GetScalarParameterValue", [](UMaterialInstanceDynamic& self, std::string name) { return self.K2_GetScalarParameterValue(FSTR(name)); })
        .def("SetVectorParameterValue", [](UMaterialInstanceDynamic& self, std::string name, FLinearColor& v) { self.SetVectorParameterValue(FSTR(name), v); })
        .def("GetVectorParameterValue", [](UMaterialInstanceDynamic& self, std::string name) { return self.K2_GetVectorParameterValue(FSTR(name)); })
        ;

    py::class_<UMaterialParameterCollection, UObject, UnrealTracker<UMaterialParameterCollection>>(m, "UMaterialParameterCollection")
        .def_static("StaticClass", []() { return UMaterialParameterCollection::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UMaterialParameterCollection>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UFXSystemAsset, UObject, UnrealTracker<UFXSystemAsset>>(m, "UFXSystemAsset")
        .def_static("StaticClass", []() { return UFXSystemAsset::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UFXSystemAsset>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UParticleSystem, UFXSystemAsset, UnrealTracker<UParticleSystem>>(m, "UParticleSystem")
        .def_static("StaticClass", []() { return UParticleSystem::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UParticleSystem>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UKismetMaterialLibrary, UObject, UnrealTracker<UKismetMaterialLibrary>>(m, "UKismetMaterialLibrary")
        .def_static("CreateDynamicMaterialInstance", [](UObject *worldCtx, UMaterialInterface *parent) { return UKismetMaterialLibrary::CreateDynamicMaterialInstance(worldCtx, parent); })
        .def_static("GetVectorParameterValue", [](UObject* worldCtx, UMaterialParameterCollection* coll, std::string name) { return UKismetMaterialLibrary::GetVectorParameterValue(worldCtx, coll, FSTR(name)); })
        ;

    py::class_<UTexture, UObject, UnrealTracker<UTexture>>(m, "UTexture")
        .def_static("StaticClass", []() { return UTexture::StaticClass(); })
        .def_static("Cast", [](UObject *w) { return Cast<UTexture>(w); }, py::return_value_policy::reference)
        ;

    py::class_<UTexture2D, UTexture, UnrealTracker<UTexture2D>>(m, "UTexture2D")
        .def_static("StaticClass", []() { return UTexture2D::StaticClass(); })
        .def_static("Cast", [](UObject *w) { return Cast<UTexture2D>(w); }, py::return_value_policy::reference)
        .def("GetSizeX", [](UTexture2D& self) { return self.GetSizeX(); })
        .def("GetSizeY", [](UTexture2D& self) { return self.GetSizeY(); })
        ;

    py::class_<UTextureRenderTarget, UTexture, UnrealTracker<UTextureRenderTarget>>(m, "UTextureRenderTarget")
        .def_static("StaticClass", []() { return UTextureRenderTarget::StaticClass(); })
        .def_static("Cast", [](UObject *w) { return Cast<UTextureRenderTarget>(w); }, py::return_value_policy::reference)
        .def_readwrite("TargetGamma", &UTextureRenderTarget::TargetGamma)
        ;

    py::class_<UTextureRenderTarget2D, UTextureRenderTarget, UnrealTracker<UTextureRenderTarget2D>>(m, "UTextureRenderTarget2D")
        .def_static("StaticClass", []() { return UTextureRenderTarget2D::StaticClass(); })
        .def_static("Cast", [](UObject *w) { return Cast<UTextureRenderTarget2D>(w); }, py::return_value_policy::reference)
        .def_readonly("SizeX", &UTextureRenderTarget2D::SizeX)
        .def_readonly("SizeY", &UTextureRenderTarget2D::SizeY)
        .def_readonly("ClearColor", &UTextureRenderTarget2D::ClearColor)
        ;

    py::class_<UCanvasRenderTarget2D, UTextureRenderTarget2D, UnrealTracker<UCanvasRenderTarget2D>>(m, "UCanvasRenderTarget2D")
        .def_static("StaticClass", []() { return UCanvasRenderTarget2D::StaticClass(); })
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
        })
        ;

    py::class_<UMediaTexture, UTexture, UnrealTracker<UMediaTexture>>(m, "UMediaTexture")
        .def_static("StaticClass", []() { return UMediaTexture::StaticClass(); })
        .def_static("Cast", [](UObject *w) { return Cast<UMediaTexture>(w); }, py::return_value_policy::reference)
        ;

    py::class_<UGameInstance, UObject, UnrealTracker<UGameInstance>>(m, "UGameInstance")
        .def_static("StaticClass", []() { return UGameInstance::StaticClass(); })
        .def_static("Cast", [](UObject *w) { return Cast<UGameInstance>(w); }, py::return_value_policy::reference)
        ;

    py::class_<AActor, UObject, UnrealTracker<AActor>>(m, "AActor")
        .def_static("StaticClass", []() { return AActor::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<AActor>(obj); }, py::return_value_policy::reference)
        .def("GetWorld", [](AActor& self) { return self.GetWorld(); }, py::return_value_policy::reference)
        .def("GetActorLocation", [](AActor& self) { return self.GetActorLocation(); })
        .def("SetActorLocation", [](AActor& self, FVector& v) { return self.SetActorLocation(v); })
        .def("GetActorRotation", [](AActor& self) { return self.GetActorRotation(); })
        .def("SetActorRotation", [](AActor& self, FRotator& r) { self.SetActorRotation(r); })
        .def("SetActorTransform", [](AActor& self, FTransform& t) { self.SetActorTransform(t); })
        .def("GetActorTransform", [](AActor& self) { return self.GetActorTransform(); })
        .def("GetTransform", [](AActor& self) { return self.GetTransform(); })
        .def("GetComponentsByClass", [](AActor& self, py::object& _klass)
        {
            py::list ret;
            for (auto comp : self.GetComponentsByClass(PyObjectToUClass(_klass)))
                ret.append(comp);
            return ret;
        }, py::return_value_policy::reference)
        .def("SetRootComponent", [](AActor& self, USceneComponent *s) { self.SetRootComponent(s); })
        .def("GetRootComponent", [](AActor& self) { return self.GetRootComponent(); })
        .def("SetActorScale3D", [](AActor& self, FVector& v) { self.SetActorScale3D(v); })
        .def("GetActorScale3D", [](AActor& self) { return self.GetActorScale3D(); })
        .def("Destroy", [](AActor& self) { self.Destroy(); })
        .def("IsActorTickEnabled", [](AActor& self) { return self.IsActorTickEnabled(); })
        .def("SetActorTickEnabled", [](AActor& self, bool enabled) { self.SetActorTickEnabled(enabled); })
        .def("SetActorTickInterval", [](AActor& self, float interval) { self.SetActorTickInterval(interval); })
        .def("GetActorTickInterval", [](AActor& self) { return self.GetActorTickInterval(); })
        .def("IsHidden", [](AActor& self) { return self.IsHidden(); })
        .def("SetActorHiddenInGame", [](AActor& self, bool b) { self.SetActorHiddenInGame(b); })
        .def("HasAuthority", [](AActor& self) { return self.HasAuthority(); })
        .def("GetOwner", [](AActor& self) { return self.GetOwner(); }, py::return_value_policy::reference)
        .def("BindOnEndPlay", [](AActor* self, py::object callback) { UEPY_BIND(self, OnEndPlay, AActor_OnEndPlay, callback); })
        .def("UnbindOnEndPlay", [](AActor* self, py::object callback) { UEPY_UNBIND(self, OnEndPlay, AActor_OnEndPlay, callback); })
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
        ;

    py::class_<AController, AActor, UnrealTracker<AController>>(m, "AController")
        .def_static("StaticClass", []() { return AController::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<AController>(obj); }, py::return_value_policy::reference)
        .def("GetPawn", [](AController& self) { return self.GetPawn(); })
        ;

    py::class_<APlayerController, AController, UnrealTracker<APlayerController>>(m, "APlayerController")
        .def_static("StaticClass", []() { return APlayerController::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<APlayerController>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<AGameStateBase, AActor, UnrealTracker<AGameStateBase>>(m, "AGameStateBase")
        .def_static("StaticClass", []() { return AGameStateBase::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<AGameStateBase>(obj); }, py::return_value_policy::reference)
        ;
    py::class_<AGameState, AGameStateBase, UnrealTracker<AGameState>>(m, "AGameState")
        .def_static("StaticClass", []() { return AGameState::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<AGameState>(obj); }, py::return_value_policy::reference)
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
        return UpdateTextureBGRA(tex, (uint8 *)bgra, width, height);
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
                    pyClass(engineObj); // the metaclass in uepy.__init__ requires engineObj to be passed as the first param; it gobbles it up and auto-sets self.engineObj on the new instance
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
                LERROR("Class %s created with invalid interface class %s", *name, *FSTR(py::repr(_interfaceClass).cast<std::string>()));
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
    });

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

    m.def("SpawnActor_", [](UWorld *world, py::object& _actorClass, FVector& location, FRotator& rotation)
    {
        UClass *actorClass = PyObjectToUClass(_actorClass);
        if (!actorClass)
            return (AActor*)nullptr;
        FActorSpawnParameters info;
        info.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        return world->SpawnActor(actorClass, &location, &rotation, info);
    }, py::arg("world"), py::arg("actorClass"), py::arg("location")=FVector(0,0,0), py::arg("rotation")=FRotator(0,0,0), py::return_value_policy::reference);

    m.def("NewObject", [](py::object& _class, UObject *owner, std::string& name)
    {
        UClass *klass = PyObjectToUClass(_class);
        if (!owner)
            owner = GetTransientPackage();
        FName instName = NAME_None;
        if (name.length())
            instName = FSTR(name);
        UObject *obj = NewObject<UObject>(owner, klass, instName);
        if (obj)
            obj->PostLoad(); // ? is this right ?
        return obj;
    }, py::return_value_policy::reference, py::arg("class"), py::arg("owner"), py::arg("name")="");

    py::class_<AActor_CGLUE, AActor, UnrealTracker<AActor_CGLUE>>(glueclasses, "AActor_CGLUE")
        .def_static("StaticClass", []() { return AActor_CGLUE::StaticClass(); }) // TODO: I think this can go away once we have the C++ APIs take PyClassOrUClass
        .def_static("Cast", [](UObject *obj) { return Cast<AActor_CGLUE>(obj); }, py::return_value_policy::reference)
        .def("SuperBeginPlay", [](AActor_CGLUE& self) { self.SuperBeginPlay(); })
        .def("SuperEndPlay", [](AActor_CGLUE& self, int reason) { self.SuperEndPlay((EEndPlayReason::Type)reason); })
        .def("SuperTick", [](AActor_CGLUE& self, float dt) { self.SuperTick(dt); })
        .def("UpdateTickSettings", [](AActor_CGLUE& self, bool canEverTick, bool startWithTickEnabled) { self.PrimaryActorTick.bCanEverTick = canEverTick; self.PrimaryActorTick.bStartWithTickEnabled = startWithTickEnabled; })
        ;

    py::class_<APawn, AActor, UnrealTracker<APawn>>(m, "APawn")
        .def_static("StaticClass", []() { return APawn::StaticClass(); })
        .def_static("Cast", [](UObject *w) { return Cast<APawn>(w); }, py::return_value_policy::reference)
        .def("IsLocallyControlled", [](APawn& self) { return self.IsLocallyControlled(); })
        ;

    py::class_<USoundClass, UObject, UnrealTracker<USoundClass>>(m, "USoundClass")
        .def_static("StaticClass", []() { return USoundClass::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<USoundClass>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UMediaPlayer, UObject, UnrealTracker<UMediaPlayer>>(m, "UMediaPlayer")
        .def_static("StaticClass", []() { return UMediaPlayer::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UMediaPlayer>(obj); }, py::return_value_policy::reference)
        .def("OpenSource", [](UMediaPlayer& self, UMediaSource* source) { return self.OpenSource(source); })
        .def("SetRate", [](UMediaPlayer& self, float rate) { return self.SetRate(rate); })
        .def("GetDuration", [](UMediaPlayer& self) { return self.GetDuration().GetTotalSeconds(); })
        .def("GetTime", [](UMediaPlayer& self) { return self.GetTime().GetTotalSeconds(); })
        .def("BindOnEndReached", [](UMediaPlayer* self, py::object callback) { UEPY_BIND(self, OnEndReached, On, callback); })
        .def("UnbindOnEndReached", [](UMediaPlayer* self, py::object callback) { UEPY_UNBIND(self, OnEndReached, On, callback); })
        .def("BindOnMediaOpenFailed", [](UMediaPlayer* self, py::object callback) { UEPY_UNBIND(self, OnMediaOpenFailed, UMediaPlayer_OnMediaOpenFailed, callback); })
        .def("UnbindOnMediaOpenFailed", [](UMediaPlayer* self, py::object callback) { UEPY_BIND(self, OnMediaOpenFailed, UMediaPlayer_OnMediaOpenFailed, callback); })
        ;

    py::class_<UMediaSource, UObject, UnrealTracker<UMediaSource>>(m, "UMediaSource")
        .def_static("StaticClass", []() { return UMediaSource::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UMediaSource>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UFileMediaSource, UMediaSource, UnrealTracker<UFileMediaSource>>(m, "UFileMediaSource") // TODO: actually it's UFileMediaSource<--UBaseMediaSource<--UMediaSource
        .def_static("StaticClass", []() { return UFileMediaSource::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UFileMediaSource>(obj); }, py::return_value_policy::reference)
        .def("SetFilePath", [](UFileMediaSource& self, py::str path) { std::string p = path; self.SetFilePath(p.c_str()); })
        ;

    py::class_<UAudioComponent, USceneComponent, UnrealTracker<UAudioComponent>>(m, "UAudioComponent")
        .def_static("StaticClass", []() { return UAudioComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UAudioComponent>(obj); }, py::return_value_policy::reference)
        .def_readwrite("VolumeMultiplier", &UAudioComponent::VolumeMultiplier)
        ;

    py::class_<USynthComponent, USceneComponent, UnrealTracker<USynthComponent>>(m, "USynthComponent")
        .def_static("StaticClass", []() { return USynthComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<USynthComponent>(obj); }, py::return_value_policy::reference)
        .def_readwrite("SoundClass", &USynthComponent::SoundClass)
        .def_property("bIsUISound", [](USynthComponent& self) { return self.bIsUISound; }, [](USynthComponent& self, bool b) { self.bIsUISound = b; }) // for some reason I couldn't bind this prop directly. Maybe because it's a uint8?
        .def_property("bAllowSpatialization", [](USynthComponent& self) { return self.bAllowSpatialization; }, [](USynthComponent& self, bool b) { self.bAllowSpatialization = b; }) // ditto
        ;

    py::class_<UMediaSoundComponent, USynthComponent, UnrealTracker<UMediaSoundComponent>>(m, "UMediaSoundComponent")
        .def_static("StaticClass", []() { return UMediaSoundComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UMediaSoundComponent>(obj); }, py::return_value_policy::reference)
        .def("SetMediaPlayer", [](UMediaSoundComponent& self, UMediaPlayer* player) { self.SetMediaPlayer(player); })
        .def("SetVolumeMultiplier", [](UMediaSoundComponent& self, float m) { self.SetVolumeMultiplier(m); })
        .def("GetAudioComponent", [](UMediaSoundComponent& self) { return self.GetAudioComponent(); })
        ;

    py::class_<ULightComponentBase, USceneComponent, UnrealTracker<ULightComponentBase>>(m, "ULightComponentBase")
        .def_static("StaticClass", []() { return ULightComponentBase::StaticClass(); })
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
        .def_static("StaticClass", []() { return ULightComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<ULightComponent>(obj); }, py::return_value_policy::reference)
        .def_property("bUseTemperature", [](ULightComponent& self) { return (bool)self.bUseTemperature; }, [](ULightComponent& self, bool b) { self.bUseTemperature=(int32)b; })
        .def("GetBoundingBox", [](ULightComponent& self) { return self.GetBoundingBox(); })
        .def("GetBoundingSphere", [](ULightComponent& self) { return self.GetBoundingSphere(); })
        .def("GetDirection", [](ULightComponent& self) { return self.GetDirection(); })
        .def("GetMaterial", [](ULightComponent& self, int index) { return self.GetMaterial(index); })
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
        .def_static("StaticClass", []() { return ULocalLightComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<ULocalLightComponent>(obj); }, py::return_value_policy::reference)
        .def("SetAttenuationRadius", [](ULocalLightComponent& self, float r) { self.SetAttenuationRadius(r); })
        .def_readonly("AttenuationRadius", &ULocalLightComponent::AttenuationRadius)
        .def("SetIntensityUnits", [](ULocalLightComponent& self, int u) { self.SetIntensityUnits((ELightUnits)u); })
        ;

    py::class_<UPointLightComponent, ULocalLightComponent, UnrealTracker<UPointLightComponent>>(m, "UPointLightComponent")
        .def_static("StaticClass", []() { return UPointLightComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UPointLightComponent>(obj); }, py::return_value_policy::reference)
        .def("SetLightFalloffExponent", [](UPointLightComponent& self, float f) { self.SetLightFalloffExponent(f); })
        .def("SetSourceRadius", [](UPointLightComponent& self, float f) { self.SetSourceRadius(f); })
        .def("SetSoftSourceRadius", [](UPointLightComponent& self, float f) { self.SetSoftSourceRadius(f); })
        .def("SetSourceLength", [](UPointLightComponent& self, float f) { self.SetSourceLength(f); })
        .def_property("bUseInverseSquaredFalloff", [](UPointLightComponent& self) { return (bool)self.bUseInverseSquaredFalloff; }, [](UPointLightComponent& self, bool b) { self.bUseInverseSquaredFalloff=(uint32)b; })
        ;

    py::class_<USpotLightComponent, UPointLightComponent, UnrealTracker<USpotLightComponent>>(m, "USpotLightComponent")
        .def_static("StaticClass", []() { return USpotLightComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<USpotLightComponent>(obj); }, py::return_value_policy::reference)
        .def("SetInnerConeAngle", [](USpotLightComponent& self, float f) { self.SetInnerConeAngle(f); })
        .def("SetOuterConeAngle", [](USpotLightComponent& self, float f) { self.SetOuterConeAngle(f); })
        ;

    py::class_<USceneCaptureComponent, USceneComponent, UnrealTracker<USceneCaptureComponent>>(m, "USceneCaptureComponent")
        .def_static("StaticClass", []() { return USceneCaptureComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<USceneCaptureComponent>(obj); }, py::return_value_policy::reference)
        .def_property("bCaptureEveryFrame", [](USceneCaptureComponent& self) { return self.bCaptureEveryFrame; }, [](USceneCaptureComponent& self, bool b) { self.bCaptureEveryFrame = b; })
        ENUM_PROP(CaptureSource, ESceneCaptureSource, USceneCaptureComponent)
        LIST_PROP(HiddenActors, AActor*, USceneCaptureComponent2D)
        ;

    py::class_<USceneCaptureComponent2D, USceneCaptureComponent, UnrealTracker<USceneCaptureComponent2D>>(m, "USceneCaptureComponent2D")
        .def_static("StaticClass", []() { return USceneCaptureComponent2D::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<USceneCaptureComponent2D>(obj); }, py::return_value_policy::reference)
        .def_readwrite("FOVAngle", &USceneCaptureComponent2D::FOVAngle)
        .def_readwrite("TextureTarget", &USceneCaptureComponent2D::TextureTarget)
        .def("CaptureScene", [](USceneCaptureComponent2D& self) { self.CaptureScene(); })
        ;

    py::class_<FSlateAtlasData>(m, "FSlateAtlasData")
        .def(py::init<UTexture*,FVector2D,FVector2D>())
        .def_readwrite("AtlasTexture", &FSlateAtlasData::AtlasTexture)
        .def_readwrite("StartUV", &FSlateAtlasData::StartUV)
        .def_readwrite("SizeUV", &FSlateAtlasData::SizeUV)
        .def("GetSourceDimensions", [](FSlateAtlasData& self) { return self.GetSourceDimensions(); })
        ;

    py::class_<UPaperSprite, UObject, UnrealTracker<UPaperSprite>>(m, "UPaperSprite")
        .def_static("StaticClass", []() { return UPaperSprite::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UPaperSprite>(obj); }, py::return_value_policy::reference)
        .def("GetBakedTexture", [](UPaperSprite& self) { return self.GetBakedTexture(); })
        .def("GetSlateAtlasData", [](UPaperSprite& self) { return self.GetSlateAtlasData(); })
        ;
}

//#pragma optimize("", on)
