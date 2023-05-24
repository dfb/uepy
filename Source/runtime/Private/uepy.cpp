// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "uepy.h"
#include "common.h"
#include "mod_uepy_umg.h"
#include "Async/Async.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

#if PLATFORM_WINDOWS
#include <fcntl.h>
#endif

//#pragma optimize("", off)

DEFINE_LOG_CATEGORY(UEPY);
DEFINE_LOG_CATEGORY(NetRep);
IMPLEMENT_MODULE(FuepyModule, uepy)
py::object _getprop(FProperty *prop, uint8* buffer, int index);

FUEPyDelegates::FPythonEvent1 FUEPyDelegates::LaunchInit;

// true once interpreter has been finalized. Used to skip some work during shutdown when things are all
// mixed up
static bool pyFinalized = false;

// source code for a builtin import hook that allows applications to register modules instead of having to ship .py/.pyc files
static std::string importHook = R"(
import sys, marshal
from collections import namedtuple
from importlib.machinery import SourcelessFileLoader
from importlib.util import spec_from_loader

ImportHookEntry = namedtuple('ImportHookEntry', 'code diskPath isPackage'.split())
class UEPYImportHook(SourcelessFileLoader):
    modules = {} # dotted name -> ImportHookEntry
    # diskPath is what module.__file__ will be set to

    @staticmethod
    def Add(dottedName, code, diskPath, isPackage):
        if type(code) is bytes: code = marshal.loads(code)
        UEPYImportHook.modules[dottedName] = ImportHookEntry(code, diskPath, isPackage)

    @staticmethod
    def find_spec(dottedName, path, target=None):
        entry = UEPYImportHook.modules.get(dottedName)
        if entry:
            ret = spec_from_loader(dottedName, UEPYImportHook(path, dottedName), is_package=entry.isPackage)
            if entry.diskPath:
                ret.origin = entry.diskPath
            return ret
        return None

    def get_code(self, dottedName):
        entry = UEPYImportHook.modules.get(dottedName)
        if entry:
            return entry.code
        return None

sys.meta_path.append(UEPYImportHook)
sys.UEPYImportHook = UEPYImportHook # make it semi-easily accessible
)";

static std::string mainSrc;
void UEPYSetMainSource(const std::string& src)
{
    mainSrc = src;
}

void FinishPythonInit()
{
    pyFinalized = false;

    {   // hackery to get builds to work - without this, UE4 has some automation tests that complain that the
        // locale is being changed
        PyStatus status;
        PyPreConfig preconfig;
        PyPreConfig_InitPythonConfig(&preconfig);

        preconfig.configure_locale = 0;
        preconfig.coerce_c_locale = 0;
        preconfig.coerce_c_locale_warn = 0;

        status = Py_PreInitialize(&preconfig);
        if (PyStatus_Exception(status)) {
            Py_ExitStatusException(status);
        }
    }
    py::initialize_interpreter(); // we delay this call so that game modules have a chance to create their embedded py modules

#if PLATFORM_WINDOWS
    // Inspired by the old python plugin: apparently Py_Initialize changes the modes
    // to O_BINARY, which causes UE4 to spew stuff in UTF-16.
    _setmode(_fileno(stdin), O_TEXT);
    _setmode(_fileno(stdout), O_TEXT);
    _setmode(_fileno(stderr), O_TEXT);
#endif

    try {
        py::module m = py::module::import("_uepy");
        py::module sys = py::module::import("sys");
#if WITH_EDITOR
        // add the Plugin Content/Scripts dir to sys.path so unpackaged code can be found
        FString pluginScriptsDir = FPaths::Combine(*FPaths::ProjectPluginsDir(), _T("uepy"), _T("Content"), _T("Scripts"));
        sys.attr("path").attr("append")(*pluginScriptsDir);
#endif

        // add a global import hook for importing Python code without shipping .py/.pyc files
        py::exec(importHook, py::globals());

#if WITH_EDITOR
        // add the Content/Scripts dir to sys.path so it can find main.py and other stuff on disk
        // in built versions, if games want to allow from-disk loading, they can adjust sys.path as needed.
        FString scriptsDir = FPaths::Combine(*FPaths::ProjectContentDir(), _T("Scripts"));
        sys.attr("path").attr("append")(*scriptsDir);
#endif

        // initialize any builtin modules
        _LoadModuleUMG(m);

        // now give all other modules a chance to startup as well
        FUEPyDelegates::LaunchInit.Broadcast(m);

        // note that main is imported *after* all plugin/game modules have received the LaunchInit event!
        // Also, we don't call any specific APIs or anything - just let the module load and whatever it needs
        // to do happens as side effects
        LOG("Loading main.py");

        // If a game provided the source code for main.py, load it now. Otherwise assume it is on disk somewhere
        // that is accessible via sys.path
        if (mainSrc.size())
        {
            py::module main = py::module_::create_extension_module("main", nullptr, new py::module_::module_def);
            sys.attr("modules")["main"] = main;
            py::exec(mainSrc, main.attr("__dict__"));
        }
        else
            py::module::import("main");
    } catchpy;
}

#if WITH_EDITOR
void OnPreBeginPIE(bool b)
{
    try {
        py::module main = py::module::import("main");
        if (py::hasattr(main, "OnPreBeginPIE"))
            main.attr("OnPreBeginPIE")();
    } catchpy;
}

void OnEndPIE(bool b)
{
    try {
        py::module main = py::module::import("main");
        if (py::hasattr(main, "OnEndPIE"))
            main.attr("OnEndPIE")();
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
    FEditorDelegates::EndPIE.AddStatic(&OnEndPIE);
#endif

}

void FuepyModule::ShutdownModule()
{
    pyFinalized = true;
    //py::finalize_interpreter(); TODO: maybe re-enable this? Right now it sometimes means we end with a crash (probably due to something not happening in the right order)
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
            /*
            for (auto& entry : globalTracker->objectMap)
            {
                FPyObjectTracker::Slot& slot = entry.Value;
                //LOG("TRK post-PIE obj: type:%s name:%s %p (%d refs)", *obj->GetClass()->GetName(), *obj->GetName(), obj, slot.refs);
            }
            for (auto d : globalTracker->delegates)
            {
                //if (d->valid)
                //    LOG("TRK post-PIE delegate: %p (engineObj %p), mc:%s, pyDel:%s", d, d->engineObj, *d->mcDelName, *d->pyDelMethodName);
            }
            */
        });
#endif
    }
    return globalTracker;
}

uint64 FPyObjectTracker::Track(UObject *o)
{
    if (pyFinalized) return 0; // we're shutting down
    if (!IsValid(o) || !o->IsValidLowLevel() || o->IsPendingKillOrUnreachable())
    {
#if WITH_EDITOR
        LERROR("TRK TIO Told to track invalid object");
#endif
        return 0;
    }

    //LOG("TRK + %s : %p", *o->GetName(), o);
    uint64 key = (((uint64)o) << 32) | o->GetUniqueID();
    FPyObjectTracker::Slot& slot = objectMap.FindOrAdd(key);
    slot.refs++;
    slot.objIndex = o->GetUniqueID();
    slot.obj = o;
#if WITH_EDITOR
    slot.objName = o->GetName();
    slot.objAddr = (uint64)o;
#endif
    return key;
}

void FPyObjectTracker::IncRef(uint64 key)
{
    FPyObjectTracker::Slot *slot = objectMap.Find(key);
    if (!slot)
    {
        LERROR("Failed to find slot for key %llX", key);
    }
    else
        slot->refs++;
}

void FPyObjectTracker::Untrack(uint64 key)
{
    if (pyFinalized) return; // we're shutting down
    FPyObjectTracker::Slot *slot = objectMap.Find(key);
    if (!slot)
    {
        // there are legitimate cases for when a slot can't be found. A common one is when we "subclass" an engine object in
        // python and get to the point where there are only self-references left (a ref cycle between the c++ and pyinst) so
        // we manually break the cycle and remove the slot entry. If the engine object is then GC'd, then on the next python GC
        // run, the pyinst handle will be cleaned up and try to call Untrack, but the slot is gone.
        //LERROR("Failed to find slot for key %llX", key);
    }
    else
        slot->refs--; // if <= 0, Purge will remove it
}

UBasePythonDelegate *UBasePythonDelegate::Create(UObject *engineObj, FString _mcDelName, FString _pyDelMethodName, py::object pyCB)
{
    UBasePythonDelegate* delegate = NewObject<UBasePythonDelegate>();
    delegate->valid = true;
    delegate->engineObj = engineObj;
    delegate->engineObjIndex = engineObj->GetUniqueID();
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

bool UBasePythonDelegate::Matches(UObject *_engineObj, FString& _mcDelName, FString& _pyDelMethodName, py::object _pyCB)
{
    // in the check below, we can't see if callback.ptr() == _pyCB.ptr() because of the way cpython works - "obj.method"
    // returns a bound method object, but that object may not be the same every time:
    // >>> class Foo:
    // ...   def bar(self): pass
    // ...
    // >>> f = Foo()
    // >>> id(f.bar)
    // 1585735241928
    // >>> f.bar
    // <bound method Foo.bar of <__main__.Foo object at 0x0000017135376E10>>
    // >>> id(f.bar)
    // 1585735241800 <--- !!!!!
    // The bound method object, however, does have a __func__ attribute, and it will point to the same underlying function object.
    py::object _pyCBOwner = _pyCB.attr("__self__");
    return engineObj == (void*)_engineObj && callbackOwner.ptr() == _pyCBOwner.ptr() && mcDelName == _mcDelName &&
           pyDelMethodName == _pyDelMethodName && callback.attr("__func__").ptr() == _pyCB.attr("__func__").ptr();
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
    FString findMCDelName = UTF8_TO_TCHAR(mcDelName);
    FString findPyDelMethodName = UTF8_TO_TCHAR(pyDelMethodName);
    for (UBasePythonDelegate* delegate : delegates)
        if (delegate->valid && delegate->Matches(engineObj, findMCDelName, findPyDelMethodName, pyCB))
            return delegate;
    return nullptr;
}

// mark invalid any delegates with the given owner
void FPyObjectTracker::UnbindDelegatesOn(py::object& obj)
{
    for (auto it = delegates.CreateIterator(); it ; ++it)
    {
        UBasePythonDelegate* delegate = *it;
        if (VALID(delegate) && delegate->valid && delegate->callbackOwner.is(obj))
        {
            //LWARN("Unbinding delegate on %s %s %s", REPR(obj), *delegate->mcDelName, *delegate->pyDelMethodName);
            delegate->valid = false;
        }
    }
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
    for (TFieldIterator<FProperty> iter(signatureFunction); iter ; ++iter)
    {
        FProperty *prop = *iter;
        if (!prop->HasAnyPropertyFlags(CPF_Parm))
            continue;
        if (prop->HasAnyPropertyFlags(CPF_OutParm))//ReturnParm))
            continue;

        args.append(_getprop(prop, (uint8*)params, 0));
    }

    py::object cb = callback;
    AsyncTask(ENamedThreads::GameThread, [cb, args]() {
        try {
            cb(*args);
        } catchpy;
    });
}

void UBasePythonDelegate::On() { if (valid) try { callback(); } catchpy; }
void UBasePythonDelegate::UComboBoxString_OnHandleSelectionChanged(FString Item, ESelectInfo::Type SelectionType) { if (valid) try { callback(*Item, (int)SelectionType); } catchpy; }
void UBasePythonDelegate::UCheckBox_OnCheckStateChanged(bool checked) { if (valid) try { callback(checked); } catchpy; }
void UBasePythonDelegate::AActor_OnEndPlay(AActor *actor, EEndPlayReason::Type reason) { if (valid) try { callback(actor, (int)reason); } catchpy; }
void UBasePythonDelegate::UMediaPlayer_OnMediaOpenFailed(FString failedURL) { std::string s = TCHAR_TO_UTF8(*failedURL); if (valid) try { callback(s); } catchpy; }
void UBasePythonDelegate::UInputComponent_OnAxis(float value) { if (valid) try { callback(value); } catchpy; }
void UBasePythonDelegate::UInputComponent_OnKeyAction(FKey key) { if (valid) try { callback(key); } catchpy; }

// removes any objects we should no longer be tracking
void FPyObjectTracker::Purge()
{
    if (pyFinalized) return; // we're shutting down
    for (auto it = objectMap.CreateIterator(); it ; ++it)
    {
        FPyObjectTracker::Slot& slot = it->Value;
        UObject* obj = slot.obj;
        if (!IsValid(obj) || !obj->IsValidLowLevel())
        {
            if (slot.refs > 0)
            {
                // this seems bad, but I'm not sure if there's anything we can do. The docs seem to imply that actors and components specifically can sometimes
                // be destroyed out from under referrers, and so in that case it nulls out UPROPERTY and TWeakObjPtr refs, so we can at least detect it. Still, it
                // seems like if we followed the UE4 GC rules (via FGCObject, flag objects via AddReferencedObjects) then it shouldn't be cleaned up, and yet,
                // it still happens, so the best we've been able to do so far is just sorta handle it gracefully.
#if WITH_EDITOR
                //LOG("TRK PSWR %s : %p - removing tracking slot %llX because obj is invalid even though %d refs remain", *slot.objName, slot.objAddr, it->Key, slot.refs);
#endif
            }
            it.RemoveCurrent();
        }
        else if (slot.refs <= 0)
            it.RemoveCurrent();
        else if (IUEPYGlueMixin *p = Cast<IUEPYGlueMixin>(obj))
        {
            // When we "subclass" an engine object in python, on the C++ side we maintain a hard ref to pyinst, and pyinst maintains a hard ref to engineObj.
            // This creates a reference cycle but is how we coordinate the life cycles between the two GC systems. Once the only remaining reference on the
            // python side is pyinst, we break the cycle by removing the slot from objectMap. At that point, if the engine object is ready to die, UE4 will
            // clean it up, which will in turn decref pyinst, causing it to be cleaned up too.
            if (Py_REFCNT(p->pyInst.ptr()) <= 1)
            {
#if WITH_EDITOR
                //LOG("TRK BR %s : %p %llX - breaking reference cycle because only remaining py ref is self", *slot.objName, slot.objAddr, it->Key);
#endif
                it.RemoveCurrent();
            }
        }
    }

    for (auto it = delegates.CreateIterator(); it ; ++it)
    {
        UBasePythonDelegate* delegate = *it;
        bool stillValid = delegate->valid && delegate->IsValidLowLevel() && !!delegate->engineObj && !!delegate->callbackOwner && Py_REFCNT(delegate->callbackOwner.ptr()) > 1;

        // we can't call IsValidLowLevel on delegate->engineObj because it's not a real (tracked) ref, so it's never safe to call APIs on that object
        // Instead, we ask the engine for the object with the known index - if it's invalid, pending kill, or a different object, we should remove this delegate
        if (stillValid)
        {
            FUObjectItem *cur = GUObjectArray.IndexToObject(delegate->engineObjIndex);
            if (!cur || !cur->Object || cur->IsPendingKill() || cur->Object != delegate->engineObj)
                stillValid = false;
        }

        if (!stillValid)
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
        InCollector.AddReferencedObject(entry.Value.obj);

    // clear out any delegates whose pyinst is not longer valid
    for (UBasePythonDelegate *delegate : delegates)
        InCollector.AddReferencedObject(delegate);

    for (TPair<UMeshComponent*, MaterialArray>& pair : matOverrideMeshComps)
    {
        InCollector.AddReferencedObject(pair.Key);
        for (UMaterialInterface* mat : pair.Value)
            InCollector.AddReferencedObject(mat);
    }
}

// CGLUE implementations. TODO: so much of this should be auto-generated!
AActor_CGLUE::AActor_CGLUE() { PrimaryActorTick.bCanEverTick = true; PrimaryActorTick.bStartWithTickEnabled = false; }
void AActor_CGLUE::BeginPlay() { try { pyInst.attr("BeginPlay")(); } catchpy; }
void AActor_CGLUE::EndPlay(const EEndPlayReason::Type reason) { try { pyInst.attr("EndPlay")((int)reason); } catchpy; }
void AActor_CGLUE::Tick(float dt) { if (PYOK && tickAllowed) try { pyInst.attr("Tick")(dt); } catchpy; }
void AActor_CGLUE::SuperBeginPlay() { Super::BeginPlay(); }
void AActor_CGLUE::SuperEndPlay(EEndPlayReason::Type reason) { Super::EndPlay(reason); }
void AActor_CGLUE::SuperTick(float dt) { Super::Tick(dt); }
void AActor_CGLUE::PostInitializeComponents() { try { pyInst.attr("PostInitializeComponents")(); } catchpy; }
void AActor_CGLUE::GatherCurrentMovement() { if (IsReplicatingMovement()) Super::GatherCurrentMovement(); } // by default, the engine still calls GCM even if not replicating movement

APawn_CGLUE::APawn_CGLUE() { PrimaryActorTick.bCanEverTick = true; PrimaryActorTick.bStartWithTickEnabled = false; }
void APawn_CGLUE::BeginPlay() { try { pyInst.attr("BeginPlay")(); } catchpy; }
void APawn_CGLUE::EndPlay(const EEndPlayReason::Type reason) { try { pyInst.attr("EndPlay")((int)reason); } catchpy; }
void APawn_CGLUE::Tick(float dt) { if (PYOK && tickAllowed) try { pyInst.attr("Tick")(dt); } catchpy; }
void APawn_CGLUE::SuperBeginPlay() { Super::BeginPlay(); }
void APawn_CGLUE::SuperEndPlay(EEndPlayReason::Type reason) { Super::EndPlay(reason); }
void APawn_CGLUE::SuperTick(float dt) { Super::Tick(dt); }
//UPawnMovementComponent* APawn_CGLUE::SuperGetMovementComponent() const { return Super::GetMovementComponent(); }
void APawn_CGLUE::PostInitializeComponents() { try { pyInst.attr("PostInitializeComponents")(); } catchpy; }
void APawn_CGLUE::SuperSetupPlayerInputComponent(UInputComponent* comp) { Super::SetupPlayerInputComponent(comp); }
void APawn_CGLUE::SetupPlayerInputComponent(UInputComponent* comp) { try { pyInst.attr("SetupPlayerInputComponent")(comp); } catchpy; }
void APawn_CGLUE::GatherCurrentMovement() { if (IsReplicatingMovement()) Super::GatherCurrentMovement(); } // by default, the engine still calls GCM even if not replicating movement
void APawn_CGLUE::PossessedBy(AController* c) { Super::PossessedBy(c); try { pyInst.attr("PossessedBy")(c); } catchpy; }
void APawn_CGLUE::UnPossessed() { Super::UnPossessed(); try { pyInst.attr("UnPossessed")(); } catchpy; }
//UPawnMovementComponent* APawn_CGLUE::GetMovementComponent() const { try { return pyInst.attr("GetMovementComponent")().cast<UPawnMovementComponent*>(); } catchpy; return nullptr; }

ACharacter_CGLUE::ACharacter_CGLUE() { PrimaryActorTick.bCanEverTick = true; PrimaryActorTick.bStartWithTickEnabled = false; }
void ACharacter_CGLUE::BeginPlay() { try { pyInst.attr("BeginPlay")(); } catchpy; }
void ACharacter_CGLUE::EndPlay(const EEndPlayReason::Type reason) { try { pyInst.attr("EndPlay")((int)reason); } catchpy; }
void ACharacter_CGLUE::Tick(float dt) { if (PYOK && tickAllowed) try { pyInst.attr("Tick")(dt); } catchpy; }
void ACharacter_CGLUE::SuperBeginPlay() { Super::BeginPlay(); }
void ACharacter_CGLUE::SuperEndPlay(EEndPlayReason::Type reason) { Super::EndPlay(reason); }
void ACharacter_CGLUE::SuperTick(float dt) { Super::Tick(dt); }
void ACharacter_CGLUE::PostInitializeComponents() { try { pyInst.attr("PostInitializeComponents")(); } catchpy; }
void ACharacter_CGLUE::SuperSetupPlayerInputComponent(UInputComponent* comp) { Super::SetupPlayerInputComponent(comp); }
void ACharacter_CGLUE::SetupPlayerInputComponent(UInputComponent* comp) { try { pyInst.attr("SetupPlayerInputComponent")(comp); } catchpy; }
void ACharacter_CGLUE::GatherCurrentMovement() { if (IsReplicatingMovement()) Super::GatherCurrentMovement(); } // by default, the engine still calls GCM even if not replicating movement
void ACharacter_CGLUE::PossessedBy(AController* c) { Super::PossessedBy(c); try { pyInst.attr("PossessedBy")(c); } catchpy; }
void ACharacter_CGLUE::UnPossessed() { Super::UnPossessed(); try { pyInst.attr("UnPossessed")(); } catchpy; }

USceneComponent_CGLUE::USceneComponent_CGLUE() { PrimaryComponentTick.bCanEverTick = true; PrimaryComponentTick.bStartWithTickEnabled = false; }
void USceneComponent_CGLUE::BeginPlay() { try { pyInst.attr("BeginPlay")(); } catchpy; }
void USceneComponent_CGLUE::EndPlay(const EEndPlayReason::Type reason) { try { pyInst.attr("EndPlay")((int)reason); } catchpy; }
void USceneComponent_CGLUE::OnRegister() { try { pyInst.attr("OnRegister")(); } catchpy ; }
void USceneComponent_CGLUE::TickComponent(float dt, ELevelTick type, FActorComponentTickFunction* func) { Super::TickComponent(dt, type, func); if (PYOK && tickAllowed) try { pyInst.attr("TickComponent")(dt, (int)type); } catchpy; }

void UBoxComponent_CGLUE::BeginPlay() { try { pyInst.attr("BeginPlay")(); } catchpy; }
void UBoxComponent_CGLUE::EndPlay(const EEndPlayReason::Type reason) { try { pyInst.attr("EndPlay")((int)reason); } catchpy; }
UBoxComponent_CGLUE::UBoxComponent_CGLUE() { PrimaryComponentTick.bCanEverTick = true; PrimaryComponentTick.bStartWithTickEnabled = false; }
void UBoxComponent_CGLUE::OnRegister() { try { pyInst.attr("OnRegister")(); } catchpy ; }
void UBoxComponent_CGLUE::TickComponent(float dt, ELevelTick type, FActorComponentTickFunction* func) { Super::TickComponent(dt, type, func); if (PYOK && tickAllowed) try { pyInst.attr("TickComponent")(dt, (int)type); } catchpy; }

void UPawnMovementComponent_CGLUE::BeginPlay() { try { pyInst.attr("BeginPlay")(); } catchpy; }
void UPawnMovementComponent_CGLUE::EndPlay(const EEndPlayReason::Type reason) { try { pyInst.attr("EndPlay")((int)reason); } catchpy; }
UPawnMovementComponent_CGLUE::UPawnMovementComponent_CGLUE() { PrimaryComponentTick.bCanEverTick = true; PrimaryComponentTick.bStartWithTickEnabled = false; }
void UPawnMovementComponent_CGLUE::OnRegister() { try { pyInst.attr("OnRegister")(); } catchpy ; }
void UPawnMovementComponent_CGLUE::TickComponent(float dt, ELevelTick type, FActorComponentTickFunction* func) { Super::TickComponent(dt, type, func); if (PYOK && tickAllowed) try { pyInst.attr("TickComponent")(dt, (int)type); } catchpy; }

void UVOIPTalker_CGLUE::OnTalkingBegin(UAudioComponent* AudioComponent) { try { pyInst.attr("OnTalkingBegin")(); } catchpy; }
void UVOIPTalker_CGLUE::OnTalkingEnd() { try { pyInst.attr("OnTalkingEnd")(); } catchpy; }


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

bool _setuprop(FProperty *prop, uint8* buffer, py::object& value, int index)
{
    try
    {
        if (auto bprop = CastField<FBoolProperty>(prop))
            bprop->SetPropertyValue_InContainer(buffer, value.cast<bool>(), index);
        else if (auto fprop = CastField<FFloatProperty>(prop))
            fprop->SetPropertyValue_InContainer(buffer, value.cast<float>(), index);
        else if (auto iprop = CastField<FIntProperty>(prop))
            iprop->SetPropertyValue_InContainer(buffer, value.cast<int>(), index);
        else if (auto ui32prop = CastField<FUInt32Property>(prop))
            ui32prop->SetPropertyValue_InContainer(buffer, value.cast<uint32>(), index);
        else if (auto i64prop = CastField<FInt64Property>(prop))
            i64prop->SetPropertyValue_InContainer(buffer, value.cast<long long>(), index);
        else if (auto iu64prop = CastField<FUInt64Property>(prop))
            iu64prop->SetPropertyValue_InContainer(buffer, value.cast<uint64>(), index);
        else if (auto strprop = CastField<FStrProperty>(prop))
        {
            std::string t = value.cast<std::string>();
            strprop->SetPropertyValue_InContainer(buffer, FSTR(t), index);
        }
        else if (auto textprop = CastField<FTextProperty>(prop))
        {
            std::string t = value.cast<std::string>();
            FString f = FSTR(t);
            textprop->SetPropertyValue_InContainer(buffer, FText::FromString(f), index);
        }
        else if (auto byteprop = CastField<FByteProperty>(prop))
            byteprop->SetPropertyValue_InContainer(buffer, value.cast<int>(), index);
        else if (auto enumprop = CastField<FEnumProperty>(prop))
            enumprop->GetUnderlyingProperty()->SetIntPropertyValue(enumprop->ContainerPtrToValuePtr<void>(buffer, index), value.cast<uint64>());
        else if (auto classprop = CastField<FClassProperty>(prop))
            classprop->SetPropertyValue_InContainer(buffer, value.cast<UClass*>(), index);
        else if (auto objprop = CastField<FObjectProperty>(prop))
            objprop->SetObjectPropertyValue_InContainer(buffer, value.cast<UObject*>(), index);
        else if (auto arrayprop = CastField<FArrayProperty>(prop))
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
        else if (auto structprop = CastField<FStructProperty>(prop))
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
void SetObjectProperty(UObject *obj, std::string k, py::object& value)
{
    FName propName = FSTR(k);
    FProperty* prop = obj->GetClass()->FindPropertyByName(propName);
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
if (auto t##enginePropClass = CastField<enginePropClass>(prop))\
{\
    cppType ret = t##enginePropClass->GetPropertyValue_InContainer(buffer, index);\
    return py::cast(ret);\
}

py::object _getprop(FProperty *prop, uint8* buffer, int index)
{
    _GETPROP(FBoolProperty, bool);
    _GETPROP(FFloatProperty, float);
    _GETPROP(FIntProperty, int);
    _GETPROP(FUInt32Property, uint32);
    _GETPROP(FInt64Property, long long);
    _GETPROP(FUInt64Property, uint64);
    _GETPROP(FByteProperty, int);
    _GETPROP(FObjectProperty, UObject*);
    if (auto strprop = CastField<FStrProperty>(prop))
    {
        FString sret = strprop->GetPropertyValue_InContainer(buffer, index);
        std::string ret = TCHAR_TO_UTF8(*sret);
        return py::cast(ret);
    }
    if (auto textprop = CastField<FTextProperty>(prop))
    {
        FText tret = textprop->GetPropertyValue_InContainer(buffer, index);
        std::string ret = TCHAR_TO_UTF8(*tret.ToString());
        return py::cast(ret);
    }
    if (auto enumprop = CastField<FEnumProperty>(prop))
    {
        void* prop_addr = enumprop->ContainerPtrToValuePtr<void>(buffer, index);
        uint64 v = enumprop->GetUnderlyingProperty()->GetUnsignedIntPropertyValue(prop_addr);
        return py::cast(v);
    }
    if (auto classprop = CastField<FClassProperty>(prop))
    {
        UClass *klass = Cast<UClass>(classprop->GetPropertyValue_InContainer(buffer, index));
        return py::cast(klass);
    }
    if (auto arrayprop = CastField<FArrayProperty>(prop))
    {
        FScriptArrayHelper_InContainer helper(arrayprop, buffer, index);
        py::list ret;
        for (int i=0; i < helper.Num(); i++)
            ret.append(_getprop(arrayprop->Inner, helper.GetRawPtr(i), 0));
        return ret;
    }

    // For structures, we handle some specific builtin ones below and then leave it up to the game code to handle
    // any others.
    if (auto structprop = CastField<FStructProperty>(prop))
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
py::object GetObjectProperty(UObject *obj, std::string k)
{
    FName propName = FSTR(k);
    FProperty* prop = obj->GetClass()->FindPropertyByName(propName);
    if (!prop)
    {
        LERROR("Failed to find property %s on object %s", *propName.ToString(), *obj->GetName());
        return py::none();
    }

    return _getprop(prop, (uint8*)obj, 0);
}

// calls a UFUNCTION on an object and returns the result
// NOTE: for now (and maybe forever) there are many, many restrictions on what does and doesn't work here!
// Allowed:
// - 0 or more parameters, all types must be supported by _setuprop
// - 0 or 1 return parameter, type must be supported by _getprop
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
    // by keeping things really simple. AFAICT it just uses the same FProperty approach as it uses with
    // getting/setting UPROPERTYs, so we allocate some space to hold the properties to pass in and then populate
    // them using the args tuple passed in from Python.
    uint8* propArgsBuffer = (uint8*)FMemory_Alloca(func->ParmsSize);
    FMemory::Memzero(propArgsBuffer, func->ParmsSize);
    FProperty *returnProp = nullptr;
    int nextPyArg = 0;
    int numPyArgs = pyArgs.size();
    for (TFieldIterator<FProperty> iter(func); iter ; ++iter)
    {
        FProperty *prop = *iter;
        if (!prop->HasAnyPropertyFlags(CPF_Parm))
            continue;
        if (prop->HasAnyPropertyFlags(CPF_OutParm) && !prop->HasAnyPropertyFlags(CPF_ConstParm|CPF_ReferenceParm))//ReturnParm)) <-- this doesn't quite work: if a function param is e.g. an array of strings, it'll have the OutParm flag. But if we just check for Return, then other function calls fail.
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
        ret = _getprop(returnProp, propArgsBuffer, 0);

    // Cleanup
    for (TFieldIterator<FProperty> iter(func) ; iter ; ++iter)
    {
        FProperty *prop = *iter;
        if (iter->HasAnyPropertyFlags(CPF_Parm))
            prop->DestroyValue_InContainer(propArgsBuffer);
    }

    return ret;
}

// generic binding of a python callback function to a multicast script delegate
void BindDelegateCallback(UObject *obj, std::string _eventName, py::object& callback)
{
    FName eventName = FSTR(_eventName);
    FProperty* prop = obj->GetClass()->FindPropertyByName(eventName);
    if (!prop)
    {
        LERROR("Failed to find property %s on object %s", *eventName.ToString(), *obj->GetName());
    }

    FMulticastDelegateProperty *mcprop = CastField<FMulticastDelegateProperty>(prop);
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
    if (!VALID(obj))
    {
        LERROR("Cannot unbind %s on invalid object", *eventName.ToString());
        return;
    }

    FProperty* prop = obj->GetClass()->FindPropertyByName(eventName);
    if (!prop)
    {
        LERROR("Failed to find property %s on object %s", *eventName.ToString(), *obj->GetName());
        return;
    }

    FMulticastDelegateProperty *mcprop = CastField<FMulticastDelegateProperty>(prop);
    if (!mcprop)
    {
        LERROR("Property %s is not a multicast delegate on object %s", *eventName.ToString(), *obj->GetName());
        return;
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
    else
    {
        LWARN("Failed to unbind %s %s", *obj->GetName(), *eventName.ToString());
    }
}

void BroadcastEvent(UObject* obj, std::string eventName, py::tuple& args)
{
    FName propName = FSTR(eventName);
    FProperty* prop = obj->GetClass()->FindPropertyByName(propName);
    if (!prop)
    {
        LERROR("Failed to find property %s on object %s", *propName.ToString(), *obj->GetName());
        return;
    }

    FMulticastInlineDelegateProperty *delprop = CastField<FMulticastInlineDelegateProperty>(prop);
    if (!delprop)
    {
        LERROR("Property %s on object %s is not a multicast delegate property", *propName.ToString(), *obj->GetName());
        return;
    }

    FMulticastScriptDelegate delegate = delprop->GetPropertyValue_InContainer(obj);
    uint8* propArgsBuffer = (uint8*)FMemory_Alloca(delprop->SignatureFunction->PropertiesSize);
    FMemory::Memzero(propArgsBuffer, delprop->SignatureFunction->PropertiesSize);

    FProperty *returnProp = nullptr;
    int nextPyArg = 0;
    int numPyArgs = args.size();
    for (TFieldIterator<FProperty> iter(delprop->SignatureFunction); iter ; ++iter)
    {
        FProperty *p = *iter;
        if (!p->HasAnyPropertyFlags(CPF_Parm))
            continue;
        if (p->HasAnyPropertyFlags(CPF_OutParm))//ReturnParm))
            continue; // these shouldn't exist on a delegate event, right??

        // convert the python value to a UProp value
        if (nextPyArg >= numPyArgs)
        {
            LERROR("Not enough arguments in call to %s", *propName.ToString());
            // TODO: we're probably leaking memory here (see cleanup code below)
            return;
        }

        py::object arg = args[nextPyArg++];
        if (!_setuprop(p, propArgsBuffer, arg, 0))
        {
            LERROR("Failed to convert Python arg %d in call to %s:", nextPyArg-1, *propName.ToString());
            return;
        }
    }

    delegate.ProcessMulticastDelegate<UObject>(propArgsBuffer);

    // Cleanup
    for (TFieldIterator<FProperty> iter(delprop->SignatureFunction) ; iter ; ++iter)
    {
        FProperty *p = *iter;
        if (iter->HasAnyPropertyFlags(CPF_Parm))
            p->DestroyValue_InContainer(propArgsBuffer);
    }

}

void UBackgroundWorker::Setup(py::object& callback)
{
    check(IsInGameThread());
    cb = callback;
    BindDelegateCallback(this, "TheEvent", callback);
    AddToRoot(); // force it to stay alive until its work is done
}

void UBackgroundWorker::Cleanup()
{
    check(IsInGameThread());
    UnbindDelegateCallback(this, "TheEvent", cb);
    RemoveFromRoot();
}

//#pragma optimize("", on)

