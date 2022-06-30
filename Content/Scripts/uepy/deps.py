'''Manages installable dependencies - used both to package them and at runtime to make them available'''
import os, shutil, zipapp, glob, sys

def Package():
    '''Called if you run deps.py from the command line, which you should do anytime you modify requirements.txt,
    i.e. add a dependency to requirements.txt (in your project's Content/Scripts dir) and then, from that dir run:
    python ~/path/to/your/prj/Plugins/uepy/Content/Scripts/uepy/deps.py
    '''
    outName = 'deps.pyz'
    if os.path.exists(outName):
        os.remove(outName)

    tempDir = 'deps.tmp'
    if os.path.exists(tempDir):
        shutil.rmtree(tempDir)
    os.mkdir(tempDir)

    assert not os.system(r'c:\python387\scripts\pip.exe install -r requirements.txt --target ' + tempDir)

    with open(os.path.join(tempDir, '__main__.py'), 'w') as f:
        f.write('print("hello")\n')

    zipapp.create_archive(tempDir, outName)
    shutil.rmtree(tempDir)

def Discover(inDir):
    '''Finds any .pyz files and adds them to sys.path. This should be called on startup by main.py.'''
    libs = glob.glob(os.path.join(inDir, '*.pyz'))
    sys.path.extend(libs)
    return libs

if __name__ == '__main__':
    Package()

