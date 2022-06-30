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
- In order for an object on the host and a corresponding object on a client to know they are the "same" object, both must register with NR
    (via self.NRRegister) in their __init__ function, using a name (aka 'netID') that is the same on both machines. The exception is for
    NR-spawned objects on the client: they should /not/ call NRRegister because they are registered automatically. Engine-spawned objects
    (such as the pawn) automatically have their engine netGUID appended to their netID (e.g. if you register with a name 'pawn' then it'll
    internally be registered as something like 'pawn_5') - this frees the application code from trying to figure out how to uniquely
    identify instances and helps avoid a chicken-and-egg problem on clients (them needing to register using a unqiue ID prior to them having
    any information from the host that could be used to uniquely identify themselves). So:
    - for engine-spawned: on host and clients call NRRegister in __init__ using a name that basically indicates the type ('vrpawn')
    - for NR-spawned: on host call NRRegister in __init__ with a unqiue name (e.g. myactor_234523453)
    - for app-spawned: on host and clients call NRRegister in __init__ using a unique name both sides know (e.g. 'mygamestate')
- A common pattern when the user is directly generating replication events (e.g. moving an object around) is to send throttled, unreliable
    events while the user interaction is underway, and then once the user is done, send a final, reliable, and unthrottled event so that
    everyone has the correct final value. In order to make this work (and because UE4 sends messages over UDP), application code needs to
    call NRStartMixedReliability once before the first intermediate/unreliable message goes out.
- Non-Actor replicated objects should call self.NRUnregister() when they are being destroyed.

The "core" replication stuff is patterned after UE4's native replication pretty closely, but we have some cases - especially with UI - where
we need to keep information in sync between autonomous proxies and simulated proxies. For example, imagine a user that has a UMG widget
attached to their VR controller. User A user can look at his/her hand and see it, but user B can also look at user A's hand and see it too.
On user B's machine, there is a simulated proxy pawn for user A, and that pawn has a copy of that UMG widget. We want the state of that widget
on user B's machine to be the same as it is on user A's machine (e.g. if the UMG has a scrollable area and user A has scrolled 30% of the way,
then on user B's machine, when user B looks at user A's hand, user B should see the scrollable area has been scrolled 30% as well). We need
all of this to work without a ton of tedious code to synchronize the state of each widget from the automated proxy (on the locally controlled
machine) to the simulated proxies (on all the other machines). We support this via "fragments" and "elements", explained below. (Note that the
explanation is in terms of UI, but nothing in fragments/elements is specifically tied to UI or UMG and so it could be used for other things too).
- a fragment represents a chunk of UI - e.g. a menu, a popup dialog, a portion of  tabbed menu that contains one or more widgets
    whose state we want to keep synchronized across copies of users, i.e. if you are on tab 2 of a menu and another user looks at
    your menu in VR, they'll see that you're on the same tab too - the UI state of the copy of your pawn running on their machine
    matches the UI state of your copy of your pawn on your machine.
- a fragment is some subset of all of the state associated with a replicated actor known as the owner, but in application code it's state
    that doesn't live directly on that actor (otherwise we'd just use repProps).
- a fragment is identified by an application-wide unique name, e.g. 'tabhome', 'files.list', etc. There may be multiple *instances*
    of that fragment in existence (each user has their own menu), but there is never more than one instance of a given fragment
    for a given owner.
- inside a fragment are one or more elements (widgets, in this case) that are objects grouped together and each has state that needs
    to be kept in sync across machines. For each element, there is an API call to register it as being grouped under a particular fragment.
- each element must be registered in a deterministic way, such that, for a given fragment, the order of its associated elements will
    be the same across all machines (which should just happen naturally).
- in app code, fragments don't exist per se - a cluster of elements just register themselves with the same fragment name. The concept
    of fragments exists to solve a couple of problems, the primary one being that they provide namespacing, otherwise every sync'd widget
    in the entire application would need a globally unique ID that would either have to be known at startup (so, no dynamically loaded
    code) or would have to be communicated across all machines (which would be gross). This is further complicated by the fact that
    things like UI code is not all loaded at startup, and when it is loaded, the order can vary across machines (e.g. you're in a session
    by yourself and load some object and look at its configurator, and then someone joins your session, and then you look at some
    other configurator. Your machine has loaded configurators [A,B] and the other machine has loaded configurator [B], so they couldn't
    be referred to by e.g. a sequential index. Within something like a menu, however, it's extremely likely that each widget we want
    to keep in sync across the network *is* created in a deterministic order, so fragments let us take advantage of that and not have
    to generate/replicate known IDs for every single replicated widget.
- while registering an element, you specify a list of property names for that element that should be synchronized, e.g. a particular
    toggle button may synchronize its visibility and toggled states but not its button text (which properties need to be synchronized
    is context-specific. With configurators, for example, often a lot of UI state is already synchronized by virtue of being driven by
    the configs of the selected objects, so there's no need to synchronize the state again via fragments and elements).
- the "direction" of synchronization is determined by which copy of the actor is locally controlled: changes that happen on the locally
    controlled copy of the actor are then synchronized to all non-locally controlled copies (so, it is important for pawns to take
    ownership of objects while the user is editing them).
- the host owns the mappings between fragment names and IDs, and sends the current mapping to clients as they join, as well as new
    mappings that get added along the way. Even with all this, there are still gaps of time when the client won't know the fragment ID
    yet, so we treat IDs as optimizations: it's always legal to reference a fragment by its name - even over the wire - but when the
    host sees a name being used instead of an ID, it will send an out of band message to that client informing it of the ID to use.
- similarly, sometimes updates to elements arrive prior to the element being instantiated or registered, so when an update arrives for
    an unknown fragment element, we save it and deliver it once the element is registered.

TODO:
- with a lot of the binary data handling, we make unnecessary copies of the data - it'd be better to use byte arrays / memoryviews
    (or at the very least, struct.unpack_from)
- figure out some way to do creation of dependent child objects that take init parameters.
- splitting data marshalling into format type codes and encoded data blob has a low ROI. It would be better to keep the two merged. We probably
    want it for array supoort anyway.
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
        self.NRUpdate(foo__set=(2,value)) # ugh
        self.NRUpdate(foo=[1,2,3]) # still allow this, but doesn't try to diff and unconditionally resends all
      we don't need to support every possible use of arrays; instead focus on likely/recommended uses of replicated arrays
    - once you support arrays, might as well add support for dictionaries and sets! (and the var__action=X form would work well for them)
- when the host leaves, the clients don't clean up all the host-spawned objects (not really sure if they should or what)
'''

from uepy import *
from uepy.enums import *
import struct, weakref, time, random, inspect, threading

class ENRBridgeMessage(Enum): NONE, SigDef, SetChannelInfo, FragmentMapping, ObjInitialState, InitialStateComplete, Call, Unregister = range(8)

class GV:
    trafficDumpF = None # if not None, an open file handle to which we'll dump all outgoing network traffic to

def SetDebugTrafficFile(f):
    '''Starts/stops debug dumping to a file. If f is None, no traffic is dumped. Otherwise, f is expected to be a file-like
    object opened in binary writing mode. All subsequent outgoing network messages will be written to the file along with
    a timestamp and reliable flag (see code below).'''
    GV.trafficDumpF = f

logLock = threading.RLock()
def _SendMessage(channel, payload, reliable):
    if not channel or not channel.IsValid():
        log('Skipping message on invalid channel from', Caller())
    else:
        channel.AddMessage(payload, reliable)
        f = GV.trafficDumpF
        if f: # debug dump enabled
            with logLock:
                try:
                    f.write(struct.pack('<dbbL', time.time(), channel.channelID, reliable, len(payload)))
                    f.write(payload)
                except:
                    logTB()

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
            _SendMessage(self.channel, payload, True)
        return n

    def StringForID(self, n):
        '''
        Inverse of IDForString. Returns None if not found.
        May happen if unreliable messages are being pushed by the server
        on client connect. Caller should ignore messages with a None
        respose.
        '''
        return self.n2s.get(n, None)

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
        self.ChannelIDChanged = Event() # (client) fires (newchannelID) when it gets assigned on a client
        self.ClientJoined = Event() # (host) fires (clientChannelID) when a connection is added
        self.JoinedHost = Event() # (client) fires () when the client has received all the initial state from the host
        self.Reset()

    def Reset(self):
        self.isHost = True # True until we learn otherwise (currently we don't get recreated when a client joins a host, so we detect dynamically)
        self.channelID = 0 # aka playerID
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
        self.fragmentNameToID = BiDict() # fragment name <--> fragment ID

    def Register(self, obj, netID, spawnType:ENRSpawnReplicatedBy):
        '''Called by NetReplicated.NRRegister'''
        # For Actor objects, also adjust replication settings
        if hasattr(obj, 'engineObj') and isinstance(obj.engineObj, AActor):
            e = obj.engineObj
            e.bAlwaysRelevant = True # we don't want the engine to do any relevancy checking
            e.SetReplicateMovement(False) # we never want this
            #e.SetReplicates(spawnType == ENRSpawnReplicatedBy.Engine) # from the engine's perspective, it's a replicated actor only if the engine spawns it
            #^--- as of 4.27, we can't call SetReplicates this early. See NetReplicates.PostInitializeComponents

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

        if spawnType == ENRSpawnReplicatedBy.App and not issubclass(type(obj.__class__), PyGlueMetaclass):
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
                _SendMessage(chan, payload, True)

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
            if obj and obj.nrSpawnType != ENRSpawnReplicatedBy.Engine and obj.IsValid():
                klass = obj.__class__
                if issubclass(klass, AActor) or issubclass(klass, AActor_PGLUE) or issubclass(type(klass), PyGlueMetaclass):
                    obj.Destroy()

    def _OnBridgeMessage_Unregister(self, chan, payload):
        nrNetIDNum = struct.unpack('<I', payload)[0]
        self.Unregister(netIDNum=nrNetIDNum)

    def OnMessage(self, chan, data, reliable):
        '''Called by NRChannel for each incoming message. Routes the message to internal handlers'''
        try:
            handlerName = '_OnBridgeMessage_%s' % ENRBridgeMessage.NameFor(data[0]) # all msgs have a ENRBridgeMessage value as the first byte
            getattr(self, handlerName)(chan, data[1:])
        except:
            logTB()
            if not reliable:
                # this message was sent as unreliable, so it's "okay" that it failed in some way. A common scenario is when the client is
                # just joining and the host is giving a brain dump of the current state of everything (all reliable messages) and our code
                # on the host is throttling things to make sure we don't cause overflow in the FBitWriter, and some sigdef messages are stuck
                # in that queue while, in parallel, some unreliable messages are sent that make use of one of those enqueued sigdefs.
                log('(possibly a non-critical error: message was unreliable)')

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
        #log('XXX netrep.SISF', obj.nrNetID, netIDNum, ' '.join(['%d:%s' % (i,k) for i,k in enumerate(obj.nrPropNames)]))
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

        # Include state of any registered fragment elements
        entries = [] # batch these up so we can prefix them with a count
        for fragmentName, frag in obj.nrFragments.items():
            fragmentID = self.GetFragmentID(fragmentName)
            for elIndex, propValues in enumerate(frag.propValues):
                formatStr, propsBlob = ValuesToBin(propValues)
                header = struct.pack('<HHHH', fragmentID, elIndex, len(formatStr), len(propsBlob))
                entries.append((header, formatStr.encode('utf8'), propsBlob))
        PA(struct.pack('<H', len(entries)))
        for header, format, props in entries:
            PA(header)
            PA(format)
            PA(props)

        # Send it!
        payload = b''.join(parts)
        for chan in channels:
            _SendMessage(chan, payload, True)

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

        log('OnChannelFromClient', chanID)
        chan.channelID = chanID
        SignatureDefinitionManager.CreateFor(chan)
        self.clientChannels[chanID] = chan

        # Sync state with the client by informing it of its channel ID, any fragment mappings, and the state of all active replicated objects
        _SendMessage(chan, struct.pack('<BB', ENRBridgeMessage.SetChannelInfo, chanID), True)
        for fragmentName, fragmentID in self.fragmentNameToID.items():
            self._SendFragmentMapping(fragmentName, fragmentID, chan)

        allObjs = []
        for netID, objRef in self.objs.items():
            obj = objRef()
            if obj:
                netIDNum = self.netIDToNum[obj.nrNetID]
                allObjs.append((netIDNum, obj))
        for netIDNum, obj in sorted(allObjs): # sorted so info gets sent in creation order
            self.SendInitialStateFor(obj, chan)

        # Tell the client it got all the things
        _SendMessage(chan, struct.pack('<B', ENRBridgeMessage.InitialStateComplete), True)

        self.ClientJoined.Fire(chanID)

    def OnChannelToHost(self, chan):
        '''Called by NRChannel when as a client we have connected to the host'''
        log('OnChannelToHost')
        self.isHost = False # oh, hey, it turns out we're not the host after all
        self.fragmentNameToID = BiDict() # forget any prior mappings, the host will send new ones
        SignatureDefinitionManager.CreateFor(chan)
        self.hostChannel = chan

    def OnChannelClosing(self, chan):
        '''Called by NRChannel when it is closing'''
        log('OnChannelClosing', chan.channelID)
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
                    outboundWhere = where # stomp any prior values and set it to playerID + user bit
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
            try:
                recipientID = self.netIDToNum[recipient.nrNetID]
            except KeyError:
                log('Failed to find netIDNum for', recipient.nrNetID, list(self.netIDToNum.keys()), 'isHost?', self.isHost)
                raise
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
                    _SendMessage(chan, payload, reliable)
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
        '''Called on clients to inform it of its channel/player/user ID'''
        assert not self.isHost
        self.channelID = payload[0]
        self.ChannelIDChanged.Fire(self.channelID)

    def _OnBridgeMessage_InitialStateComplete(self, chan, payload):
        '''Called on the client once it has received all of the initial state from the host'''
        self.JoinedHost.Fire()

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

        # Save the state and dependency information so it can later be attached to the actors
        propValues = BinToValues(formatStr, propsBlob)
        self.unconsumedState[netIDNum] = Bag(propValues=propValues, dependencies=deps)

        className = None
        if spawnType == ENRSpawnReplicatedBy.NR:
            classNameLen = payload[0]
            className = str(payload[1:classNameLen+1], 'utf8')
            payload = payload[1+classNameLen:]

        # Read any state info about fragment elements
        elementState = [] # list of (fragmentID, elementIndex, propValues)
        entryCount = struct.unpack('<H', payload[:2])[0]
        payload = payload[2:]
        for i in range(entryCount):
            fragmentID, elementIndex, formatStrLen, propsBlobLen = struct.unpack('<HHHH', payload[:8])
            payload = payload[8:]
            fragFormatStr = str(payload[:formatStrLen], 'utf8')
            payload = payload[formatStrLen:]
            fragPropsBlob = payload[:propsBlobLen]
            payload = payload[propsBlobLen:]
            fragPropValues = BinToValues(fragFormatStr, fragPropsBlob)
            elementState.append((fragmentID, elementIndex, fragPropValues))

        # Set up the same mapping the host has for this object
        self.netIDToNum[netID] = netIDNum
        self.numToNetID[netIDNum] = netID

        obj = None
        if className is not None:
            # It's an NR-spawned object, so spawn it now. It doesn't necessarily know its own netID, so
            # we take care of registration on its behalf.
            klass = NRTrackerMetaclass.All.get(className)
            assert klass, 'Unable to find class to spawn: ' + className

            if issubclass(klass, AActor) or issubclass(klass, AActor_PGLUE) or issubclass(type(klass), PyGlueMetaclass):
                # If it's an engine class of one of the Python quasi-subclasses of an engine class, we need to use
                # an engine API to spawn it (though I guess SpawnActor wouldn't work for U-subclasses)
                obj = PyInst(SpawnActor(GetWorld(), klass))
            else:
                obj = klass()
            obj.nrNetID = netID
            obj.nrSpawnType = spawnType
            self.objs[netID] = weakref.ref(obj)
        else:
            ref = self.objs.get(netID)
            if ref:
                obj = ref()

        # Apply the fragment element state - well, attach it; it'll be delivered as elements are registered
        if elementState:
            if obj:
                for fragmentID, elementIndex, propValues in elementState:
                    for propIndex, propValue in enumerate(propValues):
                        obj.NREnqueueElementUpdate(fragmentID, elementIndex, propIndex, propValue)
            else:
                # I don't *think* this ever happens, so for now, just bark about it but throw away the data
                log('ERROR: have fragment element state but no object to attach it to', className, netID)

        # If we know about the object (either because we just spawned it above, or because it already registered itself), trigger it
        # to consume its initial state. If we don't know about the obj in question, it just means that at some point it'll show up
        # and it will call checkstart on its own.
        if obj and not obj.nrOnReplicatedCalled: # if we spawned it above, then obj.BeginPlay was called and obj._NRCheckStart too
            obj._NRCheckStart()

    def _OnBridgeMessage_Call(self, senderChannel, payload):
        '''The receiving side of a NetReplicated.NRCall -> bridge.DoCall sequence. Runs it locally and/or passes it on to other clients.'''
        where, sigDefID, recipientID, mixedSessionID, reliable = struct.unpack('<BHIBB', payload[:9])
        blob = payload[9:]
        runLocal = False
        sigDef = senderChannel.recvSigDefs.StringForID(sigDefID)
        if not sigDef:
            log(f"WARNING: Unknown sigdef id {sigDefID} received")
            return
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
                _SendMessage(chan, payload, reliable)
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
                    log('ERROR: ignoring local call to unknown recipient', recipientID, sigDef)
                else:
                    # Defer the call til later
                    #log('Deferring call to', recipientID, sigDef)
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
            # We have no object to which we can send this message. See if it's an object that is in flight
            if self.unconsumedState.get(recipientID) is None:
                log('ERROR: non-existent recipient', recipientID, recipientNetID, sigDef)
            else:
                # Ok, just add this message to the list of calls that we'll emit once it finishes spawning here
                #log('XXX Recipient', recipientNetID, recipientID, 'is still in flight, deferring this call')
                self.deferredCalls.setdefault(recipientID, []).append((recipientID, sigDef, mixedSessionID, reliable, bytes(blob)))
        else:
            obj = ref()
            if obj:
                methodName, formatStr = sigDef.split('|')

                # If a mixed reliability session is active, drop any messages that don't conform, as it indicates they
                # are out of order/delayed messages that should be ignored
                if mixedSessionID != 0:
                    expectedID = obj.NRGetMixedReliabilitySessionID(methodName, reliable)
                    if expectedID != mixedSessionID:
                        log('Tossing late mixed mode message for', obj, methodName, '(expected session %d, got %d, reliable:%s)' % (expectedID, mixedSessionID, reliable))
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
            #log('Applying deferred call to', netIDNum, args)
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

    # --------------------------------------------------------------------------------------------------------------------------------------------------------------------
    # Fragments and elements
    # --------------------------------------------------------------------------------------------------------------------------------------------------------------------
    def GetFragmentID(self, fragmentName):
        '''Used by NetReplicated objs to map back and forth between NRFragment names and IDs. If fragmentName is already an ID, it is returned
        as is.'''
        if isinstance(fragmentName, int):
            return fragmentName # not sure this happens anymore
        fragmentID = self.fragmentNameToID.get(fragmentName)
        if fragmentID is not None: # yay, found a mapping
            return fragmentID

        if self.isHost:
            # Since we're the host, we can create a new mapping, and announce it to clients
            return self._IssueFragmentID(fragmentName)
        else:
            # We're not on the host, so keep using the string name as the fragment ID until the host tells us otherwise
            return fragmentName

    def GetFragmentName(self, fragmentID, missingOk=False):
        '''Inverse of GetFragmentID. Don't throw a KeyError if missingOk=True.'''
        if isinstance(fragmentID, str):
            if not self.isHost:
                # If a client receives a fragmentID that is just a string, pass it along because it is the fragment name, and at some
                # point (hopefully soon), a message will arrive from the host with an official mapping.
                return fragmentID

            # We're on the host and have encountered a message from a client using a fragment name as its ID. This would normally be for a
            # new fragment, so we'll generate a new mapping and inform all clients of it if it doesn't exist.
            fragmentName = fragmentID # to reduce my confusion!
            if fragmentName not in self.fragmentNameToID: # due to timing issues, it's possible we have generated a mapping and it's still propagating
                self._IssueFragmentID(fragmentName)
            return fragmentName
        try:
            return self.fragmentNameToID.inv(fragmentID) # let this blow up if not found, so we can troubleshoot and figure out a fix
        except KeyError:
            if missingOk:
                return None
            raise

    def _SendFragmentMapping(self, fragmentName, fragmentID, chan):
        payload = struct.pack('<BHB', ENRBridgeMessage.FragmentMapping, fragmentID, len(fragmentName)) + fragmentName.encode('utf8')
        _SendMessage(chan, payload, True)

    def _IssueFragmentID(self, fragmentName):
        '''Called when a new fragment name is encountered: generates an ID for it and sends that new mapping to all clients. Returns
        the new ID.'''
        assert self.isHost
        fragmentID = len(self.fragmentNameToID)
        self.fragmentNameToID[fragmentName] = fragmentID
        payload = struct.pack('<BHB', ENRBridgeMessage.FragmentMapping, fragmentID, len(fragmentName)) + fragmentName.encode('utf8')
        for chan in self.clientChannels.values():
            _SendMessage(chan, payload, True)
        return fragmentID

    def _OnBridgeMessage_FragmentMapping(self, chan, payload):
        '''Informs clients of a host-assigned ID for a global fragment name, so that the client can transmit the ID instead of the
        name in future messages.'''
        fragmentID, fragmentNameLen = struct.unpack('<HB', payload[:3])
        fragmentName = str(payload[3:], 'utf8')
        self.fragmentNameToID[fragmentName] = fragmentID


_bridge = NRAppBridge()
UNRChannel.SetAppBridge(_bridge)

def GetLocalPlayerID():
    '''Public API used by __init__'''
    return _bridge.channelID

def NRGetAppBridge():
    return _bridge

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
        self.isHost = _bridge.isHost # cached here since it doesn't change once code gets this far
        self.nrInitialStateSet = False # on clients, has initial state been received from the host?
        self.nrBeginPlayCalled = False # on all actors, has BeginPlay been called?
        self.nrOnReplicatedCalled = False # on all instances, has OnReplicated been called?
        self.nrDependencies = [] # list of (netID, attrName or '') of replicated objects this object depends on. Only set on host.
        self.nrWaitingDependencies = [] # netIDs of replicated objects this object is waiting on before OnReplicated will be called
        self.nrMixedSessionIDs = {} # method name -> mixed reliability session ID that is currently active
        self.nrFragments = {} # fragment name -> NRFragment instance
        self.nrQueuedElementUpdates = {} # {(fragmentID, elementIndex) -> [(propIndex, propValue)]} - undelivered updates to fragment elements

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

    def NRIsLocallyControlled(self):
        '''Returns True if this copy of this object (versus the copies of it running on other machines) is the copy that is being
        controlled by a human (autonomous proxy vs simulated proxy)'''
        if isinstance(self, APawn) or (hasattr(self, 'engineObj') and isinstance(self.engineObj, APawn)):
            return self.IsLocallyControlled()
        owner = self.GetOwner()
        if owner and isinstance(owner, APawn):
            return owner.IsLocallyControlled()
        return False # TODO: maybe also check for AController?

    def PostInitializeComponents(self):
        # Note that this was moved from bridge.Register because in 4.27 and later we can't call SetReplicates from __init__
        super().PostInitializeComponents()
        if self.HasAuthority():
            self.SetReplicates(self.nrSpawnType == ENRSpawnReplicatedBy.Engine) # from the engine's perspective, it's a replicated actor only if the engine spawns it

    def NRRegister(self, netID, spawnType:ENRSpawnReplicatedBy):
        '''Registers this instance so that future messages to the given netID will be sent to it. Must be called in all scenarios
        (from __init__) *except* in NR-spawned instances on clients.'''
        if hasattr(self, 'engineObj') and self.engineObj.IsDefaultObject():
            return # don't register if it's just the CDO
        _bridge.Register(self, netID, spawnType)

    def NRUnregister(self):
        '''Communicates to the replication system that this object will be cleaned up soon. For AActor subclasses, this call
        happens automatically via EndPlay.'''
        self.nrFragments = {}
        if self.isHost and self.nrNetID is not None:
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

    def OnReplicated(self):
        '''Called once an object has fully replicated and play has started. For Actor subclasses, this is roughly like BeginPlay,
        except that it isn't called until replication is done and self.nr.* properties all have their initial values.
        Called on all machines.'''
        pass

    def NRCall(self, where:ENRWhere, methodName, *args, reliable=True, maxCallsPerSec=-1):
        '''Performs a replicated call to one or more machines. To send a message to a specific user, use ENRWhere.Only(playerID).'''
        if WITH_EDITOR: assert IsInGameThread()
        _bridge.DoCall(self, where, methodName, args, reliable, maxCallsPerSec)

    def NRUpdate(self, where=ENRWhere.All, reliable=True, maxCallsPerSec=-1, **kwargs):
        '''Replicates an update to one or more replicated properties (to all machines by default). As a special
        case, if called from the constructor on the host, where is implicitly set to Local, as a way to override default values
        before the initial state is replicated to other machines.'''
        if WITH_EDITOR: assert IsInGameThread()
        if where == ENRWhere.NONE or not kwargs: return

        # NRUpdate piggybacks on NRCall by sending a parameter that is a string holding a list of repprop indices, then all the
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
        indices = [int(x) for x in indicesStr.split('_')] # indicesStr is a list of repprop indices in a _-separated string like '2_5_10'
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

        # The engine will start ticking actors as soon as BeginPlay has been called, but we want to prevent ticks until
        # OnReplicated, so override ticking for now.
        if issubclass(type(self.__class__), PyGlueMetaclass):
            self.engineObj.OverrideTickAllowed(False)

        super().BeginPlay()
        self.nrBeginPlayCalled = True
        self._NRCheckStart()

    def EndPlay(self, reason):
        # Note that EndPlay isn't called for non-engine subclasses, so in that case the app needs to call NRUnregister itself
        # (e.g. from __del__ or something)
        super().EndPlay(reason)
        self.NRUnregister()

    def _NRCheckStart(self):
        '''Called from various points of initialization to decide when it's time to call OnReplicated'''
        if not hasattr(self, 'nrNetID'):
            log('ERROR: object', self, 'failed to call NRRegister')
            return
        if self.nrOnReplicatedCalled:
            log('ERROR: _NRCheckStart called even though OnReplicated has already been called', self, self.nrNetID, Caller())
            return
        if not self.isHost:
            if hasattr(self, 'engineObj') and isinstance(self.engineObj, AActor) and not self.nrBeginPlayCalled: return # we're expecting a BeginPlay call that hasn't happened yet
            if not self.nrInitialStateSet:
                # See if the host has delivered our initial state
                self.nrInitialStateSet = gotIt = _bridge.TryConsumeInitialState(self)
                if not gotIt:
                    return # we'll try again later
                # otherwise, fall thru

        if self.nrWaitingDependencies: return # still waiting on one or more dependent objects to fully replicate

        # We're good to go finally!
        self.nrOnReplicatedCalled = True
        if _bridge.netIDToNum.get(self.nrNetID) is None:
            log('ERROR: missing mapping', self.nrNetID, self)

        # Allow ticking to happen (if the actor doesn't have ticks enabled, it still won't tick - it's now just allowed to tick
        # if it wants to)
        if issubclass(type(self.__class__), PyGlueMetaclass):
            self.engineObj.OverrideTickAllowed(True)

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

        self._NRDeliverQueuedElementUpdates() # see if there were any updates we received but couldn't yet deliver

    # --------------------------------------------------------------------------------------------------------------------------------------------------------------------
    # Fragments and elements
    # --------------------------------------------------------------------------------------------------------------------------------------------------------------------
    def _NRRegisterElement(self, element, fragmentName, syncPropNames):
        '''Called by NRElement to inform us of another element that needs its state sync'd'''
        frag = self.nrFragments.get(fragmentName)
        if frag is None:
            frag = NRFragment(self, fragmentName)
            self.nrFragments[fragmentName] = frag
        frag.RegisterElement(element, syncPropNames)
        self._NRDeliverQueuedElementUpdates(element) # see if there were any updates we received but couldn't yet deliver

    def NRUpdateElementProperty(self, fragmentName, elementIndex, propIndex, propValue, reliable, maxCallsPerSec):
        '''Called by NRFragment when a sychronized element property is modified'''
        # This gets called for all changes, so we just ignore any if we're not the locally controlled copy
        if not self.NRIsLocallyControlled():
            return
        fragmentID = _bridge.GetFragmentID(fragmentName) # note that the return value may be a string still, don't assume it's an int!
        self.NRCall(ENRWhere.NotMe, '_OnNRElementPropertyUpdated', fragmentID, elementIndex, propIndex, propValue, reliable=reliable, maxCallsPerSec=maxCallsPerSec)

    def _OnNRElementPropertyUpdated(self, fragmentID, elementIndex, propIndex, propValue):
        '''Called when the locally controlled copy of this object on another machine has updated a property - causes the fragment element's
        property to be updated with the new value.'''
        fragmentName = _bridge.GetFragmentName(fragmentID, missingOk=True)
        frag = self.nrFragments.get(fragmentName) if fragmentName else None
        if frag is None or len(frag.elements) <= elementIndex:
            #log(f'ERROR: received update for fragment {fragmentID}/{fragmentName} but it does not exist')
            self.NREnqueueElementUpdate(fragmentID, elementIndex, propIndex, propValue)
        else:
            frag.OnPropertyUpdated(elementIndex, propIndex, propValue)

    def NREnqueueElementUpdate(self, fragmentID, elementIndex, propIndex, propValue):
        '''Saves an element update to be applied as soon as the corresponding fragment element becomes available to receive it'''
        self.nrQueuedElementUpdates.setdefault((fragmentID, elementIndex), []).append((propIndex, propValue))

    def _NRDeliverQueuedElementUpdates(self, forElement=None):
        '''Delivers any queued element updates that can be delivered at this time - we sometimes receive updates prior to
        a fragment element being registered, and hold onto them until then. If forElement is specified, only queued updates
        for that specific element are delivered.'''
        wantFragmentID = None if forElement is None else _bridge.GetFragmentID(forElement.nrFragment.name)
        wantElementIndex = None if forElement is None else forElement.nrElementIndex
        wantFragment = None if forElement is None else forElement.nrFragment

        doneKeys = [] # any dict keys we encountered that can be completely removed now because their updates are all delivered
        for key, propEntries in self.nrQueuedElementUpdates.items():
            fragmentID, elementIndex = key

            # If we're looking for a specific one, skip everybody else
            if wantFragmentID is not None and fragmentID != wantFragmentID:
                continue
            if wantElementIndex is not None and elementIndex != wantElementIndex:
                continue

            if forElement is None:
                fragmentName = _bridge.GetFragmentName(fragmentID)
                frag = self.nrFragments.get(fragmentName)
                if frag is None or len(frag.elements) <= elementIndex: # hasn't been registered yet
                    continue
            else:
                frag = wantFragment

            doneKeys.append(key) # we're going to deliver all of this element's updates
            for propIndex, propValue in propEntries:
                frag.OnPropertyUpdated(elementIndex, propIndex, propValue)

        for key in doneKeys:
            del self.nrQueuedElementUpdates[key]


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
    if isinstance(arg, bytes): return 'y', F_Short.pack(len(arg)) + arg
    if isinstance(arg, FVector): return 'V', F_FVector.pack(*arg)
    if isinstance(arg, FVector2D): return 'v', F_FVector2D.pack(*arg)
    if isinstance(arg, FRotator): return 'R', F_FRotator.pack(*arg)
    if isinstance(arg, FLinearColor): return 'L', F_FLinearColor.pack(*arg)
    if isinstance(arg, type):
        className = arg.__name__
        return 'C', F_Short.pack(len(className)) + className.encode('utf8')
    if isinstance(arg, FTransform): return 'T', F_FTransform.pack(*arg.GetLocation(), *arg.Rotator(), *arg.GetScale3D())
    if isinstance(arg, FQuat): return 'Q', F_FQuat.pack(*arg)
    if hasattr(arg, 'nrNetID'): return 'O', F_Int.pack(_bridge.NetIDNumFromObject(arg))

    if isinstance(arg, UObject):
        # The object isn't replicated but it is an engine object, so assume it's an asset that has been loaded
        # by LoadByRef (though we should come up with some way to confirm this assumption), so generate and send a reference
        # path
        ref = GetReferencePath(arg)
        return 'A', F_Short.pack(len(ref)) + ref.encode('utf8')

    if isinstance(arg, dict):
        #for dictionaries, make a list of [keys] + [values], and ValueToBin that
        formatStr, propsBlob = ValuesToBin([k for k in arg.keys()] + [v for v in arg.values()])
        return 'D', F_Short.pack(len(formatStr)) + F_Short.pack(len(propsBlob)) + formatStr.encode('utf8') + propsBlob

    if isinstance(arg, list):
        #L, I, S, and T are allready used, so is everything from "ARRAY" except Y, so Y it is!
        formatStr, propsBlob = ValuesToBin(arg)
        return 'Y', F_Short.pack(len(formatStr)) + F_Short.pack(len(propsBlob)) + formatStr.encode('utf8') + propsBlob

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
        elif typeCode == 'y':
            bLen = F_Short.unpack_from(blob, dataIndex)[0]
            dataIndex += F_Short.size
            ret.append(bytes(blob[dataIndex:dataIndex+bLen]))
            dataIndex += bLen
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
        elif typeCode == 'D': # a dict
            fLen = F_Short.unpack_from(blob, dataIndex)[0]
            dataIndex += F_Short.size
            bLen = F_Short.unpack_from(blob, dataIndex)[0]
            dataIndex += F_Short.size
            items = BinToValues(str(blob[dataIndex:dataIndex+fLen], 'utf8'), blob[dataIndex+fLen:dataIndex+fLen+bLen])
            dataIndex += fLen + bLen
            n = len(items) // 2
            ret.append(dict(zip(items[:n], items[n:])))
        elif typeCode == 'Y': # a list
            fLen = F_Short.unpack_from(blob, dataIndex)[0]
            dataIndex += F_Short.size
            bLen = F_Short.unpack_from(blob, dataIndex)[0]
            dataIndex += F_Short.size
            items = BinToValues(str(blob[dataIndex:dataIndex+fLen], 'utf8'), blob[dataIndex+fLen:dataIndex+fLen+bLen])
            dataIndex += fLen + bLen
            ret.append(items)
        else:
            assert 0, typeCode
    return ret

# --------------------------------------------------------------------------------------------------------------------------------------------------------------------
# Fragments and elements
# --------------------------------------------------------------------------------------------------------------------------------------------------------------------
class NRFragment:
    '''Internal helper class for storing data about synchronized properties'''
    def __init__(self, owner, fragmentName):
        self.owner = owner
        self.name = fragmentName
        self.elements = [] # each is a NRElement that has been added. TODO: should these be weakrefs?
        self.propNameMaps = [] # each is a BiDict of (propName -> propIndex)
        self.propValues = [] # each is a list of current values, in propIndex order
        # Note that we store current names/values on the NRFragment and not on the actual element, so that we can receive
        # state date for an element even before it has been fully registered on this machine.

    EMPTY_PROP_VALUE = '__nr_empty_element_property_value__'
    def RegisterElement(self, element, syncPropNames):
        '''Associates the given object (that mixes in NRElement) with this fragment. Sets on that element
        .nrElementIsRegistered, .nrFragment, .nrElementIndex, and .nrSyncPropNames.'''
        element.nrElementIsRegistered = True
        element.nrFragment = self
        element.nrElementIndex = len(self.elements) # for faster lookup later
        element.nrSyncPropNames = syncPropNames[:]
        self.elements.append(element)
        propNameMap = BiDict()
        for i, name in enumerate(syncPropNames):
            propNameMap[name] = i
        self.propNameMaps.append(propNameMap)

        # If we don't have any property values yet, just add an entry to hold them later (but don't stomp already-received values)
        if len(self.propValues) < len(self.elements):
            self.propValues.append([self.EMPTY_PROP_VALUE] * len(syncPropNames))

        # If we do have property values (i.e. they arrived prior to the element being registered), go and
        # and send them as updates now.
        if not self.owner.NRIsLocallyControlled():
            propValues = self.propValues[element.nrElementIndex]
            for i, value in enumerate(propValues):
                if value == self.EMPTY_PROP_VALUE: continue # just a dummy value we inserted to reserve space
                propName = propNameMap.inv(i)
                try:
                    element.OnNRPropertyUpdated(propName, value)
                except:
                    logTB()

    def UpdateProperty(self, elIndex, propName, propValue, reliable, maxCallsPerSec):
        '''Called by NRElement.NRUpdateProperty when a sync'd property is modified'''
        # Update our local copy of this property
        propIndex = self.propNameMaps[elIndex][propName]
        self.propValues[elIndex][propIndex] = propValue
        if propValue != self.EMPTY_PROP_VALUE:
            self.owner.NRUpdateElementProperty(self.name, elIndex, propIndex, propValue, reliable, maxCallsPerSec) # this won't do anything if we're not locally controlled (which is what we want)

    def OnPropertyUpdated(self, elIndex, propIndex, propValue):
        '''Called by the owning NetReplicated object when an element's property has changed'''
        if propValue == self.EMPTY_PROP_VALUE: return

        # Update our local copy of this property
        self.propValues[elIndex][propIndex] = propValue

        # If this element has been registered, go ahead and inform it of the change. If it has not been registered
        # yet, it will receive the change once it does
        if elIndex < len(self.elements):
            element = self.elements[elIndex]
            try:
                propName = self.propNameMaps[elIndex].inv(propIndex)
                element.OnNRPropertyUpdated(propName, propValue)
            except:
                logTB()


class NRElement:
    '''Mixin class for non-actor objects that want to have bits of state replicated from the locally-controlled copy to all
    other copies. Instances must call self.NRRegisterElement to enable synchronization.'''
    def __init__(self, *args, **kwargs):
        self.nrElementIsRegistered = False # this is the only element property subclasses can safely inspect until its value becomes True
        super().__init__(*args, **kwargs)

    def NRUpdateProperty(self, propName, value, reliable=True, maxCallsPerSec=-1):
        '''Should be called anytime one of the replicated properties is changed locally. If this copy of the object is the
        locally controlled copy (the autonomous proxy), its value will be pushed to the non-locally controlled copies (the
        simulated proxies) running on other machines.'''
        self.nrFragment.UpdateProperty(self.nrElementIndex, propName, value, reliable, maxCallsPerSec)

    def OnNRPropertyUpdated(self, propName, value):
        '''Called on simulated proxies when the a replicated property was changed on the autonomous proxy (i.e. the locally
        controlled copy of this object is on some other machine, and one of the replicated properties was changed over there,
        and now the updated value has arrived here, and this is a non-locally controlled copy). Subclasses should override
        this to actually act on the change and apply it as needed. This method is not called on the locally controlled
        copy (the autonomous proxy).'''
        pass

    def NRRegisterElement(self, owner, fragmentName, syncPropNames):
        '''Links this object into the replication system. owner is the NetReplicated object with which this object is
        associated. fragmentName is the name of the cluster of elements grouped together, and syncPropNames is an ordered
        list of properties on this object that will be kept in sync across the copy of this object running on each machine.'''
        owner._NRRegisterElement(self, fragmentName, syncPropNames)

