// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "uepy.h"
#include "common.h"
#include "mod_uepy_umg.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

DEFINE_LOG_CATEGORY(UEPY);
IMPLEMENT_MODULE(FuepyModule, uepy)

FUEPyDelegates::FPythonEvent1 FUEPyDelegates::LaunchInit;

void FinishPythonInit()
{
    py::initialize_interpreter(); // we delay this call so that game modules have a chance to create their embedded py modules
    try {
        py::module m = py::module::import("_uepy");
        py::module sys = py::module::import("sys");
#if WITH_EDITOR
        // add the Plugin Content/Scripts dir to sys.path so unpackaged code can be found
        FString pluginScriptsDir = FPaths::Combine(*FPaths::ProjectPluginsDir(), _T("uepy"), _T("Content"), _T("Scripts"));
        sys.attr("path").attr("append")(*pluginScriptsDir);
#endif

        // add the Content/Scripts dir to sys.path so it can find main.py
        FString scriptsDir = FPaths::Combine(*FPaths::ProjectContentDir(), _T("Scripts"));
        sys.attr("path").attr("append")(*scriptsDir);

        // initialize any builtin modules
        _LoadModuleUMG(m);

        // now give all other modules a chance to startup as well
        FUEPyDelegates::LaunchInit.Broadcast(m);

        // note that main.py's Init is called *after* all plugin/game modules have received the LaunchInit event!
        LOG("Loading main.py");
        py::module main = py::module::import("main");
        //main.reload();
        main.attr("Init")();
    } catchpy;
}

#if WITH_EDITOR
void OnPreBeginPIE(bool b)
{
    try {
        py::module main = py::module::import("main");
        main.attr("OnPreBeginPIE")();
    } catchpy;
}
#endif

void FuepyModule::StartupModule()
{
    // This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
    FPyObjectTracker::Get();

    // we need the engine to start up before we do much more Python stuff
    FCoreDelegates::OnPostEngineInit.AddStatic(&FinishPythonInit);

#if WITH_EDITOR
    // TODO: maybe this should live in uepyEditor?
    FEditorDelegates::PreBeginPIE.AddStatic(&OnPreBeginPIE);
#endif

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
        FEditorDelegates::PreBeginPIE.AddLambda([](bool b) { LOG("TRK PreBeginePIE"); });
        FEditorDelegates::BeginPIE.AddLambda([](bool b) { LOG("TRK BeginPIE"); });
        FEditorDelegates::PostPIEStarted.AddLambda([](bool b) { LOG("TRK PostPIEStarted"); });
        FEditorDelegates::PrePIEEnded.AddLambda([](bool b) { LOG("TRK PrePIEEnded"); });
        FEditorDelegates::EndPIE.AddLambda([](bool b)
        {
            LOG("TRK EndPIE");
            for (auto obj : globalTracker->objects)
                LOG("TRK post-PIE: %s %p", *obj->GetName(), obj);
        });
#endif
    }
    return globalTracker;
}

void FPyObjectTracker::Track(UObject *o)
{
    //LOG("TRK Track %s, %p", *o->GetName(), o);
    objects.Emplace(o);
}

void FPyObjectTracker::Untrack(UObject *o)
{
    //LOG("TRK Untrack %s, %p", *o->GetName(), o);
    objects.Remove(o);
}

// called by the engine
void FPyObjectTracker::AddReferencedObjects(FReferenceCollector& InCollector)
{
    TArray<UObject *> toRemove;
    for (UObject *obj : objects)
    {
        if (obj && obj->IsValidLowLevel())
            InCollector.AddReferencedObject(obj);
        else
            toRemove.Emplace(obj);
    }

    for (UObject* obj : toRemove)
        objects.Remove(obj);
}

// AActor_CGLUE
AActor_CGLUE::AActor_CGLUE() { PrimaryActorTick.bCanEverTick = true; } // TODO: make this controllable from Python
void AActor_CGLUE::BeginPlay() { try { pyInst.attr("BeginPlay")(); } catchpy; }
void AActor_CGLUE::Tick(float dt) { try { pyInst.attr("Tick")(dt); } catchpy; }
void AActor_CGLUE::SuperBeginPlay() { Super::BeginPlay(); }
void AActor_CGLUE::SuperTick(float dt) { Super::Tick(dt); }

UClass *PyObjectToUClass(py::object& klassThing)
{
    // see if it's a registered subclass of a glue class
    if (py::hasattr(klassThing, "engineClass"))
        return klassThing.attr("engineClass").cast<UClass*>();

    // see if it's a glue class
    if (py::hasattr(klassThing, "cppGlueClass"))
        return klassThing.attr("cppGlueClass").attr("StaticClass")().cast<UClass*>()->GetSuperClass();

    // see if it'll cast to a UClass directly (i.e. caller already called StaticClass)
    try {
        return klassThing.cast<UClass*>();
    }
    catch (std::exception e)
    {
        // maybe it's a pybind11-exposed class?
        if (py::hasattr(klassThing, "StaticClass"))
            return klassThing.attr("StaticClass")().cast<UClass*>();
        return nullptr;
    }
}


