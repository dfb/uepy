#include "INRActorMixin.h"
#include "Engine/NetDriver.h"
#include "common.h"
#include "INRPlayerControllerMixin.h"
#include "Engine/PackageMapClient.h"

// type codes: short strings (often single char) that are used to send data type information across the wire.
// At some point we may allow application-level code to register additional types. Each type code needs to be
// handled in TypeCodeFor, CoerceValue, MarshalPyObject, and UnmarshalPyObject.

FString TypeCodeFor(py::object& value)
{
    // TODO: needs support for dynamically registered (custom) types

    // Note: if you add a type code here, be sure it is unique!
    if (py::isinstance<py::float_>(value)) return TEXT("F");
    if (py::isinstance<py::int_>(value)) return TEXT("I");
    if (py::isinstance<py::bool_>(value)) return TEXT("B");
    if (py::isinstance<py::str>(value)) return TEXT("S");
    if (py::isinstance<FVector>(value)) return TEXT("V");
    if (py::isinstance<FVector2D>(value)) return TEXT("V2");
    if (py::isinstance<FRotator>(value)) return TEXT("R");
    if (py::isinstance<FLinearColor>(value)) return TEXT("LC");
    if (py::isinstance<UObject*>(value)) return TEXT("O");
    if (py::isinstance<FTransform>(value)) return TEXT("T");
    return TEXT("");
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
        else if (typeCode == TEXT("S")) outValue = py::cast<py::str>(inValue);

        // all others here
        else if ((typeCode == TEXT("V") && !py::isinstance<FVector>(inValue)) ||
                 (typeCode == TEXT("V2") && !py::isinstance<FVector2D>(inValue)) ||
                 (typeCode == TEXT("R") && !py::isinstance<FRotator>(inValue)) ||
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
bool FNRPropHolder::AddProperty(FString name, py::object& defaultValue)
{
    // reject if it's a known ID
    uint16* idPtr = namesToIDs.Find(name);
    if (idPtr)
    {
        LERROR("Prop holder already has property %s", *name);
        return false;
    }

    FString typeCode = TypeCodeFor(defaultValue);
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
    namesToIDs.Add(name, nextPropID);
    names.Emplace(name);
    values.Emplace(defaultValue); // TODO: should we make a copy of this object?
    defaults.Emplace(defaultValue);
    typeCodes.Emplace(typeCode);
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
    else if (typeCode == TEXT("O"))
    {
        if (!driver)
        {
            LERROR("No net driver available to marshall UObject");
            return false;
        }
        else
        {
            UObject* engineObj = obj.cast<UObject*>();
            FNetworkGUID objID = driver->GuidCache->GetOrAssignNetGUID(engineObj);
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
void INRActorMixin::NoteBeginPlay()
{
    beginPlayCalled = true;
    UNetDriver *driver = Cast<AActor>(this)->GetWorld()->GetNetDriver();
    bool isHost = (driver && driver->GetNetMode() != ENetMode::NM_Client);
    if (isHost)
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
// clients have joined, this initial call seems somewhat goofy since each client will already have all of the
// initial state (because at this point in time, all replicated properties have their default values, which the
// clients have as well), so it's tempting to just skip the initial notification and the OnReplicated event, and
// just use the normal BeginPlay. But for cases where a client joins sometime after the start of play, we don't want
// the replicated actor to really "start" until it has received its state at that point in time, otherwise the client
// could have strange behavior until (and while) properties are replicated. In the scenario of a late joiner, when the
// NRChannel is opened, it calls all INRActorMixin actors' GenChannelReplicationPayload method to get their current
// state and sends it to the client with the isInitialState flag set just like it is in RegisterProps, and then after
// the data has arrived, OnReplicated is called on each client.
void INRActorMixin::RegisterProps()
{
    UWorld* world = Cast<AActor>(this)->GetWorld();
    spawnTS = world->GetRealTimeSeconds();

    // trigger an NRUpdate call to everywhere for the initial spawn replication. But, since all copies of the actor
    // will have the same defaults, just send an empty message.
    UNetDriver *driver = Cast<AActor>(this)->GetWorld()->GetNetDriver();
    bool isHost = (driver && driver->GetNetMode() != ENetMode::NM_Client);
    if (isHost)
    {
        py::dict empty;
        NRUpdate(ENRWhere::Owner|ENRWhere::NonOwners, true, empty); // owner|nonowners == "everywhere"
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
    // TODO: should we skip properties who are being updated but the value isn't changing?

    UNetDriver *driver = Cast<AActor>(this)->GetWorld()->GetNetDriver();
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
    else if (typeCode == TEXT("O"))
    {
        if (!driver)
        {
            LERROR("No net driver available to unmarshall UObject");
            return false;
        }
        else
        {
            FNetworkGUID g;
            ar << g;
            UObject *engineObj = driver->GuidCache->GetObjectFromNetGUID(g, false);
            if (!engineObj || !engineObj->IsValidLowLevel())
            {
                LERROR("Unmarshalled object not found for netguid %d", g.Value);
                return false;
            }
            else
                obj = py::cast(engineObj);
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

    // unmarshall the message and update affected properties
    UNetDriver *driver = Cast<AActor>(this)->GetWorld()->GetNetDriver();
    FMemoryReader reader(payload);
    uint8 isInitialState;
    reader << isInitialState;
    TArray<FString> modifiedPropNames;
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

