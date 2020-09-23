from _uepy import *

from importlib import reload
import sys, shlex, json

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
        if type(b) is PyGlueMetaclass and b.__name__.endswith('_PGLUE'):
            return b

    # Keep looking
    for b in bases:
        g = FindGlueClass(b)
        if g:
            return g
    return None

_allGlueClasses = {} # class name -> each glue class that has been defined
_allNonGlueClasses = {} # class name -> class object of all registered non-glue Python subclasses that extend engine classes
def GetPythonEngineSubclasses():
    return list(_allNonGlueClasses.values())
def GetAllGlueClasses():
    return list(_allGlueClasses.values())

class PyGlueMetaclass(type):
    def __new__(metaclass, name, bases, dct):
        isGlueClass = not bases # (a glue class has no bases)

        # Each UClass in UE4 has to have a unique name, but with separate directories, we could potentially have
        # a naming collision. So internally we name each class <objectLib>__<class>, which should be
        # sufficiently unique.
        #moduleName = dct.get('__module__')
        #if moduleName and not isGlueClass:
        #    name = moduleName.replace('.', '__') + '__' + name
        # I disabled the above because I hate what it does to class names. For now at least, people just gotta take care and
        # name stuff uniquely on their own.

        newPyClass = super().__new__(metaclass, name, bases, dct)
        if isGlueClass:
            _allGlueClasses[name] = newPyClass
        else:
            _allNonGlueClasses[name] = newPyClass

            # A glue class has no base classes, so this class is /not/ a glue class, so we need to find its glue class
            # so that we can automatically cast its engineObj when creating instances.
            pyGlueClass = FindGlueClass(newPyClass)
            assert pyGlueClass, 'Failed to find py glue class for ' + repr((name, bases))

            # We've found the Python side of the glue class, now get the C++ side as that's what we need to use to register
            # with the engine
            cppGlueClassName = pyGlueClass.__name__[:-6] + '_CGLUE'
            cppGlueClass = getattr(glueclasses, cppGlueClassName, None)
            assert cppGlueClass, 'Failed to find C++ glue class for ' + repr((name, bases))
            newPyClass.cppGlueClass = cppGlueClass

            # Register this class with UE4 so that BPs, the editor, the level, etc. can all refer to it by name
            newPyClass.engineClass = RegisterPythonSubclass(name, cppGlueClass.StaticClass(), newPyClass)

        return newPyClass

    def __call__(cls, engineObj, *args, **kwargs):
        # Instead of requiring every subclass to take an engineObj parameter and then pass it to some super.__init__ function,
        # we intercept it, strip it out, and set it for them.
        inst = object.__new__(cls)

        # engineObj is right now just a plain UObject pointer from pybind's perspective, but we want it to be a pointer to
        # the glue class.
        inst.engineObj = cls.cppGlueClass.Cast(engineObj)
        inst.__init__(*args, **kwargs)
        return inst

class AActor_PGLUE(metaclass=PyGlueMetaclass):
    '''Glue class for AActor'''
    def GetWorld(self): return self.engineObj.GetWorld()
    def SetActorLocation(self, v): self.engineObj.SetActorLocation(v) # this works because engineObj is a pointer to a real instance, and we will also write wrapper code to expose these APIs anyway
    def GetActorLocation(self): return self.engineObj.GetActorLocation()
    def GetActorRotation(self): return self.engineObj.GetActorRotation()
    def SetActorRotation(self, r): self.engineObj.SetActorRotation(r)
    def CreateUStaticMeshComponent(self, name): return self.engineObj.CreateUStaticMeshComponent(name)
    def GetRootComponent(self): return self.engineObj.GetRootComponent()
    def SetRootComponent(self, s): self.engineObj.SetRootComponent(s)
    def BeginPlay(self): pass
    def Tick(self, dt): pass
    def SuperBeginPlay(self): self.engineObj.SuperBeginPlay() # TODO: it'd be nice to make this call happen via super().BeginPlay() at some point
    def SuperTick(self, dt): self.engineObj.SuperTick(dt) # # TODO: ditto
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

class UUserWidget_PGLUE(metaclass=PyGlueMetaclass):
    '''Base class of all Python subclasses from AActor-derived C++ classes'''

class UEPYAssistantActor(AActor_PGLUE):
    '''Spawn one of these into a level to have it watch for source code changes and automatically reload modified code.
    Attempts to load the module specified in self.mainModuleName; override the default value prior to BeginPlay.'''
    def __init__(self):
        self.SetActorTickEnabled(True)
        self.mainModuleName = 'scratchpad'

    def BeginPlay(self):
        import sourcewatcher as S
        reload(S)
        S.log = log
        S.logTB = logTB
        self.watcher = S.SourceWatcher(self.mainModuleName)
        self.SuperBeginPlay()

    def Tick(self, dt):
        self.watcher.Check()

def CPROPS(cls, *propNames):
    '''Creates Python read/write properties for C++ properties'''
    for _name in propNames:
        def setup(name):
            def _get(self): return getattr(self.engineObj, name)
            def _set(self, value): setattr(self.engineObj, name, value)
            setattr(cls, name, property(_get, _set))
        setup(_name) # create a closure so we don't lose the name

def SpawnActor(world, klass, location=None, rotation=None, **kwargs):
    '''Extends __uepy.SpawnActor_ so that you can also pass in values for any UPROPERTY fields'''
    if location is None: location = FVector(0,0,0)
    if rotation is None: rotation = FRotator(0,0,0)
    actor = SpawnActor_(world, klass, location, rotation)
    if actor is not None:
        for name, value in kwargs.items():
            actor.Set(name, value)
    return actor

