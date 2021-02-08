// Copyright 2016-2021 FractalMob, LLC. All Rights Reserved.
// an interface that is implemented by any actors that want to have replicated properties

#pragma once
#include "uepy.h"
#include "NRChannel.h"
#include "INRActorMixin.generated.h"

// manages a set of replicated properties on an actor. Set up to be accessible from both Python and C++
// simultaneously
struct FNRPropHolder
{
    // adds (defines) a new property, inferring the property data type from the given default value. Returns false on
    // error, e.g. if the value is not of a supported type.
    bool AddProperty(FString name, py::object& defaultValue);

    // returns the current value of the given property
    py::object GetValue(FString name);

    // returns the unique (to this propholder) ID for the given property, or -1 if the property does not exist
    int GetPropertyID(FString name);

    // internal stuff
    TMap<FString,uint16> namesToIDs; // property names to their IDs (indexes)
    TArray<FString> names; // names of each property, by index.
    TArray<py::object> values; // current values for each property, by index
    TArray<py::object> defaults; // default value for each property, by index
    TArray<FString> typeCodes; // the marshalling type code for each property, by index
};

UINTERFACE()
class UEPY_API UNRActorMixin : public UInterface
{
    GENERATED_BODY()
};

class UEPY_API INRActorMixin
{
    GENERATED_BODY()
    friend class INRPlayerControllerMixin;
    friend class UNRChannel;

protected:
    void _CallOnReplicated();
    void OnInternalNRCall(FString signature, TArray<uint8>& payload); // this one is for non-application messages (e.g. variable replication)
    void GenChannelReplicationPayload(UNetDriver* driver, FString& signature, TArray<uint8>& payload);
    bool beginPlayCalled = false; // true once the BeginPlay method has been called
    bool initialStateReplicated = false; // on clients, true once the initial info for replicated variables has been received and processed
    bool onReplicatedCalled = false; // mostly for debugging to ensure we never call it twice
    float spawnTS = 0; // when this actor was spawned

public:
    // subclasses *must* call this from their BeginPlay (but not do much else in it - see OnReplicated)
	void NoteBeginPlay();

    // Called from Python to inform this object of its replicated properties
    void RegisterProps();

    // called to deliver a netrep message to this actor
    virtual void OnNRCall(FString signature, TArray<uint8>& payload) {};
    virtual void OnNRCall(FString signature, py::object args) {};

    // used to trigger an update of replicated props. Even for local-only updates, this method must be used (vs direct modification)
    void NRUpdate(ENRWhere where, bool isInitialState, py::dict& kwargs, bool reliable=true, float maxCallsPerSec=-1.0);

    // called to inform this actor that some of its replicated properties have been updated (i.e. a call to NRUpdate
    // triggers a call to this)
    virtual void OnNRUpdate(TArray<FString>& modifiedPropertyNames) {};

    FNRPropHolder repProps; // this actor's replicated properties

    // The main "we are starting" method that subclasses should implement instead of using BeginPlay. This method is called once BeginPlay
    // has been called *and* all replicated variables have their correct values (something you don't get with BeginPlay normally).
    virtual void OnReplicated() {};
};

// Given a Python tuple of function parameters, marshall the parameters into blob and set formatStr to hold the variable data types
void TupleToBlob(UWorld *world, py::tuple args, TArray<uint8>& blob, FString& formatStr);

// Given a data type code and an archive holding raw data, unmarshalls the object into obj
bool UnmarshalPyObject(UNetDriver *driver, FString& typeCode, FArchive& ar, py::object& obj);

