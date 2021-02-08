// Copyright 2016-2021 FractalMob, LLC. All Rights Reserved.
// an interface that should be implemented by player controllers that want to use the new replication stuff

#pragma once
#include "NRChannel.h"
#include "INRPlayerControllerMixin.generated.h"

UINTERFACE()
class UEPY_API UNRPlayerControllerMixin : public UInterface
{
    GENERATED_BODY()
};

class UEPY_API INRPlayerControllerMixin
{
    GENERATED_BODY()

    // used by net call
    void _LocalNRCall(bool isInternal, AActor *recipient, const FString signature, TArray<uint8>& payload);
    void _RemoteNRCall(UNetConnection *conn, ENRWhere where, AActor *recipient, const FString signature, TArray<uint8>& payload, bool reliable, float maxCallsPerSec);

public:
    // called by the global NRCall function
    void NRCall(ENRWhere where, AActor *recipient, FString signature, TArray<uint8>& payload, bool reliable, float maxCallsPerSec);
};

// global API, callable by anyone anywhere
void UEPY_API NRCall(ENRWhere where, AActor *recipient, const FString signature, TArray<uint8>& payload, bool reliable=true, float maxCallsPerSec=-1.0f);

// Note that there is no global NRUpdate API; this is because NRUpdate is /intended/ to be used by an actor on itself - i.e. there's no technical reason
// why you couldn't call NRUpdate on some other actor, but it's supposed to be how an actor updates its internal replicated state, something that an agent
// outside of the caller wouldn't do directly (somebody external would normally call an API that the actor exposes, and that API would in turn call self.NRUpdate).

