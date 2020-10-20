// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

#pragma warning(push)
#pragma warning (disable : 4686 4191 340)
#pragma push_macro("check")
#undef check
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#include <pybind11/operators.h>
#pragma pop_macro("check")
#pragma warning(pop)

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/ContentWidget.h"
#include "Components/DecalComponent.h"
#include "Components/EditableTextBox.h"
#include "Components/GridPanel.h"
#include "Components/GridSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/NamedSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Particles/ParticleSystemComponent.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/Spacer.h"
#include "Components/SphereComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Widget.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "FileMediaSource.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MediaPlayer.h"
#include "MediaSoundComponent.h"
#include "Paper2D/Classes/PaperSprite.h"
#include "Particles/ParticleSystem.h"
#include "Runtime/CoreUObject/Public/UObject/GCObject.h"
#include <functional>

#include "uepy.generated.h"

namespace py = pybind11;

// pybind11 lets python exceptions bubble up to be C++ exceptions, which is rarely what we want, so
// instead do try { ... } catchpy
#define catchpy catch (std::exception e) { LERROR("%s", UTF8_TO_TCHAR(e.what())); }

// std::string --> FString, sort of
#define FSTR(stdstr) UTF8_TO_TCHAR((stdstr).c_str())

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
})

#define STR_PROP(propName, className)\
.def_property(#propName, [](className& self) { std::string s = TCHAR_TO_UTF8(*self.propName); return s; }, [](className& self, std::string& v) { self.propName = FSTR(v); })

#define FNAME_PROP(propName, className)\
.def_property(#propName, [](className& self) { std::string s = TCHAR_TO_UTF8(*self.propName.ToString()); return s; }, [](className& self, std::string& v) { self.propName = FSTR(v); })

#define ENUM_PROP(propName, propType, className)\
.def_property(#propName, [](className& self) { return (int)self.propName; }, [](className& self, int v) { self.propName = (propType)v; })

// in order to be able pass from BP to Python any custom (game-specific) structs, the game has to provide a conversion function for them
typedef std::function<py::object(UScriptStruct* s, void *value)> BPToPyFunc;
UEPY_API void PyRegisterStructConverter(BPToPyFunc converterFunc);

class FToolBarBuilder;
class FMenuBuilder;

class FuepyModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool IsGameModule() const override { return true; }
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
    FString mcDelName; // the name of the multicast delegate
    FString pyDelMethodName; // the name of one of our On* methods

    static UBasePythonDelegate *Create(UObject *engineObj, FString mcDelName, FString pyDelMethodName, py::object pyCB);
    bool Matches(UObject *engineObj, FString& mcDelName, FString& pyDelMethodName, py::object pyCBOwner);

    UFunction *signatureFunction=nullptr; // used for BP events instead of one of the On functions below - we use this to get the signature
    virtual void ProcessEvent(UFunction *function, void *params) override;

    // Each different method signature for different multicast events needs a method here (or in a subclass I guess)
    // TODO: now that we have support for multicast delegates binding and firing via the UE4 reflection system, do we really need these one-off
    // events? They might be slightly more efficient (but maybe not), but is it enough to ever matter?

    // generic - also used for BP callbacks where we have to have a registered UFUNCTION that exists for the check right before it calls ProcessEvent
    UFUNCTION() void On() { if (valid) callback(); }

    // UComboBoxString
    UFUNCTION() void UComboBoxString_OnHandleSelectionChanged(FString Item, ESelectInfo::Type SelectionType) { if (valid) callback(*Item, (int)SelectionType); }

    // UCheckBox
    UFUNCTION() void UCheckBox_OnCheckStateChanged(bool checked) { if (valid) callback(checked); }

    // AActor
    UFUNCTION() void AActor_OnEndPlay(AActor *actor, EEndPlayReason::Type reason) { if (valid) callback(actor, (int)reason); }

    // UMediaPlayer
    UFUNCTION() void UMediaPlayer_OnMediaOpenFailed(FString failedURL) { std::string s = TCHAR_TO_UTF8(*failedURL); callback(s); }
};

// a singleton that taps into the engine's garbage collection system to keep some engine objects alive as long as they are
// being referenced from Python
class UEPY_API FPyObjectTracker : public FGCObject
{
    typedef struct Slot {
        // normally, an engine object shows up once in objectMap and there is a single corresponding shared wrapped Python instance. In some cases however
        // (such as calling a Cast() function from Python), we can end up with multiple Python instances for the same UObject, so we have to do reference
        // counting here
        int refs=0;
#if WITH_EDITOR
        FString objName; // for debugging - save it at the time of tracking so we can display it even if the obj gets force-GC'd by the engine out from under us
#endif
    } Slot;
    TMap<UObject*,Slot> objectMap; // an engine object we're keeping alive because it's being referenced in Python --> how many references in Python there are
    TArray<UBasePythonDelegate*> delegates; // bound delegates we need to keep alive so they don't get cleaned up by the engine since nobody references them directly

public:
    FPyObjectTracker() {};

    // used for binding engine multicast delegates to python functions
    UBasePythonDelegate *CreateDelegate(UObject *engineObj, const char *mcDelName, const char *pyDelMethodName, py::object pyCB);
    UBasePythonDelegate *FindDelegate(UObject *engineObj, const char *mcDelName, const char *pyDelMethodName, py::object pyCB);

    static FPyObjectTracker *Get();
    virtual void AddReferencedObjects(FReferenceCollector& InCollector) override;
    void Track(UObject *o);
    void Untrack(UObject *o);
    void Purge();
};

template <typename T> class UnrealTracker {
    struct Deleter {
        void operator()(T *t) { FPyObjectTracker::Get()->Untrack(t); }
    };

    std::unique_ptr<T, Deleter> ptr;
public:
    UnrealTracker(T *p) : ptr(p, Deleter()) { FPyObjectTracker::Get()->Track(p); };
    T *get() { return ptr.get(); }
};

// macros for binding a Python function to an engine object's multicast delegate
// engineObj - a UObject* to the engine object that has a delegate to bind to
// mcDel- the name of the multicast delegate member var on engineObj
// pyDelMethod- the name of one of the On* methods defined in UBasePythonDelegate
// pyCB - the Python function to be called when the delegate event fires
// Example: UEPY_BIND(someButtonVar, OnClicked, On, somePyFunctionVar);
// TODO: Should we get rid of this now that we have delegates working via the reflection system?
#define UEPY_BIND(engineObj, mcDel, pyDelMethod, pyCB)\
{\
    if (engineObj && engineObj->IsValidLowLevel())\
    {\
        UBasePythonDelegate *delegate = FPyObjectTracker::Get()->CreateDelegate(engineObj, #mcDel, #pyDelMethod, pyCB);\
        if (delegate)\
            engineObj->mcDel.AddDynamic(delegate, &UBasePythonDelegate::pyDelMethod);\
    }\
}

// To unbind, pass in the same original args
#define UEPY_UNBIND(engineObj, mcDel, pyDelMethod, pyCB)\
{\
    UBasePythonDelegate *delegate = FPyObjectTracker::Get()->FindDelegate(engineObj, #mcDel, #pyDelMethod, pyCB);\
    if (delegate && engineObj && engineObj->IsValidLowLevel())\
        engineObj->mcDel.RemoveDynamic(delegate, &UBasePythonDelegate::pyDelMethod);\
}

// Engine objects passed to Python have to be kept alive as long as Python is keeping a ref to them; we achieve this by connecting
// them to the root set during that time - pybind's default holder is just unique_ptr, so we just wrap it to get the same effect.
PYBIND11_DECLARE_HOLDER_TYPE(T, UnrealTracker<T>, true);

#define UTYPE_HOOK(uclass) \
    template<> struct polymorphic_type_hook<uclass> { \
        static const void *get(const UObject *src, const std::type_info*& type) { \
            if (src && src->StaticClass() == uclass::StaticClass()) { \
                type = &typeid(uclass); \
                return static_cast<const uclass*>(src); \
            } \
            return src; \
        } \
    }

namespace pybind11 {
    // TODO: figure out if there's some way to avoid this, and also figure out if the order matters
    UTYPE_HOOK(AActor);
    UTYPE_HOOK(APlayerController);
    UTYPE_HOOK(UActorComponent);
    UTYPE_HOOK(UAudioComponent);
    UTYPE_HOOK(UBlueprintGeneratedClass);
    UTYPE_HOOK(UBorder);
    UTYPE_HOOK(UBorderSlot);
    UTYPE_HOOK(UButton);
    UTYPE_HOOK(UCanvasPanel);
    UTYPE_HOOK(UCanvasPanelSlot);
    UTYPE_HOOK(UCheckBox);
    UTYPE_HOOK(UClass);
    UTYPE_HOOK(UComboBoxString);
    UTYPE_HOOK(UContentWidget);
    UTYPE_HOOK(UDecalComponent);
    UTYPE_HOOK(UEditableTextBox);
    UTYPE_HOOK(UFileMediaSource);
    UTYPE_HOOK(UGridPanel);
    UTYPE_HOOK(UGridSlot);
    UTYPE_HOOK(UHorizontalBox);
    UTYPE_HOOK(UHorizontalBoxSlot);
    UTYPE_HOOK(UImage);
    UTYPE_HOOK(UInterface);
    UTYPE_HOOK(ULightComponent);
    UTYPE_HOOK(ULightComponentBase);
    UTYPE_HOOK(ULocalLightComponent);
    UTYPE_HOOK(UMaterial);
    UTYPE_HOOK(UMaterialInstance);
    UTYPE_HOOK(UMaterialInstanceConstant);
    UTYPE_HOOK(UMaterialInstanceDynamic);
    UTYPE_HOOK(UMaterialInterface);
    UTYPE_HOOK(UMediaPlayer);
    UTYPE_HOOK(UMediaSoundComponent);
    UTYPE_HOOK(UMeshComponent);
    UTYPE_HOOK(UNamedSlot);
    UTYPE_HOOK(UObject);
    UTYPE_HOOK(UOverlay);
    UTYPE_HOOK(UOverlaySlot);
    UTYPE_HOOK(UPanelSlot);
    UTYPE_HOOK(UPanelWidget);
    UTYPE_HOOK(UPaperSprite);
    UTYPE_HOOK(UParticleSystem);
    UTYPE_HOOK(UParticleSystemComponent);
    UTYPE_HOOK(UPointLightComponent);
    UTYPE_HOOK(UPrimitiveComponent);
    UTYPE_HOOK(USceneComponent);
    UTYPE_HOOK(USizeBox);
    UTYPE_HOOK(USizeBoxSlot);
    UTYPE_HOOK(USoundClass);
    UTYPE_HOOK(USpacer);
    UTYPE_HOOK(USphereComponent);
    UTYPE_HOOK(USpotLightComponent);
    UTYPE_HOOK(UStaticMesh);
    UTYPE_HOOK(UStaticMeshComponent);
    UTYPE_HOOK(UTextBlock);
    UTYPE_HOOK(UTexture);
    UTYPE_HOOK(UTexture2D);
    UTYPE_HOOK(UUserWidget);
    UTYPE_HOOK(UVerticalBox);
    UTYPE_HOOK(UVerticalBoxSlot);
    UTYPE_HOOK(UVisual);
    UTYPE_HOOK(UWidget);
    UTYPE_HOOK(UWorld);
    UTYPE_HOOK(UWrapBox);
    UTYPE_HOOK(UWrapBoxSlot);
} // namespace pybind11

// any engine class we want to extend via Python should implement the IUEPYGlueMixin interface
UINTERFACE()
class UEPY_API UUEPYGlueMixin : public UInterface
{
    GENERATED_BODY()
};

class IUEPYGlueMixin
{
    GENERATED_BODY()

public:
    py::object pyInst;
};

struct UEPY_API FUEPyDelegates
{
    DECLARE_MULTICAST_DELEGATE_OneParam(FPythonEvent1, py::module&);
    static FPythonEvent1 LaunchInit; // called during initial engine startup
};

// Generic glue class for cases where you just want to subclass AActor in Python directly
UCLASS()
class UEPY_API AActor_CGLUE : public AActor, public IUEPYGlueMixin
{
    GENERATED_BODY()

    AActor_CGLUE();

public:
    void SuperBeginPlay();
    void SuperEndPlay(EEndPlayReason::Type reason);
    void SuperTick(float dt);

protected:
    virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float dt) override;
};

// helper class for any API that accepts as an argument a UClass parameter. Allows the caller to pass in
// a UClass pointer, a C++ class that has been exposed via pybind11, or a Python class object that is a subclass
// of a glue class. In all cases, it finds the appropriate UClass object and returns it.
UEPY_API UClass *PyObjectToUClass(py::object& klassThing);

// stuff for integrating into the UE4 reflection system (e.g. calling BPs)
py::object GetObjectUProperty(UObject *obj, std::string k);
void SetObjectUProperty(UObject *obj, std::string k, py::object& value);
py::object CallObjectUFunction(UObject *obj, std::string funcName, py::tuple& args);
void BindDelegateCallback(UObject *obj, std::string eventName, py::object& callback);
void UnbindDelegateCallback(UObject *obj, std::string eventName, py::object& callback);
void RegisterCustomStruct(UScriptStruct *s);

