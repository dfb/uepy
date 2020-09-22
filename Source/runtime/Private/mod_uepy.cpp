// sets up the main 'uepy' builtin module
#include "uepy.h"
#include "common.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameState.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "EngineUtils.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include <map>

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

    const TArray<uint8> *uncompressedData;
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

    // given an engine obj that you know to actually be a Python-implemented, get the associated Python instance from it (or None if you were wrong)
    m.def("PyInst", [](UObject *obj)
    {
        py::object ret = py::none();
        IUEPYGlueMixin *p = Cast<IUEPYGlueMixin>(obj);
        if (p)
            ret = p->pyInst;
        return ret;
    });

    py::class_<FPaths>(m, "FPaths")
        .def_static("ProjectDir", []() { return std::string(TCHAR_TO_UTF8(*FPaths::ProjectDir())); })
        .def_static("ProjectContentDir", []() { return std::string(TCHAR_TO_UTF8(*FPaths::ProjectContentDir())); })
        .def_static("ProjectPluginsDir", []() { return std::string(TCHAR_TO_UTF8(*FPaths::ProjectPluginsDir())); })
        ;

    py::class_<UObject, UnrealTracker<UObject>>(m, "UObject")
        .def_static("StaticClass", []() { return UObject::StaticClass(); })
        .def("GetClass", [](UObject& self) { return self.GetClass(); })
        .def("GetName", [](UObject& self) { std::string s = TCHAR_TO_UTF8(*self.GetName()); return s; })
        .def("IsA", [](UObject& self, py::object& _klass)
        {
            UClass *klass = PyObjectToUClass(_klass);
            return self.IsA(klass);
        })

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
        .def("ImplementsInterface", [](UClass& self, py::object interfaceClass)
        {
            UClass *k = PyObjectToUClass(interfaceClass);
            return k ? self.ImplementsInterface(k) : false;
        })
        ;

    py::class_<UBlueprintGeneratedClass, UClass, UnrealTracker<UBlueprintGeneratedClass>>(m, "UBlueprintGeneratedClass")
        ;

    py::class_<UInterface, UObject, UnrealTracker<UInterface>>(m, "UInterface")
        .def_static("StaticClass", []() { return UInterface::StaticClass(); })
        ;

    py::class_<UStaticMesh, UnrealTracker<UStaticMesh>>(m, "UStaticMesh")
        .def_static("StaticClass", []() { return UStaticMesh::StaticClass(); })
        ;

    py::class_<UActorComponent, UObject, UnrealTracker<UActorComponent>>(m, "UActorComponent")
        .def_static("StaticClass", []() { return UActorComponent::StaticClass(); })
        .def("SetActive", [](UActorComponent& self, bool a) { self.SetActive(a); })
        .def("IsRegistered", [](UActorComponent& self) { return self.IsRegistered(); })
        .def("RegisterComponent", [](UActorComponent& self) { self.RegisterComponent(); })
        .def("UnregisterComponent", [](UActorComponent& self) { self.UnregisterComponent(); })
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
        .def("DetachFromComponent", [](USceneComponent& self) { self.DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform); })
        ;

    py::class_<UPrimitiveComponent, USceneComponent, UnrealTracker<UPrimitiveComponent>>(m, "UPrimitiveComponent");
    py::class_<UMeshComponent, UPrimitiveComponent, UnrealTracker<UMeshComponent>>(m, "UMeshComponent")
        .def("SetCollisionObjectType", [](UMeshComponent& self, int c) { self.SetCollisionObjectType((ECollisionChannel)c); })
        ;

    py::class_<UStaticMeshComponent, UMeshComponent, UnrealTracker<UStaticMeshComponent>>(m, "UStaticMeshComponent")
        .def_static("StaticClass", []() { return UStaticMeshComponent::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UStaticMeshComponent>(obj); }, py::return_value_policy::reference)
        .def("SetStaticMesh", [](UStaticMeshComponent& self, UStaticMesh *newMesh) -> bool { return self.SetStaticMesh(newMesh); })
        .def("SetMaterial", [](UStaticMeshComponent& self, int index, UMaterialInterface *newMat) -> void { self.SetMaterial(index, newMat); }) // technically, UMaterialInterface
        ;

    py::class_<UWorld, UnrealTracker<UWorld>>(m, "UWorld")
        .def_property_readonly("WorldType", [](UWorld& self) { return (int)self.WorldType; })
        .def("GetAllActors", [](UWorld* self)
        {
            py::list ret;
            for (TActorIterator<AActor> it(self); it ; ++it)
                ret.append(*it);
            return ret;
        })
        ;

    m.def("GetAllWorlds", []()
    {
        py::list ret;
        for (TObjectIterator<UWorld> iter; iter; ++iter)
            ret.append(*iter);
        return ret;
    });

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
            })
        ;

    py::class_<UKismetMathLibrary, UObject, UnrealTracker<UKismetMathLibrary>>(m, "UKismetMathLibrary")
        .def_static("TEase", [](FTransform& a, FTransform& b, float alpha, int easingFunc, float blend, int steps) { return UKismetMathLibrary::TEase(a, b, alpha, (EEasingFunc::Type)easingFunc, blend, steps); })
        .def_static("VEase", [](FVector& a, FVector& b, float alpha, int easingFunc, float blend, int steps) { return UKismetMathLibrary::VEase(a, b, alpha, (EEasingFunc::Type)easingFunc, blend, steps); })
        .def_static("REase", [](FRotator& a, FRotator& b, float alpha, bool shortestPath, int easingFunc, float blend, int steps) { return UKismetMathLibrary::REase(a, b, alpha, shortestPath, (EEasingFunc::Type)easingFunc, blend, steps); })
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

    py::class_<UGameInstance, UObject, UnrealTracker<UGameInstance>>(m, "UGameInstance")
        .def_static("StaticClass", []() { return UGameInstance::StaticClass(); })
        .def_static("Cast", [](UObject *w) { return Cast<UGameInstance>(w); }, py::return_value_policy::reference)
        ;

    py::class_<AActor, UObject, UnrealTracker<AActor>>(m, "AActor")
        .def_static("StaticClass", []() { return AActor::StaticClass(); })
        .def("GetWorld", [](AActor& self) { return self.GetWorld(); }, py::return_value_policy::reference)
        .def("GetActorLocation", [](AActor& self) { return self.GetActorLocation(); })
        .def("SetActorLocation", [](AActor& self, FVector& v) { return self.SetActorLocation(v); })
        .def("GetActorRotation", [](AActor& self) { return self.GetActorRotation(); })
        .def("SetActorRotation", [](AActor& self, FRotator& r) { self.SetActorRotation(r); })
        .def("SetActorTransform", [](AActor& self, FTransform& t) { self.SetActorTransform(t); })
        .def("GetActorTransform", [](AActor& self) { return self.GetActorTransform(); })
        .def("SetRootComponent", [](AActor& self, USceneComponent *s) { self.SetRootComponent(s); })
        .def("GetRootComponent", [](AActor& self) { return self.GetRootComponent(); })
        .def("SetActorScale3D", [](AActor& self, FVector& v) { self.SetActorScale3D(v); })
        .def("GetActorScale3D", [](AActor& self) { return self.GetActorScale3D(); })
        .def("Destroy", [](AActor& self) { self.Destroy(); })
        .def("IsActorTickEnabled", [](AActor& self) { return self.IsActorTickEnabled(); })
        .def("SetActorTickEnabled", [](AActor& self, bool enabled) { self.SetActorTickEnabled(enabled); })
        .def("SetActorTickInterval", [](AActor& self, float interval) { self.SetActorTickInterval(interval); })
        .def("GetActorTickInterval", [](AActor& self) { return self.GetActorTickInterval(); })
        .def("SetActorHiddenInGame", [](AActor& self, bool b) { self.SetActorHiddenInGame(b); })
        .def("BindOnEndPlay", [](AActor* self, py::object callback) { UEPY_BIND(self, OnEndPlay, AActor_OnEndPlay, callback); })
        .def("UnbindOnEndPlay", [](AActor* self, py::object callback) { UEPY_UNBIND(self, OnEndPlay, AActor_OnEndPlay, callback); })
        .def_property("Tags", [](AActor& self)
            {
                py::list ret;
                for (FName& tag : self.Tags)
                {
                    std::string stag = TCHAR_TO_UTF8(*tag.ToString());
                    ret.append(stag);
                }
                return ret;
            },
            [](AActor& self, py::list pytags)
            {
                self.Tags.Empty();
                for (const py::handle pytag : pytags)
                    self.Tags.Emplace(pytag.cast<std::string>().c_str());
            })

        // methods for getting/setting UPROPERTYs - TODO: maybe move this to UObject so that anything with a UPROPERTY can be accessed
        .def("Set", [](AActor* self, std::string k, py::object& value) { SetObjectUProperty(self, k, value); })
        ;

    py::class_<AGameStateBase, AActor, UnrealTracker<AGameStateBase>>(m, "AGameStateBase")
        ;
    py::class_<AGameState, AGameStateBase, UnrealTracker<AGameState>>(m, "AGameState")
        ;

    py::class_<FVector2D>(m, "FVector2D")
        .def(py::init<float,float>(), "x"_a=0.0f, "y"_a=0.0f)
        .def_readwrite("x", &FVector2D::X)
        .def_readwrite("X", &FVector2D::X)
        .def_readwrite("y", &FVector2D::Y)
        .def_readwrite("Y", &FVector2D::Y)
        .def(py::self + py::self)
        .def(-py::self)
        .def(float() * py::self)
        .def(py::self * float())
        .def("AsList", [](FVector2D& self) // lame hackery til I figure out a clean way to implement __iter__
        {
            py::list ret;
            ret.append(self.X);
            ret.append(self.Y);
            return ret;
        })
        ;

    py::class_<FVector>(m, "FVector")
        .def(py::init<float,float,float>(), "x"_a=0.0f, "y"_a=0.0f, "z"_a=0.0f)
        .def_readwrite("x", &FVector::X)
        .def_readwrite("X", &FVector::X)
        .def_readwrite("y", &FVector::Y)
        .def_readwrite("Y", &FVector::Y)
        .def_readwrite("z", &FVector::Z)
        .def_readwrite("Z", &FVector::Z)
        .def(py::self + py::self)
        .def(-py::self)
        .def(float() * py::self)
        .def(py::self * float())
        .def("AsList", [](FVector& self)
        {
            py::list ret;
            ret.append(self.X);
            ret.append(self.Y);
            ret.append(self.Z);
            return ret;
        })
        ;

    py::class_<FRotator>(m, "FRotator")
        .def(py::init([](float roll=0, float pitch=0, float yaw=0) { FRotator r; r.Roll=roll; r.Pitch=pitch; r.Yaw=yaw; return r; })) //<float, float, float>(), "roll"_a=0.0f, "pitch"_a=0.0f, "yaw"_a=0.0f) // weird order, but matches UnrealEnginePython
        .def_readwrite("roll", &FRotator::Roll)
        .def_readwrite("Roll", &FRotator::Roll)
        .def_readwrite("pitch", &FRotator::Pitch)
        .def_readwrite("Pitch", &FRotator::Pitch)
        .def_readwrite("yaw", &FRotator::Yaw)
        .def_readwrite("Yaw", &FRotator::Yaw)
        .def("RotateVector", [](FRotator& self, FVector& v) { return self.RotateVector(v); })
        .def("UnrotateVector", [](FRotator& self, FVector& v) { return self.UnrotateVector(v); })
        .def("AsList", [](FRotator& self)
        {
            py::list ret;
            ret.append(self.Roll);
            ret.append(self.Pitch);
            ret.append(self.Yaw);
            return ret;
        })
        ;

    py::class_<FTransform>(m, "FTransform")
        .def(py::init<>())
        .def("Rotator", [](FTransform& self) { return self.Rotator(); })
        .def("GetRotation", [](FTransform& self) { return self.Rotator(); })
        .def("SetRotation", [](FTransform& self, FRotator& r) { FQuat q(r); self.SetRotation(q); })
        .def("GetTranslation", [](FTransform& self) { return self.GetTranslation(); })
        .def("GetLocation", [](FTransform& self) { return self.GetLocation(); })
        .def("SetTranslation", [](FTransform& self, FVector& t) { self.SetTranslation(t); })
        .def("SetLocation", [](FTransform& self, FVector& t) { self.SetLocation(t); })
        .def("GetScale3D", [](FTransform& self) { return self.GetScale3D(); })
        .def("SetScale3D", [](FTransform& self, FVector& v) { self.SetScale3D(v); })
        ;

    py::class_<FLinearColor>(m, "FLinearColor")
        .def(py::init<float, float, float, float>(), "r"_a=0.0f, "g"_a=0.0f, "b"_a=0.0f, "a"_a=0.0f)
        .def_readwrite("R", &FLinearColor::R)
        .def_readwrite("r", &FLinearColor::R)
        .def_readwrite("G", &FLinearColor::G)
        .def_readwrite("g", &FLinearColor::G)
        .def_readwrite("B", &FLinearColor::B)
        .def_readwrite("b", &FLinearColor::B)
        .def_readwrite("A", &FLinearColor::A)
        .def_readwrite("a", &FLinearColor::A)
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

    m.def("RegisterPythonSubclass", [](py::str fqClassName, UClass *engineParentClass, const py::object pyClass) -> UClass*
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
            if (!engineObj->HasAnyFlags(RF_ClassDefaultObject)) // during CDO creation, we don't want to create a pyinst
            {
                try {
                    IUEPYGlueMixin *p = Cast<IUEPYGlueMixin>(engineObj);
                    FString className = engineObj->GetClass()->GetName();
                    py::object& pyClass = pyClassMap[className];
                    p->pyInst = pyClass(engineObj); // the metaclass in uepy.__init__ requires engineObj to be passed as the first param; it gobbles it up and auto-sets self.engineObj on the new instance
                }
                catchpy;
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

    m.def("StaticLoadObject", [](py::object& _typeClass, std::string refPath) {
        UClass *typeClass = PyObjectToUClass(_typeClass);
		UObject *obj = StaticLoadObject(typeClass, NULL, UTF8_TO_TCHAR(refPath.c_str()));
		return Cast<UClass>(obj);
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

    m.def("NewObject", [](py::object& _class, UObject *owner)
    {
        UClass *klass = PyObjectToUClass(_class);
        if (!owner)
            owner = GetTransientPackage();
        UObject *obj = NewObject<UObject>(owner, klass);
        if (obj)
            obj->PostLoad(); // ? is this right ?
        return obj;
	}, py::return_value_policy::reference);

    py::class_<AActor_CGLUE, AActor, UnrealTracker<AActor_CGLUE>>(glueclasses, "AActor_CGLUE")
        .def_static("StaticClass", []() { return AActor_CGLUE::StaticClass(); }) // TODO: I think this can go away once we have the C++ APIs take PyClassOrUClass
        .def_static("Cast", [](UObject *obj) { return Cast<AActor_CGLUE>(obj); }, py::return_value_policy::reference)
        .def("SuperBeginPlay", [](AActor_CGLUE& self) { self.SuperBeginPlay(); })
        .def("SuperTick", [](AActor_CGLUE& self, float dt) { self.SuperTick(dt); })
        ;

	py::class_<USoundClass, UObject, UnrealTracker<USoundClass>>(m, "USoundClass")
		.def_static("StaticClass", []() { return USoundClass::StaticClass(); })
		.def_static("Cast", [](UObject *obj) { return Cast<USoundClass>(obj); }, py::return_value_policy::reference)
		;

	py::class_<UMediaPlayer, UObject, UnrealTracker<UMediaPlayer>>(m, "UMediaPlayer")
		.def_static("StaticClass", []() { return UMediaPlayer::StaticClass(); })
		.def_static("Cast", [](UObject *obj) { return Cast<UMediaPlayer>(obj); }, py::return_value_policy::reference)
		// TODO: bind/unbind OnEndReached, OnMediaOpenFailed
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
		.def_readwrite("SoundClass", &USynthComponent::SoundClass)
		.def_property("bIsUISound", [](USynthComponent& self) { return self.bIsUISound; }, [](USynthComponent& self, bool b) { self.bIsUISound = b; }) // for some reason I couldn't bind this prop directly. Maybe because it's a uint8?
		.def_property("bAllowSpatialization", [](USynthComponent& self) { return self.bAllowSpatialization; }, [](USynthComponent& self, bool b) { self.bAllowSpatialization = b; }) // ditto
		.def_static("StaticClass", []() { return USynthComponent::StaticClass(); })
		.def_static("Cast", [](UObject *obj) { return Cast<USynthComponent>(obj); }, py::return_value_policy::reference)
        ;

	py::class_<UMediaSoundComponent, USynthComponent, UnrealTracker<UMediaSoundComponent>>(m, "UMediaSoundComponent")
		.def_static("StaticClass", []() { return UMediaSoundComponent::StaticClass(); })
		.def_static("Cast", [](UObject *obj) { return Cast<UMediaSoundComponent>(obj); }, py::return_value_policy::reference)
		.def("SetMediaPlayer", [](UMediaSoundComponent& self, UMediaPlayer* player) { self.SetMediaPlayer(player); })
		.def("SetVolumeMultiplier", [](UMediaSoundComponent& self, float m) { self.SetVolumeMultiplier(m); })
		.def("GetAudioComponent", [](UMediaSoundComponent& self) { return self.GetAudioComponent(); })
		;
}

