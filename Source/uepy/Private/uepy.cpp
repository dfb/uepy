// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "uepy.h"
#include "common.h"
#include "py_module.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

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
#if WITH_EDITOR
        FEditorDelegates::PreBeginPIE.AddLambda([](bool b) { LOG("TTT PreBeginePIE"); });
        FEditorDelegates::BeginPIE.AddLambda([](bool b) { LOG("TTT BeginPIE"); });
        FEditorDelegates::PostPIEStarted.AddLambda([](bool b) { LOG("TTT PostPIEStarted"); });
        FEditorDelegates::PrePIEEnded.AddLambda([](bool b) { LOG("TTT PrePIEEnded"); });
        FEditorDelegates::EndPIE.AddLambda([](bool b) { LOG("TTT EndPIE"); });
        FEditorDelegates::PrePIEEnded.AddLambda([](bool b) { LOG("TTT PrePIEEnded"); });
#endif
    }
    return globalTracker;
}

void FPyObjectTracker::Track(UObject *o)
{
    LOG("TTT Track %s, %p", *o->GetName(), o);
    objects.Emplace(o);
}

void FPyObjectTracker::Untrack(UObject *o)
{
    LOG("TTT Untrack %s, %p", *o->GetName(), o);
    objects.Remove(o);
}

// called by the engine
void FPyObjectTracker::AddReferencedObjects(FReferenceCollector& InCollector)
{
    for (UObject *obj : objects)
        InCollector.AddReferencedObject(obj);
}

IMPLEMENT_MODULE(FuepyModule, uepy)

