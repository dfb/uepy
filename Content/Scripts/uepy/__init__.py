from _uepy import *

from importlib import reload
import sys, shlex, json, time, weakref, inspect

from . import enums

# Capture sys.stdout/stderr
class OutRedir:
    def write(self, buf):
        log(buf)
    def flush(self):
        pass
    def isatty(self):
        return False
sys.stdout = OutRedir()
sys.stderr = OutRedir()
del OutRedir

# some stuff for interactive
__builtins__['reload'] = reload

class Bag(dict):
    def __setattr__(self, k, v): self[k] = v

    def __getattr__(self, k):
        try: return self[k]
        except KeyError: raise AttributeError('No such attribute %r' % k)

    def __delattr__(self, k):
        try: del self[k]
        except KeyError: raise AttributeError('No such attribute %r' % k)

    @staticmethod
    def FromJSON(j):
        return json.loads(j, object_pairs_hook=Bag)

    def ToJSON(self, indent=0):
        if indent > 0:
            return json.dumps(self, indent=indent, sort_keys=True)
        return json.dumps(self)

# The command line args passed to UE4, not including "sys.argv[0]" (the application)
# NOTE: UE4 strips out quotes before it gets to any of our code, so pretty much anything with spaces in
# it will not work - you have to escape%20your%20parameters, unfortunately.
# TODO: stuff these into sys.argv?
commandLineArgs = shlex.split(commandLineRaw)

def GetWorld():
    '''Returns the best guess of what the "current" world to use is'''
    WT = enums.EWorldType
    worlds = {} # worldType -> *first* world of that type
    for w in GetAllWorlds():
        t = w.WorldType
        if worlds.get(t) is None:
            worlds[t] = w
    return worlds.get(WT.Game) or worlds.get(WT.PIE) or worlds.get(WT.Editor)

def GetUserID():
    '''Returns the session-unique user ID for the current user. User IDs are small integer values that are unique
    among the active users (i.e. if a client disconnects and a new client joins, it's possible that the new client will
    be assigned the old client's user ID).'''
    from . import netrep
    return netrep.GetUserID()

class Event:
    '''Utility class for firing events locally among objects. Convention is for objects to declare a public event member variable that
    other objects access directly to add/remove listener callbacks. User Add/Remove to register/unregister a function to be called when
    the event owner calls event.Fire. The signature of the event is up to the owner and supports args and kwargs. Callbacks are weak
    referenced and it is not required to Remove a registered callback, but it must be owned by an object (the callback function object
    must be a method bound to an object).'''
    def __init__(self):
        self.callbacks = [] # list of (owner, cb method)

    def Add(self, method):
        self.callbacks.append(weakref.WeakMethod(method))

    def Remove(self, method):
        for i, ref in enumerate(self.callbacks):
            if ref() == method:
                self.callbacks.pop(i)
                return
        log('ERROR: failed to remove', method)

    def Fire(self, *args, **kwargs):
        '''Called by the owner to emit an event'''
        for methodRef in self.callbacks[:]:
            cb = methodRef()
            if not cb:
                self.callbacks.remove(methodRef)
            else:
                # Special case: if the owner of the callback is a Python wrapper for an engine object, it's possible that the
                # underlying engine object is no longer valid, but the engine hasn't run GC yet, so the Python object still exists,
                # so we need to detect that scenario and not call the callback.
                engineObj = getattr(cb.__self__, 'engineObj', None)
                if engineObj and not engineObj.IsValid():
                    self.callbacks.remove(methodRef)
                    continue

                try:
                    cb(*args, **kwargs)
                except:
                    logTB()

def DestroyAllActorsOfClass(klass):
    for a in UGameplayStatics.GetAllActorsOfClass(GetWorld(), klass):
        a.Destroy()

def FindGlueClass(klass):
    '''Given a class object, returns it if it is a glue class. Otherwise, recursively checks each of its base classes
    until it finds a glue class, and returns it. Otherwise, returns None.'''
    bases = klass.__bases__
    for b in bases:
        # NOTE: if you reload(uepy) in the UE4 editor py console, this check will fail because you'll probably have multiple
        # versions of PyGlueMetaclass floating around - in that case, it's best to just restart UE4 for now. :(
        if type(b) is PyGlueMetaclass and b.__name__.endswith('_PGLUE'):
            return b

    # Keep looking
    for b in bases:
        g = FindGlueClass(b)
        if g is not None:
            return g

    return None

_allGlueClasses = {} # class name -> each glue class that has been defined
_allNonGlueClasses = {} # class name -> class object of all registered non-glue Python subclasses that extend engine classes
def GetPythonEngineSubclasses():
    return _allNonGlueClasses
def GetAllGlueClasses():
    return list(_allGlueClasses.values())

def MergedClassDefaults(klass):
    '''Recursively merges all klass.classDefaults into a single dict and returns them'''
    ret = {}
    for base in klass.__bases__[::-1]:
        ret.update(MergedClassDefaults(base))
    ret.update(getattr(klass, 'classDefaults', {}))
    return ret

class NRTrackerMetaclass(type):
    '''Tracks all subclasses of NetReplicated.'''
    # Note that not all NetReplicated objects are engine objects
    All = {} # class name --> class instance
    def __new__(metaclass, name, bases, dct):
        klass = super().__new__(metaclass, name, bases, dct)
        if bases:
            NRTrackerMetaclass.All[klass.__name__] = klass # don't use name that was passed in, as PyGlueMetaclass modifies it
        return klass

MANGLE_CLASS_NAMES = True
class PyGlueMetaclass(NRTrackerMetaclass):
    def __new__(metaclass, name, bases, dct):
        isGlueClass = not bases # (a glue class has no bases)

        # Each UClass in UE4 has to have a unique name, but with separate directories, we could potentially have
        # a naming collision. So internally we name each class <objectLib>__<class>, which should be
        # sufficiently unique.
        if MANGLE_CLASS_NAMES and not isGlueClass and not dct.get('_uepy_no_mangle_name'):
            moduleName = dct.get('__module__')
            if moduleName:
                name = moduleName.replace('.', '__') + '__' + name

        newPyClass = super().__new__(metaclass, name, bases, dct)
        if isGlueClass:
            _allGlueClasses[name] = newPyClass
        else:
            _allNonGlueClasses[name] = newPyClass

            # A glue class has no base classes, so this class is /not/ a glue class, so we need to find its glue class
            # so that we can automatically cast its engineObj when creating instances.
            pyGlueClass = FindGlueClass(newPyClass)
            assert pyGlueClass is not None, 'Failed to find py glue class for ' + repr((name, bases))

            # We've found the Python side of the glue class, now get the C++ side as that's what we need to use to register
            # with the engine
            cppGlueClassName = pyGlueClass.__name__[:-6] + '_CGLUE'
            if MANGLE_CLASS_NAMES:
                cppGlueClassName = cppGlueClassName.split('__')[-1]
            cppGlueClass = getattr(glueclasses, cppGlueClassName, None)
            assert cppGlueClass, 'Failed to find C++ glue class for ' + repr((name, bases))
            newPyClass.cppGlueClass = cppGlueClass

            # Register this class with UE4 so that BPs, the editor, the level, etc. can all refer to it by name
            interfaces = getattr(newPyClass, '__interfaces__', [])
            ec = newPyClass.engineClass = RegisterPythonSubclass(name, cppGlueClass.StaticClass(), newPyClass, interfaces)

            # Apply any CDO properties (in the py class's 'classDefaults' dict)
            if not hasattr(cppGlueClass, 'Cast'):
                log('WARNING: No Cast method found for', ec.GetName())
            else:
                cdo = cppGlueClass.Cast(ec.GetDefaultObject())
                for k, v in MergedClassDefaults(newPyClass).items():
                    try:
                        setattr(cdo, k, v)
                    except:
                        logTB()
                        log('Python class %s declares class default "%s" but setting the property failed' % (name, k))

        return newPyClass

    def __call__(cls, engineObj, *args, **kwargs):
        # Instead of requiring every subclass to take an engineObj parameter and then pass it to some super.__init__ function,
        # we intercept it, strip it out, and set it for them.
        inst = object.__new__(cls)

        # Before calling the Python constructor, we need to make sure that the pyInst member is set on the C++ side, because
        # it's possible that the __init__ function calls some C++ function that in turn tries to call one of the Python
        # instance's methods. But in order for that to work, pyInst has to be set at that point.
        InternalSetPyInst(engineObj, inst)

        # engineObj is right now just a plain UObject pointer from pybind's perspective, but we want it to be a pointer to
        # the glue class.
        inst.engineObj = cls.cppGlueClass.Cast(engineObj)
        try:
            inst.__init__(*args, **kwargs)
        except:
            logTB()
        return inst

def CPROPS(cls, *propNames):
    '''Creates Python read/write properties for C++ properties'''
    for _name in propNames:
        def setup(name):
            def _get(self): return getattr(self.engineObj, name)
            def _set(self, value): setattr(self.engineObj, name, value)
            setattr(cls, name, property(_get, _set))
        setup(_name) # create a closure so we don't lose the name

def BPPROPS(cls, *propNames):
    '''Creates Python read/write properties for BP (reflection system) properties'''
    for _name in propNames:
        def setup(name):
            def _get(self): return self.engineObj.Get(name)
            def _set(self, value): self.engineObj.Set(name, value)
            setattr(cls, name, property(_get, _set))
        setup(_name) # create a closure so we don't lose the name

def IsHost():
    return GetWorld().IsServer()

class AActor_PGLUE(metaclass=PyGlueMetaclass):
    '''Glue class for AActor'''
    repProps = Bag(
        loc = FVector(0,0,0),
        rot = FRotator(0,0,0),
    )

    def __init__(self):
        self.EndingPlay = Event() # fires (self) on EndPlay.
        super().__init__()

    def OnRep_loc(self): self.SetActorLocation(self.nr.loc)
    def OnRep_rot(self): self.SetActorRotation(self.nr.rot)
    def PostInitializeComponents(self): self.engineObj.SuperPostInitializeComponents()
    def GetName(self): return self.engineObj.GetName()
    def GetIsReplicated(self): return self.engineObj.GetIsReplicated()
    def SetReplicates(self, b): self.engineObj.SetReplicates(b)
    def HasLocalNetOwner(self): return self.engineObj.HasLocalNetOwner()
    def GetWorld(self): return self.engineObj.GetWorld()
    def GetOwner(self): return self.engineObj.GetOwner()
    def SetOwner(self, o): self.engineObj.SetOwner(o)
    def GetTransform(self): return self.engineObj.GetTransform()
    def SetActorLocation(self, v): self.engineObj.SetActorLocation(v) # this works because engineObj is a pointer to a real instance, and we will also write wrapper code to expose these APIs anyway
    def GetActorLocation(self): return self.engineObj.GetActorLocation()
    def GetActorRotation(self): return self.engineObj.GetActorRotation()
    def GetActorTransform(self): return self.engineObj.GetActorTransform()
    def SetActorRotation(self, r): self.engineObj.SetActorRotation(r)
    def GetActorScale3D(self): return self.engineObj.GetActorScale3D()
    def SetActorScale3D(self, s): self.engineObj.SetActorScale3D(s)
    def CreateUStaticMeshComponent(self, name): return self.engineObj.CreateUStaticMeshComponent(name)
    def GetRootComponent(self): return self.engineObj.GetRootComponent()
    def SetRootComponent(self, s): self.engineObj.SetRootComponent(s)
    def GetComponentsByClass(self, klass): return self.engineObj.GetComponentsByClass(klass)
    def IsValid(self): return self.engineObj.IsValid()
    def BeginPlay(self): self.engineObj.SuperBeginPlay()
    def EndPlay(self, reason):
        self.EndingPlay.Fire(self)
        self.engineObj.SuperEndPlay(reason)
    def Tick(self, dt): self.engineObj.SuperTick(dt)
    def HasAuthority(self): return self.engineObj.HasAuthority()
    def IsActorTickEnabled(self): return self.engineObj.IsActorTickEnabled()
    def SetActorTickEnabled(self, e): self.engineObj.SetActorTickEnabled(e)
    def SetActorTickInterval(self, i): self.engineObj.SetActorTickInterval(i)
    def GetActorTickInterval(self): return self.engineObj.GetActorTickInterval()
    def SetActorHiddenInGame(self, h): self.engineObj.SetActorHiddenInGame(h)
    def SetReplicateMovement(self, b): self.engineObj.SetReplicateMovement(b)
    def Destroy(self): return self.engineObj.Destroy()
    def BindOnEndPlay(self, cb): self.engineObj.BindOnEndPlay(cb) # NOTE: you may be better off using the EndingPlay Event instance.
    def UnbindOnEndPlay(self, cb): self.engineObj.UnbindOnEndPlay(cb)
    def Set(self, k, v): self.engineObj.Set(k, v)
    def Get(self, k): return self.engineObj.Get(k)
    def Call(self, funcName, *args): return self.engineObj.Call(funcName, *args)
    def UpdateTickSettings(self, canEverTick, startWithTickEnabled): self.engineObj.UpdateTickSettings(canEverTick, startWithTickEnabled)
    def OnReplicated(self): pass

    def GetFilteredComponents(self, ofClass=UPrimitiveComponent, onlyVisible=True, ignore=None, ignoreTag=None, includeAttachedActors=False):
        '''Returns a list of all of this actor's visible mesh component (including descendants) that are instances of the
        given component class (or one of its descendents). If ignore is provided, it should be a list of components to omit. If onlyVisible
        is True, excludes any hidden components. If includeAttachedActors is True, also return components of attached children actors.
        If ignoreTag is provided, any components that has that tag will be skipped.'''
        ret = []
        root = self.GetRootComponent()
        if not root:
            return ret

        thisActorAddr = AddressOf(self.engineObj)
        ignore = ignore or []
        ignore = [ofClass.Cast(x) for x in ignore] # this is so the check against the casted value works
        ignore = [x for x in ignore if x]
        ignoreTag = (ignoreTag or '').lower()
        for comp in root.GetChildrenComponents(True):
            if thisActorAddr != AddressOf(comp.GetOwner()) and not includeAttachedActors:
                continue
            if onlyVisible and not comp.IsVisible():
                continue
            if ignoreTag and ignoreTag in comp.ComponentTags:
                continue
            comp = ofClass.Cast(comp)
            if not comp:
                continue
            if comp in ignore:
                continue
            ret.append(comp)
        return ret

    @property
    def configStr(self):
        return self.engineObj.configStr
CPROPS(AActor_PGLUE, 'bAlwaysRelevant', 'bReplicates', 'Tags')

class APawn_PGLUE(AActor_PGLUE):
    def IsLocallyControlled(self): return self.engineObj.IsLocallyControlled()
    def SetupPlayerInputComponent(self, comp): self.engineObj.SuperSetupPlayerInputComponent(comp)
    def GetPlayerState(self): return self.engineObj.GetPlayerState()

class USceneComponent_PGLUE(metaclass=PyGlueMetaclass):
    @classmethod
    def Cast(cls, obj): return cls.engineClass.Cast(obj)
    def IsValid(self): return self.engineObj.IsValid()

    def BeginPlay(self): self.engineObj.SuperBeginPlay()
    def EndPlay(self, reason): self.engineObj.SuperEndPlay(reason)
    def OnRegister(self): self.engineObj.SuperOnRegister()
    def ComponentHasTag(self, tag): return self.engineObj.ComponentHasTag(tag)
    def GetOwner(self): return self.engineObj.GetOwner()
    def GetName(self): return self.engineObj.GetName()
    def SetIsReplicated(self, r): self.engineObj.SetIsReplicated(r)
    def SetActive(self, a): self.engineObj.SetActive(a)
    def IsRegistered(self): return self.engineObj.IsRegistered()
    def RegisterComponent(self): self.engineObj.RegisterComponent()
    def UnregisterComponent(self): self.engineObj.UnregisterComponent()
    def DestroyComponent(self): self.engineObj.DestroyComponent()
    def GetRelativeLocation(self): return self.engineObj.GetRelativeLocation()
    def SetRelativeLocation(self, x): self.engineObj.SetRelativeLocation(x)
    def GetRelativeRotation(self): return self.engineObj.GetRelativeRotation()
    def SetRelativeRotation(self, x): self.engineObj.SetRelativeRotation(x)
    def GetRelativeScale3D(self): return self.engineObj.GetRelativeScale3D()
    def SetRelativeScale3D(self, x): self.engineObj.SetRelativeScale3D(x)
    def GetRelativeTransform(self): return self.engineObj.GetRelativeTransform()
    def SetRelativeTransform(self, x): self.engineObj.SetRelativeTransform(x)
    def SetRelativeLocationAndRotation(self, loc, rot): self.engineObj.SetRelativeLocationAndRotation(loc, rot)
    def ResetRelativeTransform(self): return self.engineObj.ResetRelativeTransform()
    def AttachToComponent(self, parent, socket=''): return self.engineObj.AttachToComponent(parent, socket)
    def SetupAttachment(self, parent, socket=''): return self.engineObj.SetupAttachment(parent, socket)
    def DetachFromComponent(self): return self.engineObj.DetachFromComponent()
    def SetVisibility(self, vis, propagate=True): self.engineObj.SetVisibility(vis, propagate)
    def IsVisible(self): return self.engineObj.IsVisible()
    def GetHiddenInGame(self): return self.engineObj.GetHiddenInGame()
    def SetHiddenInGame(self, h, propagate=True): self.engineObj.SetHiddenInGame(h, propagate)
    def GetForwardVector(self): return self.engineObj.GetForwardVector()
    def GetRightVector(self): return self.engineObj.GetRightVector()
    def GetUpVector(self): return self.engineObj.GetUpVector()
    def GetComponentLocation(self): return self.engineObj.GetComponentLocation()
    def GetComponentRotation(self): return self.engineObj.GetComponentRotation()
    def GetComponentQuat(self): return self.engineObj.GetComponentQuat()
    def GetComponentScale(self): return self.engineObj.GetComponentScale()
    def GetComponentToWorld(self): return self.engineObj.GetComponentToWorld()
    def GetAttachParent(self): return self.engineObj.GetAttachedParent()
    def GetChildrenComponents(self, includeAllDescendents): return self.engineObj.GetChildrenComponents(includeAllDescendents)
    def SetWorldLocation(self, x): self.engineObj.SetWorldLocation(x)
    def SetWorldRotation(self, x): self.engineObj.SetWorldRotation(x)
    def GetSocketTransform(self, name): return self.engineObj.GetSocketTransform(name)
    def GetSocketLocation(self, name): return self.engineObj.GetSocketLocation(name)
    def GetSocketRotation(self, name): return self.engineObj.GetSocketRotation(name)
    def CalcBounds(self, locToWorld): return self.engineObj.CalcBounds(locToWorld)
    def SetMobility(self, m): self.engineObj.SetMobility(m)
CPROPS(USceneComponent_PGLUE, 'ComponentTags')

class UBoxComponent_PGLUE(USceneComponent_PGLUE):
    def SetCollisionEnabled(self, e): self.engineObj.SetCollisionEnabled(e) # this is actually from UPrimitiveComponent
    def BeginPlay(self): self.engineObj.SuperBeginPlay()
    def EndPlay(self, reason): self.engineObj.SuperEndPlay(reason)
    def OnRegister(self): self.engineObj.SuperOnRegister()
    def SetBoxExtent(self, e): self.engineObj.SetBoxExtent(e)
    def GetUnscaledBoxExtent(self): return self.engineObj.GetUnscaledBoxExtent()

class UEPYAssistantActor(AActor_PGLUE):
    '''Spawn one of these into a level to have it watch for source code changes and automatically reload modified code.'''
    def __init__(self):
        self.SetReplicates(False)
        super().__init__()
        self.SetActorTickEnabled(True)
        self.lastCheck = 0
        self.watcher = None
        self.forceDevModuleReload = True

    def BeginPlay(self):
        super().BeginPlay()
        self.start = time.time()

    def Tick(self, dt):
        now = time.time()
        if self.watcher is None:
            import sourcewatcher as S
            if now-self.start > 1:
                # Try to grab a global sourcewatcher that already exists
                self.watcher = S.GetGlobalInstance()
                if self.watcher is None:
                    log('Starting new source watcher')
                    S.log = log
                    S.logTB = logTB
                    self.watcher = S.SourceWatcher()
                else:
                    log('Reusing global source watcher')
        elif now-self.lastCheck > 0.25:
            self.watcher.Check(forceDevModuleReload=self.forceDevModuleReload)
            self.forceDevModuleReload = False # we just want to force scratchpad to reload on start
            self.lastCheck = time.time()

def AddHelper():
    SpawnActor(GetWorld(), UEPYAssistantActor)

class UUserWidget_PGLUE(metaclass=PyGlueMetaclass):
    '''Base class of all Python subclasses from AActor-derived C++ classes'''
    # TODO: why doesn't this live in umg.py?
    # We do not implement a default Tick but instead have the C++ only call into Python if a Tick function is defined
    #def Tick(self, geometry, dt): pass

def SpawnActor(world, klass, location=None, rotation=None, **kwargs):
    '''Extends __uepy.SpawnActor_ so that you can also pass in values for any UPROPERTY fields'''
    if location is None: location = FVector(0,0,0)
    if rotation is None: rotation = FRotator(0,0,0)
    return SpawnActor_(world, klass, location, rotation, kwargs)

class RefCache:
    '''Asset loader. Repeated calls to load the same asset will be fast because previous loads are cached. All assets are
    loaded by their UE4 editor reference string (right-click on asset and choose to copy the reference).'''
    def __init__(self):
        self.cache = {} # editor reference string --> loaded instance of an asset

        # prefix portion of the editor reference string; each type a new type of asset is loaded, an entry is added to the map.
        # In most cases, the prefix is the class name without the U prefix (e.g. a 'PaperSprite' --> UPaperSprite), but any
        # special cases should be added to the initial values below.
        self.classMap = dict(
            Blueprint=UBlueprintGeneratedClass,
            MaterialInstanceConstant=UMaterialInstance,
            WidgetBlueprint=UBlueprintGeneratedClass,
        )

    def Load(self, ref):
        '''Given an editor reference string, returns the cached asset, loading it if needed. Returns None if it can't be loaded.'''
        # TODO: UE4 often seems to blur the lines between assets that are classes for generating stuff and assets that are stuff,
        # and that blurriness carries over to this function. We might want to someday have e.g. LoadClassByRef and LoadObjectByRef
        # functions to make the caller's intentions more explicit. Or maybe it never really matters.
        obj = self.cache.get(ref)
        if obj is not None: # cache hit!
            return obj

        objType, path, ignore = ref.split("'") # it's something like Blueprint'/Game/Path/To/Some/Object'

        # Find the UE4 class for this reference, trying to auto-discover it this is the first encounter
        cls = self.classMap.get(objType)
        if cls is None:
            cls = globals().get('U' + objType)
            if cls is not None:
                self.classMap[objType] = cls # yay, remember for next time
        if cls is None:
            raise Exception('RefCache cannot handle references of type ' + repr(objType))

        if cls == UBlueprintGeneratedClass and not path.endswith('_C'):
            # The UE4 reference will be like "Blueprint'/Game/Whatever'" but we want the class that the BP generates
            path += '_C'
        obj = StaticLoadObject(cls, path)
        if obj is not None:
            if cls != UBlueprintGeneratedClass: # if we cast these, it breaks stuff because the result isn't what we want
                obj = cls.Cast(obj)
            self.cache[ref] = obj
        return obj

    def Clear(self):
        '''Removes from memory all cached references'''
        self.cache.clear()

    def GetReferencePath(self, obj):
        '''Given an asset, such as a UMaterialInstance, returns a reference path for it, in the same format as right-clicking
        an asset in the editor and choosing "Copy Reference"'''
        className = obj.GetClass().GetName()
        classPath = obj.GetPathName()
        if className.endswith('GeneratedClass') and classPath.endswith('_C'):
            className = className[:-len('GeneratedClass')]
            classPath = classPath[:-2]
        return "%s'%s'" % (className, classPath)

# global ref cache
_refCache = RefCache()
LoadByRef = _refCache.Load
ClearRefCache = _refCache.Clear
GetReferencePath = _refCache.GetReferencePath

def Caller(level=2):
    '''Debugging helper - returns info on the call stack (default=who called the caller of the caller)'''
    frame = inspect.stack()[level]
    return '[%s:%d]' % (frame.function, frame.lineno)

