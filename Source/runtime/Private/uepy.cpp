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
py::object _getuprop(UProperty *prop, uint8* buffer, int index);

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
                LOG("TRK post-PIE obj: type:%s name:%s %p (%d refs)", *obj->GetClass()->GetName(), *obj->GetName(), obj, slot.refs);
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
        //too verbose! LWARN("TRK Untracking %s (%p) but it is not being tracked", (o && o->IsValidLowLevel()) ? *o->GetName() : TEXT("???"), o);
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

    // interesting! If you have a Python class Foo that has a method Bar, and do f = Foo(), and then ask the Python garbage collector
    // who refers to f.Bar, you'll get back... an empty list! The problem for us is that we cannot look at the refcount on the callback
    // to see when to auto-release the delegate binding. For now we deal with this by:
    // - requiring that pyCB be a method instead of a plain function
    // - acquiring a ref to the object instance that the method is bound to
    // - auto-freeing the delegate once we're the only one still maintaining a ref
    if (!py::hasattr(pyCB, "__self__"))
    {
        LERROR("Delegates can only be bound to methods, not plain Python functions (%s %s)", *engineObj->GetName(), *_mcDelName);
        return nullptr;
    }
    delegate->callbackOwner = pyCB.attr("__self__"); // save a ref to the owner
    delegate->callback = pyCB;
    return delegate;
}

bool UBasePythonDelegate::Matches(UObject *_engineObj, FString& _mcDelName, FString& _pyDelMethodName, py::object _pyCBOwner)
{
    return engineObj == (void*)_engineObj && callbackOwner.ptr() == _pyCBOwner.ptr() && mcDelName == _mcDelName && pyDelMethodName == _pyDelMethodName;
}

UBasePythonDelegate *FPyObjectTracker::CreateDelegate(UObject *engineObj, const char *mcDelName, const char *pyDelMethodName, py::object pyCB)
{
    UBasePythonDelegate* delegate = UBasePythonDelegate::Create(engineObj, UTF8_TO_TCHAR(mcDelName), UTF8_TO_TCHAR(pyDelMethodName), pyCB);
    if (delegate)
        delegates.Emplace(delegate);
    return delegate;
}

UBasePythonDelegate *FPyObjectTracker::FindDelegate(UObject *engineObj, const char *mcDelName, const char *pyDelMethodName, py::object pyCB)
{
    py::object pyCBOwner = pyCB.attr("__self__");
    FString findMCDelName = UTF8_TO_TCHAR(mcDelName);
    FString findPyDelMethodName = UTF8_TO_TCHAR(pyDelMethodName);
    for (UBasePythonDelegate* delegate : delegates)
        if (delegate->Matches(engineObj, findMCDelName, findPyDelMethodName, pyCBOwner))
            return delegate;
    return nullptr;
}

void UBasePythonDelegate::ProcessEvent(UFunction *function, void *params)
{
    // ProcessEvent is called in one of two scenarios:
    // 1) An event was bound via the reflection system and the delegate has fired. In this case, the 'function' parameter is useless
    //      (it's the dummy function we listed at the time of binding; we needed something or ProcessEvent would never have been called),
    //      so we instead use the signatureFunction we got from the MC delegate property at the time of binding - we use this function
    //      to extract the parameters from the event.
    // 2) An event was bound using one of the Bind* functions we explicitly exposed from C++ to Python. In this case, there is not
    //      signatureFunction, so we fall back to the default behavior and let the event get dispatched normally.
    if (!signatureFunction)
    {
        Super::ProcessEvent(function, params);
        return;
    }

    if (!valid) // about to be cleaned up
        return;

    py::list args;
    for (TFieldIterator<UProperty> iter(signatureFunction); iter ; ++iter)
    {
        UProperty *prop = *iter;
        if (!prop->HasAnyPropertyFlags(CPF_Parm))
            continue;
        if (prop->HasAnyPropertyFlags(CPF_OutParm))//ReturnParm))
            continue;

        args.append(_getuprop(prop, (uint8*)params, 0));
    }

    try {
        callback(*args);
    } catchpy;
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
        if (!delegate->valid || !delegate->IsValidLowLevel() || !delegate->engineObj || !delegate->engineObj->IsValidLowLevel() ||
            !delegate->callbackOwner || Py_REFCNT(delegate->callbackOwner.ptr()) <= 1)
        {
            delegate->valid = false;
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
void AActor_CGLUE::EndPlay(const EEndPlayReason::Type reason) { try { pyInst.attr("EndPlay")((int)reason); } catchpy; }
void AActor_CGLUE::Tick(float dt) { try { pyInst.attr("Tick")(dt); } catchpy; }
void AActor_CGLUE::SuperBeginPlay() { Super::BeginPlay(); }
void AActor_CGLUE::SuperEndPlay(EEndPlayReason::Type reason) { Super::EndPlay(reason); }
void AActor_CGLUE::SuperTick(float dt) { Super::Tick(dt); }

UClass *PyObjectToUClass(py::object& klassThing)
{   
    // TODO: the logic in here is super fragile

    if (klassThing.is_none())
    {
        LERROR("Cannot cast None to UClass");
        return nullptr;
    }
    // see if it's a registered subclass of a glue class
    if (py::hasattr(klassThing, "engineClass"))
    {
        UClass* klass = klassThing.attr("engineClass").cast<UClass*>();
        //LOG("XXXA %s --> %s", UTF8_TO_TCHAR(py::repr(klassThing).cast<std::string>().c_str()), *klass->GetName());
        return klass;
    }

    // see if it's a glue class
    if (py::hasattr(klassThing, "cppGlueClass"))
    {
        UClass *klass = klassThing.attr("cppGlueClass").attr("StaticClass")().cast<UClass*>()->GetSuperClass();
        //LOG("XXXB %s --> %s", UTF8_TO_TCHAR(py::repr(klassThing).cast<std::string>().c_str()), *klass->GetName());
        return klass;
    }

    try {
        UObject *uobj = klassThing.cast<UObject*>();
        if (UClass *klass = Cast<UClass>(uobj))
        {
            //LOG("XXXD %s --> %s", UTF8_TO_TCHAR(py::repr(klassThing).cast<std::string>().c_str()), *klass->GetName());
            return klass; // caller already called StaticClass on it
        }

        return uobj->GetClass();
    } catch (std::exception e)
    {
        // maybe it's a pybind11-exposed class?
        if (py::hasattr(klassThing, "StaticClass"))
        {
            UClass* klass = klassThing.attr("StaticClass")().cast<UClass*>();
            //LOG("XXXC %s --> %s", UTF8_TO_TCHAR(py::repr(klassThing).cast<std::string>().c_str()), *klass->GetName());
            return klass;
        }

        std::string s = py::repr(klassThing);
        LERROR("XXXE Failed to convert %s to UClass", UTF8_TO_TCHAR(s.c_str()));
        return nullptr;
    }

    /*
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
    */
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
        else if (auto ui32prop = Cast<UUInt32Property>(prop))
            ui32prop->SetPropertyValue_InContainer(buffer, value.cast<uint32>(), index);
        else if (auto i64prop = Cast<UInt64Property>(prop))
            i64prop->SetPropertyValue_InContainer(buffer, value.cast<long long>(), index);
        else if (auto iu64prop = Cast<UUInt64Property>(prop))
            iu64prop->SetPropertyValue_InContainer(buffer, value.cast<uint64>(), index);
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
        else if (auto objprop = Cast<UObjectProperty>(prop))
            objprop->SetObjectPropertyValue_InContainer(buffer, value.cast<UObject*>(), index);
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

        // TODO: Name, Object, Map, Set, Delegate
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

std::vector<BPToPyFunc> structHandlerFuncs;
void PyRegisterStructConverter(BPToPyFunc converterFunc)
{
    structHandlerFuncs.push_back(converterFunc);
}

#define _GETPROP(enginePropClass, cppType) \
if (auto t##enginePropClass = Cast<enginePropClass>(prop))\
{\
    cppType ret = t##enginePropClass->GetPropertyValue_InContainer(buffer, index);\
    return py::cast(ret);\
}

py::object _getuprop(UProperty *prop, uint8* buffer, int index)
{
    _GETPROP(UBoolProperty, bool);
    _GETPROP(UFloatProperty, float);
    _GETPROP(UIntProperty, int);
    _GETPROP(UUInt32Property, uint32);
    _GETPROP(UInt64Property, long long);
    _GETPROP(UUInt64Property, uint64);
    _GETPROP(UByteProperty, int);
    _GETPROP(UObjectProperty, UObject*);
    if (auto strprop = Cast<UStrProperty>(prop))
    {
        FString sret = strprop->GetPropertyValue_InContainer(buffer, index);
        std::string ret = TCHAR_TO_UTF8(*sret);
        return py::cast(ret);
    }
    if (auto textprop = Cast<UTextProperty>(prop))
    {
        FText tret = textprop->GetPropertyValue_InContainer(buffer, index);
        std::string ret = TCHAR_TO_UTF8(*tret.ToString());
        return py::cast(ret);
    }
    if (auto enumprop = Cast<UEnumProperty>(prop))
    {
        void* prop_addr = enumprop->ContainerPtrToValuePtr<void>(buffer, index);
        uint64 v = enumprop->GetUnderlyingProperty()->GetUnsignedIntPropertyValue(prop_addr);
        return py::cast(v);
    }
    if (auto classprop = Cast<UClassProperty>(prop))
    {
        UClass *klass = Cast<UClass>(classprop->GetPropertyValue_InContainer(buffer, index));
        return py::cast(klass);
    }
    if (auto arrayprop = Cast<UArrayProperty>(prop))
    {
        FScriptArrayHelper_InContainer helper(arrayprop, buffer, index);
        py::list ret;
        for (int i=0; i < helper.Num(); i++)
            ret.append(_getuprop(arrayprop->Inner, helper.GetRawPtr(i), 0));
        return ret;
    }

    // For structures, we handle some specific builtin ones below and then leave it up to the game code to handle
    // any others.
    if (auto structprop = Cast<UStructProperty>(prop))
    {
        UScriptStruct* theStruct = structprop->Struct;
        if (theStruct == TBaseStructure<FVector>::Get())
        {
            FVector v = *structprop->ContainerPtrToValuePtr<FVector>(buffer, index);
            return py::cast(v);
        }
        if (theStruct == TBaseStructure<FVector2D>::Get())
        {
            FVector2D v = *structprop->ContainerPtrToValuePtr<FVector2D>(buffer, index);
            return py::cast(v);
        }
        if (theStruct == TBaseStructure<FRotator>::Get())
        {
            FRotator v = *structprop->ContainerPtrToValuePtr<FRotator>(buffer, index);
            return py::cast(v);
        }
        if (theStruct == TBaseStructure<FTransform>::Get())
        {
            FTransform v = *structprop->ContainerPtrToValuePtr<FTransform>(buffer, index);
            return py::cast(v);
        }
        if (theStruct == TBaseStructure<FLinearColor>::Get())
        {
            FLinearColor v = *structprop->ContainerPtrToValuePtr<FLinearColor>(buffer, index);
            return py::cast(v);
        }

        // now give the game code a try
        void *raw = structprop->ContainerPtrToValuePtr<void*>(buffer, index);
        for (auto converter : structHandlerFuncs)
        {
            py::object obj = converter(theStruct, raw);
            if (!obj.is_none())
                return obj;
        }
    }

    LERROR("Failed to convert property %s to python", *prop->GetName());
    return py::none();
}

// gets a UPROPERTY from an object (including BPs)
py::object GetObjectUProperty(UObject *obj, std::string k)
{
    FName propName = FSTR(k);
    UProperty* prop = obj->GetClass()->FindPropertyByName(propName);
    if (!prop)
    {
        LERROR("Failed to find property %s on object %s", *propName.ToString(), *obj->GetName());
        return py::none();
    }

    return _getuprop(prop, (uint8*)obj, 0);
}

// calls a UFUNCTION on an object and returns the result
// NOTE: for now (and maybe forever) there are many, many restrictions on what does and doesn't work here!
// Allowed:
// - 0 or more parameters, all types must be supported by _setuprop
// - 0 or 1 return parameter, type must be supported by _getuprop
// - all parameters must be passed in (i.e. can't leave some off to have them use their default values)
// - positional arguments only, no kwargs
// - I have no idea how it works in the presence of super() calls
// - I've only tested it interacting with BPs - there are some very different engine code paths for calling C++ functions via
//   the reflection system and tweaks may be required for that scenario.
// NOTE: earlier we created a patch for UnrealEnginePython that allowed for crazier stuff like multiple output parameters, so
// if we were to need that functionality, we could go dig up the patch.
py::object CallObjectUFunction(UObject *obj, std::string _funcName, py::tuple& pyArgs)
{
    FName funcName = FSTR(_funcName);
    UFunction *func = obj->FindFunction(funcName);
    if (!func)
    {
        LERROR("Failed to find function %s on object %s", *funcName.ToString(), *obj->GetName());
        return py::none();
    }

    // the engine code for this sort of thing doesn't have much in the way of comments, so we get by here
    // by keeping things really simple. AFAICT it just uses the same UProperty approach as it uses with
    // getting/setting UPROPERTYs, so we allocate some space to hold the properties to pass in and then populate
    // them using the args tuple passed in from Python.
    uint8* propArgsBuffer = (uint8*)FMemory_Alloca(func->ParmsSize);
    FMemory::Memzero(propArgsBuffer, func->ParmsSize);
    UProperty *returnProp = nullptr;
    int nextPyArg = 0;
    int numPyArgs = pyArgs.size();
    for (TFieldIterator<UProperty> iter(func); iter ; ++iter)
    {
        UProperty *prop = *iter;
        if (!prop->HasAnyPropertyFlags(CPF_Parm))
            continue;
        if (prop->HasAnyPropertyFlags(CPF_OutParm))//ReturnParm))
        {
            returnProp = prop;
            continue;
        }

        // convert the python value to a UProp value
        if (nextPyArg >= numPyArgs)
        {
            LERROR("Not enough arguments in call to %s", *funcName.ToString());
            return py::none();
            // TODO: we're probably leaking memory here (see cleanup code below)
        }

        py::object arg = pyArgs[nextPyArg++];
        if (!_setuprop(prop, propArgsBuffer, arg, 0))
        {
            LERROR("Failed to convert Python arg %d in call to %s:", nextPyArg-1, *funcName.ToString());
            return py::none();
        }
    }

    obj->ProcessEvent(func, propArgsBuffer);

    py::object ret = py::none();
    if (returnProp)
        ret = _getuprop(returnProp, propArgsBuffer, 0);

    // Cleanup
    for (TFieldIterator<UProperty> iter(func) ; iter ; ++iter)
    {
        UProperty *prop = *iter;
        if (iter->HasAnyPropertyFlags(CPF_Parm))
            prop->DestroyValue_InContainer(propArgsBuffer);
    }

    return ret;
}

// generic binding of a python callback function to a multicast script delegate
void BindDelegateCallback(UObject *obj, std::string _eventName, py::object& callback)
{
    FName eventName = FSTR(_eventName);
    UProperty* prop = obj->GetClass()->FindPropertyByName(eventName);
    if (!prop)
    {
        LERROR("Failed to find property %s on object %s", *eventName.ToString(), *obj->GetName());
    }

    UMulticastDelegateProperty *mcprop = Cast<UMulticastDelegateProperty>(prop);
    if (!mcprop)
    {
        LERROR("Property %s is not a multicast delegate on object %s", *eventName.ToString(), *obj->GetName());
    }

    UBasePythonDelegate *delegate = FPyObjectTracker::Get()->CreateDelegate(obj, _eventName.c_str(), "On", callback);
    if (delegate)
    {
        delegate->signatureFunction = mcprop->SignatureFunction;
        FScriptDelegate scriptDel;
        scriptDel.BindUFunction(delegate, FName("On")); // this refers to the generic UBasePythonDelegate::On method - we just have to provide /something/ but it never gets used
        mcprop->AddDelegate(scriptDel, obj);
    }
}

void UnbindDelegateCallback(UObject *obj, std::string _eventName, py::object& callback)
{
    FName eventName = FSTR(_eventName);
    UProperty* prop = obj->GetClass()->FindPropertyByName(eventName);
    if (!prop)
    {
        LERROR("Failed to find property %s on object %s", *eventName.ToString(), *obj->GetName());
    }

    UMulticastDelegateProperty *mcprop = Cast<UMulticastDelegateProperty>(prop);
    if (!mcprop)
    {
        LERROR("Property %s is not a multicast delegate on object %s", *eventName.ToString(), *obj->GetName());
    }

    UBasePythonDelegate *delegate = FPyObjectTracker::Get()->FindDelegate(obj, _eventName.c_str(), "On", callback);
    if (delegate)
    {
        FScriptDelegate scriptDel;
        scriptDel.BindUFunction(delegate, FName("On")); // this refers to the generic UBasePythonDelegate::On method - we just have to provide /something/ but it never gets used
        mcprop->RemoveDelegate(scriptDel, obj);
        scriptDel.Clear();
        delegate->valid = false;
    }
}

//#pragma optimize("", on)

