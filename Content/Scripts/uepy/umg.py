from _uepy import *
from _uepy._umg import *

def CreateWidget(owner, widgetClass, name=None, **kwargs):
    return CreateWidget_(owner, widgetClass, name or '', kwargs)
