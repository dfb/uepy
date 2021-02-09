# uepy

UE4 (>= 4.23) plugin that lets you implement game logic in Python 3.

# pseudo-subclassing
[pybind11](https://github.com/pybind/pybind11) provides some great support for taking a C++ class and subclassing it in Python. Unfortunately,
getitng it to work with UE4 was problematic (case in point: you can't create new instances directly, but instead use NewObject or an
actor-spawning API). So instead of subclassing directly, we achieve the same results by creating a shim on both sides - in C++ and Python -
and then have them work together. For each C++ class we want to be subclassable in Python, we create:

1. A C++ glue class (naming convention: C++ class name + `_CGLUE"`, e.g. `AActor_CGLUE`)
    1. It implements the IUEPYGlueMixin interface (a thin mixin that has a Python instance attached to it).
    1. For each overrideable method, it provides an implementation that calls the same method on the Python instance
    1. For each overrideable method, it provides a SuperXXX version so that Python instances can call e.g. Super::BeginPlay as needed
    1. The code to expose the C++ glue class /always/ exposes it via the builtin `_uepy._glueClasses` module. (even those coming from game modules)
1. A Python glue class (naming convention: C++ class name + `_PGLUE`, e.g. `AActor_PGLUE`)
    1. For each overrideable method, it provides a default implementation that calls self.engineObj.<that method> to get the call to C++
    1. (optional) For each overrideable method, it provides a SuperXXX version that calls self.engineObj.SuperXXX. TODO: ideally we'd instead magically hook into the normal super().XXX functionality
    1. It uses as its metaclass uepy.PyGlueMetaclass.
    1. The metaclass automatically provides each instance with an 'engineObj' member that is the pointer to the C++ UObject instance
    1. The metaclass also takes care of registering the class with the engine (in this way, you can spawn Python-based actors from BP, you can have your game instance be implemented in Python, etc. - the engine considers it a valid, "normal" class)

Put another way, we have glue classes on both sides of the language divide; the class on either side is considered more or less abstract
in the sense that it's not something you'd ever instantiate directly, but exists for the purposes of getting between the two languages.

By extension, the glue classes are 1:1 - you would never e.g. have 3 Python subclasses of a single C++ glue class. Instead you would have
the C++ glue class, the corresponding Python glue class, and then 3 subclasses of that Python glue class. Examples:

- AActor -> AActor_CGLUE -> AActor_PGLUE -> MyActor
- AActor -> ASomeGameActor -> ASomeGameActor_CGLUE -> ASomeGameActor_PGLUE - > MyGameActorThing
- AGameState -> AMyCustomGameState -> AMyCustomGameState_CGLUE -> AMyCustomGameState_PGLUE -> MyCustomGameState

With singletons it's weird and seems "wasteful" but operates the same. The above is somewhat tedious on the surface, but:

- so far it's not too bad as it's write-once-use-many code
- over time we should be able to have better macros and even some automation that generates most or even all of it (it's all pretty boilerplate stuff)
- there are usually a very small number of glue classes (e.g. once you create glue AActor, you could have hundreds of different actor classes in Python all using it)

Net result: even though we don't have true Python subclassing, it gets pretty close, and from the user's perspective, it's /almost/ indistinguishable from the real thing.

Note: the `_PGLUE` convention exists because usually the normal class will also be exposed to Python, e.g. AActor_PGLUE is the class to subclass from,
but uepy.AActor is the C++ AActor class exposed to Python via pybind11, so that you can use pointers to AActor from Python.

Note: if a method you want to override in Python is declared as a BlueprintNativeEvent, then you need to override `MethodName_Implementation`, not `MethondName`.

# Installation

Eventually I hope to make this less manual, but for now:

- In your UE4 project, create a Plugins directory
- Clone uepy into it (afterwards, yourprj/Plugins/uepy/uepy.uplugin should exist, among other things)
- Inside Plugins/uepy, create a 'python' directory to hold whatever 64-bit version of Python you intend to use (I've tried 3.6.7 and 3.8.5). The final layout needs to be like:
    - Plugins/uepy/python/include # containing stuff like `eval.h` and `import.h`
    - Plugins/uepy/python/libs # containing stuff like `python3.lib`
    - Plugins/uepy/python # containing stuff like `_ctypes.pyd` and `select.pyd`

    There are probably much better ways to set this up, but this is what I did:
    - download the Windows x64 embeddable zip file and unzip it into Plugins/uepy/python
    - use e.g. the web installer from python.org to install the same version of Python. During the installation process, set a custom install location to some temp directory, uncheck all the options to register programs, modify your path, etc., but do check the option to download debugging symbols
    - after installation, move the libs and include folders to Plugins/uepy/python and then erase that temporary installation
- Inside Plugins/uepy, clone pybind11 so that you end up with e.g. `Plugins/uepy/pybind11/include/pybind11/eval.h`
- In uepy/Source/editor/uepyEditor.Build.cs and uepy/Source/runtime/uepy.Build.cs, modify the pythonXX.lib filename (in the PublicAdditionalLibraries line) as needed (i.e. depending on the version of Python you're using).

# other stuff to document

- we had to make some minor changes to pybind11 (check --> pybind11_check)
- python and uobject instance life cycles
- obj.is_a
- conventions: use engine naming, e.g. resist the temptation to expose FVector as Vector, or AActor as Actor - just causes confusion later on
    - member vars included, but if you /must/ override them, also include the original. Ex: we expose FRotator.yaw but also FRotator.Yaw
- dev scratchpad / sourcewatcher
- main.py module
- editor_spawner example
- so far I've been using VS2017 Community Edition. I hope to move to VS2019 soon (when I move to UE4.25 probably).
- add bUseRTTI = true; to build.cs
- every code that exposes a C++ class should probably expose a Cast method
- every glue class must implement StaticClass!
- it's ok for PGLUE classes to subclass other PGLUE classes
- if you mix reliable and non-reliable net msgs, include a counter/timestamp because there is no ordering preserved for unreliable messages, so even if you send a bunch of unreliable and end with a reliable, you could end up receiving the reliable followed by an unreliable, so app code needs to sort out which ones to toss vs keep
- using NetRep automatically resolves some of the batching/ordering issues with UE4 replication since reliable msgs are processed in order on a single channel (versus lots of actor channels)
- if you're using NetRep, keep in mind that replicating your actors' transforms is on you now

# Why?

- Iterative development is much faster in Python than C++, and with UE4, the gap is 10x bigger than normal (if not more)
- UE4 provides Blueprints, but they don't scale well (spaghetti BPs are a nightmare) and they are in binary (can't diff, can't merge, etc.)
- [UnrealEnginePython](https://github.com/20tab/UnrealEnginePython) was pretty good, but it's been abandoned.
- We get some nice goodies like support for third-party actors, being able to develop against a packaged build

# Replication TODOs

- Add support for replicated variables of custom structs & other types; add something like NRRegisterMarsalledType(typecode, marshaller, unmarshaller)
- Automatic message splitting for large messages
- Message compression - do some tests to find some min threshold and if msg is above, say, 64 bytes, try compressing it. If compressed version is smaller, send that version and set the isCompressed bit. Note that UE4 has LZ4 libs already built in.
- app-defined signatures for internal use (e.g. UI replication) - expose a get-signature-for-str API
- when throttling, we need to keep the most recent tossed msg and then send it if no newer msg comes in after we drop below the rate limit


