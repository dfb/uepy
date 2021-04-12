'''
Provides a development tool that monitors the source files used, reloading stuff when
something changes. The developer provides a "dev module" that is used to set up the
work area. Any module can opt-in to be informed about reloads by implementing any of:

- OnModuleBeforeReload(watcher) - called right before the module is reloaded. Any value returned is
    state to be persisted across the reload.
- OnModuleAfterReload(watcher, state) - called right after the reload; state is whatever was returned
    from OnModuleBeforeReload.

The dev module *must* define a top-level variable called MODULE_SOURCE_ROOTS, a list of directory
roots that should be monitored for file changes (well, only files actually used, matter, but it will
restrict its observations to files in these trees).

'''

import sys, ast, os, importlib, time, traceback

# these get patched
# TODO: use logging code from toolbox.utils
def log(*args):
    print(' '.join(str(x) for x in args))
def logTB():
    for line in traceback.format_exc().split('\n'):
        log(line)

class ImportFinder(ast.NodeVisitor):
    def __init__(self):
        super().__init__()
        self.funcDepth = 0 # used to track if we're inside a function definition or not
        self.moduleNames = set()

    def visit_FunctionDef(self, node):
        self.funcDepth += 1
        try:
            self.generic_visit(node)
        finally:
            self.funcDepth -= 1

    def visit_Import(self, node):
        if self.funcDepth > 0:
            # Ignore inner imports as those often create cycles that are too hard to handle properly
            return
        for alias in node.names:
            self.moduleNames.add(alias.name)

    def visit_ImportFrom(self, node):
        if self.funcDepth > 0:
            # Ignore inner imports as those often create cycles that are too hard to handle properly
            return
        # Multiple cases here:
        # from module import submodule
        # from module import someVar
        # We assume all are modules and then later when we try to get them out of sys.modules, the ones that were just someVar
        # imports will be skipped over
        if node.module is not None:
            self.moduleNames.add(node.module)
            for alias in node.names:
                self.moduleNames.add(node.module + '.' + alias.name)

    @staticmethod
    def Scan(filename):
        '''Returns a list of module names that the given python file imports. Returns an empty list
        if the filename is None, is not a .py file, cannot be found, or has syntax errors.'''
        if not filename or not filename.lower().endswith('.py') or not os.path.exists(filename):
            return []
        with open(filename) as f:
            src = f.read()

        f = ImportFinder()
        f.visit(ast.parse(src))
        return list(f.moduleNames)

class ModuleInfoTracker:
    '''Builds up a dependency graph of modules'''
    class ModuleInfo:
        def __init__(self, name, filename, isInSourceRoots):
            self.name = name
            self.iisr = isInSourceRoots
            self.savedState = None # returned from module's OnModuleBeforeReload
            self.imports = [] # ModuleInfo instances for modules this module directly imports
            self.importedBy = set() # ModuleInfo instances for modules that directly import this module
            f = self.filename = filename
            self.isAppSource = f and not f.endswith('.pyd') and isInSourceRoots # True if this module comes from a .py file and that file lives in one of the source roots
            self.UpdateLastMod()

        def UpdateLastMod(self):
            if self.filename and os.path.exists(self.filename):
                self.lastMod = os.path.getmtime(self.filename)

        def IsRoot(self):
            '''Returns True if this module is not imported by anyone else'''
            return not self.importedBy

        def IsLeaf(self):
            '''Returns True if this module imports no modules, or imports only builtin/system modules'''
            for m in self.imports:
                if m.isAppSource:
                    return False
            return True

        def HasChanged(self):
            '''Returns True if the module file on disk has changed'''
            if not self.isAppSource:
                return False

            try:
                curLastMod = os.path.getmtime(self.filename)
                return curLastMod != self.lastMod
            except:
                return False # I guess?

        def CallModuleHook(self, name, *args):
            '''Calls the module's hook if it defines one, returning the result'''
            m = sys.modules.get(self.name)
            if m:
                cb = getattr(m, name, None)
                if cb:
                    try:
                        return cb(*args)
                    except:
                        logTB()
            return None

        def ReloadIfNeeded(self, watcher):
            reloaded = False
            if self.needsReload:
                self.savedState = self.CallModuleHook('OnModuleBeforeReload', watcher)
                m = sys.modules.get(self.name)
                if m:
                    m = importlib.reload(m) # don't wrap this in a try/except block - we need an exception to bubble up if something is wrong
                    self.UpdateLastMod()
                    self.needsReload = False # so that other traversals of the dependency tree won't cause it to be loaded yet again
                    reloaded = True
                    self.CallModuleHook('OnModuleAfterReload', watcher, self.savedState)
                else:
                    log('ERROR: can no longer find module', mi)
            return reloaded

    def __init__(self):
        self.Reset()

    def Reset(self, newSourceRoots=None):
        self.modules = {} # module.__name__ -> ModuleInfo
        self.sourceRoots = (newSourceRoots or [])[:] # we ignore modules not found in these directories or their children

    def SetSourceRoots(self, roots):
        self.sourceRoots = [os.path.abspath(x.strip()).lower().replace('\\','/') for x in roots]

    def _IsInSourceRoots(self, filename):
        '''Returns True if the given filename is in one of our source roots'''
        if not filename:
            return False
        f = os.path.abspath(filename).lower().strip().replace('\\','/')
        for src in self.sourceRoots:
            if f.startswith(src):
                return True
        return False

    def InfoFor(self, module):
        '''Returns the ModuleInfo for the given module, creating it (and the entries for any
        dependencies as needed)'''
        name = module.__name__
        mi = self.modules.get(name)
        if mi is None:
            filename = getattr(module, '__file__', None)
            mi = ModuleInfoTracker.ModuleInfo(name, filename, self._IsInSourceRoots(filename))
            self.modules[name] = mi

            # Kick off a check for all modules this module depends on
            if mi.isAppSource:
                for otherName in ImportFinder.Scan(mi.filename):
                    otherModule = sys.modules.get(otherName)
                    if not otherModule:
                        continue
                    otherMI = self.InfoFor(otherModule)
                    mi.imports.append(otherMI)
                    otherMI.importedBy.add(mi)
        return mi

    def MarkReloadModules(self, skipMarking=None):
        '''Sets .needsReload=T|F on any module based on whether or not each has changed or if any of
        its dependencies have changed. Returns the list of names of modules that need to be reloaded. if
        skipMarking is not None, it is a list of module names that can still be scanned but that should not
        be marked as needing a reload.'''
        rootMIs = [x for x in self.modules.values() if x.IsRoot()]
        reloadNames = []
        scannedNames = []
        for mi in rootMIs:
            reloadNames.extend(self._MarkReloadTree(mi, skipMarking or [], scannedNames))
        return reloadNames # Note: this list may contain duplicates, even though we won't reload a module multiple times in one go

    def _MarkReloadTree(self, mi, skipMarking, scannedNames):
        if mi.name not in scannedNames:
            scannedNames.append(mi.name)
        reloadNames = []
        needsReload = False
        for otherMI in mi.imports:
            if otherMI.name not in scannedNames:
                reloadNames.extend(self._MarkReloadTree(otherMI, skipMarking, scannedNames))
            if getattr(otherMI, 'needsReload', False):
                needsReload = True

        mi.needsReload = (needsReload or mi.HasChanged()) and mi.name not in skipMarking
        if mi.needsReload:
            reloadNames.append(mi.name)
        return reloadNames

    def ReloadMarkedModules(self):
        '''Reload all modules marked by MarkReloadModules. Returns the names of modules that were reloaded, in order.'''
        # Find all root modules - the ones that are not imported by anyone else
        reloaded = []
        rootMIs = [x for x in self.modules.values() if x.IsRoot()]
        processedMIs = []
        for mi in rootMIs:
            reloaded.extend(self._ReloadTreeIfNeeded(mi, processedMIs))
        return reloaded

    def _ReloadTreeIfNeeded(self, mi, processedMIs):
        '''Reloads any dependency modules if they have been marked as needing reload, then reloads this module
        if it has been flagged too.'''
        if mi in processedMIs: # prevent infinite recursion if modules depend on each other (e.g. via delayed / localfunc import)
            return []
        processedMIs.append(mi)

        reloaded = []
        for otherMI in mi.imports:
            if otherMI not in processedMIs:
                reloaded.extend(self._ReloadTreeIfNeeded(otherMI, processedMIs))

        if mi.ReloadIfNeeded(self):
            reloaded.append(mi.name)
        return reloaded

GLOBAL_NAME = '__global_source_watcher__'
def GetGlobalInstance():
    '''Returns the globally shared source watcher instance if it exists, else None'''
    return __builtins__.get(GLOBAL_NAME)

class SourceWatcher:
    '''Monitors the source files for one or more classes, triggering an ordered reload when something changes'''
    def __init__(self, devModuleName='scratchpad', installGlobally=False):
        self.devModuleName = devModuleName # name of a module we'll import; this is the main module the developer uses as their work environment
        self.devModule = None # the actual module itself, once we've successfully imported it
        self.sourceRoots = [] # roots of directory trees that we'll monitor for changes to classes we're watching
        self.nextFirstLoadTry = 0
        self.mit = ModuleInfoTracker()

        # In some scenarios, it's useful to have a global shared source watcher
        if installGlobally:
            if GetGlobalInstance():
                log('WARNING: SourceWatcher told to install itself globally but one already exists (will overwrite it)')
            __builtins__[GLOBAL_NAME] = self

    def Cleanup(self):
        '''If the dev module is loaded, tries to call its before reload hook'''
        if self.devModule:
            mi = self.mit.InfoFor(self.devModule)
            mi.CallModuleHook('OnModuleBeforeReload', self)

    def UpdateSourceRoots(self):
        if self.devModule:
            self.sourceRoots = [os.path.abspath(x) for x in getattr(self.devModule, 'MODULE_SOURCE_ROOTS', [])]
            if not self.sourceRoots:
                log('WARNING: dev module did not provide MODULE_SOURCE_ROOTS')
            self.mit.SetSourceRoots(self.sourceRoots)

    def Check(self, skipDevModuleReload=False, forceDevModuleReload=False):
        '''Called frequently to see if anything has happened. If skipDevModuleReload is True, then it won't get reloaded
        even if changes have been detected. forceDevModuleReload does the opposite - it forces the dev module to be
        reloaded even if no changes have been detected. (both of these options are for working with PIE)'''
        try:
            # TODO: make handling of devModule less special-case and reduce duplication below for first load vs reload
            # If we've never successfully loaded the module before, try to do so but don't spin like crazy
            now = time.time()
            if (not skipDevModuleReload) and (forceDevModuleReload or (not self.devModule and now >= self.nextFirstLoadTry)):
                # We haven't even imported it yet, at least not successfully
                try:
                    self.mit.Reset(self.sourceRoots)
                    m = sys.modules.get(self.devModuleName)
                    if m:
                        self.devModule = importlib.reload(m)
                    else:
                        self.devModule = importlib.import_module(self.devModuleName)
                    self.UpdateSourceRoots()
                    mi = self.mit.InfoFor(self.devModule)
                    mi.CallModuleHook('OnModuleAfterReload', self, mi.savedState)
                except ModuleNotFoundError:
                    # Same as below, but don't log a noisy error
                    self.devModule = None
                    self.nextFirstLoadTry = time.time() + 1
                    return
                except:
                    self.devModule = None
                    self.nextFirstLoadTry = time.time() + 1
                    logTB()
                    log('ERROR: failed to load dev module', self.devModuleName)
                    return

            reloadNames = self.mit.MarkReloadModules([self.devModuleName] if skipDevModuleReload else None)
            if reloadNames:
                reloadNames = self.mit.ReloadMarkedModules() # reloadNames originally was provisional and could have had duplicates, now has the actual list of what was reordered
                self.mit.Reset(self.sourceRoots)
                log('Reloaded modules', reloadNames)
                if self.devModuleName in reloadNames:
                    self.devModule = sys.modules[self.devModuleName]
                    self.UpdateSourceRoots()
                if self.devModule:
                    self.mit.InfoFor(self.devModule)
        except:
            logTB()
            self.devModule = None

if __name__ == '__main__':
    sw = SourceWatcher('scratchpad')
    while 1:
        sw.Check()
        time.sleep(0.25)

