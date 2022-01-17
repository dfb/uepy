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
#include "uepy.h"
#include "NRChannel.generated.h"

// Combinable flags for the different destinations for networked messages
enum class ENRWhere2 : uint8
{
    Nowhere = 0, Local = 1, Host = 2, Owner = 4, NonOwners = 8,
    All = 1|2|4|8,
    Internal = 128 // not for application code but for internal use, e.g. replicated props
};
ENUM_CLASS_FLAGS(ENRWhere2)

class FNRMessage
{
public:
    bool reliable;
    TArray<uint8> payload;
};

typedef TArray<TSharedPtr<FNRMessage>> FNR2MessageList;

UCLASS()
class UEPY_API UNRChannel : public UChannel
{
    GENERATED_BODY()

    FNR2MessageList messagesToSend; // outgoing messages to be sent on the next Tick
    FNR2MessageList messagesToProcess; // incoming messages to be dispatched on the next Tick

public:
    int channelID=-1; // unique ID for this channel, same on client and host (tried to use UNetConnection ID, but it doesn't get replicated). Set by app bridge.

    // Establishes a bridge between low level NR code and the application layer
    static void SetAppBridge(py::object& bridge);

    virtual void Init(UNetConnection* InConnection, int32 InChIndex, EChannelCreateFlags CreateFlags) override;
    virtual void ReceivedBunch(FInBunch& Bunch) override;
    virtual bool CleanUp( const bool bForDestroy, EChannelCloseReason CloseReason );
    virtual void Tick() override;

    UNRChannel(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    // low level API for enqueuing a message for sending
    void AddMessage(TArray<uint8>& payload, bool reliable);
};

