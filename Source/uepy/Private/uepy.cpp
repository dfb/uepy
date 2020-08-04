// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "uepy.h"
#include "common.h"
#include "py_module.h"

DEFINE_LOG_CATEGORY(UEPY);

UPyObjectTracker *TRACKER = nullptr;
UPyObjectTracker *GetTracker() { return TRACKER;  }

void FuepyModule::StartupModule()
{
    // This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
    TRACKER = NewObject<UPyObjectTracker>();

    // we need the engine to start up before we do much more Python stuff
    FCoreDelegates::OnPostEngineInit.AddLambda([]() -> void { LoadPyModule(); });
}

void FuepyModule::ShutdownModule()
{
    py::finalize_interpreter();
}

IMPLEMENT_MODULE(FuepyModule, uepy)

