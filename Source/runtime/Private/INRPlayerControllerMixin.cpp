#include "INRPlayerControllerMixin.h"
#include "INRActorMixin.h"
#include "uepy.h"
#include "GameFramework/PlayerController.h"
#include "Engine/PackageMapClient.h"
#include "Engine/NetConnection.h"
#include "common.h"

// Based on 'where' flags, sends the given message to the recipient on the correct machines (the current machine, the host,
// certain clients, everywhere, etc.)
void INRPlayerControllerMixin::NRCall(ENRWhere where, AActor *recipient, const FString signature, TArray<uint8>& payload, bool reliable, float maxCallsPerSec)
{
    if (where == ENRWhere::Nowhere)
    {
        LERROR("Called without any destinations for %s", *signature);
        return;
    }

    if (!VALID(recipient))
    {
        LERROR("Called with an invalid recipient for %s", *signature);
        return;
    }

    APlayerController *self = Cast<APlayerController>(this);
    bool runHost = (where & ENRWhere::Host) != ENRWhere::Nowhere;
    bool runLocal = (where & ENRWhere::Local) != ENRWhere::Nowhere;
    bool runOwner = (where & ENRWhere::Owner) != ENRWhere::Nowhere;
    bool runNonOwners = (where & ENRWhere::NonOwners) != ENRWhere::Nowhere;
    bool isInternal = (where & ENRWhere::Internal) != ENRWhere::Nowhere;
    AActor *recipientOwner = recipient->GetOwner();
    bool isOwner = recipientOwner == self;

    UWorld *world = recipient->GetWorld();
    UNetDriver *driver = world->GetNetDriver();
    if (!driver || (driver && driver->GetNetMode() != ENetMode::NM_Client))
    {   // running on the host - possible actions are: run local here, tell owner to run local, tell non-owners to run local
        bool netSendOwner = false;
        bool netSendNonOwners = runNonOwners;

        // Figure out what actions we need to take
        if (runHost)
            runLocal = true; // we ARE the host
        if (runOwner)
        {
            if (isOwner)
                runLocal = true;
            else
                netSendOwner = true;
        }

        if (runNonOwners && !isOwner)
            runLocal = true; // we're one of the non-owners so we need to run it here

        // if we're in single player mode, don't send over the wire
        if (!driver)
        {
            netSendOwner = false;
            netSendNonOwners = false;
        }

        // Carry out those actions - we always fire off any remote messages before running locally, so that if the local call in turn
        // triggers more net messages, these first messages will arrive before any additional messages.
        if (netSendOwner || netSendNonOwners)
        {
            ENRWhere newFlags = ENRWhere::Local;
            if (isInternal)
                newFlags |= ENRWhere::Internal;
            for (auto conn : driver->ClientConnections)
            {
                if ((netSendNonOwners && conn->OwningActor != recipientOwner) || (netSendOwner && conn->OwningActor == recipientOwner))
                    _RemoteNRCall(conn, newFlags, recipient, signature, payload, reliable, maxCallsPerSec);
            }
        }
        if (runLocal)
            _LocalNRCall(reliable, isInternal, recipient, signature, payload);
    }
    else
    {   // running on a client - possible actions are: error, run local here, tell host to run local, tell host to tell all non-owners to run local
        ENRWhere newFlags = ENRWhere::Nowhere; // we won't call the host unless we end up with a remote destination
        if (runHost)
            newFlags |= ENRWhere::Local; // tell the host to run the function locally
        if (runOwner && !isOwner)
            newFlags |= ENRWhere::Owner;
        if (runNonOwners)
            newFlags |= ENRWhere::NonOwners; // tell the host to tell all non-owners to run the function
        if (isInternal)
            newFlags |= ENRWhere::Internal; // preserve the Internal flag
        if ((isOwner && runOwner) || (!isOwner && runNonOwners))
            runLocal = true;

        // see note above - do any remote calls and then run locally
        if (driver && driver->ServerConnection && newFlags != ENRWhere::Nowhere)
            _RemoteNRCall(driver->ServerConnection, newFlags, recipient, signature, payload, reliable, maxCallsPerSec);
        if (runLocal)
            _LocalNRCall(reliable, isInternal, recipient, signature, payload);
    }
}

// called by NRCall in cases where one of the destinations of the net call is the current machine
// (which may be the host or a client)
void INRPlayerControllerMixin::_LocalNRCall(bool reliable, bool isInternal, AActor *recipient, const FString signature, TArray<uint8>& payload)
{
    INRActorMixin *dest = Cast<INRActorMixin>(recipient);
    if (!dest || !VALID(recipient))
    {
        LERROR("Invalid destination for %s", *signature);
        return;
    }
    dest->RouteNRCall(reliable, isInternal, signature, payload);
}

// helper to locate the NRChannel instance for this connection
UNRChannel* FindNRChannel(UNetConnection* conn)
{
    for (UChannel *chan : conn->OpenChannels)
    {
        UNRChannel *repChan = Cast<UNRChannel>(chan);
        if (VALID(repChan))
            return repChan;
    }
    return nullptr;
}

// called by NRCall when we need to have the remote side of a connection run a function
void INRPlayerControllerMixin::_RemoteNRCall(UNetConnection *conn, ENRWhere where, AActor *recipient, const FString signature, TArray<uint8>& payload, bool reliable, float maxCallsPerSec)
{
    UNRChannel *repChanRef = FindNRChannel(conn);
    if (!VALID(repChanRef))
    {
        LERROR("Failed to find UNRChannel for connection");
        return;
    }

    repChanRef->AddNRCall(where, recipient, signature, payload, reliable, maxCallsPerSec);
}

// this implements the global NRCall function that application code uses
void NRCall(ENRWhere where, AActor *recipient, const FString signature, TArray<uint8>& payload, bool reliable, float maxCallsPerSec)
{
    //LOG("where:%d, to:%s, sig:%s", (int)where, *recipient->GetName(), *signature);
    if (!VALID(recipient))
    {
        LERROR("Invalid recipient for call to %s", *signature);
        return;
    }

    if (!Cast<INRActorMixin>(recipient))
    {
        LERROR("Recipient %s for call to %s does not implement INRActorMixin", *recipient->GetName(), *signature);
        return;
    }

    APlayerController *pc = recipient->GetWorld()->GetFirstPlayerController(); // I guess this means we don't work on listen-only servers yet
    if (!VALID(pc))
    {
        LERROR("Failed to get any player controller for %s", *signature);
        return;
    }

    INRPlayerControllerMixin *repPC = Cast<INRPlayerControllerMixin>(pc);
    if (!repPC)
    {
        LERROR("PlayerController does not implement INRPlayerControllerMixin");
        return;
    }

    // Sometimes it's convenient to have an Actor class that has INRActorMixin in its class hierarchy even though it's
    // not replicated
    if (!recipient->GetIsReplicated())
    {
        bool isInternal = (where & ENRWhere::Internal) != ENRWhere::Nowhere;
        where = ENRWhere::Local;
        if (isInternal)
            where |= ENRWhere::Internal;
    }
    repPC->NRCall(where, recipient, signature, payload, reliable, maxCallsPerSec);
}

int NRGetChannelID(UWorld* world)
{
    UNetDriver* driver = world->GetNetDriver();
    if (!driver || driver->GetNetMode() != ENetMode::NM_Client || !driver->ServerConnection)
        return 0; // host is always 0

    UNRChannel *ch = FindNRChannel(driver->ServerConnection);
    if (!VALID(ch))
    {
        LERROR("Cannot find NRChannel");
        return 0;
    }

    return ch->channelID;
}
