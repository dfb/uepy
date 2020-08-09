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
#include "uepy.generated.h"

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
    UTYPE_HOOK(UObject);

} // namespace pybind11

namespace py = pybind11;

// any engine class we want to extend via Python should implement the IPyBridgeMixin interface
UINTERFACE()
class UPyBridgeMixin : public UInterface // TODO: don't we need a UEPY_API here?
{
	GENERATED_BODY()
};

class IPyBridgeMixin
{
    GENERATED_BODY()

public:
    py::object pyInst;
};

struct UEPY_API FPythonDelegates
{
	DECLARE_MULTICAST_DELEGATE_OneParam(FPythonEvent1, py::module&);
    static FPythonEvent1 LaunchInit; // called during initial engine startup
};

/*
// currently this exists just to expose RootComponent (protected) to Python
// maybe we should instead just expose Get/SetRootComponent APIs that already exist on AActor?
UCLASS()
class UEPY_API AUEPyActor : public AActor, public IPyBridgeMixin
{
    GENERATED_BODY()

public:
    using AActor::RootComponent;
};

*/

