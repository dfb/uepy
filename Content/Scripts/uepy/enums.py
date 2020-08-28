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

