'''
Engine enumerations exposed to Python, along with a base enum class that adds a reverse mapping (value-to-name) API.
'''

class EnumMeta(type):
    '''Used by Enum class'''
    def __new__(metaclass, name, bases, dct):
        # Create an inverse mapping of values to name - note that in the case of multiple names mapping to
        # the same value, you can't know which one will be returned when NameFor is called.
        inverse = {}
        for k,v in dct.items():
            if type(v) is int:
                inverse[v] = k
        dct['_inverse'] = inverse
        return super().__new__(metaclass, name, bases, dct)

class Enum(metaclass=EnumMeta):
    '''Base class for all Enums we expose to Python'''
    @classmethod
    def NameFor(cls, v):
        '''Given an enum value, returns the string name of that value'''
        return cls._inverse[v]

    @classmethod
    def Inverse(cls):
        '''Returns a mapping of enum value --> name'''
        return cls._inverse

class EForceInit(Enum):
    ForceInit, ForceInitToZero = range(2)

class EEngineMode(Enum):
    '''Tells which mode we're in right now'''
    Unknown, Build, SrcCLI, Editor, PIE = range(5)

class EWorldType(Enum):
    NONE, Game, Editor, PIE, EditorPreview, GamePreview, Inactive = range(7)

class EHorizontalAlignment(Enum):
    HAlign_Fill = Fill = 0
    HAlign_Left = Left = 1
    HAlign_Center = Center = 2
    HAlign_Right = Right = 3

class EVerticalAlignment(Enum):
    VAlign_Fill = Fill = 0
    VAlign_Top = Top = 1
    VAlign_Center = Center = 2
    VAlign_Bottom = Bottom = 3

class ECollisionChannel(Enum):
    ECC_WorldStatic = 0
    ECC_WorldDynamic = 1
    ECC_Pawn = 2
    ECC_Visibility = 3
    ECC_Camera = 4
    ECC_PhysicsBody = 5
    ECC_Vehicle = 6
    ECC_Destructible = 7

class ECollisionEnabled(Enum):
    NoCollision, QueryOnly, PhysicsOnly, QueryAndPhysics = range(4)

class ECollisionResponse(Enum):
    ECR_Ignore, ECR_Overlap, ECR_Block = range(3)
    Ignore = ECR_Ignore
    Overlap = ECR_Overlap
    Block = ECR_Block

class EEasingFunc(Enum):
    Linear, Step, SinusoidalIn, SinusoidalOut, SinusoidalInOut, EaseIn, EaseOut, EaseInOut, ExpoIn, ExpoOut, ExpoInOut, CircularIn, CircularOut, CircularInOut = range(14)

class EDrawDebugTrace(Enum):
    NONE, ForOneFrame, ForDuration, Persistent = range(4)

class ESlateColorStylingMode(Enum):
    UseColor_Specified, UseColor_Specified_Link, UseColor_Foreground, UseColor_Foreground_Subdued = range(4)

class ESlateVisibility(Enum):
    Visible, Collapsed, Hidden, HitTestInvisible, SelfHitTestInvisible = range(5)

class ESlateSizeRule(Enum):
    Automatic, Fill = range(2)

class ETextJustify(Enum):
    Left, Center, Right = range(3)

class EOrientation(Enum):
    Orient_Horizontal, Orient_Vertical = range(2)
    Horizontal, Vertical = Orient_Horizontal, Orient_Vertical

class EControllerHand(Enum):
    Left, Right, AnyHand = range(3)

class ELightUnits(Enum):
    Unitless, Candelas, Lumens = range(3)

class ESceneCaptureSource(Enum):
    SCS_SceneColorHDR, SCS_SceneColorHDRNoAlpha, SCS_FinalColorLDR, SCS_SceneColorSceneDepth, SCS_SceneDepth, SCS_DeviceDepth, SCS_Normal, SCS_BaseColor, SCS_FinalColorHDR = range(9)

class EVisibilityPropagation(Enum):
    NoPropagation, DirtyOnly, Propagate = range(3)

class EStretchDirection: Both, DownOnly, UpOnly = range(3)
class EStretch: NONE, Fill, ScaleToFit, ScaleToFitX, ScaleToFitY, ScaleToFill, ScaleBySafeZone, UserSpecified = range(8)
