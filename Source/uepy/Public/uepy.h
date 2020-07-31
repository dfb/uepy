// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#include "Materials/MaterialInstanceDynamic.h"

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

class FToolBarBuilder;
class FMenuBuilder;

class FuepyModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool IsGameModule() const override { return true; }

    /** This function will be bound to Command (by default it will bring up plugin window) */
    void PluginButtonClicked();

private:

    void AddToolbarExtension(FToolBarBuilder& Builder);
    void AddMenuExtension(FMenuBuilder& Builder);

    TSharedRef<class SDockTab> OnSpawnPluginTab(const class FSpawnTabArgs& SpawnTabArgs);
    TSharedPtr<class FUICommandList> PluginCommands;
};

