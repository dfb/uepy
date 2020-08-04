#include "py_module.h"
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

PYBIND11_EMBEDDED_MODULE(uepy, m) {
    py::class_<UObject, UnrealTracker<UObject>>(m, "UObject")
        ;

    py::class_<UStaticMesh, UnrealTracker<UStaticMesh>>(m, "UStaticMesh")
        ;

    py::class_<UStaticMeshComponent, UnrealTracker<UStaticMeshComponent>>(m, "UStaticMeshComponent")
        .def("SetStaticMesh", [](UStaticMeshComponent& self, UStaticMesh *newMesh) -> bool { return self.SetStaticMesh(newMesh); })
        .def("SetMaterial", [](UStaticMeshComponent& self, int index, UMaterialInterface *newMat) -> void { self.SetMaterial(index, newMat); }) // technically, UMaterialInterface
        ;

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
        .def("GetWorld", [](AActor& self) -> UWorld* { return self.GetWorld(); }, py::return_value_policy::reference)
		.def("SetActorLocation", [](AActor& self, FVector v) -> bool { return self.SetActorLocation(v); })
        .def("SetActorRotation", [](AActor& self, FRotator r) -> void { self.SetActorRotation(r); })
        ;

    py::class_<FVector>(m, "FVector")
        .def(py::init<float,float,float>())
        .def_readwrite("x", &FVector::X)
        .def_readwrite("y", &FVector::Y)
        .def_readwrite("z", &FVector::Z)
        ;

    py::class_<FRotator>(m, "FRotator")
        .def(py::init<float, float, float>())
        .def_readwrite("roll", &FRotator::Roll)
        .def_readwrite("pitch", &FRotator::Pitch)
        .def_readwrite("yaw", &FRotator::Yaw)
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

    m.def("RegisterPythonSubclass", [](py::str fqClassName, py::str engineParentClassPath, const py::object pyClass)
    {
        UClass *engineParentClass = FindObject<UClass>(ANY_PACKAGE, UTF8_TO_TCHAR(((std::string)engineParentClassPath).c_str()));
        if (!engineParentClass->ImplementsInterface(UPyBridgeMixin::StaticClass()))
        {
            LERROR("Class does not implement IPyBridgeMixin");
            return;
        }

        // TODO: allow re-creation of class instead of always new'ing it
        std::string sname = fqClassName;
        FString name(UTF8_TO_TCHAR(sname.c_str()));
        pyClassMap[name] = pyClass; // GRR: saving the class to a map because I can't get the lambda below to work with captured arguments
        UClass *engineClass = NewObject<UClass>(engineParentClass->GetOuter(), *name, RF_Public | RF_Transient | RF_MarkAsNative);
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

        engineClass->ClearFunctionMapsCaches();
        engineClass->Bind();
        engineClass->StaticLink(true);
        engineClass->GetDefaultObject();
    });
}

void LoadPyModule()
{
    py::initialize_interpreter(); // we delay this call so that game modules have a chance to create their embedded py modules
    LOG("Loading engine_startup.py");
    try {
        py::module m = py::module::import("uepy");

        // add the Content/Scripts dir to sys.path so it can find engine_startup.py
        FString scriptsDir = FPaths::Combine(*FPaths::ProjectContentDir(), _T("Scripts"));
        py::module sys = py::module::import("sys");
        sys.attr("path").attr("append")(*scriptsDir);

        py::module startup = py::module::import("engine_startup");
        //startup.reload();
		startup.attr("Init")();
	} catch (std::exception e)
    {
        LOG("EXCEPTION %s", UTF8_TO_TCHAR(e.what()));
    }
}

