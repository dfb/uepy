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
    void Track(UObject *o) { o->AddToRoot(); objects.Emplace(o); }
    void Untrack(UObject *o) { objects.Remove(o); }
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
    UTYPE_HOOK(UObject);
    UTYPE_HOOK(UStaticMesh);
    UTYPE_HOOK(UStaticMeshComponent);
    UTYPE_HOOK(UTexture2D);
    UTYPE_HOOK(UWorld);
} // namespace pybind11

namespace py = pybind11;

// any engine class we want to extend via Python should implement the IPyBridgeMixin interface
UINTERFACE()
class UPyBridgeMixin : public UInterface
{
	GENERATED_BODY()
};

class IPyBridgeMixin
{
    GENERATED_BODY()

public:
    py::object pyInst;
};


