// sets up the main 'uepy' builtin module
#include "uepy.h"
#include "common.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameState.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include <map>
#include "PythonClass.h"

static std::map<FString, py::object> pyClassMap; // class name --> python class

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

    const TArray<uint8>* uncompressedData = nullptr;
    if (imageWrapper->GetRaw(ERGBFormat::BGRA, 8, uncompressedData))
    {
        UTexture2D *tex = UTexture2D::CreateTransient(imageWrapper->GetWidth(), imageWrapper->GetHeight(), PF_B8G8R8A8);
        if (tex)
        {
            void *textureData = tex->PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
            FMemory::Memcpy(textureData, uncompressedData->GetData(), uncompressedData->Num());
            tex->PlatformData->Mips[0].BulkData.Unlock();
            tex->UpdateResource();
            return tex;
        }
    }
    LWARN("Failed to read image data for %s", *path);
    return nullptr;
}

using namespace pybind11::literals;

// this module is automagically loaded by virtual of the global declaration and the use of the embedded module macro
// other builtin modules get added via FUEPythonDelegates::LaunchInit
PYBIND11_EMBEDDED_MODULE(uepy, m) {

#if WITH_EDITOR
    m.attr("WITH_EDITOR") = true;
#else
    m.attr("WITH_EDITOR") = false;
#endif

    py::class_<UObject, UnrealTracker<UObject>>(m, "UObject")
        .def_static("StaticClass", []() { return UObject::StaticClass(); }) // TODO: we shouldn't need to do this for every class we expose
        // TODO: for APIs that take a UClass, have a helper that calls StaticClass if needed to get from py class to UClass

        // TODO: we could create a generic CreateDefaultSubobject utility func that takes a UClass of what to create
        // and just return it as a UObject, though the caller would then need to also cast it, e.g.
        // self.foo = uepy.AsUStaticMeshComponent(self.CreateDefaultSubObject(uepy.UStaticMeshComponent, 'mymesh'))
        // which seems like a lot of typing, so for now I'm creating class-specific versions.
        .def("CreateUStaticMeshComponent", [](UObject& self, py::str name)
        {
            std::string sname = name;
            return self.CreateDefaultSubobject<UStaticMeshComponent>(UTF8_TO_TCHAR(sname.c_str()));
        })
        ;
    py::class_<UClass, UObject, UnrealTracker<UClass>>(m, "UClass") // TODO: UClass --> UStruct --> UField --> UObject
        .def("ImplementsInterface", [](UClass& self, UClass *interfaceClass) { return self.ImplementsInterface(interfaceClass); })
        ;

    py::class_<UInterface, UObject, UnrealTracker<UInterface>>(m, "UInterface")
        .def_static("StaticClass", []() { return UInterface::StaticClass(); })
        ;

    py::class_<UStaticMesh, UnrealTracker<UStaticMesh>>(m, "UStaticMesh")
        .def_static("StaticClass", []() { return UStaticMesh::StaticClass(); })
        ;

    py::class_<UActorComponent, UObject, UnrealTracker<UActorComponent>>(m, "UActorComponent")
        .def_static("StaticClass", []() { return UActorComponent::StaticClass(); })
        ;

    py::class_<USceneComponent, UActorComponent, UnrealTracker<USceneComponent>>(m, "USceneComponent")
        .def_static("StaticClass", []() { return USceneComponent::StaticClass(); })
        .def("GetRelativeLocation", [](USceneComponent& self) { return self.RelativeLocation; }) // todo: GetRelativeLocation is added in 4.25
        .def("SetRelativeLocation", [](USceneComponent& self, FVector v) { self.SetRelativeLocation(v); })
        .def("GetRelativeRotation", [](USceneComponent& self) { return self.RelativeRotation; })
        .def("SetRelativeRotation", [](USceneComponent& self, FRotator r) { self.SetRelativeRotation(r); })
        .def("GetRelativeScale3D", [](USceneComponent& self) { return self.RelativeScale3D; })
        .def("SetRelativeScale3D", [](USceneComponent& self, FVector v) { self.SetRelativeScale3D(v); })
        .def("AttachToComponent", [](USceneComponent& self, USceneComponent *parent) { return self.AttachToComponent(parent, FAttachmentTransformRules::KeepRelativeTransform); }) // TODO: AttachmentRules, socket
        ;
    py::class_<UPrimitiveComponent, USceneComponent, UnrealTracker<UPrimitiveComponent>>(m, "UPrimitiveComponent");
    py::class_<UMeshComponent, UPrimitiveComponent, UnrealTracker<UMeshComponent>>(m, "UMeshComponent");
    py::class_<UStaticMeshComponent, UMeshComponent, UnrealTracker<UStaticMeshComponent>>(m, "UStaticMeshComponent")
        .def_static("StaticClass", []() { return UStaticMeshComponent::StaticClass(); })
        .def("SetStaticMesh", [](UStaticMeshComponent& self, UStaticMesh *newMesh) -> bool { return self.SetStaticMesh(newMesh); })
        .def("SetMaterial", [](UStaticMeshComponent& self, int index, UMaterialInterface *newMat) -> void { self.SetMaterial(index, newMat); }) // technically, UMaterialInterface
        ;

    // TODO: I guess we do one of these for every exposed class?
    m.def("AsUStaticMeshComponent", [](UObject *engineObj) -> UStaticMeshComponent* { return Cast<UStaticMeshComponent>(engineObj); }, py::return_value_policy::reference);

	py::class_<UWorld, UnrealTracker<UWorld>>(m, "UWorld")
		;

    py::class_<UMaterialInterface, UnrealTracker<UMaterialInterface>>(m, "UMaterialInterface");

	py::class_<UMaterial, UMaterialInterface, UnrealTracker<UMaterial>>(m, "UMaterial")
		;

	py::class_<UMaterialInstance, UMaterialInterface, UnrealTracker<UMaterialInstance>>(m, "UMaterialInstance");
	py::class_<UMaterialInstanceDynamic, UMaterialInstance, UnrealTracker<UMaterialInstanceDynamic>>(m, "UMaterialInstanceDynamic")
        .def("SetTextureParameterValue", [](UMaterialInstanceDynamic& self, std::string paramName, UTexture2D* value) -> void { self.SetTextureParameterValue(UTF8_TO_TCHAR(paramName.c_str()), value); })
		;

	py::class_<UTexture2D, UnrealTracker<UTexture2D>>(m, "UTexture2D")
		;

    py::class_<UGameInstance, UnrealTracker<UGameInstance>>(m, "UGameInstance")
        ;

    py::class_<AGameState, UnrealTracker<AGameState>>(m, "AGameState")
        ;

    py::class_<AActor, UObject, UnrealTracker<AActor>>(m, "AActor")
        .def_static("StaticClass", []() { return AActor::StaticClass(); })
        .def("GetWorld", [](AActor& self) { return self.GetWorld(); }, py::return_value_policy::reference)
		.def("GetActorLocation", [](AActor& self) { return self.GetActorLocation(); })
		.def("SetActorLocation", [](AActor& self, FVector v) { return self.SetActorLocation(v); })
        .def("GetActorRotation", [](AActor& self) { return self.GetActorRotation(); })
        .def("SetActorRotation", [](AActor& self, FRotator r) { self.SetActorRotation(r); })
        .def("SetRootComponent", [](AActor& self, USceneComponent *s) { self.SetRootComponent(s); })
        .def("GetRootComponent", [](AActor&self) { return self.GetRootComponent(); })
        ;

    py::class_<FVector>(m, "FVector")
        .def(py::init<float,float,float>(), "x"_a=0.0f, "y"_a=0.0f, "z"_a=0.0f)
        .def_readwrite("x", &FVector::X)
        .def_readwrite("X", &FVector::X)
        .def_readwrite("y", &FVector::Y)
        .def_readwrite("Y", &FVector::Y)
        .def_readwrite("z", &FVector::Z)
        .def_readwrite("Z", &FVector::Z)
        ;

    py::class_<FRotator>(m, "FRotator")
        .def(py::init<float, float, float>(), "pitch"_a=0.0f, "yaw"_a=0.0f, "roll"_a=0.0f) // weird order, but matches UE4
        .def_readwrite("roll", &FRotator::Roll)
        .def_readwrite("Roll", &FRotator::Roll)
        .def_readwrite("pitch", &FRotator::Pitch)
        .def_readwrite("Pitch", &FRotator::Pitch)
        .def_readwrite("yaw", &FRotator::Yaw)
        .def_readwrite("Yaw", &FRotator::Yaw)
        ;

    py::class_<FMargin>(m, "FMargin")
        .def(py::init<float,float,float,float>(), "left"_a=0.0f, "top"_a=0.0f, "right"_a=0.0f, "bottom"_a=0.0f)
        .def_readwrite("left", &FMargin::Left)
        .def_readwrite("Left", &FMargin::Left)
        .def_readwrite("top", &FMargin::Top)
        .def_readwrite("Top", &FMargin::Top)
        .def_readwrite("right", &FMargin::Right)
        .def_readwrite("Right", &FMargin::Right)
        .def_readwrite("bottom", &FMargin::Bottom)
        .def_readwrite("Bottom", &FMargin::Bottom)
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

    m.def("CreateDynamicMaterialInstance", [](UObject *owner, UMaterialInterface *parent) -> UMaterialInstanceDynamic*
    {
        return UMaterialInstanceDynamic::Create(parent, owner);
    }, py::return_value_policy::reference);

    m.def("RegisterPythonSubclass", [](py::str fqClassName, py::str engineParentClassPath, const py::object pyClass) -> UClass*
    {
        UClass *engineParentClass = FindObject<UClass>(ANY_PACKAGE, UTF8_TO_TCHAR(((std::string)engineParentClassPath).c_str()));
        if (!engineParentClass->ImplementsInterface(UPyBridgeMixin::StaticClass()))
        {
            LERROR("Class does not implement IPyBridgeMixin");
            return nullptr;
        }

        std::string sname = fqClassName;
        FString name(UTF8_TO_TCHAR(sname.c_str()));
        pyClassMap[name] = pyClass; // GRR: saving the class to a map because I can't get the lambda below to work with captured arguments

        UClass *engineClass = FindObject<UClass>(ANY_PACKAGE, *name);
        if (!engineClass)
            engineClass = NewObject<UClass>(engineParentClass->GetOuter(), *name, RF_Public | RF_Transient | RF_MarkAsNative);

        engineClass->ClassAddReferencedObjects = engineParentClass->ClassAddReferencedObjects;
        engineClass->SetSuperStruct(engineParentClass);
        LOG("FULL PATH: %s", *engineClass->GetPathName());
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
            if (!engineObj->HasAnyFlags(RF_ClassDefaultObject))
            {
                try {
                    IPyBridgeMixin *p = Cast<IPyBridgeMixin>(engineObj);
                    FString className = engineObj->GetClass()->GetName();
                    py::object& pyClass = pyClassMap[className];
                    p->pyInst = pyClass(engineObj);
				}
				catch (std::exception e)
				{
					LOG("EXCEPTION %s", UTF8_TO_TCHAR(e.what()));
				}
            }
        };

        for (FImplementedInterface& info : engineParentClass->Interfaces)
            engineClass->Interfaces.Add(info);

        engineClass->ClearFunctionMapsCaches();
        engineClass->Bind();
        engineClass->StaticLink(true);
        engineClass->AssembleReferenceTokenStream();
        engineClass->GetDefaultObject();
        return engineClass;
    });

    m.def("SpawnActor", [](UWorld *world, UClass *actorClass, FVector& location, FRotator& rotation)
    {
        FActorSpawnParameters info;
        info.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        return world->SpawnActor(actorClass, &location, &rotation, info);
    }, py::arg("world"), py::arg("actorClass"), py::arg("location")=FVector(0,0,0), py::arg("rotation")=FRotator(0,0,0), py::return_value_policy::reference);
}

