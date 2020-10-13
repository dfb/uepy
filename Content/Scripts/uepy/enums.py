class EEngineMode:
    '''Tells which mode we're in right now'''
    Unknown, Build, SrcCLI, Editor, PIE = range(5)

class EWorldType:
    NONE, Game, Editor, PIE, EditorPreview, GamePreview, Inactive = range(7)

class EHorizontalAlignment:
    HAlign_Fill = Fill = 0
    HAlign_Left = Left = 1
    HAlign_Center = Center = 2
    HAlign_Right = Right = 3

class EVerticalAlignment:
    VAlign_Fill = Fill = 0
    VAlign_Top = Top = 1
    VAlign_Center = Center = 2
    VAlign_Bottom = Bottom = 3

class ECollisionChannel:
    ECC_WorldStatic = 0
    ECC_WorldDynamic = 1
    ECC_Pawn = 2
    ECC_Visibility = 3
    ECC_Camera = 4
    ECC_PhysicsBody = 5
    ECC_Vehicle = 6
    ECC_Destructible = 7

class ECollisionEnabled:
    NoCollision, QueryOnly, PhysicsOnly, QueryAndPhysics = range(4)

class ECollisionResponse:
    ECR_Ignore, ECR_Overlap, ECR_Block = range(3)
    Ignore = ECR_Ignore
    Overlap = ECR_Overlap
    Block = ECR_Block

class EEasingFunc:
    Linear, Step, SinusoidalIn, SinusoidalOut, SinusoidalInOut, EaseIn, EaseOut, EaseInOut, ExpoIn, ExpoOut, ExpoInOut, CircularIn, CircularOut, CircularInOut = range(14)

class EDrawDebugTrace:
    NONE, ForOneFrame, ForDuration, Persistent = range(4)

class ESlateColorStylingMode:
    UseColor_Specified, UseColor_Specified_Link, UseColor_Foreground, UseColor_Foreground_Subdued = range(4)

class ESlateVisibility:
    Visible, Collapsed, Hidden, HitTestInvisible, SelfHitTestInvisible = range(5)

class ESlateSizeRule:
    Automatic, Fill = range(2)

class ETextJustify:
    Left, Center, Right = range(3)

class EOrientation:
    Orient_Horizontal, Orient_Vertical = range(2)
    Horizontal, Vertical = Orient_Horizontal, Orient_Vertical

class EControllerHand:
    Left, Right, AnyHand = range(3)

