// Copyright 2016-2021 FractalMob, LLC. All Rights Reserved.
// Network replication channel that is independent of UE4's builtin replication.
//
// To enable, add the following to Config/DefaultEngine.ini:
// [/Script/Engine.NetDriver]
// +ChannelDefinitions=(ChannelName=NRChannel, ClassName=/Script/uepy.NRChannel, bTickOnCreate=true, bServerOpen=true, bClientOpen=true, bInitialServer=true, bInitialClient=true)
//
// Unlike normal actor channels, there is one NRChannel per connection, so each client machine has one and then the host has several, one per client.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Channel.h"
#include "NRChannel.generated.h"

// Combinable flags for the different destinations for networked messages
enum class ENRWhere : uint8
{
    Nowhere = 0, Local = 1, Host = 2, Owner = 4, NonOwners = 8,
    Internal = 128 // not for application code but for internal use, e.g. replicated props
};
ENUM_CLASS_FLAGS(ENRWhere)

// There are a small number of core message types that are sent over the channel. The first byte of each message is (in MSB->LSB order):
// b7-b6: multipart message support (isFirstChunk, isLastChunk)
// b5: isCompressed
// b4-b2: reserved (probably another bit will be used by ENRWireMessageType at some point)
// b1-b0: NRWireMessageType
// Note that property replication is built on top of NRCall so it doesn't need a separate message
enum class ENRWireMessageType : uint8
{
    Invalid, Init, SignatureDef, Call,
};

// Base class for all NetRep messages
class FNRBaseMessage
{
public:
    ENRWireMessageType type; // added this because TSharedPtr doesn't allow dynamic casts, so we need some other way to ID them
    bool reliable = true; // ok if this message gets dropped?
    FNRBaseMessage(ENRWireMessageType t) : type(t) {};
    virtual void Serialize(FArchive& ar) {};
    virtual ~FNRBaseMessage() {};
};

// A message sent from the host to the client when the connection is first established
class FNRInitMessage : public FNRBaseMessage
{
public:
    FNRInitMessage() : FNRBaseMessage(ENRWireMessageType::Init) {};
    ~FNRInitMessage() {};
    int channelID; // a unique ID for this channel that is the same, for a given channel, on host and the client
    virtual void Serialize(FArchive& ar) override;
};

// A message that defines a new mapping between a function name and its parameter data types and a channel- and direction-specific ID
// Signature defs allow us to have dynamic message dispatch without the overhead of constantly including parameter data type info as
// part of each message's payload.
class FNRSignatureDefMessage : public FNRBaseMessage
{
public:
    FNRSignatureDefMessage() : FNRBaseMessage(ENRWireMessageType::SignatureDef) {};
    ~FNRSignatureDefMessage() {};
    FString signature; // a function name or a function name followed by a list of the data types for its parameters
    uint16 id; // the ID for this signature; not shared across channels, not even shared across directions (i.e. the same signature could have a different ID for C->S vs S->C)
    virtual void Serialize(FArchive& ar) override;
};

// A message for issuing a function call from one machine to another
class FNRCallMessage : public FNRBaseMessage
{
public:
    FNRCallMessage() : FNRBaseMessage(ENRWireMessageType::Call) {};
    ~FNRCallMessage() {}
    ENRWhere where; // all the places where the function call should happen
    FNetworkGUID recipient; // the AActor that will receive the message. Must be marked as replicating in UE4's replication system.
    uint16 signatureID;
    TArray<uint8> payload; // the message itself, can be an opaque blob of bytes or parameters that have been marshalled.
    virtual void Serialize(FArchive& ar) override;
    int deliveryAttempts = 0; // on the receiving side, how many times we tried to find a recipient so far
};

typedef TArray<TSharedPtr<FNRBaseMessage>> FNRMessageList;

UCLASS()
class UEPY_API UNRChannel : public UChannel
{
    GENERATED_BODY()

    FNRMessageList messagesToSend; // outgoing messages to be sent on the next Tick
    FNRMessageList messagesToProcess; // incoming messages to be dispatched on the next Tick
    TMap<FString,uint16> sendSigDefs; // known signature defs from our side to the remote side
    TMap<uint16,FString> recvSigDefs; // known signature defs from the remote side to our side
    TMap<uint64,float> callTimes; // for cheesy throttling; sigID -> last timestamp of when a messaage was sent with this signature

public:
    int channelID=-1; // unique ID for this channel, same on client and host (tried to use UNetConnection ID, but it doesn't get replicated)

    virtual void Init(UNetConnection* InConnection, int32 InChIndex, EChannelCreateFlags CreateFlags) override;
    virtual void ReceivedBunch(FInBunch& Bunch) override;
    virtual int64 Close(EChannelCloseReason Reason);
    virtual bool CleanUp( const bool bForDestroy, EChannelCloseReason CloseReason );
    virtual void Tick() override;

    UNRChannel(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    // used by the player controller mixin to add a message to the outgoing message queue
    void AddNRCall(ENRWhere where, AActor *recipient, FString signature, TArray<uint8>& payload, bool reliable, float maxCallsPerSec);
};

