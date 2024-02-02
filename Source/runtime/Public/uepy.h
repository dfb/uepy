// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "incpybind.h"
#include "IUEPYGlueMixin.h"
#include "Runtime/CoreUObject/Public/UObject/GCObject.h"
#include <functional>
#include "Components/BoxComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Components/WidgetInteractionComponent.h"
#include "Net/VoiceConfig.h"
#include "uepy.generated.h"

// pybind11 lets python exceptions bubble up to be C++ exceptions, which is rarely what we want, so
// instead do try { ... } catchpy
#define catchpy catch (py::error_already_set e) { LERROR("%s", UTF8_TO_TCHAR(e.what())); }

#define VALID(obj) (IsValid(obj) && obj->IsValidLowLevel())

// for CGLUE methods: evals to true if it appears ok for us to use pyInst
#define PYOK (IsValid(this) && !pyInst.is_none())

// std::string --> FString, sort of
#define FSTR(stdstr) UTF8_TO_TCHAR((stdstr).c_str())

// FString --> std::string
#define PYSTR(fstr) std::string(TCHAR_TO_UTF8(*fstr))

// used for log msgs
#define REPR(pyObj) UTF8_TO_TCHAR(py::repr(pyObj).cast<std::string>().c_str())

// helpers for declaring py::class_ properties
#define LIST_PROP(listName, listType, className)\
.def_property(#listName, [](className& self)\
{\
    py::list ret;\
    for (auto item : self.listName)\
        ret.append(item);\
    return ret;\
}, [](className& self, py::list& _items)\
{\
    TArray<listType> items;\
    for (py::handle item : _items)\
        items.Emplace(item.cast<listType>());\
    self.listName = items;\
}, py::return_value_policy::reference)

// bitprop is because UE4 declares lots of boolean properties like 'uint8 bWhatever : 1;'
#define BIT_PROP(propName, className)\
.def_property(#propName, [](className& self) { return (bool)self.propName; }, [](className& self, bool v) { self.propName = v; })

#define STR_PROP(propName, className)\
.def_property(#propName, [](className& self) { std::string s = TCHAR_TO_UTF8(*self.propName); return s; }, [](className& self, std::string& v) { self.propName = FSTR(v); })

#define FNAME_PROP(propName, className)\
.def_property(#propName, [](className& self) { std::string s = TCHAR_TO_UTF8(*self.propName.ToString()); return s; }, [](className& self, std::string& v) { self.propName = FSTR(v); })

#define ENUM_PROP(propName, propType, className)\
.def_property(#propName, [](className& self) { return (int)self.propName; }, [](className& self, int v) { self.propName = (propType)v; })

#define UEPY_EXPOSE_CLASS_EX(className, parentClassName, inModule, exposedName)\
    py::class_<className, parentClassName, UnrealTracker<className>>(inModule, #exposedName)\
        .def_static("StaticClass", []() { return className::StaticClass(); }, py::return_value_policy::reference)\
        .def_static("Cast", [](UObject *w) { return VALID(w) ? Cast<className>(w) : nullptr; }, py::return_value_policy::reference)\
        .def("__repr__", [](className* self) { py::str name = PYSTR(self->GetName()); return py::str("<{} {:X}>").format(name, (unsigned long long)self); })
#define UEPY_EXPOSE_CLASS(className, parentClassName, inModule) UEPY_EXPOSE_CLASS_EX(className, parentClassName, inModule, className)

// in order to be able pass from BP to Python any custom (game-specific) structs, the game has to provide a conversion function for them
typedef std::function<py::object(UScriptStruct* s, void *value)> BPToPyFunc;
UEPY_API void PyRegisterStructConverter(BPToPyFunc converterFunc);

// games can provide a main.py on disk or use the API to provide the code for a virtual one
UEPY_API void UEPYSetMainSource(const std::string& src);

class FToolBarBuilder;
class FMenuBuilder;

class FuepyModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool IsGameModule() const override { return true; }
};

UCLASS()
class UEPY_API UBackgroundWorker : public UObject
{
    // helper for running background tasks that call into Python once done (or multiple times). Subclasses should call
    // Setup and Cleanup at the beginning and end, respectively, of their work in the game thread. They should also
    // create a UPROPERTY delegate variable called TheEvent and broadcast it to call Python.
    GENERATED_BODY()

protected:
    py::object cb;
    void Setup(py::object& callback);
    void Cleanup();
};

// base class (and maybe the only class?) that acts as an intermediary when binding a python callback function to a
// multicast delegate on an engine object. Since the binding can't happen directly (obviously), we create an intermediate
// object that can bind to an engine object and then forwards the event to the python callback.
// TODO: an alternative approach would be to use lambdas instead of defining a new method, but I couldn't come up with
// anything that was simpler in the end, especially in terms of reuse.
UCLASS()
class UBasePythonDelegate : public UObject
{
    GENERATED_BODY()

public:
    bool valid; // becomes false once the python callback is GC'd
    py::object callbackOwner; // the object the callback is a method on. See note in ::Create for details.
    py::object callback;
    py::object cleanup;

    // these members are kept for debugging and so we can later unbind
    UObject *engineObj; // N.B. this is a pointer to a UObject just so we can test to compare addresses, but we don't maintain a ref to it
    uint32 engineObjIndex; // UObject.InternalIndex, so we can early detect corruption or auto-unbind
    FString mcDelName; // the name of the multicast delegate
    FString pyDelMethodName; // the name of one of our On* methods

    static UBasePythonDelegate *Create(UObject *engineObj, FString mcDelName, FString pyDelMethodName, py::object pyCB);
    bool Matches(UObject *engineObj, FString& mcDelName, FString& pyDelMethodName, py::object pyCB);

    UFunction *signatureFunction=nullptr; // used for BP events instead of one of the On functions below - we use this to get the signature
    virtual void ProcessEvent(UFunction *function, void *params) override;

    // Each different method signature for different multicast events needs a method here (or in a subclass I guess)
    // TODO: now that we have support for multicast delegates binding and firing via the UE4 reflection system, do we really need these one-off
    // events? They might be slightly more efficient (but maybe not), but is it enough to ever matter?

    UFUNCTION() void On(); // generic - also used for BP callbacks where we have to have a registered UFUNCTION that exists for the check right before it calls ProcessEvent
    UFUNCTION() void UComboBoxString_OnHandleSelectionChanged(FString Item, ESelectInfo::Type SelectionType);
    UFUNCTION() void UCheckBox_OnCheckStateChanged(bool checked);
    UFUNCTION() void AActor_OnEndPlay(AActor *actor, EEndPlayReason::Type reason);
    UFUNCTION() void UMediaPlayer_OnMediaOpenFailed(FString failedURL);
    UFUNCTION() void UInputComponent_OnAxis(float value);
    UFUNCTION() void UInputComponent_OnKeyAction(FKey key);
};

typedef TArray<UMaterialInterface*> MaterialArray;

// a singleton that taps into the engine's garbage collection system to keep some engine objects alive as long as they are
// being referenced from Python
class UEPY_API FPyObjectTracker : public FGCObject
{
    typedef struct Slot {
        // normally, an engine object shows up once in objectMap and there is a single corresponding shared wrapped Python instance. In some cases however
        // (such as calling a Cast() function from Python), we can end up with multiple Python instances for the same UObject, so we have to do reference
        // counting here
        int refs=0;
        uint32 objIndex=0; // UObject.InternalIndex, so we can early detect corruption
        UObject* obj; // this is a reference but not one the engine knows about (i.e. a raw pointer)
#if WITH_EDITOR
        uint64 objAddr; // for debugging
        FString objName; // for debugging - save it at the time of tracking so we can display it even if the obj gets force-GC'd by the engine out from under us
#endif
    } Slot;
    TMap<uint64,Slot> objectMap; // an engine object we're keeping alive because it's being referenced in Python --> how many references in Python there are
    TArray<UBasePythonDelegate*> delegates; // bound delegates we need to keep alive so they don't get cleaned up by the engine since nobody references them directly

public:
    FPyObjectTracker() {};

    // used for binding engine multicast delegates to python functions
    UBasePythonDelegate *CreateDelegate(UObject *engineObj, const char *mcDelName, const char *pyDelMethodName, py::object pyCB);
    UBasePythonDelegate *FindDelegate(UObject *engineObj, const char *mcDelName, const char *pyDelMethodName, py::object pyCB);
    void UnbindDelegatesOn(py::object& obj);

    static FPyObjectTracker *Get();
    virtual void AddReferencedObjects(FReferenceCollector& InCollector) override;
    uint64 Track(UObject *o);
    void Untrack(uint64 key);
    void IncRef(uint64 key);
    void Purge();

    // holds all mesh comps that are temporarily having their materials overridden - for each such comp, the key is the comp and the value
    // is a list of original materials, one per material slot. Lives here so that we can ensure referenced objs don't get GC'd.
    TMap<UMeshComponent*, MaterialArray> matOverrideMeshComps;
};

template <typename T> class UnrealTracker {
    T* ptr = nullptr;
    uint64 key = 0;
public:
    UnrealTracker(T *p) : ptr(p) { if (p) key = FPyObjectTracker::Get()->Track(p); }
    UnrealTracker(const UnrealTracker& ut)
    {
        ptr = ut.ptr;
        key = ut.key;
        if (key != 0) FPyObjectTracker::Get()->IncRef(key);
    }
    UnrealTracker(UnrealTracker&& mv) : ptr(mv.ptr), key(mv.key) { mv.ptr = nullptr; mv.key = 0; }
    ~UnrealTracker() { if (key != 0) FPyObjectTracker::Get()->Untrack(key); }

    UnrealTracker& operator=(const UnrealTracker& other)
    {
        if (this != &other)
        {
            auto tracker = FPyObjectTracker::Get();
            if (key != 0) tracker->Untrack(key);
            ptr = other.ptr;
            key = other.key;
            if (key != 0) tracker->IncRef(key);
        }
        return *this;
    }
    UnrealTracker& operator=(UnrealTracker&& other)
    {
        if (this != &other)
        {
            if (key != 0) FPyObjectTracker::Get()->Untrack(key);
            ptr = other.ptr;
            key = other.key;
            other.ptr = nullptr;
            other.key = 0;
        }
        return *this;
    }

    T& operator*() const { return *ptr; }
    T* get() const { return ptr; }
    T* operator->() const { return ptr; }
    bool operator==(const UnrealTracker& other) { return key == other.key; }
};

// Engine objects passed to Python have to be kept alive as long as Python is keeping a ref to them; we achieve this by connecting
// them to the root set during that time
PYBIND11_DECLARE_HOLDER_TYPE(T, UnrealTracker<T>, true);

namespace pybind11 {
    template <typename itype>
    struct polymorphic_type_hook<itype, detail::enable_if_t<std::is_base_of<UObject, itype>::value>>
    {
        static const void *get(const itype *src, const std::type_info*& type)
        {
            if (!src)
                type = nullptr;
            else
                type = &typeid(src->StaticClass());
            return src;
        }
    };
}

struct UEPY_API FUEPyDelegates
{
    DECLARE_MULTICAST_DELEGATE_OneParam(FPythonEvent1, py::module&);
    static FPythonEvent1 LaunchInit; // called during initial engine startup
};

// Generic glue classes for cases where you just want to subclass certain engine classes in Python directly
UCLASS()
class UEPY_API AActor_CGLUE : public AActor, public IUEPYGlueMixin
{
    GENERATED_BODY()

    AActor_CGLUE();

public:
    bool tickAllowed = true;
    void SuperBeginPlay();
    void SuperEndPlay(EEndPlayReason::Type reason);
    void SuperPostInitializeComponents() { Super::PostInitializeComponents(); }
    void SuperTick(float dt);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void GatherCurrentMovement() override;
    virtual void Tick(float dt) override;
    virtual void PostInitializeComponents() override;
};

UCLASS()
class UEPY_API APawn_CGLUE : public APawn, public IUEPYGlueMixin
{
    GENERATED_BODY()

    APawn_CGLUE();

public:
    bool tickAllowed = true;
    void SuperBeginPlay();
    void SuperEndPlay(EEndPlayReason::Type reason);
    void SuperPostInitializeComponents() { Super::PostInitializeComponents(); }
    void SuperTick(float dt);
    void SuperSetupPlayerInputComponent(UInputComponent* comp);
	//UPawnMovementComponent* SuperGetMovementComponent() const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void GatherCurrentMovement() override;
    virtual void Tick(float dt) override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
    virtual void PostInitializeComponents() override;
    virtual void PossessedBy(AController* NewController) override;
    virtual void UnPossessed() override;
	//virtual UPawnMovementComponent* GetMovementComponent() const override;
};

UCLASS()
class UEPY_API ACharacter_CGLUE : public ACharacter, public IUEPYGlueMixin
{
    GENERATED_BODY()

    ACharacter_CGLUE();

public:
    bool tickAllowed = true;
    void SuperBeginPlay();
    void SuperEndPlay(EEndPlayReason::Type reason);
    void SuperPostInitializeComponents() { Super::PostInitializeComponents(); }
    void SuperTick(float dt);
    void SuperSetupPlayerInputComponent(UInputComponent* comp);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void GatherCurrentMovement() override;
    virtual void Tick(float dt) override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
    virtual void PostInitializeComponents() override;
    virtual void PossessedBy(AController* NewController) override;
    virtual void UnPossessed() override;
};

UCLASS()
class UEPY_API USceneComponent_CGLUE : public USceneComponent, public IUEPYGlueMixin
{
    GENERATED_BODY()

    USceneComponent_CGLUE();

public:
    bool tickAllowed = true;
    void SuperBeginPlay() { Super::BeginPlay(); }
    void SuperEndPlay(EEndPlayReason::Type reason) { Super::EndPlay(reason); }
    void SuperOnRegister() { Super::OnRegister(); }
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void OnRegister() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
};

UCLASS()
class UEPY_API UBoxComponent_CGLUE : public UBoxComponent, public IUEPYGlueMixin
{
    GENERATED_BODY()

    UBoxComponent_CGLUE();

public:
    bool tickAllowed = true;
    void SuperBeginPlay() { Super::BeginPlay(); }
    void SuperEndPlay(EEndPlayReason::Type reason) { Super::EndPlay(reason); }
    void SuperOnRegister() { Super::OnRegister(); }
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void OnRegister() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
};

UCLASS()
class UEPY_API UPawnMovementComponent_CGLUE : public UPawnMovementComponent, public IUEPYGlueMixin
{
    GENERATED_BODY()

    UPawnMovementComponent_CGLUE();

public:
    bool tickAllowed = true;
    void SuperBeginPlay() { Super::BeginPlay(); }
    void SuperEndPlay(EEndPlayReason::Type reason) { Super::EndPlay(reason); }
    void SuperOnRegister() { Super::OnRegister(); }
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void OnRegister() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
};

UCLASS()
class UEPY_API  UVOIPTalker_CGLUE : public UVOIPTalker, public IUEPYGlueMixin
{
    GENERATED_BODY()

public:
    virtual void OnTalkingBegin(UAudioComponent* AudioComponent) override;
    virtual void OnTalkingEnd() override;
};

UCLASS()
class UEPY_API UCustomWidgetInteractionComponent : public UWidgetInteractionComponent
{
    GENERATED_BODY()

    UCustomWidgetInteractionComponent() : UWidgetInteractionComponent() {}

public:
	void ClearPointerKey(FKey Key)
    {
        if (PressedKeys.Contains(Key))
            ReleasePointerKey(Key);
    }
	virtual void PressPointerKey(FKey Key) override
    {
        // we never want a PressPointerKey call to be ignored, so this is a stop-gap to make it go through, but really
        // we should prevent this situation by always calling ReleasePointerKey at the right time or, if that's not
        // always possible, by calling ClearPointerKey instead.
        if (PressedKeys.Contains(Key))
            ReleasePointerKey(Key);
        Super::PressPointerKey(Key);
    }
};

// helper class for any API that accepts as an argument a UClass parameter. Allows the caller to pass in
// a UClass pointer, a C++ class that has been exposed via pybind11, or a Python class object that is a subclass
// of a glue class. In all cases, it finds the appropriate UClass object and returns it.
UEPY_API UClass *PyObjectToUClass(py::object& klassThing);

// stuff for integrating into the UE4 reflection system (e.g. calling BPs)
py::object GetObjectProperty(UObject *obj, std::string k);
void SetObjectProperty(UObject *obj, std::string k, py::object& value);
py::object CallObjectUFunction(UObject *obj, std::string funcName, py::tuple& args);
void BindDelegateCallback(UObject *obj, std::string eventName, py::object& callback);
void UnbindDelegateCallback(UObject *obj, std::string eventName, py::object& callback);
void BroadcastEvent(UObject* obj, std::string eventName, py::tuple& args);

void SetInternalSpawnArgs(py::dict& kwargs);
void ClearInternalSpawnArgs();

