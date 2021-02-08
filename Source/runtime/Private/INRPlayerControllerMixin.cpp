#include "INRPlayerControllerMixin.h"
#include "INRActorMixin.h"
#include "Engine/PackageMapClient.h"
#include "Engine/NetConnection.h"
#include "common.h"

//#pragma optimize("", off)

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
    if (driver && driver->GetNetMode() != ENetMode::NM_Client)
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

        // Carry out those actions
        if (runLocal)
            _LocalNRCall(isInternal, recipient, signature, payload);

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
    }
    else
    {   // running on a client - possible actions are: error, run local here, tell host to run local, tell host to tell all non-owners to run local
        if (!isOwner && !isInternal && (runHost || runNonOwners || runOwner))
        {
            LERROR("Non-owner trying to run %s but runHost:%d, runNonOwners:%d, runOwner:%d", *signature, runHost, runNonOwners, runOwner);
            return;
        }

        ENRWhere newFlags = ENRWhere::Nowhere; // we won't call the host unless we end up with a remote destination
        if (runHost)
            newFlags |= ENRWhere::Local; // tell the host to run the function locally
        if (runNonOwners)
            newFlags |= ENRWhere::NonOwners; // tell the host to tell all non-owners to run the function
        if (isInternal)
            newFlags |= ENRWhere::Internal; // preserve the Internal flag
        if (runOwner)
            runLocal = true;

        if (runLocal)
            _LocalNRCall(isInternal, recipient, signature, payload);
        if (driver && driver->ServerConnection && newFlags != ENRWhere::Nowhere)
            _RemoteNRCall(driver->ServerConnection, newFlags, recipient, signature, payload, reliable, maxCallsPerSec);
    }
}

// called by NRCall in cases where one of the destinations of the net call is the current machine
// (which may be the host or a client)
void INRPlayerControllerMixin::_LocalNRCall(bool isInternal, AActor *recipient, const FString signature, TArray<uint8>& payload)
{
    INRActorMixin *dest = Cast<INRActorMixin>(recipient);
    if (!dest || !VALID(recipient))
    {
        LERROR("Invalid destination for %s", *signature);
        return;
    }

    // See if this is a type-annotated signature or a vanilla one
    TArray<FString> sigParts;
    signature.ParseIntoArray(sigParts, TEXT("|"));
    if (sigParts.Num() == 1)
    {   // just an opaque blob from our perspective, so pass it along
        TArray<uint8> copy(payload);
        if (isInternal) // internal is stuff like variable replication that builds on top of NRCall
            dest->OnInternalNRCall(signature, copy);
        else // route it to application-level code
            dest->OnNRCall(signature, copy);
        return;
    }

    if (isInternal)
    {
        LERROR("Type annotated NRCalls not yet supported on internal messages, ignoring call to %s", *signature);
    }

    // We have type info, so convert the payload into python objects
    UNetDriver *driver = recipient->GetWorld()->GetNetDriver();
    py::list args;
    TArray<FString> typeInfos;
    sigParts[1].ParseIntoArray(typeInfos, TEXT(","));
    FMemoryReader reader(payload);
    for (auto type : typeInfos)
    {
        py::object arg;
        if (!UnmarshalPyObject(driver, type, reader, arg))
        {
            LERROR("Unhandled typeInfo %s", *type);
            return;
        }
        args.append(arg);
    }
    dest->OnNRCall(sigParts[0], args);
}

// called by NRCall when we need to have the remote side of a connection run a function
void INRPlayerControllerMixin::_RemoteNRCall(UNetConnection *conn, ENRWhere where, AActor *recipient, const FString signature, TArray<uint8>& payload, bool reliable, float maxCallsPerSec)
{
    UNRChannel *repChanRef = nullptr;
    // TODO: we had a member var that was caching this value so we didn't have to look it up each time, but that
    // doesn't work as a simple member var since it has to be per-connection, LOL
    if (!VALID(repChanRef))
    {   // first time through, look it up
        for (UChannel *chan : conn->OpenChannels)
        {
            UNRChannel *repChan = Cast<UNRChannel>(chan);
            if (VALID(repChan))
            {
                repChanRef = repChan;
                break;
            }
        }
    }

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

    repPC->NRCall(where, recipient, signature, payload, reliable, maxCallsPerSec);
}

//#pragma optimize("", on)

