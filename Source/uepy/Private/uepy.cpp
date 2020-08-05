// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "uepy.h"
#include "common.h"
#include "py_module.h"

DEFINE_LOG_CATEGORY(UEPY);

void FuepyModule::StartupModule()
{
    // This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
    FPyObjectTracker::Get();

    // we need the engine to start up before we do much more Python stuff
    FCoreDelegates::OnPostEngineInit.AddLambda([]() -> void { LoadPyModule(); });
}

void FuepyModule::ShutdownModule()
{
    py::finalize_interpreter();
}

FPyObjectTracker *FPyObjectTracker::Get()
{
    static FPyObjectTracker *globalTracker;
    if (!globalTracker)
    {
        // create the singleton instance - I don't think locking is needed here because it's called on the game thread
        globalTracker = new FPyObjectTracker();

        // wire up to receive GC events from the engine
        //FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddRaw(globalTracker, &FPyObjectTracker::OnPreGC);
    }
    return globalTracker;
}

// called by the engine
void FPyObjectTracker::AddReferencedObjects(FReferenceCollector& InCollector)
{
    for (UObject *obj : objects)
        InCollector.AddReferencedObject(obj);
}

IMPLEMENT_MODULE(FuepyModule, uepy)

