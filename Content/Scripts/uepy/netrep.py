'''
Python side of network replication (aka NetRep or NR) code. Misc info/notes:
- Any object (not just actors) can declare itself as replicated and send/receive network messages; each object that does so needs to
    have a unique network name/ID known as the netID.
- Replicated objects declare an optional 'repProps' Bag that lists all its replicated properties and their default values.
- Replicated objects have an OnReplicated method that is called once the object is fully replicated; for replicated actors, this is
    effectively BeginPlay (and they should generally not use BeginPlay at all).
- Replicated objects can optionally declare that they depend on other replicated objects in order to function properly, in which case
    OnReplicated is not called until those other objects have fully replicated first. Use self.NRSetDependencies(*deps) where each
    dependency can be a NetReplicatd object or the name of a property on self that holds a reference to a NetReplicated object. So the
    host might do something like the following in its OnReplicated method:
        self.foo = Foo() # Foo is a NetReplicated objects
        self.NRSetDependencies('foo')
    and then on the client, when OnReplicated is called, self.foo will reference that same Foo instance, and OnReplicated will be called
    only after foo fully replicates. It is up to application code to avoid dependency loops. Note that these are not repProps, so they
    are accessible via self.x and not self.nr.x. Also note that right now the usefulness of dependencies is somewhat limited because
    if NR is in charge of spawning the object client side, it must support a no-args constructor call.
- Replicated objects are spawned across the network in 3 different ways:
    - Engine - a core engine object such as gamestate or the player's pawn. Engine spawns on the host and on the client, and then we
        link it into the NR subsystem. Hopefully there are very few of these, but we need them because we don't yet know of a way for
        us to be in charge of replicating their spawn but still have them work completely with the engine's innards. Hopefully there
        are very few of these, because we don't have control over the order in which these get spawned on clients.
    - NR - any sort of game-specific replicated object. Engine spawns on the host and we link it into NR, and then send an NR message
        to all clients with instructions on how to spawn instances on the client and link those instances to the one on the host.
    - App - for replicated objects that get spawned "on their own", e.g. some replicated parts of the UI or something where the spawning
        of some other object causes these to be spawned (such that we don't want the engine or NR to take care of spawning them separately)
        but that we still want to be able to send/receive replication messages and to have state sync'd across machines.
    Outside of *how* the objects are spawned, replication works pretty much exactly the same in all cases. A huge amount of the complexity
    of this module is due to engine-spawned objects (i.e. synchronizing replicated object creation between two different replication systems)
    so it would be nice to eventually not need it.
- Engine-spawned and app-spawned objects either intrinsically know or can deduce their netIDs on their own, so they self register with NR
    on the host and the client. NR-spawned objects on the host either know/gen their netID or they get it from their spawn parameters, so
    they also self-register with NR. NR-spawned objects on the client are registered with NR automatically. Subclasses can use the global
    IsHost() function in their constructors to know whether or not they need to call NR.Register (since self.isHost isn't set until OnReplicated).
    As a special case, engine-spawned replicated objects have their UE4 netGUID appended to their registration name automatically so that
    the linking with the engine object instances can happen reliably (e.g. if you call NRRegister('mypawn', ENRSpawnReplicatedBy.Engine), and
    if the engine assigns the object a netGUID of 1005, then on *both* the host and clients that object will be registered with NR under
    the name 'mypawn_1005' - note that this also frees the application code from trying to figure out how to uniquely identify instances).
- A common pattern when the user is directly generating replication events (e.g. moving an object around) is to send throttled, unreliable
    events while the user interaction is underway, and then once the user is done, send a final, reliable, and unthrottled event so that
    everyone has the correct final value. In order to make this work (and because UE4 sends messages over UDP), application code needs to
    call NRStartMixedReliability once before the first intermediate/unreliable message goes out.
- Non-Actor replicated objects should call self.NRUnregister() when they are being destroyed.

TODO:
- with a lot of the binary data handling, we make unnecessary copies of the data - it'd be better to use byte arrays / memoryviews
    (or at the very least, struct.unpack_from)
- figure out some way to do creation of dependent child objects that take init parameters.
- splitting data marshalling into format type codes and encoded data blob has a low ROI. It would be better to keep the two merged. We probably
    want it for array supoort anyway.
- for everything except object refs, self.nr.pre_<name> to get the value from before the change? (and with obj refs, we just don't want to
    keep objs alive, so it could be a weak ref in that case)
- for data type codes, use an enum instead of a char, and then use a set high bit to mean "array" and add support for replicating arrays.
    - probably ok to limit max len to 255
    - probably /not/ ok to require all same type (think lists with None, but maybe that's the only exception?)
    - the diff format could be something like <1B:action:add/update/remove><1B:index>[for add/update: <1B:datatype><1B marshalled value>]
    - NRUpdate could detect the changes only if the caller doesn't modify the array exactly, e.g.
        foo = self.nr.foo
        foo.append(x)
        self.NRUpdate(foo=foo)
      wouldn't detect any changes, so we'd need to always use 'foo = self.nr.foo[:]' - maybe that's not too big a deal to require that?
    - maybe we should instead (or additionally) support ways to tell NRUpdate the modification we want to make, e.g.
        self.NRUpdate(foo__append=x)
        self.NRUpdate(foo__pop=2)
        self.NRUpdate(foo__remove=x)
        self.NRUpdate(foo=[1,2,3]) # still allow this, but doesn't try to diff and unconditionally resends all
      we don't need to support every possible use of arrays; instead focus on likely/recommended uses of replicated arrays
    - once you support arrays, might as well add support for dictionaries and sets! (and the var__action=X form would work well for them)
'''

from uepy import *
from uepy.enums import *
import struct, weakref, time, random, inspect

class ENRBridgeMessage(Enum): NONE, SigDef, SetChannelInfo, ObjInitialState, Call, Unregister = range(6)

class SignatureDefinitionManager:
    '''Helper for managing "signature definitions", which are simply mappings between strings and uint16 IDs. In a nutshell, there are a lot
    of strings we would send over the wire many times for a given application, so we instead map those strings to IDs and send the IDs instead.
    What makes it interesting/tricky is that we don't know the set of strings until they are about to be sent. SigDefs are to address the
    following problem:
    - we don't want to force developers to declare up front every network API call and the combination of all parameter types that might be used
    - we also don't want to constantly transmit type information since it's a lot of overhead
    In theory the set of strings the might be sent is "infinite" (for example, for strings representing type information for function call
    parameters), but in practice and for a given application, the set of strings used is actually relatively small - there are only so many
    method names and parameter type info combinations that an application will use.
    Because sigdefs are completely dynamic (they don't
    exist until the moment a mappable string is about to be sent over the wire), they are not only shared across channels, there are not even
    shared across directions on the same channel, so each channel has a set of sigdefs for sending as well as receiving.'''
    def __init__(self, channel):
        self.channel = channel # so we can send out the new mappings as they get created
        self.s2n = {} # string -> numerical ID
        self.n2s = {} # numerical ID -> string

    def Set(self, n, s):
        '''Used by channels when they receive a signature definition from the other side'''
        self.s2n[s] = n
        self.n2s[n] = s

    def IDForString(self, s):
        '''Given some string, returns an ID for it, creating it if needed.'''
        n = self.s2n.get(s)
        if n is None:
            # Create a new entry
            n = len(self.s2n)
            self.s2n[s] = n
            self.n2s[n] = s
            payload = struct.pack('<BH', ENRBridgeMessage.SigDef, n) + s.encode('utf8')
            self.channel.AddMessage(payload, True)
        return n

    def StringForID(self, n):
        '''Inverse of IDForString. Raises KeyError if not found, though that should never happen.'''
        return self.n2s[n]

    @staticmethod
    def CreateFor(channel):
        channel.recvSigDefs = SignatureDefinitionManager(channel)
        channel.sendSigDefs = SignatureDefinitionManager(channel)

class NRAppBridge:
    '''Connects Python code to the netrep C++ code so that Python code can send NR messages and so that
    incoming messages can be routed to Python. On the C++ side is NRChannel, which is basically a socket - it sends and receives
    messages between two endpoints, without an understanding of what the data means.'''
    # Note: this code has both netIDs (strings) and netIDNums (uint32s). All code outside of this class uses the former,
    # while over the wire and internally in some places (for efficiency) we use the latter.
    def __init__(self):
        self.ChannelIDChanged = Event() # fires (newchannelID) when it gets assigned on a client
        self.ClientJoined = Event() # fires (clientChannelID) when a connection is added
        self.Reset()

    def Reset(self):
        self.isHost = True # True until we learn otherwise (currently we don't get recreated when a client joins a host, so we detect dynamically)
        self.channelID = 0 # aka playerID or userID
        self.hostChannel = None # on clients, the UNRChannel to the host
        self.clientChannels = {} # on the host, all channels to connected clients, channelID -> UNRChannel instance
        self.lastCallTimes = {} # sigDefStr -> timestamp of last time we issued a remote call for it
        self.nextNetIDNum = 1 # the next net ID number we'll assign in a Register call. Only used on the host
        self.netIDToNum = {} # netID name --> numerical ID (a uint32) - nobody outside of the AppBridge knows/cares about numerical ID mapping
        self.numToNetID = {} # inverse of netIDToNum
        self.objs = {} # netID -> weakref to instance; we don't use netIDNum here because clients may register objects by netID prior to getting the netIDNum from the server
        self.dependencyWaiters = {} # netIDNum -> [list of (netIDNum waiting on it, attrName to use when assigning it to the waiter or '')]
        self.unconsumedState = {} # netIDNum -> Bag(.propValues, .dependencies) of replication state we've received but not yet "delivered"
        self.waitingForNetIDNum = {} # on clients, netID -> obj instance that has registered but the mapping has not yet arrived from the server
        self.deferredCalls = {} # unknown netIDNum -> [list of tuples of args for calling _ExecuteLocalCall for calls being held until the recipient replicates]

    def Register(self, obj, netID, spawnType:ENRSpawnReplicatedBy):
        '''Called by NetReplicated.NRRegister'''
        # For engine-wrapped objects, also adjust replication settings
        if hasattr(obj, 'engineObj'):
            e = obj.engineObj
            e.bAlwaysRelevant = True # we don't want the engine to do any relevancy checking
            e.SetReplicateMovement(False) # we never want this
            e.SetReplicates(spawnType == ENRSpawnReplicatedBy.Engine) # from the engine's perspective, it's a replicated actor only if the engine spawns it

        obj.nrSpawnType = spawnType
        if spawnType == ENRSpawnReplicatedBy.Engine:
            # For engine-spawned objects, on the host we auto-append their engine-assigned netGUID to give
            # them a unique name (without this, it becomes really difficult for app code to communicate in time
            # a unique name to client instances - they need to know a unique name prior to them knowing how to
            # come up with it.
            if self.isHost or hasattr(obj, '_nrDeferredNetID'): # if it does have this prop, it means BeginPlay is happening
                netGUID = GetOrAssignNetGUID(obj.GetWorld(), obj.engineObj)
                netID += '_%d' % netGUID
            else:
                # On clients, it's too early to ask the engine for the netGUID, so all we do for now is save the netID
                # for later auto-registration in BeginPlay.
                obj._nrDeferredNetID = netID
                return

        # NOTE: if we add more steps here that are relevant to instances on clients, be sure to update _OnBridgeMessage_ObjInitialState accordingly
        if self.isHost:
            # Allocate a new netIDNum
            num = self.nextNetIDNum
            self.nextNetIDNum += 1
            self.netIDToNum[netID] = num
            self.numToNetID[num] = netID

        # Remember this object, and also set a few data members on it
        self.objs[netID] = weakref.ref(obj)
        obj.nrNetID = netID

        if spawnType == ENRSpawnReplicatedBy.App:
            obj._NRCheckStart() # otherwise, on the host, non-Actor subclasses will never call this

    def Unregister(self, *, netID=None, netIDNum=None):
        '''Flags this object as going away. When called on the host, causes a message to be sent to all clients to also unregister it.'''
        if netID is not None:
            netIDNum = self.netIDToNum[netID]
        elif netIDNum is not None:
            netID = self.numToNetID[netIDNum]
        else:
            assert 0 # gotta supply one or the other

        if self.isHost:
            # Notify all clients that this object is unregistering
            payload = struct.pack('<BI', ENRBridgeMessage.Unregister, netIDNum)
            for chan in self.clientChannels.values():
                chan.AddMessage(payload, True)

        # Forget everything about this object
        self.netIDToNum.pop(netID, None)
        self.numToNetID.pop(netIDNum, None)
        self.dependencyWaiters.pop(netIDNum, None)
        self.unconsumedState.pop(netIDNum, None)
        self.waitingForNetIDNum.pop(netID, None)
        ref = self.objs.pop(netID, None)

        # On clients, we need to kill off objects that are Actor subclasses that were not spawned by the engine
        if ref and not self.isHost:
            obj = ref()
            if obj and obj.nrSpawnType != ENRSpawnReplicatedBy.Engine:
                klass = obj.__class__
                if issubclass(klass, AActor) or issubclass(AActor_PGLUE) or issubclass(type(klass), PyGlueMetaclass):
                    obj.Destroy()

    def _OnBridgeMessage_Unregister(self, chan, payload):
        nrNetIDNum = struct.unpack('<I', payload)
        self.Unregister(netIDNum=nrNetIDNum)

    def OnMessage(self, chan, data):
        '''Called by NRChannel for each incoming message. Routes the message to internal handlers'''
        handlerName = '_OnBridgeMessage_%s' % ENRBridgeMessage.NameFor(data[0]) # all msgs have a ENRBridgeMessage value as the first byte
        getattr(self, handlerName)(chan, data[1:])

    def SendInitialStateFor(self, obj, chan=None):
        '''Called on the host by all NetReplicated objects once they finish spawning (i.e. after returning from their OnReplicated call).
        If chan is not None, the state is sent to a specific channel (for a late joining player), otherwise it is sent to all channels.'''
        assert self.isHost, obj
        if chan is None:
            channels = self.clientChannels.values()
        else:
            channels = [chan]

        if not channels:
            return # happens on host when stuff is spawned but there are no clients yet

        # Build up the message payload - if we add any more messages this convoluted, we should probably write/get a helper lib
        parts = [] ; PA = parts.append

        # message type, spawn type, net ID num, and net ID len+data (so client can recreate the mapping)
        netIDNum = self.netIDToNum[obj.nrNetID]
        PA(struct.pack('<BBIB', ENRBridgeMessage.ObjInitialState, obj.nrSpawnType, netIDNum, len(obj.nrNetID)))
        PA(obj.nrNetID.encode('utf8'))

        # Create an ordered list of repProp values - no need for naming them since the receiving side knows the same prop ordering
        # send the format string for repprops (len+str) and the blob of data (len+data) for it
        formatStr, propsBlob = ValuesToBin([obj.nr[k] for k in obj.nrPropNames])
        PA(struct.pack('<BH', len(formatStr), len(propsBlob)))
        PA(formatStr.encode('utf8'))
        PA(propsBlob)

        # Add info about replicated objects this object depends on. The format is super cheesy, but easy to debug, and often not as
        # inefficient as it looks since netIDs are 4 bytes but rarely that many digits, especially in hex
        deps = [] # list of strings in the form '<netID>:<attrName or emptystr>'
        for netID, attrName in obj.nrDependencies:
            netIDNum = self.netIDToNum[netID]
            deps.append('%X:%s' % (netIDNum, attrName))
        depsStr = '|'.join(deps)
        PA(struct.pack('<H', len(depsStr)))
        PA(depsStr.encode('utf8'))

        # If the objet is NR-spawned, we also need to include its class name so that the client knows what to spawn
        if obj.nrSpawnType == ENRSpawnReplicatedBy.NR:
            className = obj.__class__.__name__
            PA(struct.pack('<B', len(className)))
            PA(className.encode('utf8'))

        # Send it!
        payload = b''.join(parts)
        for chan in channels:
            chan.AddMessage(payload, True)

    def OnChannelFromClient(self, chan):
        '''Called on the host when a channel to a client is connecting to us'''
        # Generate a unique ID for this new channel. This ID can be used as a player/user ID, so there are advantages to
        # keeping this number low, so scan for the next available channel ID as opposed to just using a counter.
        usedIDs = self.clientChannels.keys()
        chanID = 1 # by convention, the host is always player 0, so clients are all >= 1
        while 1:
            if chanID in usedIDs:
                chanID += 1
            else:
                break

        chan.channelID = chanID
        SignatureDefinitionManager.CreateFor(chan)
        self.clientChannels[chanID] = chan

        # Sync state with the client by informing it of its channel ID and the state of all active replicated objects
        chan.AddMessage(struct.pack('<BB', ENRBridgeMessage.SetChannelInfo, chanID), True)
        allObjs = []
        for netID, objRef in self.objs.items():
            obj = objRef()
            if obj:
                netIDNum = self.netIDToNum[obj.nrNetID]
                allObjs.append((netIDNum, obj))
        for netIDNum, obj in sorted(allObjs): # sorted so info gets sent in creation order
            self.SendInitialStateFor(obj, chan)

        self.ClientJoined.Fire(chanID)

    def OnChannelToHost(self, chan):
        '''Called by NRChannel when as a client we have connected to the host'''
        self.isHost = False # oh, hey, it turns out we're not the host after all
        SignatureDefinitionManager.CreateFor(chan)
        self.hostChannel = chan

    def OnChannelClosing(self, chan):
        '''Called by NRChannel when it is closing'''
        if self.isHost:
            self.clientChannels.pop(chan.channelID, None)
        else:
            # We are disconnecting, so now we are the host again (because we are now standalone), so we need to reset
            self.Reset()

    def DoCall(self, recipient, where, methodName, args, reliable, maxCallsPerSec):
        '''Used by NetReplicated to carry out the call logic'''
        if where == ENRWhere.NONE:
            log('WARNING: NRCall to nowhere', recipient, methodName, args)
            return

        channels = set() # channels that will receive the message
        outboundWhere = ENRWhere.NONE # If we do make a remote call, the 'where' value to send in that call
        runLocal = False
        if self.isHost:
            # When called on the host, the valid outcomes are a combination of running locally, telling all channels to run
            # locally, telling one specific channel to run locally.
            if where & ENRWhere.USER:
                chanID = where - ENRWhere.USER
                if chanID == self.channelID:
                    runLocal = True # caller wants it to run on a specific user, and the host is that user
                else:
                    chan = self.clientChannels.get(chanID)
                    if chan:
                        channels.add(chan)
                        outboundWhere |= ENRWhere.Local
            else:
                if where & ENRWhere.Local: runLocal = True
                if where & ENRWhere.Host: runLocal = True # cuz we are the host
                if where & ENRWhere.NotMe:
                    channels.update(self.clientChannels.values())
                    outboundWhere |= ENRWhere.Local
        else:
            # When called on a client, the valid outcomes are a combination of running locally, telling the host to run
            # locally, telling the host to tell all channels (minus this one) to run locally, or telling the host to tell
            # one specific channel to run locally.
            callHost = False
            if where & ENRWhere.USER:
                chanID = where - ENRWhere.USER
                if chanID == self.channelID:
                    runLocal = True # caller wants it to run on a specific user, and we are that user
                else:
                    callHost = True
                    outboundWhere = where # stomp any prior values and set it to userID + user bit
            else:
                if where & ENRWhere.Local: runLocal = True
                if where & ENRWhere.Host:
                    callHost = True
                    outboundWhere |= ENRWhere.Local
                if where & ENRWhere.NotMe:
                    callHost = True
                    outboundWhere |= ENRWhere.All # the receiving side knows this should not include our channel since we're the caller

            if callHost:
                channels.add(self.hostChannel)

        # Make any remote calls
        if channels:
            recipientID = self.netIDToNum[recipient.nrNetID]
            formatStr, blob = ValuesToBin(args)
            fullSig = methodName + '|' + formatStr

            sendMessage = True
            if maxCallsPerSec > 0:
                # Caller wants throttling of remote calls enabled so as to not flood the network (local calls are
                # never throttled though). Right now the throttling is pretty blunt and simply doesn't allow two
                # calls to the same signature (method name + arg types) to happen within a timeframe that exceeds
                # the overall rate, but if needed we could add something a bit smarter that e.g. allows some fast
                # back-to-back calls as long as they don't exceed some number per second overall.
                now = time.time()
                lastCall = self.lastCallTimes.get(fullSig, 0)
                nextAllowedTime = lastCall + 1.0/maxCallsPerSec
                if now < nextAllowedTime:
                    sendMessage = False
                else:
                    self.lastCallTimes[fullSig] = now

            if sendMessage:
                mixedSessionID = recipient.NRGetMixedReliabilitySessionID(methodName, reliable)
                for chan in channels:
                    sigDefID = chan.sendSigDefs.IDForString(fullSig)
                    payload = struct.pack('<BBHIBB', ENRBridgeMessage.Call, outboundWhere, sigDefID, recipientID, mixedSessionID, reliable) + blob
                    chan.AddMessage(payload, reliable)
                    # (Yes, we really do want to pass reliable as a parameter to send in the call and also as a parameter *to* _Send)

        # Call locally - we do this after any remote calls so that if the local call triggers more NR calls, they will also
        # be sent to the remote machines in the same order
        if runLocal:
            self._DoLocalCall(recipient, methodName, args)

    def _OnBridgeMessage_SigDef(self, chan, payload):
        ''''Called when we receive a sigdef from the other side of the channel'''
        sigDefID = struct.unpack('<H', payload[:2])[0]
        sigDef = str(payload[2:], 'utf8')
        chan.recvSigDefs.Set(sigDefID, sigDef)

    def _OnBridgeMessage_SetChannelInfo(self, chan, payload):
        '''Called on client to inform it of its channel/player/user ID'''
        assert not self.isHost
        self.channelID = payload[0]
        self.ChannelIDChanged.Fire(self.channelID)

    def _OnBridgeMessage_ObjInitialState(self, chan, payload):
        '''Called on client to replicate the initial state of an actor. If the object is NR-spawned, also causes the object
        to be spawned.'''
        # Parse this mess in the least efficient way possible
        spawnType, netIDNum, netIDLen = struct.unpack('<BIB', payload[:6])
        payload = payload[6:]
        netID = str(payload[:netIDLen], 'utf8')
        payload = payload[netIDLen:]

        formatStrLen, propsBlobLen = struct.unpack('<BH', payload[:3])
        payload = payload[3:]
        formatStr = str(payload[:formatStrLen], 'utf8')
        payload = payload[formatStrLen:]
        propsBlob = payload[:propsBlobLen]
        payload = payload[propsBlobLen:]

        depsStrLen = struct.unpack('<H', payload[:2])[0]
        payload = payload[2:]
        depsStr = str(payload[:depsStrLen], 'utf8')
        payload = payload[depsStrLen:]
        deps = []
        if depsStr:
            for dep in depsStr.split('|'):
                depNetIDNum, attrName = dep.split(':')
                depNetID = self.numToNetID[int(depNetIDNum, 16)]
                deps.append((depNetID, attrName))

        className = None
        if spawnType == ENRSpawnReplicatedBy.NR:
            classNameLen = payload[0]
            className = str(payload[1:classNameLen+1], 'utf8')
            payload = payload[1+classNameLen:]

        # Set up the same mapping the host has for this object
        self.netIDToNum[netID] = netIDNum
        self.numToNetID[netIDNum] = netID

        # Save the state and dependency information so it can later be attached to the actors
        propValues = BinToValues(formatStr, propsBlob)
        self.unconsumedState[netIDNum] = Bag(propValues=propValues, dependencies=deps)

        obj = None
        if className is not None:
            # It's an NR-spawned object, so spawn it now. It doesn't necessarily know its own netID, so
            # we take care of registration on its behalf.
            klass = NRTrackerMetaclass.All.get(className)
            assert klass, 'Unable to find class to spawn: ' + className

            if issubclass(klass, AActor) or issubclass(klass, AActor_PGLUE) or issubclass(type(klass), PyGlueMetaclass):
                # If it's an engine class of one of the Python quasi-subclasses of an engine class, we need to use
                # an engine API to spawn it (though I guess SpawnActor wouldn't work for U-subclasses)
                obj = SpawnActor(GetWorld(), klass)
            else:
                obj = klass()
            obj.nrNetID = netID
            obj.nrSpawnType = spawnType
            self.objs[netID] = weakref.ref(obj)
        else:
            ref = self.objs.get(netID)
            if ref:
                obj = ref()

        # If we know about the object (either because we just spawned it above, or because it already registered itself), trigger it
        # to consume its initial state. If we don't know about the obj in question, it just means that at some point it'll show up
        # and it will call checkstart on its own.
        if obj:
            obj._NRCheckStart()

    def _OnBridgeMessage_Call(self, senderChannel, payload):
        '''The receiving side of a NetReplicated.NRCall -> bridge.DoCall sequence. Runs it locally and/or passes it on to other clients.'''
        where, sigDefID, recipientID, mixedSessionID, reliable = struct.unpack('<BHIBB', payload[:9])
        blob = payload[9:]
        runLocal = False
        sigDef = senderChannel.recvSigDefs.StringForID(sigDefID)
        if self.isHost:
            # When a message is received on the host, valid outcomes are to run it locally, to forward it on to all channels
            # except for the one that sent the message, or to forward it on to one specific channel
            channels = [] # Channels we'll forward the message to
            if where & ENRWhere.USER:
                chanID = where - EWhere.USER
                if chanID == self.channelID:
                    runLocal = True
                else:
                    # The requested channel is some other machine, so forward it on
                    chan = self.clientChannels.get(chanID)
                    if chan: channels.append(chan)
            else:
                if where & ENRWhere.Local:
                    runLocal = True
                if where == ENRWhere.All:
                    # forward it to all clients *except* the caller
                    senderID = senderChannel.channelID
                    for chanID, chan in self.clientChannels.items():
                        if chanID != senderID:
                            channels.append(chan)

            # Tell the channels we're forwarding to to run it locally
            for chan in channels:
                sigDefID = chan.sendSigDefs.IDForString(sigDef)
                payload = struct.pack('<BBHIBB', ENRBridgeMessage.Call, ENRWhere.Local, sigDefID, recipientID, mixedSessionID, reliable) + blob # we include 'reliable' here only cuz the msg format wants it
                chan.AddMessage(payload, reliable)
        else:
            # When a message is received on a client, the only valid scenario is a command to run it locally
            if where != ENRWhere.Local:
                log('ERROR: client received a remote call with a non-local destination:', where)
            else:
                runLocal = True

        if runLocal:
            # Find the recipient and call it, if it is known. If it is an unknown recipient, we assume the call is for an object
            # that will be replicated soon, so we'll add it to a list of calls to delay until then.
            recipientNetID = self.numToNetID.get(recipientID)
            if recipientNetID is None:
                if self.isHost:
                    # No point in deferring the call - we are the host, so maybe it's just a late call for a recently-deceased obj?
                    log('ERROR: ignoring local call to unknown recipient', recipientID)
                else:
                    # Defer the call til later
                    log('Deferring call to', recipientID, sigDef)
                    self.deferredCalls.setdefault(recipientID, []).append((recipientID, sigDef, mixedSessionID, reliable, bytes(blob)))
            else:
                self._ExecuteLocalCall(recipientID, sigDef, mixedSessionID, reliable, blob)

    def _ExecuteLocalCall(self, recipientID, sigDef, mixedSessionID, reliable, blob):
        '''Helper used for running local function calls - broken out from _OnBridgeMessage_Call because we need to sometimes
        defer function calls until after an object has fully replicated. And yes, it is annoying that we have both this and
        _DoLocalCall.'''
        recipientNetID = self.numToNetID.get(recipientID)
        ref = self.objs.get(recipientNetID)
        if not ref:
            log('ERROR: non-existent recipient', recipientID, recipientNetID, sigDef)
        else:
            obj = ref()
            if obj:
                methodName, formatStr = sigDef.split('|')

                # If a mixed reliability session is active, drop any messages that don't conform, as it indicates they
                # are out of order/delayed messages that should be ignored
                if mixedSessionID != 0:
                    expectedID = obj.NRGetMixedReliabilitySessionID(methodName, reliable)
                    if expectedID != mixedSessionID:
                        log('Tossing late mixed mode message for', obj, methodName, '(expected session %d, got %d, reliable:%s)' % expectedID, mixedSessionID, reliable)
                        return

                args = BinToValues(formatStr, blob)
                self._DoLocalCall(obj, methodName, args)

    def _DoLocalCall(self, obj, methodName, args):
        '''Helper for dispatching an NRCall to a NetReplicated object'''
        method = getattr(obj, methodName, None)
        if method is None:
            log('ERROR: NRCall to', obj, 'but it has no method named', methodName)
        else:
            try:
                method(*args)
            except:
                logTB()

    def NoteObjectReplicated(self, obj):
        '''Called by NetReplicated._NRCheckStart once it has fully replicated on a client. Executes any differed calls, and also informs
        any objects waiting on this object to replicate that that dependency has been met.'''
        # Execute any NRCalls that arrived prior to the object being fully replicated
        netIDNum = self.netIDToNum[obj.nrNetID]
        for args in self.deferredCalls.pop(netIDNum, []):
            self._ExecuteLocalCall(*args)

        # Find all other objects that have been waiting on this object to finish replicating
        for waiterNetIDNum, attrName in self.dependencyWaiters.pop(netIDNum, []):
            waiterNetID = self.numToNetID[waiterNetIDNum]
            waiterRef = self.objs.get(waiterNetID)
            if waiterRef:
                waiterObj = waiterRef()
                if waiterObj:
                    try: waiterObj.nrWaitingDependencies.remove(obj.nrNetID)
                    except ValueError: pass # shouldn't happen but...
                    if attrName:
                        # The waiting object wants a ref of the newly-replicated object stored on it
                        setattr(waiterObj, attrName, obj)

                    if not waiterObj.nrWaitingDependencies:
                        # This object is no longer waiting for any dependencies, so trigger it to possibly finish its own replication now
                        waiterObj._NRCheckStart()

    def TryConsumeInitialState(self, obj):
        '''Called on clients by an object's _NRCheckStart as it tries to complete replication. If initial state for the object
        has been received (via _OnBridgeMessage_ObjInitialState), applies it and returns True, indicating that it can proceed with other
        initialization steps. Otherwise, returns False, indicating it should try again later. If state is found, the objects repProp
        values are updated accordingly, and the object's dependency information is set up.'''
        netIDNum = self.netIDToNum.get(obj.nrNetID)
        if netIDNum is None:
            # We don't even have a mapping for this object yet
            return False # try again later!

        state = self.unconsumedState.pop(netIDNum, None)
        if state is None:
            return False # try again later!

        # We have this object's state, so set its repProp values - we received just an ordered list of values since we know
        # the property names already.
        for propName, propValue in zip(obj.nrPropNames, state.propValues):
            setattr(obj.nr, propName, propValue)

        # Also resolve or set up tracking of any replicated objects this object depends on
        waitingFor = [] # netIDs of objs that have not yet arrived
        for depNetID, attrName in state.dependencies:
            # See if the object exists and is fully replicated already
            depNetIDNum = self.netIDToNum.get(depNetID)
            if depNetIDNum is not None:
                ref = self.objs.get(depNetID)
                if ref:
                    depObj = ref()
                    if depObj and depObj.nrOnReplicatedCalled:
                        # Yay, this object has already fully replicated
                        if attrName:
                            setattr(obj, attrName, depeObj) # this object wants a named reference to that other object
                        continue

            # Either the object doesn't exist yet or it does but it too is still replicating, so we'll wait to list this
            # object as waiting on it
            waitingFor.append(depNetID)
            self.dependencyWaiters.setdefault(depNetIDNum, []).append((netIDNum, attrName))

        obj.nrWaitingDependencies = waitingFor
        return True # you've got your state info, so quit nagging us about it

    def NetIDNumFromObject(self, obj):
        '''Helper for marshalling - given a replicated object, return its netID number'''
        netID = getattr(obj, 'nrNetID', None)
        assert netID is not None, 'NetIDNumFromObject called on unreplicated object ' + repr(obj)
        return self.netIDToNum[netID]

    def ObjectFromNetIDNum(self, netIDNum):
        '''Inverse of the above; returns None if the object can't be found'''
        netID = self.numToNetID.get(netIDNum)
        if netID is None:
            log('ERROR: bridge failed to find netID for', netIDNum)
            return None
        ref = self.objs.get(netID)
        if ref is None:
            log('ERROR: bridge failed to find object for', netIDNum, netID)
            return None
        return ref()

_bridge = NRAppBridge()
UNRChannel.SetAppBridge(_bridge)

def GetUserID():
    '''Public API used by __init__'''
    return _bridge.channelID

class NRWrappedDefault:
    '''Used for repProps where the application code needs additional annotation information - apps can create custom subclasses
    instead of providing "bare" default values; NR will just take the .defaultValue member as the default. For example:
    class Config(NRWrappedDefault): pass
    repProps = Bag(foo=Config(0), bar=3)'''
    def __init__(self, defaultValue):
        self.defaultValue = defaultValue

class NetReplicated(metaclass=NRTrackerMetaclass):
    '''Mixin class to add to any class (doesn't have to be an actor) that wants to use network
    replication'''
    # subclasses should declare their own repProps Bag, where each key is the property name (it will later be
    # available in self.nr.<that name> and the value is the default value for that property. repProps from
    # parent classes are automatically folded into subclasses and do not need to be repeated.
    repProps = Bag()
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.nrNetID = None # unique ID for this object, set by calling bridge.Register.
        self.nrSpawnType = ENRSpawnReplicatedBy.App # who is in charge of replicating this object
        self.isHost = GetWorld().IsServer() # cached here since it doesn't change once code gets this far
        self.nrInitialStateSet = False # on clients, has initial state been received from the host?
        self.nrBeginPlayCalled = False # on all actors, has BeginPlay been called?
        self.nrOnReplicatedCalled = False # on all instances, has OnReplicated been called?
        self.nrDependencies = [] # list of (netID, attrName or '') of replicated objects this object depends on. Only set on host.
        self.nrWaitingDependencies = [] # netIDs of replicated objects this object is waiting on before OnReplicated will be called
        self.nrMixedSessionIDs = {} # method name -> mixed reliability session ID that is currently active

        # Generate the full set of replicated properties and set them to their defaults. By convention, properties
        # are referenced by index in alphabetical order, and no object can have more than 255 replicated properties.
        allProps = Bag()
        for klass in self.__class__.__mro__[::-1]: # reverse order so we start at the top of the MRO list
            allProps.update(**getattr(klass, 'repProps', {}))
        self.nrPropNames = sorted(allProps.keys()) # the official, ordered list of valid replicated property names for this object

        propInfo = {} # prop name -> Bag of info about it
        self.nr = Bag() # At some point we may make this a read-only struct so you have to use NRUpdate
        for i, propName in enumerate(self.nrPropNames):
            v = rawV = allProps[propName]
            if isinstance(v, NRWrappedDefault):
                v = v.defaultValue
            self.nr[propName] = v
            propInfo[propName] = Bag(index=i, type=type(v), default=v, rawDefault=rawV)
        self.nrPropInfo = propInfo # in case apps need to extract info for other purposes

    def NRRegister(self, netID, spawnType:ENRSpawnReplicatedBy):
        '''Registers this instance so that future messages to the given netID will be sent to it. Must be called in all scenarios
        (from __init__) *except* in NR-spawned instances on clients.'''
        _bridge.Register(self, netID, spawnType)

    def NRUnregister(self):
        '''Communicates to the replication system that this object will be cleaned up soon. For AActor subclasses, this call
        happens automatically via EndPlay.'''
        _bridge.Unregister(netID=self.nrNetID)

    def NRSetDependencies(self, *depInfos):
        '''Called on the host to declare that this object should not be considered fully replicated on clients until all of the listed
        objects have also fully replicated. Each item can be either a NetReplicated object instance or the name of a property on self
        that holds a ref to a NetReplicated object. In the latter case, once that object has replicated to a client, a reference to it
        will be set on self using that name.'''
        assert self.isHost
        deps = self.nrDependencies = [] # (netID, attrName or ''). These will be used by bridge.SendInitialStateFor.
        for d in depInfos:
            if isinstance(d, str):
                # The name of a property on self that references a NetReplicated object
                ref = getattr(self, d, None)
                assert ref is not None, 'NRSetDependencies cannot find property %r' % d
                assert isinstance(ref, NetReplicated), 'NRSetDependencies cannot depend on a non-replicated object: ' + repr((d, ref))
                deps.append((ref.nrNetID, d))
            else:
                assert isinstance(d, NetReplicated), 'NRSetDependencies cannot depend on a non-replicated object: ' + repr(d)
                deps.append((d.nrNetID, ''))

    def NRStartMixedReliability(self, methodName):
        '''Signals that the application code is going to have some unreliable message calls followed by a terminating reliable call.
        A recurring pattern in multiplayer stuff is to replicate some action from a user that involves a lot of updates, such as
        dragging an object around, and then letting go of it to drop the object somewhere. While the drag is happening, you want
        most players to get updates that that drag is happening, but those updates can be throttled & unreliable. When the user
        releases the object at its final location, you want to send a final, reliable message. Since the messages are all sent
        over UDP, it's possible for one of the intermediate messages to show up after the final message, which results in incorrect
        placement of the object. The solution is to call NRStartMixedReliability when the user begins the drag operation; we generate a small
        random ID for the (recipient, method) combination and include it with the messages. The receiving side then rejects any
        unreliable messages that use an invalid (or out of date) ID as a way to detect and prevent the above scenario.'''
        sessID = random.randint(1, 255) # 0=no active session, so pick [1..255]
        self.NRCall(ENRWhere.All, '_OnStartMixedReliability', methodName, sessID)

    def _OnStartMixedReliability(self, methodName, sessID):
        '''Receives word that a mixed reliability session (stream of unreliable messages followed by a final, reliable message) is starting'''
        self.nrMixedSessionIDs[methodName] = sessID

    def NRGetMixedReliabilitySessionID(self, methodName, reliable):
        '''Used by the bridge to get the current mixed reliability session ID for calls to the given method. Used for both sending
        messages (to know what session ID to include or 0 if none is active) and for receiving messages (to detect if an out of order
        unreliable message is being received after the final reliable message).'''
        sessID = self.nrMixedSessionIDs.get(methodName)
        if sessID is None:
            return 0 # no active session

        # if the message being sent or received is reliable, it signals the end of the mixed reliability session, so we should
        # remove the ID for use in future calls.
        if reliable:
            self.nrMixedSessionIDs.pop(methodName)
            return 0
        return sessID

    def NRCall(self, where:ENRWhere, methodName, *args, reliable=True, maxCallsPerSec=-1):
        '''Performs a replicated call to one or more machines. To send a message to a specific user, use ENRWhere.Only(userID).'''
        _bridge.DoCall(self, where, methodName, args, reliable, maxCallsPerSec)

    def NRUpdate(self, where=ENRWhere.All, reliable=True, maxCallsPerSec=-1, **kwargs):
        '''Replicates an update to one or more replicated properties (to all machines by default). As a special
        case, if called from the constructor on the host, where is implicitly set to Local, as a way to override default values
        before the initial state is replicated to other machines.'''
        if where == ENRWhere.NONE or not kwargs: return

        # NRUpdate piggybacks on NRCall by sending a parameter that is a string holding a list of repprop indicces, then all the
        # values. We put the properties in order by their indices so that the same combination of parameters will use the same sigdef.
        propNames = [] # list of all the properties being updated
        pairs = [] # (propIndex, new value)
        invalidNames = []
        for propName, value in kwargs.items():
            propNames.append(propName)
            try:
                propIndex = self.nrPropNames.index(propName)
                pairs.append((propIndex, value))
            except ValueError:
                invalidNames.append(propName)
                continue

        assert not invalidNames, 'NRUpdate called with one or more invalid property names: ' + repr(invalidNames)

        if not self.nrOnReplicatedCalled:
            # Probably just being called from the constructor on the host to override initial default values
            assert self.isHost # clients shouldn't try to override defaults since their state will be immediately overwritten by the initial state from the host
            assert where in (ENRWhere.All, ENRWhere.Local)
            for k,v in kwargs.items():
                self.nr[k] = v
        elif where == ENRWhere.Local:
            # Special case but common enough that we want to handle it here: it's just a local update, so skip a whole bunch of extra work
            for k,v in kwargs.items():
                setattr(self.nr, k, v)
            self.OnNRUpdate(propNames)
        else:
            indices = []
            values = []
            for propIndex, value in sorted(pairs): # put them in index order
                indices.append(str(propIndex))
                values.append(value)
            indicesStr = '_'.join(indices)
            # At some point we could do like NRCall and combine the indicesStr with the method name so that it can be sent as a sigdef,
            # though it'd require NRCall to handle NRUpdate as a special case.
            self.NRCall(where, '_OnNRUpdate', indicesStr, *values, reliable=reliable, maxCallsPerSec=maxCallsPerSec)

    def _OnNRUpdate(self, indicesStr, *args):
        '''The receiving end of NRUpdate'''
        indices = [int(x) for x in indicesStr.split('_')] # indicesStr is a list of repprop indices in a _-separateed string like '2_5_10'
        propNames = []
        for propIndex, value in zip(indices, args):
            # Convert the index to a prop name and update that property to its new value
            propName = self.nrPropNames[propIndex]
            propNames.append(propName)
            setattr(self.nr, propName, value)
        self.OnNRUpdate(propNames)

    def OnNRUpdate(self, modifiedPropNames):
        '''Called anytime an NRUpdate has happend on this object. Subclasses can override. Default implementation triggers
        and OnRep_* methods to be called.'''
        for name in modifiedPropNames:
            handler = getattr(self, 'OnRep_' + name, None)
            if handler:
                try:
                    handler()
                except:
                    logTB()

    def BeginPlay(self):
        '''(Used only on AActor subclasses)'''
        if self.nrSpawnType == ENRSpawnReplicatedBy.Engine and not self.isHost:
            # The NRRegister call made by app code on the client actually had no effect because we didn't know
            # our netGUID yet. But now that we've gotten this far, the engine has assigned us a netGUID, so we
            # can use it to register ourselves using the same unique name that happend for this instance on the host.
            _bridge.Register(self, self._nrDeferredNetID, ENRSpawnReplicatedBy.Engine)
        super().BeginPlay()
        self.nrBeginPlayCalled = True
        self._NRCheckStart()

    def EndPlay(self, reason):
        # Note that EndPlay isn't called for non-engine subclasses, so in that case the app needs to call NRUnregister itself
        # (e.g. from __del__ or something)
        self.NRUnregister()
        super().EndPlay(reason)

    def _NRCheckStart(self):
        '''Called from various points of initialization to decide when it's time to call OnReplicated'''
        if not hasattr(self, 'nrNetID'):
            log('ERROR: object', self, 'failed to call NRRegister')
            return
        if self.nrOnReplicatedCalled:
            log('ERROR: _NRCheckStart called even though OnReplicated has already been called', self, self.nrNetID, Caller())
            return
        if not self.isHost:
            if hasattr(self, 'engineObj') and not self.nrBeginPlayCalled: return # we're expecting a BeginPlay call that hasn't happened yet
            if not self.nrInitialStateSet:
                # See if the host has delivered our initial state
                self.nrInitialStateSet = gotIt = _bridge.TryConsumeInitialState(self)
                if not gotIt:
                    return # we'll try again later
                # otherwise, fall thru

        if self.nrWaitingDependencies: return # still waiting on one or more dependent objects to fully replicate

        # We're good to go finally!
        self.nrOnReplicatedCalled = True
        try:
            self.OnReplicated()
        except:
            logTB()
        if self.isHost:
            # Now trigger replication of our state - we defer to here so that subclasses can do any further setup that they
            # want to do and to give them a chance to declare any replication dependencies
            _bridge.SendInitialStateFor(self)
        else:
            # Now that this object is fully replicated, objects that are depending on it can move forward
            _bridge.NoteObjectReplicated(self)

# Stuff for marshalling objects over the wire. We use a single-character type code and then the data itself in some binary format.
# The typecodes are combined into a format string so that they are not sent over and over, though in retrospect it adds a lot of complexity
# for very little savings. Maybe in v3 we'll get rid of it!

# struct packers for given types
F_Float = struct.Struct('<f') # Note: technically we should use 'd' (8 bytes) because Python's floats seem to be closer to doubles in terms of precision
F_Int = struct.Struct('<i')
F_Short = struct.Struct('<H')
F_FVector = struct.Struct('<fff')
F_FVector2D = struct.Struct('<ff')
F_FRotator = struct.Struct('<fff')
F_FLinearColor = struct.Struct('<ffff')
F_FTransform = struct.Struct('<fffffffff')
F_FQuat = struct.Struct('<ffff')

def ValueToBin(arg):
    '''Given a value to pass over the network, returns (typeCode, value)'''
    # Note: if you add more, (a) add it to ValuesToBin and (b) keep the type code to a single character
    if arg is None: return '0', b''
    if isinstance(arg, bool): return 'B', struct.pack('?', arg)
    if isinstance(arg, float): return 'F', F_Float.pack(arg)
    if isinstance(arg, int): return 'I', F_Int.pack(arg)
    if isinstance(arg, str): return 'S', F_Short.pack(len(arg)) + arg.encode('utf8')
    if isinstance(arg, FVector): return 'V', F_FVector.pack(*arg)
    if isinstance(arg, FVector2D): return 'v', F_FVector2D.pack(*arg)
    if isinstance(arg, FRotator): return 'R', F_FRotator.pack(*arg)
    if isinstance(arg, FLinearColor): return 'L', F_FLinearColor.pack(*arg)
    if isinstance(arg, type):
        className = arg.__name__
        return 'C', F_Short.pack(len(className)) + className.encode('utf8')
    if isinstance(arg, FTransform): return 'T', F_FTransform.pack(*arg.GetLocation(), *arg.GetRotation(), *arg.GetScale3D())
    if isinstance(arg, FQuat): return 'Q', F_FQuat.pack(*arg)
    if hasattr(arg, 'nrNetID'): return 'O', F_Int.pack(_bridge.NetIDNumFromObject(arg))

    if hasattr(arg, 'engineObj'):
        # The object isn't replicated but it is an engine object, so assume it's an asset that has been loaded
        # by LoadByRef (though we should come up with some way to confirm this assumption), so generate and send a reference
        # path
        ref = GetReferencePath(arg)
        return 'A', F_Short.pack(len(ref)) + ref.encode('utf8')

    assert 0, 'Do not know how to marshall ' + repr((type(arg), arg))

def ValuesToBin(args):
    '''Given a list/tuple of args, returns (formatStr, binary blob)'''
    formatParts = []
    blobParts = []
    for arg in args:
        f,b = ValueToBin(arg)
        formatParts.append(f)
        blobParts.append(b)
    return ''.join(formatParts), b''.join(blobParts)

def BinToValues(formatStr, blob):
    '''Inverse of ValuesToBin, returns a list of Python objects'''
    if not formatStr:
        return ()
    ret = []
    dataIndex = 0 # where in blob we're reading from next
    for typeCode in formatStr:
        if typeCode == '0':
            ret.append(None)
            # Do not increase dataIndex
        elif typeCode == 'B':
            ret.append(blob[dataIndex] != 0)
            dataIndex += 1
        elif typeCode == 'F':
            ret.append(F_Float.unpack_from(blob, dataIndex)[0])
            dataIndex += F_Float.size
        elif typeCode == 'I':
            ret.append(F_Int.unpack_from(blob, dataIndex)[0])
            dataIndex += F_Int.size
        elif typeCode == 'S':
            sLen = F_Short.unpack_from(blob, dataIndex)[0]
            dataIndex += F_Short.size
            ret.append(str(blob[dataIndex:dataIndex+sLen], 'utf8'))
            dataIndex += sLen
        elif typeCode == 'V':
            ret.append(FVector(*F_FVector.unpack_from(blob, dataIndex)))
            dataIndex += F_FVector.size
        elif typeCode == 'v':
            ret.append(FVector2D(*F_FVector2D.unpack_from(blob, dataIndex)))
            dataIndex += F_FVector2D.size
        elif typeCode == 'R':
            ret.append(FRotator(*F_FRotator.unpack_from(blob, dataIndex)))
            dataIndex += F_FRotator.size
        elif typeCode == 'L':
            ret.append(FLinearColor(*F_FLinearColor.unpack_from(blob, dataIndex)))
            dataIndex += F_FLinearColor.size
        elif typeCode == 'C':
            sLen = F_Short.unpack_from(blob, dataIndex)[0]
            dataIndex += F_Short.size
            className = str(blob[dataIndex:dataIndex+sLen], 'utf8')
            dataIndex += sLen
            klass = NRTrackerMetaclass.All.get(className)
            assert klass is not None, 'BinToValues unable to find class ' + className
            ret.append(klass)
        elif typeCode == 'T':
            loc = FVector(*F_FVector.unpack_from(blob, dataIndex))
            dataIndex += F_FVector.size
            rot = FRotator(*F_FRotator.unpack_from(blob, dataIndex))
            dataIndex += F_FRotator.size
            scale = FVector(*F_FVector.unpack_from(blob, dataIndex))
            dataIndex += F_FVector.size
            ret.append(FTransform(loc, rot, scale))
        elif typeCode == 'Q':
            ret.append(FQuat(*F_FQuat.unpack_from(blob, dataIndex)))
            dataIndex += F_FQuat.size
        elif typeCode == 'O': # a reference to a replicated object
            netIDNum = F_Int.unpack_from(blob, dataIndex)[0]
            dataIndex += F_Int.size
            obj = _bridge.ObjectFromNetIDNum(netIDNum)
            if not obj:
                log('ERROR: BinToValues failed to find object for', repr(netIDNum))
            ret.append(obj)
        elif typeCode == 'A': # reference path
            sLen = F_Short.unpack_from(blob, dataIndex)[0]
            dataIndex += F_Short.size
            ret.append(LoadByRef(str(blob[dataIndex:dataIndex+sLen], 'utf8')))
            dataIndex += sLen
        else:
            assert 0, typeCode
    return ret

