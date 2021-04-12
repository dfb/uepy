// Copyright 2016-2021 FractalMob, LLC. All Rights Reserved.
#include "NRChannel.h"
#include "INRPlayerControllerMixin.h"
#include "INRActorMixin.h"
#include "common.h"
#include "Engine/NetConnection.h"
#include "Engine/PackageMapClient.h"
#include "Kismet/GameplayStatics.h"

//#pragma optimize("", off)

void FNRInitMessage::Serialize(FArchive& ar)
{
    if (ar.IsSaving()) // for the loading case we will have already consumed the first byte
    {
        uint8 msgType = (uint8)ENRWireMessageType::Init;
        ar << msgType; // TODO: also add multipart support and compression
    }
    ar << channelID;
}

void FNRSignatureDefMessage::Serialize(FArchive& ar)
{
    if (ar.IsSaving()) // for the loading case we will have already consumed the first byte
    {
        uint8 msgType = (uint8)ENRWireMessageType::SignatureDef;
        ar << msgType; // TODO: also add multipart support and compression
    }
    ar << id;
    ar << signature;
}

void FNRCallMessage::Serialize(FArchive& ar)
{
    if (ar.IsSaving()) // for the loading case we will have already consumed the first byte
    {
        uint8 msgType = (uint8)ENRWireMessageType::Call;
        ar << msgType; // TODO: also add multipart support and compression
    }
    ar << where;
    ar << recipient;
    ar << signatureID;
    ar << payload;
}

UNRChannel::UNRChannel(const FObjectInitializer& ObjectInitializer) : UChannel(ObjectInitializer)
{
    ChName = FName(_TEXT("NRChannel"));
}

// used by Init to generate a unique ID for a new channel. This ID can be used as a player ID, so there are
// advantages to keeping this number low, so it looks for the next available ID as opposed to using an
// ever-increasing counter
int GenConnectionID(UNetDriver* driver)
{
    if (!driver || !driver->ClientConnections.Num())
        return 0;

    // this looks inefficient, but it's actually not that bad because we're doing this only when the
    // channel opens and there are a small number of connections
    int nextID = 1;
    while (1)
    {
        bool inUse = false;
        for (auto conn : driver->ClientConnections)
        {
            for (auto chan : conn->OpenChannels)
            {
                UNRChannel *ch = Cast<UNRChannel>(chan);
                if (ch && ch->channelID == nextID)
                {
                    inUse = true;
                    break;
                }
            }

            if (inUse)
                break;
        }

        if (!inUse)
            return nextID;
        nextID++;
    }
}

void UNRChannel::Init(UNetConnection* InConnection, int32 InChIndex, EChannelCreateFlags CreateFlags)
{
    Super::Init(InConnection, InChIndex, CreateFlags);
    bPendingDormancy = true; // this means do NOT stop ticking

    // if we are the host side of the channel, we need to init the client and send then inform the client of all replicated actors
    if (InConnection->Driver && InConnection->Driver->GetNetMode() != ENetMode::NM_Client)
    {
        // send a message to officially initialize the client side
        channelID = GenConnectionID(InConnection->Driver);
        TSharedPtr<FNRInitMessage> m = MakeShareable(new FNRInitMessage());
        m->channelID = channelID;
        messagesToSend.Add(m);
        LOG("Setting up channel %d with max bunch size bits %d", channelID, InConnection->GetMaxSingleBunchSizeBits()); // typical 7625 bits or 953.125 bytes

        // send replicated actor info, sorting them in order of their spawn time to make it more likely for the
        // client to get the info in a useful order (i.e. reduce odds of a big list of replication messages for
        // actors that haven't been replicated to the client yet)
        TArray<AActor*> nrActors;
        UGameplayStatics::GetAllActorsWithInterface(InConnection->Driver->World, UNRActorMixin::StaticClass(), nrActors);
        nrActors.Sort([](const AActor& a, const AActor& b)
        {
            const INRActorMixin* pa = Cast<INRActorMixin>(&a);
            const INRActorMixin* pb = Cast<INRActorMixin>(&b);
            return pa->spawnTS < pb->spawnTS;
        });

        // ask each one to generate an NRUpdate payload of all properties that do not have their default values
        for (auto _actor : nrActors)
        {
            if (!VALID(_actor) || _actor->IsPendingKill())
                continue;
            INRActorMixin* actor = Cast<INRActorMixin>(_actor);
            if (actor && _actor->GetIsReplicated() && !_actor->HasAnyFlags(RF_ClassDefaultObject))
            {
                FString signature;
                TArray<uint8> payload;
                actor->GenChannelReplicationPayload(InConnection->Driver, signature, payload);
                AddNRCall(ENRWhere::Local|ENRWhere::Internal, _actor, signature, payload, true, -1);
            }
        }
    }
}

int64 UNRChannel::Close(EChannelCloseReason Reason)
{
    LOG("Closing %d", (int)Reason);
    return Super::Close(Reason);
}

bool UNRChannel::CleanUp( const bool bForDestroy, EChannelCloseReason CloseReason )
{
    LOG("Cleaning up %d", (int)CloseReason);
    return Super::CleanUp(bForDestroy, CloseReason);
}

// reads one or more incoming messages into message structs and saves them for processing during the next Tick
void UNRChannel::ReceivedBunch(FInBunch& bunch)
{
    while (!bunch.AtEnd())
    {
        ENRWireMessageType msgType;
        bunch << msgType;
        TSharedPtr<FNRBaseMessage> message;
        if (msgType == ENRWireMessageType::Init)
            message = MakeShareable(new FNRInitMessage());
        else if (msgType == ENRWireMessageType::SignatureDef)
            message = MakeShareable(new FNRSignatureDefMessage());
        else if (msgType == ENRWireMessageType::Call)
            message = MakeShareable(new FNRCallMessage());
        message->Serialize(bunch);

        if (msgType == ENRWireMessageType::Init)
        {   // no need to wait - just process this one right now
            TSharedPtr<FNRInitMessage> m = StaticCastSharedPtr<FNRInitMessage>(message);
            channelID = m->channelID;
            LOG("Channel is now setup with ID %d", channelID);
        }
        else
            messagesToProcess.Add(message);

        /*
        if (msgType == ENRWireMessageType::SignatureDef)
        {
            TSharedPtr<FNRSignatureDefMessage> m = StaticCastSharedPtr<FNRSignatureDefMessage>(message);
            LOG("Received sigdef msg (%d=%s), have %d messages to process", m->id, *m->signature, messagesToProcess.Num());
        }
        else if (msgType == ENRWireMessageType::Call)
        {
            TSharedPtr<FNRCallMessage> m = StaticCastSharedPtr<FNRCallMessage>(message);
            AActor *r = Cast<AActor>(NRGetObjectFromNetGUID(Connection->Driver, m->recipient));
            LOG("Received call msg (recpt:%d %s, sig:%d), have %d messages to process", m->recipient.Value, r ? *r->GetName() : TEXT("??"), m->signatureID, messagesToProcess.Num());
        }
        */
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
            bunch.bReliable = true;
            messagesToSend[index]->Serialize(bunch);
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
    if (messagesToProcess.Num() > 0)
    {
        FNRMessageList messagesToRetry; // messages to try again on later
        for (auto _message : messagesToProcess)
        {
            if (_message->type == ENRWireMessageType::SignatureDef)
            {   // the other side is informing us of a new signature definition
                auto sigDefMsg = StaticCastSharedPtr<FNRSignatureDefMessage>(_message);
                recvSigDefs.Emplace(sigDefMsg->id, sigDefMsg->signature);
            }
            else if (_message->type == ENRWireMessageType::Call)
            {   // a function call
                auto callMsg = StaticCastSharedPtr<FNRCallMessage>(_message);
                FString *sigPtr = recvSigDefs.Find(callMsg->signatureID);
                if (!sigPtr)
                {
                    LERROR("Failed to locate signature %d", callMsg->signatureID);
                    continue;
                }
                FString sig = *sigPtr;

                //LOG("Processing recv'd msg where:%d, to:%d, sig:%s", (int)callMsg->where, callMsg->recipient.Value, *sig);
                AActor *recipient = Cast<AActor>(NRGetObjectFromNetGUID(Connection->Driver, callMsg->recipient));
                if (!VALID(recipient))
                {
                    if ((callMsg->where & ENRWhere::Internal) != ENRWhere::Nowhere)
                    {   // this may just be because the actor hasn't been replicated yet, so save this message for a retry
                        LWARN("Failed to find recipient for internal call to %s for recipient %u", *sig, callMsg->recipient.Value);
                        if (++callMsg->deliveryAttempts < 0) // disabling this for now - it never seems to be due to us getting ahead of engine replication
                            messagesToRetry.Emplace(callMsg);
                        else
                        {
                            LERROR("Gave up trying to send that message");
                        }
                    }
                    else
                    {
                        LERROR("Failed to find recipient for netguid for call to %s for recipient %u", *sig, callMsg->recipient.Value);
                    }
                    continue;
                }

                NRCall(callMsg->where, recipient, sig, callMsg->payload);
            }
            else
            {
                LERROR("Unknown wire message type: %d", _message->type);
                continue;
            }
        }
        messagesToProcess = messagesToRetry;
    }
}

// used by INRPlayerControllerMixin to add a NRCall to the outgoing message queue
void UNRChannel::AddNRCall(ENRWhere where, AActor *recipient, FString signature, TArray<uint8>& payload, bool reliable, float maxCallsPerSec)
{
    // find (or add) the signature ID
    uint16* idPtr = sendSigDefs.Find(signature);
    uint16 sigID = 0;
    if (idPtr == nullptr)
    {
        sigID = sendSigDefs.Num();
        sendSigDefs.Emplace(signature, sigID);
        if (sigID >= 44000) // 44k == about 2/3rds
        {   // Warn if we are really chewing up available IDs (we should probably never come remotely close to this)
            LOG("WARNING: Issued sigID %d for signature %d - an unusually high number of sigIDs have been registered", sigID, *signature);
            // but fall through and keep going
        }

        // we're defining a new signature, so we need to inform the other side of the connection
        TSharedPtr<FNRSignatureDefMessage> m = MakeShareable(new FNRSignatureDefMessage());
        m->signature = signature;
        m->id = sigID;
        messagesToSend.Add(m);
    }
    else
        sigID = *idPtr;

    // we don't implement throttling until here because this is the easiest place to implement a throttling ID
    // and because throttling is per-channel
    FNetworkGUID recipientID = NRGetOrAssignNetGUID(Connection->Driver, recipient);
    //LOG("Sending %s to %s (%u), where:%d", *signature, *recipient->GetName(), recipientID.Value, (int)where);
    if (maxCallsPerSec > 0.0f)
    {
        uint64 lastCallID = (uint64)recipientID.Value | ((uint64)(sigID) << 32);
        float now = recipient->GetWorld()->GetRealTimeSeconds();
        float* lastCall = callTimes.Find(lastCallID);
        if (lastCall)
        {   // we've called it before, make sure not too recently
            // TODO: use an exponential moving average of times between calls (maybe not)
            // TODO: or, maybe keep the most recent message and deliver it if enough time passes and we don't receiver something newer?
            float nextAllowedTime = *lastCall + 1.0f/maxCallsPerSec;
            if (now < nextAllowedTime)
            {   // too soon!
                //LOG("WARNING: Throttling %s call to %s", *signature, *recipient->GetName());
                return;
            }
        }
        callTimes.Emplace(lastCallID, now);
    }

	TSharedPtr<FNRCallMessage> message = MakeShareable(new FNRCallMessage());
    message->reliable = reliable;
    message->where = where;
    message->recipient = recipientID;
    message->signatureID = sigID;
    message->payload = payload;
    messagesToSend.Add(message);
    //LOG("Have %d messages to send", messagesToSend.Num());
}

//#pragma optimize("", on)

