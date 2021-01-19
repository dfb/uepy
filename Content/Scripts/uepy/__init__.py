from _uepy import *

from importlib import reload
import sys, shlex, json, time

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

MANGLE_CLASS_NAMES = True
class PyGlueMetaclass(type):
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

class AActor_PGLUE(metaclass=PyGlueMetaclass):
    '''Glue class for AActor'''
    def GetWorld(self): return self.engineObj.GetWorld()
    def GetOwner(self): return self.engineObj.GetOwner()
    def GetTransform(self): return self.engineObj.GetTransform()
    def SetActorLocation(self, v): self.engineObj.SetActorLocation(v) # this works because engineObj is a pointer to a real instance, and we will also write wrapper code to expose these APIs anyway
    def GetActorLocation(self): return self.engineObj.GetActorLocation()
    def GetActorRotation(self): return self.engineObj.GetActorRotation()
    def GetActorTransform(self): return self.engineObj.GetActorTransform()
    def SetActorRotation(self, r): self.engineObj.SetActorRotation(r)
    def CreateUStaticMeshComponent(self, name): return self.engineObj.CreateUStaticMeshComponent(name)
    def GetRootComponent(self): return self.engineObj.GetRootComponent()
    def SetRootComponent(self, s): self.engineObj.SetRootComponent(s)
    def GetComponentsByClass(self, klass): return self.engineObj.GetComponentsByClass(klass)
    def BeginPlay(self): self.engineObj.SuperBeginPlay()
    def EndPlay(self, reason): pass
    def Tick(self, dt): self.engineObj.SuperTick(dt) # # TODO: ditto
    def SuperEndPlay(self, reason): self.engineObj.SuperEndPlay(reason)
    def HasAuthority(self): return self.engineObj.HasAuthority()
    def IsActorTickEnabled(self): return self.engineObj.IsActorTickEnabled()
    def SetActorTickEnabled(self, e): self.engineObj.SetActorTickEnabled(e)
    def SetActorTickInterval(self, i): self.engineObj.SetActorTickInterval(i)
    def GetActorTickInterval(self): return self.engineObj.GetActorTickInterval()
    def Destroy(self): return self.engineObj.Destroy()
    def BindOnEndPlay(self, cb): self.engineObj.BindOnEndPlay(cb)
    def UnbindOnEndPlay(self, cb): self.engineObj.UnbindOnEndPlay(cb)
    def Set(self, k, v): self.engineObj.Set(k, v)
    def Get(self, k): return self.engineObj.Get(k)
    def Call(self, funcName, *args): return self.engineObj.Call(funcName, *args)
    def UpdateTickSettings(self, canEverTick, startWithTickEnabled): self.engineObj.UpdateTickSettings(canEverTick, startWithTickEnabled)

    @property
    def configStr(self):
        return self.engineObj.configStr
CPROPS(AActor_PGLUE, 'Tags', 'useNewTool', 'useNewRotationStuff', 'useNewStretching')

class UEPYAssistantActor(AActor_PGLUE):
    '''Spawn one of these into a level to have it watch for source code changes and automatically reload modified code.
    Attempts to load the module specified in self.mainModuleName; override the default value prior to BeginPlay.'''
    def __init__(self):
        self.SetActorTickEnabled(True)
        self.mainModuleName = 'scratchpad'
        self.lastCheck = 0

    def BeginPlay(self):
        self.start = time.time()
        self.watcher = None
        super().BeginPlay()

    def Tick(self, dt):
        now = time.time()
        if self.watcher is None:
            if now-self.start > 1:
                log('Starting source watcher for', self.mainModuleName)
                import sourcewatcher as S
                reload(S)
                S.log = log
                S.logTB = logTB
                self.watcher = S.SourceWatcher(self.mainModuleName)
        elif now-self.lastCheck > 0.25:
            self.watcher.Check()
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
    actor = SpawnActor_(world, klass, location, rotation)
    if actor is not None:
        for name, value in kwargs.items():
            actor.Set(name, value)
    return actor

