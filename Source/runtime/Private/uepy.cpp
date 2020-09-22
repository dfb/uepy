// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "uepy.h"
#include "common.h"
#include "mod_uepy_umg.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

//#pragma optimize("", off)

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
            globalTracker->Purge();
            LOG("TRK EndPIE");
            for (auto& entry : globalTracker->objectMap)
            {
                UObject *obj = entry.Key;
                FPyObjectTracker::Slot& slot = entry.Value;
                LOG("TRK post-PIE obj: %s %p (%d refs)", *obj->GetName(), obj, slot.refs);
            }
            for (auto d : globalTracker->delegates)
            {
                if (d->valid)
                    LOG("TRK post-PIE delegate: %p (engineObj %p), mc:%s, pyDel:%s", d, d->engineObj, *d->mcDelName, *d->pyDelMethodName);
            }
        });
#endif
    }
    return globalTracker;
}

void FPyObjectTracker::Track(UObject *o)
{
    //LOG("TRK Track %s, %p", *o->GetName(), o);
    FPyObjectTracker::Slot& slot = objectMap.FindOrAdd(o);
    slot.refs++;
#if WITH_EDITOR
    slot.objName = o->GetName();
#endif
}

void FPyObjectTracker::Untrack(UObject *o)
{
    //LOG("TRK Untrack %s, %p", *o->GetName(), o);
    FPyObjectTracker::Slot *slot = objectMap.Find(o);
    if (!slot)
    {
#if WITH_EDITOR
        LWARN("TRK Untracking %s (%p) but it is not being tracked", (o && o->IsValidLowLevel()) ? *o->GetName() : TEXT("???"), o);
        // TODO: with editor, save obj name during tracking
#endif
    }
    else
        slot->refs--; // Purge will take care of removing it
}

UBasePythonDelegate *UBasePythonDelegate::Create(UObject *engineObj, FString _mcDelName, FString _pyDelMethodName, py::object pyCB)
{
    UBasePythonDelegate* delegate = NewObject<UBasePythonDelegate>();
    delegate->valid = true;
    delegate->engineObj = engineObj;
    delegate->mcDelName = _mcDelName;
    delegate->pyDelMethodName = _pyDelMethodName;
    delegate->callback = py::object(pyCB); // this increfs pyCB, which seems wrong, but in the end, seems to work. I spent a whole day trying to get weakrefs to handle it right, but to no avail.
    return delegate;
}

bool UBasePythonDelegate::Matches(UObject *_engineObj, FString& _mcDelName, FString& _pyDelMethodName, py::object _pyCB)
{
    return engineObj == (void*)_engineObj && mcDelName == _mcDelName && pyDelMethodName == _pyDelMethodName && callback.ptr() == _pyCB.ptr();
}

UBasePythonDelegate *FPyObjectTracker::CreateDelegate(UObject *engineObj, const char *mcDelName, const char *pyDelMethodName, py::object pyCB)
{
    UBasePythonDelegate* delegate = UBasePythonDelegate::Create(engineObj, UTF8_TO_TCHAR(mcDelName), UTF8_TO_TCHAR(pyDelMethodName), pyCB);
    delegates.Emplace(delegate);
    return delegate;
}

UBasePythonDelegate *FPyObjectTracker::FindDelegate(UObject *engineObj, const char *mcDelName, const char *pyDelMethodName, py::object pyCB)
{
    FString findMCDelName = UTF8_TO_TCHAR(mcDelName);
    FString findPyDelMethodName = UTF8_TO_TCHAR(pyDelMethodName);
    for (UBasePythonDelegate* delegate : delegates)
        if (delegate->Matches(engineObj, findMCDelName, findPyDelMethodName, pyCB))
            return delegate;
    return nullptr;
}

// removes any objects we should no longer be tracking
void FPyObjectTracker::Purge()
{
    for (auto it = objectMap.CreateIterator(); it ; ++it)
    {
        UObject *obj = it->Key;
        FPyObjectTracker::Slot& slot = it->Value;
        if (!obj || !obj->IsValidLowLevel() || slot.refs <= 0)
            it.RemoveCurrent();
    }

    for (auto it = delegates.CreateIterator(); it ; ++it)
    {
        UBasePythonDelegate* delegate = *it;
        if (!delegate->valid || !delegate->IsValidLowLevel() || !delegate->engineObj || !delegate->engineObj->IsValidLowLevel() || !delegate->callback || Py_REFCNT(delegate->callback.ptr()) <= 1)
        {
            if (delegate->callback)
            {
                delegate->callback.release();
                delegate->valid = false;
            }
            it.RemoveCurrent();
        }
    }
}

// called by the engine
void FPyObjectTracker::AddReferencedObjects(FReferenceCollector& InCollector)
{
    Purge();
    for (auto& entry : objectMap)
        InCollector.AddReferencedObject(entry.Key);

    // clear out any delegates whose pyinst is not longer valid
    for (UBasePythonDelegate *delegate : delegates)
        InCollector.AddReferencedObject(delegate);
}

// AActor_CGLUE
AActor_CGLUE::AActor_CGLUE() { PrimaryActorTick.bCanEverTick = true; PrimaryActorTick.bStartWithTickEnabled = false; }
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
    catch (std::exception e) // this logs a "pybind11::cast_error at memory location" error even though we are catching it
    {
		// maybe it's a pybind11-exposed class?
		if (py::hasattr(klassThing, "StaticClass"))
			return klassThing.attr("StaticClass")().cast<UClass*>();
		return nullptr;
	}
}

bool _setuprop(UProperty *prop, uint8* buffer, py::object& value, int index)
{
    try
    {
        if (auto bprop = Cast<UBoolProperty>(prop))
            bprop->SetPropertyValue_InContainer(buffer, value.cast<bool>(), index);
        else if (auto fprop = Cast<UFloatProperty>(prop))
            fprop->SetPropertyValue_InContainer(buffer, value.cast<float>(), index);
        else if (auto iprop = Cast<UIntProperty>(prop))
            iprop->SetPropertyValue_InContainer(buffer, value.cast<int>(), index);
        else if (auto strprop = Cast<UStrProperty>(prop))
            strprop->SetPropertyValue_InContainer(buffer, UTF8_TO_TCHAR(value.cast<std::string>().c_str()), index);
        else if (auto textprop = Cast<UTextProperty>(prop))
            textprop->SetPropertyValue_InContainer(buffer, FText::FromString(UTF8_TO_TCHAR(value.cast<std::string>().c_str())), index);
        else if (auto byteprop = Cast<UByteProperty>(prop))
            byteprop->SetPropertyValue_InContainer(buffer, value.cast<int>(), index);
        else if (auto enumprop = Cast<UEnumProperty>(prop))
            enumprop->GetUnderlyingProperty()->SetIntPropertyValue(enumprop->ContainerPtrToValuePtr<void>(buffer, index), value.cast<uint64>());
        else if (auto classprop = Cast<UClassProperty>(prop))
            classprop->SetPropertyValue_InContainer(buffer, value.cast<UClass*>(), index);
        else if (auto arrayprop = Cast<UArrayProperty>(prop))
        {
            try {
                py::list list = value.cast<py::list>();
                int size = py::len(list);

                FScriptArrayHelper_InContainer helper(arrayprop, buffer, index);
                if (helper.Num() < size)
                    helper.AddValues(size-helper.Num());
                else if (helper.Num() > size)
                    helper.RemoveValues(size, helper.Num()-size);

                for (int i=0; i < size; i++)
                {
                    py::object item = list[i];
                    if (!_setuprop(arrayprop->Inner, helper.GetRawPtr(i), item, 0))
                        return false;
                }
            } catchpy;
        }
        else if (auto structprop = Cast<UStructProperty>(prop))
        {
            UScriptStruct* theStruct = structprop->Struct;
            void *src = value.cast<void*>();
            void *dest = (void *)structprop->ContainerPtrToValuePtr<UScriptStruct>(buffer, index);
            memcpy(dest, src, theStruct->GetStructureSize());
        }
        else
            return false;

        // TODO: UInt32, Int64, UInt64, Name, Object, Map, Set, Delegate
        return true;
    } catchpy;
    return false;
}

// sets a UPROPERTY on an object (including BPs). With the old system, this is basically the approach we used everywhere, now we use
// it only when we have no other choice
void SetObjectUProperty(UObject *obj, std::string k, py::object& value)
{
    FName propName = FSTR(k);
    UProperty* prop = obj->GetClass()->FindPropertyByName(propName);
    if (!prop)
    {
        LERROR("Failed to find property %s on object %s", *propName.ToString(), *obj->GetName());
        return;
    }

    if (!_setuprop(prop, (uint8*)obj, value, 0))
    {
        LERROR("Failed to set property %s on object %s", *propName.ToString(), *obj->GetName());
    }
}

//#pragma optimize("", on)

