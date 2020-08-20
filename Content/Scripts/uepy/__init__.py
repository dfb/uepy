from _uepy import *

from importlib import reload
import sys, shlex

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

# The command line args passed to UE4, not including "sys.argv[0]" (the application)
# NOTE: UE4 strips out quotes before it gets to any of our code, so pretty much anything with spaces in
# it will not work - you have to escape%20your%20parameters, unfortunately.
# TODO: stuff these into sys.argv?
commandLineArgs = shlex.split(commandLineRaw)

class EWorldType:
    NONE, Game, Editor, PIE, EditorPreview, GamePreview, Inactive = range(7)

def GetWorld():
    '''Returns the best guess of what the "current" world to use is'''
    worlds = {} # worldType -> *first* world of that type
    for w in GetAllWorlds():
        t = w.WorldType
        if worlds.get(t) is None:
            worlds[t] = w
    return worlds.get(EWorldType.Game) or worlds.get(EWorldType.PIE) or worlds.get(EWorldType.Editor)

def DestroyAllActorsOfClass(klass):
    for a in GetAllActorsOfClass(GetWorld(), klass):
        a.Destroy()

