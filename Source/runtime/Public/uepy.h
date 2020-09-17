// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

#pragma warning(push)
#pragma warning (disable : 4686 4191 340)
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#pragma warning(pop)

#include "Materials/MaterialInstanceDynamic.h"
#include "Runtime/CoreUObject/Public/UObject/GCObject.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/TextBlock.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Widget.h"
#include "uepy.generated.h"

// pybind11 lets python exceptions bubble up to be C++ exceptions, which is rarely what we want, so
// instead do try { ... } catchpy
#define catchpy catch (std::exception e) { LERROR("%s", UTF8_TO_TCHAR(e.what())); }

// std::string --> FString, sort of
#define FSTR(stdstr) UTF8_TO_TCHAR((stdstr).c_str())

class FToolBarBuilder;
class FMenuBuilder;

class FuepyModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool IsGameModule() const override { return true; }
};

class UEPY_API FPyObjectTracker : public FGCObject
{
    TSet<UObject *> objects;
public:
    FPyObjectTracker()
    {
    }

    static FPyObjectTracker *Get();
	virtual void AddReferencedObjects(FReferenceCollector& InCollector) override;
    void Track(UObject *o);
    void Untrack(UObject *o);
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
    UTYPE_HOOK(UMaterial);
    UTYPE_HOOK(UMaterialInstance);
    UTYPE_HOOK(UMaterialInstanceDynamic);
    UTYPE_HOOK(UMaterialInterface);
    UTYPE_HOOK(UStaticMesh);

    // I /think/ we want to order these bottom-up so the type hook checker finds the most specific type first. Maybe.
    UTYPE_HOOK(UStaticMeshComponent);
    UTYPE_HOOK(UMeshComponent);
    UTYPE_HOOK(UPrimitiveComponent);
    UTYPE_HOOK(USceneComponent);
    UTYPE_HOOK(UActorComponent);

    UTYPE_HOOK(UTexture2D);
    UTYPE_HOOK(UWorld);

    UTYPE_HOOK(UClass);
    UTYPE_HOOK(UInterface);

    UTYPE_HOOK(UPanelSlot);
    UTYPE_HOOK(UEditableTextBox);
    UTYPE_HOOK(UVerticalBox);
    UTYPE_HOOK(UHorizontalBox);
    UTYPE_HOOK(UVerticalBoxSlot);
    UTYPE_HOOK(UHorizontalBoxSlot);
    UTYPE_HOOK(UCheckBox);
    UTYPE_HOOK(UComboBoxString);
    UTYPE_HOOK(UTextBlock);
    UTYPE_HOOK(UButton);
    UTYPE_HOOK(UWidget);

    UTYPE_HOOK(UObject);

} // namespace pybind11

namespace py = pybind11;

// any engine class we want to extend via Python should implement the IUEPYGlueMixin interface
UINTERFACE()
class UUEPYGlueMixin : public UInterface // TODO: don't we need a UEPY_API here?
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

UCLASS()
class UBasePythonDelegate : public UObject
{
    GENERATED_BODY()
    
public:
    py::object callback;

    // generic
    UFUNCTION() void On() { callback(); }

    // UComboBoxString
    UFUNCTION() void OnUComboBoxString_HandleSelectionChanged(FString Item, ESelectInfo::Type SelectionType) { callback(*Item, (int)SelectionType); }

    // UCheckBox
    UFUNCTION() void OnUCheckBox_OnCheckStateChanged(bool checked) { callback(checked); }

};

// Generic glue class for cases where you just want to subclass AActor in Python directly
UCLASS()
class UEPY_API AActor_CGLUE : public AActor, public IUEPYGlueMixin
{
    GENERATED_BODY()

    AActor_CGLUE();

public:
    void SuperBeginPlay();
    void SuperTick(float dt);

protected:
	virtual void BeginPlay() override;
    virtual void Tick(float dt) override;
};

// helper class for any API that accepts as an argument a UClass parameter. Allows the caller to pass in
// a UClass pointer, a C++ class that has been exposed via pybind11, or a Python class object that is a subclass
// of a glue class. In all cases, it finds the appropriate UClass object and returns it.
UEPY_API UClass *PyObjectToUClass(py::object& klassThing);

