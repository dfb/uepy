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

        # Also make each enum accessible as a purely lowercase name to aid in translating user input (or
        # some other source that may not know the proper capitalization) to enum values
        for v,k in inverse.items():
            dct[k.lower()] = v
        return super().__new__(metaclass, name, bases, dct)

class Enum(metaclass=EnumMeta):
    '''Base class for all Enums we expose to Python'''
    @classmethod
    def NameFor(cls, v):
        '''Given an enum value, returns the string name of that value'''
        return cls._inverse[v]

    @classmethod
    def FromName(cls, name):
        '''Looks up the value from the name. If 'name' is not a string, assumes the input is already a value
        and just returns it.'''
        if isinstance(name, str):
            return getattr(cls, name)
        return name

    @classmethod
    def Inverse(cls):
        '''Returns a mapping of enum value --> name'''
        return cls._inverse

    @classmethod
    def Values(cls):
        '''Returns a list of all of the enum's values'''
        return list(cls._inverse.keys())

class EForceInit(Enum):
    ForceInit, ForceInitToZero = range(2)

class EEngineMode(Enum):
    '''Tells which mode we're in right now'''
    Unknown, Build, SrcCLI, Editor, PIE = range(5)

class EWorldType(Enum):
    NONE, Game, Editor, PIE, EditorPreview, GamePreview, Inactive = range(7)

class EHorizontalAlignment(Enum): # For use with UMG/Slate
    HAlign_Fill = Fill = 0
    HAlign_Left = Left = 1
    HAlign_Center = Center = 2
    HAlign_Right = Right = 3

class EVerticalAlignment(Enum): # For use with UMG/Slate
    VAlign_Fill = Fill = 0
    VAlign_Top = Top = 1
    VAlign_Center = Center = 2
    VAlign_Bottom = Bottom = 3

class EHorizTextAligment(Enum): # For use with UTextRenderComponent
    EHTA_Left = Left = 0
    EHTA_Center = Center = 1
    EHTA_Right = Right = 2
EHorizTextAlignment = EHorizTextAligment

class EVerticalTextAligment(Enum): # For use with UTextRenderComponent
    EVRTA_TextTop = Top = 0
    EVRTA_TextCenter = Center = 1
    EVRTA_TextBottom = Bottom = 2
    EVRTA_QuadTop = QuadTop = 3
EVerticalTextAlignment = EVerticalTextAligment

class ECollisionChannel(Enum):
    ECC_WorldStatic, ECC_WorldDynamic, ECC_Pawn, ECC_Visibility, ECC_Camera, ECC_PhysicsBody, ECC_Vehicle, ECC_Destructible, ECC_EngineTraceChannel1,\
    ECC_EngineTraceChannel2, ECC_EngineTraceChannel3, ECC_EngineTraceChannel4, ECC_EngineTraceChannel5, ECC_EngineTraceChannel6, ECC_GameTraceChannel1,\
    ECC_GameTraceChannel2, ECC_GameTraceChannel3, ECC_GameTraceChannel4, ECC_GameTraceChannel5, ECC_GameTraceChannel6, ECC_GameTraceChannel7,\
    ECC_GameTraceChannel8, ECC_GameTraceChannel9, ECC_GameTraceChannel10, ECC_GameTraceChannel11, ECC_GameTraceChannel12, ECC_GameTraceChannel13,\
    ECC_GameTraceChannel14, ECC_GameTraceChannel15, ECC_GameTraceChannel16, ECC_GameTraceChannel17, ECC_GameTraceChannel18 = range(32)

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

class EHMDTrackingOrigin(Enum): Floor, Eye, Stage = range(3)

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
    NONE = -1

class ELightUnits(Enum):
    Unitless, Candelas, Lumens = range(3)

class ESceneCaptureSource(Enum):
    SCS_SceneColorHDR, SCS_SceneColorHDRNoAlpha, SCS_FinalColorLDR, SCS_SceneColorSceneDepth, SCS_SceneDepth, SCS_DeviceDepth, SCS_Normal, SCS_BaseColor, SCS_FinalColorHDR, SCS_FinalToneCurveHDR = range(10)

class ESceneCapturePrimitiveRenderMode(Enum): PRM_LegacySceneCapture, PRM_RenderScenePrimitives, PRM_UseShowOnlyList = range(3)

class EVisibilityPropagation(Enum):
    NoPropagation, DirtyOnly, Propagate = range(3)

class EStretchDirection: Both, DownOnly, UpOnly = range(3)
class EStretch: NONE, Fill, ScaleToFit, ScaleToFitX, ScaleToFitY, ScaleToFill, ScaleBySafeZone, UserSpecified = range(8)

class EInputEvent(Enum): IE_Pressed, IE_Released, IE_Repeat, IE_DoubleClick, IE_Axis, IE_MAX = range(6)

class ENRSpawnReplicatedBy(Enum): NONE, App, Engine, NR = range(4) # Who is in charge of replicating the act of spawning a particular object

class ENRWhere(Enum):
    NONE, Local, Host, NotHere = [0,1,2,4]
    All = Local|Host|NotHere
    USER = 128 # special flag indicating the lower 7 bits are a user ID
    NotMe = NotHere # deprecated
    @staticmethod
    def Only(userID):
        '''Helper to create a value that means "send this message only to a specific user"'''
        assert userID < 128, userID
        return userID | ENRWhere.USER

class EWidgetSpace(Enum): World, Screen = range(2)

class EWidgetGeometryMode(Enum): Plane, Cylinder = range(2)

class EWidgetInteractionSource(Enum): World, Mouse, CenterScreen, Custom = range(4)

class ESplineCoordinateSpace(Enum): Local, World = range(2)

class ESplinePointType(Enum): Linear, Curve, Constant, CurveClamped, CurveCustomTangent = range(5)

class EComponentMobility(Enum): Static, Stationary, Movable = range(3)

class ERelativeTransformSpace(Enum): RTS_World, RTS_Actor, RTS_Component, RTS_ParentBoneSpace = range(4)

class EOnJoinSessionCompleteResult(Enum): Success, SessionIsFull, SessionDoesNotExist, CouldNotRetrieveAddress, AlreadyInSession, UnknownError = range(6)

# this is a made-up enum since we don't yet expose FAttachmentTransformRules
class EAttachmentTransformRule(Enum): KeepRelativeTransform, KeepWorldTransform = range(2)

class ESkyLightSourceType(Enum): SLS_CapturedScene, SLS_SpecifiedCubemap = range(2)

class ETickingGroup(Enum): TG_PrePhysics, TG_StartPhysics, TG_DuringPhysics, TG_EndPhysics, TG_PostPhysics, TG_PostUpdateWork, TG_LastDemotable = range(7)

class ENaturalSoundFalloffMode(Enum): Continues, Silent, Hold = range(3)

class EAttenuationShape(Enum): Sphere, Capsule, Box, Cone = range(4)
class EPriorityAttenuationMethod(Enum): Linear, CustomCurve, Manual = range(3)
class EAttenuationDistanceModel(Enum): Linear, Logarithmic, Inverse, LogReverse, NaturalSound, Custom = range(6)

#class EPixelFormat(Enum): there are so many, just look up the one you need

class ETextureRenderTargetFormat(Enum): RTF_R8, RTF_RG8, RTF_RGBA8, RTF_RGBA8_SRGB, RTF_R16f, RTF_RG16f, RTF_RGBA16f, RTF_R32f, RTF_RG32f, RTF_RGBA32f, RTF_RGB10A2 = range(11)

class ESpectatorScreenMode(Enum): Disabled, SingleEyeLetterboxed, Undistorted, Distorted, SingleEye, SingleEyeCroppedToFill, Texture, TexturePlusEye = range(8)

