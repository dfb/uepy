#include "INRActorMixin.h"
#include "uepy.h"
#include "common.h"
#include "INRPlayerControllerMixin.h"
#include "Engine/PackageMapClient.h"

extern py::object GetPyClassFromName(FString& name); // defined in mod_uepy.cpp for now - TODO: clean this up

// In single player mode, we don't have a UNetDriver, so we don't have a FNetGuidCache for mapping between objects
// and network GUIDs, which means NRCalls with parameters that are UObject* don't work, even if we're just sending
// the message locally.
// TODO: we just sorta let the dummy cache grow without bounds - it shouldn't ever get /too/ big but still...
static TMap<FNetworkGUID,TWeakObjectPtr<UObject>>* g2o = nullptr;
static TMap<TWeakObjectPtr<UObject>,FNetworkGUID>* o2g = nullptr;
void _EnsureFakeGUIDCache()
{
    if (!g2o) g2o = new TMap<FNetworkGUID,TWeakObjectPtr<UObject>>();
    if (!o2g) o2g = new TMap<TWeakObjectPtr<UObject>,FNetworkGUID>();
}

UObject* NRGetObjectFromNetGUID(UNetDriver *driver, FNetworkGUID& g)
{
    if (driver)
        return driver->GuidCache->GetObjectFromNetGUID(g, false); // TODO: not sure what bIgnoreMustBeMapped is for

    _EnsureFakeGUIDCache();
    TWeakObjectPtr<UObject>* oPtr = g2o->Find(g);
    if (!oPtr)
        return nullptr;
    UObject* obj = oPtr->Get();
    if (!obj || !obj->IsValidLowLevel())
        return nullptr;
    return obj;
}

FNetworkGUID NRGetOrAssignNetGUID(UNetDriver* driver, UObject* obj)
{
    if (driver)
        return driver->GuidCache->GetOrAssignNetGUID(obj);
    if (!obj || !obj->IsValidLowLevel())
        return FNetworkGUID();

    _EnsureFakeGUIDCache();
	TWeakObjectPtr<UObject> oPtr = MakeWeakObjectPtr(obj);
    FNetworkGUID* gPtr = o2g->Find(oPtr);
    if (gPtr)
        return *gPtr;

    // create a new guid and add to both maps
    FNetworkGUID g(o2g->Num()+1);
    g2o->Emplace(g, oPtr);
    o2g->Emplace(oPtr, g);
    return g;
}

// type codes: short strings (often single char) that are used to send data type information across the wire.
// At some point we may allow application-level code to register additional types. Each type code needs to be
// handled in TypeCodeFor, CoerceValue, MarshalPyObject, and UnmarshalPyObject.

FString TypeCodeFor(py::object& value, bool isSpecial=false)
{
    // TODO: needs support for dynamically registered (custom) types

    if (isSpecial)
    {   // see uepy/__init__.py's SPECIAL_REP_PROPS for these
        std::string _s = value.cast<std::string>();
        FString s = FSTR(_s);
        if (s == TEXT("__empty_uclass__")) return TEXT("C");
        if (s == TEXT("__empty_uobject__")) return TEXT("O");
        if (s == TEXT("__empty_pyuobject__")) return TEXT("P");
        LERROR("Unhandled special case: %s", *s);
        return TEXT("");
    }
    else if (py::hasattr(value, "engineObj"))
    {   // special case - if it's an engine object that has been subclasssed in Python, we pass it across
        // the wire the same way as with UObject pointers, but use a different type code so that the receiving
        // end knows to grab the pyInst before returning the object to the client.
        return TEXT("P");
    }
    else if (py::isinstance<UObject*>(value))
        return TEXT("O");

    // Note: if you add a type code here, be sure it is unique!
    if (py::isinstance<py::float_>(value)) return TEXT("F");
    if (py::isinstance<py::int_>(value)) return TEXT("I");
    if (py::isinstance<py::bool_>(value)) return TEXT("B");
    if (py::isinstance<py::bytes>(value)) return TEXT("by");
    if (py::isinstance<py::str>(value)) return TEXT("S");
    if (py::isinstance<FVector>(value)) return TEXT("V");
    if (py::isinstance<FVector2D>(value)) return TEXT("V2");
    if (py::isinstance<FRotator>(value)) return TEXT("R");
    if (py::isinstance<FQuat>(value)) return TEXT("Q");
    if (py::isinstance<FLinearColor>(value)) return TEXT("LC");
    if (py::isinstance<FTransform>(value)) return TEXT("T");
    if (PyObjectToUClass(value)) return TEXT("C");

    return TEXT(""); // unsupported type
}

// Given a registered type code, coerces a Python object to a Python object
// of the desired type, returning true on success. Note that structs or other objects
// are *NOT* coerced; the coercion is merely to handle cases where a caller
// has provided a legal alternative (e.g. we're expecting a float and the caller
// provided an int). For other types, logs an error and returns false if the input
// is not of the correct type already.
bool CoerceValue(FString& typeCode, py::object& inValue, py::object& outValue)
{
    try {
        // coercible types here
        if (typeCode == TEXT("F")) outValue = py::cast<py::float_>(inValue);
        else if (typeCode == TEXT("I")) outValue = py::cast<py::int_>(inValue);
        else if (typeCode == TEXT("B")) outValue = py::cast<py::bool_>(inValue);
        else if (typeCode == TEXT("by")) outValue = py::cast<py::bytes>(inValue);
        else if (typeCode == TEXT("S")) outValue = py::cast<py::str>(inValue);

        // all others here
        else if ((typeCode == TEXT("V") && !py::isinstance<FVector>(inValue)) ||
                 (typeCode == TEXT("V2") && !py::isinstance<FVector2D>(inValue)) ||
                 (typeCode == TEXT("R") && !py::isinstance<FRotator>(inValue)) ||
                 (typeCode == TEXT("Q") && !py::isinstance<FQuat>(inValue)) ||
                 (typeCode == TEXT("LC") && !py::isinstance<FLinearColor>(inValue)) ||
                 (typeCode == TEXT("O") && !py::isinstance<UObject*>(inValue)) ||
                 (typeCode == TEXT("T") && !py::isinstance<FTransform>(inValue)))
        {
            std::string s = py::repr(inValue);
            FString ss = FSTR(s);
            LERROR("Input value %s is of the wrong type", *ss);
            return false;
        }
        else
            outValue = inValue;
        return true;
    } catch (std::exception e)
    {
        LERROR("%s", UTF8_TO_TCHAR(e.what()));
        return false;
    }
}

const uint16 END_OF_PROPS = 65535; // not allowed to have this many properties (note that this includes dynamically-allocated properties, e.g. for UI replication)

// adds a new entry to the list of replicated props
bool FNRPropHolder::AddProperty(FString name, py::object& defaultValue, bool isSpecial)
{
    // reject if it's a known ID
    uint16* idPtr = namesToIDs.Find(name);
    if (idPtr)
    {
        LERROR("Prop holder already has property %s", *name);
        return false;
    }

    FString typeCode = TypeCodeFor(defaultValue, isSpecial);
    if (typeCode == "")
    {
        LERROR("Unsupported property type for %s", *name);
        return false;
    }

    uint16 nextPropID = values.Num(); // each "ID" is just its index
    if (nextPropID == END_OF_PROPS)
    {
        LERROR("Too many replicated properties, are you crazy?");
        return false;
    }

    // for now at least, all the special cases are where the default value is null/None so there's no way to infer
    // the type, so the default value is the name of the special case, which TypeCodeFor has already handled, so now
    // we can safely set it to the true default of None.
    if (isSpecial)
        defaultValue = py::none();

    namesToIDs.Add(name, nextPropID);
    names.Emplace(name);
    values.Emplace(defaultValue); // TODO: should we make a copy of this object?
    defaults.Emplace(defaultValue);
    initOverrides.Emplace(false);
    typeCodes.Emplace(typeCode);
    return true;
}

bool FNRPropHolder::InitSetProperty(FString name, py::object& value)
{
    int propID = GetPropertyID(name);
    if (propID == -1)
    {
        LERROR("Prop holder has no property %s", *name);
        return false;
    }

    FString typeCode = typeCodes[propID];
    py::object finalValue;
    if (!CoerceValue(typeCode, value, finalValue))
    {
        LERROR("Property value for %s is of the wrong type", *name);
        return false;
    }

    values[propID] = finalValue;
    initOverrides[propID] = true;
    return true;
}

int FNRPropHolder::GetPropertyID(FString name)
{
    uint16 *idPtr = namesToIDs.Find(name);
    if (!idPtr)
        return -1;
    return (int)(*idPtr);
}

py::object FNRPropHolder::GetValue(FString name)
{
    int index = GetPropertyID(name);
    if (index == -1)
    {
        LERROR("Unknown property %s", *name);
        return py::none();
    }
    return values[index];
}

// Given a Python object and its typecode, writes it to the given archive
bool MarshalPyObject(UNetDriver *driver, FString& typeCode, py::object& obj, FArchive& ar)
{
    if (typeCode == TEXT("F"))
    {
        float f = obj.cast<float>();
        ar << f;
    }
    else if (typeCode == TEXT("I"))
    {
        int i = obj.cast<int>();
        ar << i;
    }
    else if (typeCode == TEXT("B"))
    {
        bool b = obj.cast<bool>();
        ar << b;
    }
    else if (typeCode == TEXT("by"))
    {   // not sure if there's a better way to do this...
        py::bytes pybytes = obj.cast<py::bytes>();
        char *buff;
        ssize_t buffLen;
        PYBIND11_BYTES_AS_STRING_AND_SIZE(pybytes.ptr(), &buff, &buffLen);
        TArray<uint8> bytes;
        bytes.AddUninitialized(buffLen);
        memcpy(bytes.GetData(), buff, buffLen);
        ar << bytes;
    }
    else if (typeCode == TEXT("S"))
    {
        FString s = FSTR(obj.cast<std::string>());
        ar << s;
    }
    else if (typeCode == TEXT("R"))
    {
        FRotator r = obj.cast<FRotator>();
        ar << r;
    }
    else if (typeCode == TEXT("Q"))
    {
        FQuat q = obj.cast<FQuat>();
        ar << q;
    }
    else if (typeCode == TEXT("V"))
    {
        FVector v = obj.cast<FVector>();
        ar << v;
    }
    else if (typeCode == TEXT("V2"))
    {
        FVector2D v = obj.cast<FVector2D>();
        ar << v;
    }
    else if (typeCode == TEXT("LC"))
    {
        FLinearColor c = obj.cast<FLinearColor>();
        ar << c;
    }
    else if (typeCode == TEXT("T"))
    {
        FTransform t = obj.cast<FTransform>();
        ar << t;
    }
    else if (typeCode == TEXT("C"))
    {   // we marshall classes as strings because they aren't replicated per se (both sides already know about them)
        FString className = "";
        UClass *engineClass = PyObjectToUClass(obj);
        if (engineClass)
            className = engineClass->GetName();
        ar << className;
    }
    else if (typeCode == TEXT("O") || typeCode == TEXT("P"))
    {
        if (obj.is_none())
        {
            FNetworkGUID nil(0);
            ar << nil;
        }
        else
        {
            py::object actualObj = obj;
            if (typeCode == TEXT("P"))
            {
                try {
                    actualObj = obj.attr("engineObj");
                } catch (std::exception e)
                {
                    LERROR("Cannot marshal object with typeCode P because it has no engineObj: %s", UTF8_TO_TCHAR(e.what()));
                    return false;
                }
            }
            UObject* engineObj = actualObj.cast<UObject*>();
            FNetworkGUID objID = NRGetOrAssignNetGUID(driver, engineObj);
            ar << objID;
        }
    }
    else
    {
        std::string s = py::repr(obj);
        LERROR("Failed to marshal object %s of type %s", FSTR(s), *typeCode);
        return false;
    }
    return true;
    // TODO: quat, lists of uniform types (all float, all int, etc.), custom types
    // done: F I B S V V2 R LC T O
}

// Called by LLNRCall - given a tuple, marshalls it to binary data in the given blob, and returns
// the detected type info in a comma-separated string. For awhile I thought about requiring type annotation info
// in the method being called, but that makes it harder to use this stuff from C++, should we ever need to do that.
// Also for now we're supporting *args but not **kwargs, though there's no reason we couldn't add that if we wanted to.
void TupleToBlob(UWorld *world, py::tuple args, TArray<uint8>& blob, FString& formatStr)
{
    TArray<FString> typeInfo;
    FMemoryWriter writer(blob);
    UNetDriver *driver = world->GetNetDriver();
    for (auto h : args)
    {
        py::object arg = h.cast<py::object>();
        FString typeCode = TypeCodeFor(arg);
        typeInfo.Add(typeCode);
        if (!MarshalPyObject(driver, typeCode, arg, writer))
        {
            std::string s = py::repr(arg);
            LERROR("Failed to marshal object %s of type %s", FSTR(s), *typeCode);
            return; // LOL
        }
    }

    formatStr = FString::Join(typeInfo, TEXT(","));
}

// the official way to trigger OnReplicated - just makes sure it happens only once
void INRActorMixin::_CallOnReplicated()
{
    check(!onReplicatedCalled);
    onReplicatedCalled = true;
    OnReplicated();
}

// called from subclass BeginPlay methods. TODO: it'd be nice to not need this, especially since
// it seems like something really easy to forget, but see the note for RegisterProps.
void INRActorMixin::NRNoteBeginPlay()
{
    beginPlayCalled = true;
    UNetDriver *driver = Cast<AActor>(this)->GetWorld()->GetNetDriver();
    bool isHost = (driver && driver->GetNetMode() != ENetMode::NM_Client);
    if (isHost || !driver) // if there's no driver, we're in single player mode
    {   // on the host, everything is by definition up to date, so we can immediately
        // proceed to call OnReplicated.
        initialStateReplicated = true;
        _CallOnReplicated();
    }
    else if (initialStateReplicated)
        _CallOnReplicated();
}

// Called once an object is done calling AddProperty. When called from the host, triggers a notification
// to all clients so that they can detect when initial replication is complete. For actors spawned once all
// clients have joined, this initial call sends only properties with overwritten defaults (properties for which
// InitSetProperty has been called) because all copies of the actors will already have the correct initial values
// for all other properties (all other properties will just be at their default values, which all copies of the
// object know about). For cases where a client joins sometime after the start of play, however, we don't want
// the replicated actor to really "start" until it has received its state at that point in time, otherwise the client
// could have strange behavior until (and while) properties are replicated. In the scenario of a late joiner, when the
// NRChannel is opened, it calls all INRActorMixin actors' GenChannelReplicationPayload method to get their current
// state and sends it to the client with the isInitialState flag set just like it is in RegisterProps, and then after
// the data has arrived, OnReplicated is called on each client.
void INRActorMixin::NRRegisterProps()
{
    UWorld* world = Cast<AActor>(this)->GetWorld();
    if (!world)
    {
        LERROR("No world - was this called on the CDO?");
        return;
    }

    spawnTS = world->GetRealTimeSeconds();

    // trigger an NRUpdate call to everywhere for the initial spawn replication. But, since all copies of the actor
    // will have the same defaults, just send any properties that have had their default value overwritten.
    UNetDriver *driver = Cast<AActor>(this)->GetWorld()->GetNetDriver();
    bool isHost = (driver && driver->GetNetMode() != ENetMode::NM_Client);
    ENRWhere flags = ENRWhere::Nowhere;
    if (isHost)
        flags = ENRWhere::Owner | ENRWhere::NonOwners; // everywhere
    else if (!driver)
        flags = ENRWhere::Local; // we're in single player mode

    // ugly hack: if the game state implements INRActorMixin, it will be spawned prior to an player controllers
    // existing, so the NRUpdate call will fail.
    if (!world->GetFirstPlayerController())
    {
        flags = ENRWhere::Nowhere;
        initialStateReplicated = true;
        if (beginPlayCalled)
        {
            LOG("WARNING: %s ugly hack calling OnReplicated", *Cast<AActor>(this)->GetName());
            _CallOnReplicated();
        }
    }

    if (flags != ENRWhere::Nowhere)
    {
        py::dict props;
        for (uint16 propID=0; propID < (uint16)repProps.names.Num(); propID++)
        {
            if (!repProps.initOverrides[propID])
                continue;
            std::string key = PYSTR(repProps.names[propID]);
            props[key.c_str()] = repProps.values[propID];
        }
        NRUpdate(flags, true, props);
    }
}

// the internal signature we use for any NRUpdate calls
const FString NR_UPDATE_SIG("__nrupdate__");

// Special method called by NRChannel on the host when it needs to replicate current actor state to a player that
// is joining a session that is already underway. Passes out the signature to use and fills the payload buffer
void INRActorMixin::GenChannelReplicationPayload(UNetDriver* driver, FString& signature, TArray<uint8>& payload)
{
    signature = NR_UPDATE_SIG;
    FMemoryWriter writer(payload);
    uint8 isInitialState = 1; // a bool takes up 4 byte on the wire for some reason
    writer << isInitialState;

    // include any properties whose values are different than their defaults
    for (uint16 propID=0; propID < (uint16)repProps.names.Num(); propID++)
    {
        py::object& def = repProps.defaults[propID];
        py::object& cur = repProps.values[propID];
        if (!def.equal(cur))
        {
            writer << propID;
            MarshalPyObject(driver, repProps.typeCodes[propID], cur, writer);
        }
    }

    // mark the end of the properties list
    uint16 eop = END_OF_PROPS;
    writer << eop;
}

// Triggers replicated variables to be updated on this actor on various machines in the session. where should almost always
// include Local and Host flags, though maybe there's some weird special case where it makes sense for them to not be set.
void INRActorMixin::NRUpdate(ENRWhere where, bool isInitialState, py::dict& kwargs, bool reliable, float maxCallsPerSec)
{
    // Sometimes it's convenient to have an Actor class that has INRActorMixin in its class hierarchy even though it's
    // not replicated
    AActor *self = Cast<AActor>(this);
    if (!self->GetIsReplicated())
        where = ENRWhere::Local;

    // TODO: should we skip properties who are being updated but the value isn't changing?

    UNetDriver *driver = self->GetWorld()->GetNetDriver();
    TArray<uint8> payload;
    FMemoryWriter writer(payload);
    uint8 iis = (uint8)isInitialState;
    writer << iis;
    for (auto item : kwargs)
    {
        // find the property's index
        std::string _propName = item.first.cast<std::string>();
        FString propName = FSTR(_propName);
        py::object propValue = py::cast<py::object>(item.second);
        uint16 propID = repProps.GetPropertyID(propName);
        if (propID == END_OF_PROPS)
        {
            LERROR("Unknown property %s", *propName);
            return;
        }
        writer << propID;

        // make sure the value is of the correct type, converting it if we can
        py::object finalPropValue;
        FString typeCode = repProps.typeCodes[propID];
        if (!CoerceValue(typeCode, propValue, finalPropValue))
        {
            LERROR("Property value for %s is of the wrong type", *propName);
            return;
        }
        if (!MarshalPyObject(driver, typeCode, finalPropValue, writer))
        {
            LERROR("Failed to marshal property %s", *propName);
            return;
        }
    }

    // mark the end of the properties list - marking the end of the list probably wastes a byte (versus a numProps byte at
    // the beginning) but it's far more convienent, especially in the case where GenChannelReplicationPayload is generating
    // this same message
    uint16 eop = END_OF_PROPS;
    writer << eop;

    // NRUpdate reuses the NRCall machinery and sets the Internal flag
    NRCall(where|ENRWhere::Internal, Cast<AActor>(this), NR_UPDATE_SIG, payload, reliable, maxCallsPerSec);
}

// Reads a python object of the given type code out of the given archive, storing it in the
// given obj out parameter. Returns false on failure.
bool UnmarshalPyObject(UNetDriver *driver, FString& typeCode, FArchive& ar, py::object& obj)
{
    if (typeCode == TEXT("F"))
    {
        float f;
        ar << f;
        obj = py::cast(f);
    }
    else if (typeCode == TEXT("I"))
    {
        int i;
        ar << i;
        obj = py::cast(i);
    }
    else if (typeCode == TEXT("B"))
    {
        bool b;
        ar << b;
        obj = py::cast(b);
    }
    else if (typeCode == TEXT("by"))
    {
        TArray<uint8> bytes;
        ar << bytes;
        obj = py::bytes((char*)bytes.GetData(), bytes.Num());
    }
    else if (typeCode == TEXT("S"))
    {
        FString s;
        ar << s;
        obj = py::cast(PYSTR(s));
    }
    else if (typeCode == TEXT("R"))
    {
        FRotator r;
        ar << r;
        obj = py::cast(r);
    }
    else if (typeCode == TEXT("Q"))
    {
        FQuat q;
        ar << q;
        obj = py::cast(q);
    }
    else if (typeCode == TEXT("V"))
    {
        FVector v;
        ar << v;
        obj = py::cast(v);
    }
    else if (typeCode == TEXT("V2"))
    {
        FVector v2;
        ar << v2;
        obj = py::cast(v2);
    }
    else if (typeCode == TEXT("LC"))
    {
        FLinearColor c;
        ar << c;
        obj = py::cast(c);
    }
    else if (typeCode == TEXT("T"))
    {
        FTransform t;
        ar << t;
        obj = py::cast(t);
    }
    else if (typeCode == TEXT("C"))
    {   // class objects are marshaled as strings (their full class name). Upon reception, we try to find a Python
        // subclass of that name before falling back to an engine class of that name
        FString className;
        ar << className;
        obj = GetPyClassFromName(className);
        if (obj.is_none())
        {
            UClass *engineClass = FindObject<UClass>(ANY_PACKAGE, *className);
            if (!engineClass)
            {
                LERROR("Failed to unmarshal class %s", *className);
                return false;
            }
            obj = py::cast(engineClass);
        }
    }
    else if (typeCode == TEXT("O") || typeCode == TEXT("P"))
    {
        FNetworkGUID g;
        ar << g;
        if (g.Value == 0)
        {   // just passing None/null
            obj = py::none();
            return true;
        }

        UObject *engineObj = NRGetObjectFromNetGUID(driver, g);

        if (!engineObj || !engineObj->IsValidLowLevel())
        {
            LERROR("Unmarshalled object not found for netguid %d", g.Value);
            return false;
        }
        else
        {
            if (typeCode == TEXT("P"))
            {
                IUEPYGlueMixin *p = Cast<IUEPYGlueMixin>(engineObj);
                if (!p)
                {
                    LERROR("Unmarshalled object of type P is not a IEUPYGlueMixin instance");
                    return false;
                }
                obj = p->pyInst;
            }
            else
            {   // todo: in the case of a UClass, we might want to see if we can get the python subclass, if there is one
                obj = py::cast(engineObj);
            }
        }
    }
    else
    {
        LERROR("Unexpected type code %s", *typeCode);
        return false;
    }
    return true;
}

// NRUpdate is built on top of NRCall and just has an "is internal" flag set in the message. When variables are being
// replicated, NRCall delivers it to each client, and then instead of the call being routed to OnNRUpdate, it is routed
// here instead. If we later need to have additional internal messages, support for them could be added here.
void INRActorMixin::OnInternalNRCall(FString signature, TArray<uint8>& payload)
{
    if (signature != NR_UPDATE_SIG)
    {
        LERROR("Unable to handle internal call for %s", *signature);
        return;
    }

    // unmarshal the message and update affected properties
    UNetDriver *driver = Cast<AActor>(this)->GetWorld()->GetNetDriver();
    FMemoryReader reader(payload);
    uint8 isInitialState;
    reader << isInitialState;
    TArray<FString> modifiedPropNames;
    /*
    if (isInitialState)
    {
        AActor *self = Cast<AActor>(this);
        FNetworkGUID g = NRGetOrAssignNetGUID(driver, self);
        LOG("Received initial state for %s (fng %d)", *self->GetName(), g.Value);
    }
    */

    while (1)
    {
        uint16 propID;
        reader << propID;
        if (propID == END_OF_PROPS)
            break; // reached the end

        if (propID >= repProps.names.Num())
        {
            LERROR("Invalid property ID %d (have %d)", propID, repProps.names.Num());
            return;
        }

        modifiedPropNames.Emplace(repProps.names[propID]);
        FString typeCode = repProps.typeCodes[propID];
        py::object propValue;
        if (!UnmarshalPyObject(driver, typeCode, reader, propValue))
        {
            LERROR("Failed to unmarshall property %d (type %s)", propID, *typeCode);
            return;
        }

        // save the value. The python side has its own ref to repProps, so saving the value here
        // also means it's updated from the perspective of any Python code.
        repProps.values[propID] = propValue;
    }

    if (!initialStateReplicated)
    {   // this is the initial state for this actor being replicated, either because it just spawned or because
        // we are client that just joined an existing session
        if (!isInitialState)
        {   // well, this is weird.
            LERROR("Initial state hasn't been replicated yet, but received a non-initial state replication message");
            // uh... I guess just fall through?
        }

        // if BeginPlay has already been called, then we've just been waiting on this message to trigger OnReplicated.
        // Otherwise, flag that we've gotten this far so that when BeginPlay does eventually get called, OnReplicated
        // will then be called.
        initialStateReplicated = true;
        if (beginPlayCalled)
            _CallOnReplicated();
    }
    else
    {   // this isn't the initial blob of replicated variable info, but is instead some later update, so just let the
        // application code know that properties have been updated
        OnNRUpdate(modifiedPropNames);
    }
}

