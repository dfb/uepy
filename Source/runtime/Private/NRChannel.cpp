// Copyright 2016-2021 FractalMob, LLC. All Rights Reserved.
#include "NRChannel.h"
#include "common.h"
#include "Engine/NetConnection.h"
#include "Engine/PackageMapClient.h"
#include "Kismet/GameplayStatics.h"

//#pragma optimize("", off)
static py::object appBridge = py::none();

UNRChannel::UNRChannel(const FObjectInitializer& ObjectInitializer) : UChannel(ObjectInitializer)
{
    ChName = FName(_TEXT("NRChannel"));
}

void UNRChannel::Init(UNetConnection* InConnection, int32 InChIndex, EChannelCreateFlags CreateFlags)
{
    Super::Init(InConnection, InChIndex, CreateFlags);
    bPendingDormancy = true; // this means do NOT stop ticking

    if (!InConnection || !InConnection->Driver)
    {
        LERROR("No driver for connection");
        return;
    }

    if (appBridge.is_none())
    {
        LERROR("No app bridge set.");
        return;
    }

    if (InConnection->Driver->GetNetMode() != ENetMode::NM_Client)
    {   // we're the host and a client is joining
        try { appBridge.attr("OnChannelFromClient")(this); } catchpy;
    }
    else
    {   // we're the client and we're joining the host
        try { appBridge.attr("OnChannelToHost")(this); } catchpy;
    }
}

bool UNRChannel::CleanUp( const bool bForDestroy, EChannelCloseReason CloseReason )
{
    LOG("Cleaning up channel %d, forDestroy:%d, reason:%d", channelID, (int)bForDestroy, (int)CloseReason);
    try { appBridge.attr("OnChannelClosing")(this); } catchpy;
    return Super::CleanUp(bForDestroy, CloseReason);
}

// reads one or more incoming messages into message structs and saves them for processing during the next Tick
void UNRChannel::ReceivedBunch(FInBunch& bunch)
{
    while (!bunch.AtEnd())
    {
        TSharedPtr<FNRMessage> message = MakeShareable(new FNRMessage());
        bunch << message->payload;
        message->reliable = bunch.bReliable; // used in debugging but also in mixed reliablility logic
        messagesToProcess.Add(message);
    }
}

void UNRChannel::Tick()
{
    Super::Tick();
    if (!Connection->Driver || !Connection->Driver->World || !Connection->OwningActor)
        return;

    // send queued outgoing messages across the wire
    if (messagesToSend.Num() > 0)
    {   // loosely based on VoiceChannel.cpp's implementation
        int32 index = 0;
        for (index=0; index < messagesToSend.Num(); index++)
        {
            if (!Connection->IsNetReady(0))
                break;

            FOutBunch bunch(this, false);
            TSharedPtr<FNRMessage> msg = messagesToSend[index];
            bunch.bReliable = msg->reliable;
            bunch << msg->payload;
            if (!bunch.IsError())
                SendBunch(&bunch, 1);
        }

        // remove sent messages
        if (index >= messagesToSend.Num())
            messagesToSend.Empty(); // fer efficiency!
        else if (index > 0)
            messagesToSend.RemoveAt(0, index);
    }

    // dispatch any inbound messages we received since last time
    if (messagesToProcess.Num() > 0 && !appBridge.is_none())
    {
        for (auto _message : messagesToProcess)
        {   // dispatch the msgs to Python
            try { appBridge.attr("OnMessage")(this, py::memoryview::from_memory(_message->payload.GetData(), _message->payload.Num(), true)); } catchpy;
        }
        messagesToProcess.Empty();
    }
}

void UNRChannel::AddMessage(TArray<uint8>& payload, bool reliable)
{
	TSharedPtr<FNRMessage> message = MakeShareable(new FNRMessage());
    message->payload = payload;
    message->reliable = reliable;
    messagesToSend.Add(message);
}

void UNRChannel::SetAppBridge(py::object& bridge)
{
    appBridge = bridge;
}

//#pragma optimize("", on)

