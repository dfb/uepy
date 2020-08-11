#include "uepyEditor.h"
#include "uepyStyle.h"
#include "common.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

static const FName consoleTabName("uepy Console");
static const FName spawnerTabName("uepy Spawner");

#define LOCTEXT_NAMESPACE "FuepyEditorModule"

void FuepyCommands::RegisterCommands()
{
	UI_COMMAND(OpenConsole, "Console", "Open uepy console/log", EUserInterfaceActionType::Button, FInputGesture());
	UI_COMMAND(OpenSpawner, "Spawner", "Open uepy actor spawner", EUserInterfaceActionType::Button, FInputGesture());
}

DEFINE_LOG_CATEGORY(UEPYED);

void FuepyEditorModule::StartupModule()
{
    FuepyStyle::Initialize();
    FuepyStyle::ReloadTextures();

    /*
    FuepyCommands::Register();
    
    PluginCommands = MakeShareable(new FUICommandList);
    PluginCommands->MapAction(FuepyCommands::Get().OpenConsole, FExecuteAction::CreateLambda([]() { FGlobalTabmanager::Get()->InvokeTab(consoleTabName); }), FCanExecuteAction());
    PluginCommands->MapAction(FuepyCommands::Get().OpenSpawner, FExecuteAction::CreateLambda([]() { FGlobalTabmanager::Get()->InvokeTab(spawnerTabName); }), FCanExecuteAction());

    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
    
    {
        TSharedPtr<FExtender> MenuExtender = MakeShareable(new FExtender());
        MenuExtender->AddMenuExtension("General", EExtensionHook::After, PluginCommands, FMenuExtensionDelegate::CreateLambda([](FMenuBuilder& builder)
        {
            builder.AddSubMenu(FText::FromString(TEXT("uepy")), FText::FromString(TEXT("uepy utils")), FNewMenuDelegate::CreateLambda([](FMenuBuilder& builder)
            {
                builder.AddMenuEntry(FuepyCommands::Get().OpenConsole);
                builder.AddMenuEntry(FuepyCommands::Get().OpenSpawner);
            }));
        }));

        LevelEditorModule.GetMenuExtensibilityManager()->AddExtender(MenuExtender);
    }
    */
    
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(consoleTabName, FOnSpawnTab::CreateRaw(this, &FuepyEditorModule::OnSpawnConsole))
        .SetDisplayName(FText::FromName(consoleTabName)).SetMenuType(ETabSpawnerMenuType::Enabled)
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory());
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(spawnerTabName, FOnSpawnTab::CreateRaw(this, &FuepyEditorModule::OnSpawnSpawner))
        .SetDisplayName(FText::FromName(spawnerTabName)).SetMenuType(ETabSpawnerMenuType::Enabled)
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory());
}

void FuepyEditorModule::ShutdownModule()
{
    // This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
    // we call this function before unloading the module.
    FuepyStyle::Shutdown();

    FuepyCommands::Unregister();

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(consoleTabName);
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(spawnerTabName);
}

// spawns the actor spawner utility panel
TSharedRef<SDockTab> FuepyEditorModule::OnSpawnSpawner(const FSpawnTabArgs& SpawnTabArgs)
{
    FText WidgetText = FText::Format(
        LOCTEXT("WindowWidgetText", "Add code to {0} in {1} to override this window's contents"),
        FText::FromString(TEXT("FuepyEditorModule::OnSpawnPluginTab")),
        FText::FromString(TEXT("uepyEditor.cpp"))
        );

    //auto poo = NewObject<UTestEditorWidget>();
    UUserWidget* poo = CreateWidget<UTestEditorWidget>(GEditor->GetEditorWorldContext().World(), UTestEditorWidget::StaticClass());

    TSharedPtr<SBox> box;
    TSharedRef<SDockTab> tab = SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SAssignNew(box, SBox)
            .HAlign(HAlign_Center)
            .VAlign(VAlign_Center)
        ];
    box->SetContent(poo->TakeWidget());
    return tab;
    /*
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SBox)
            .HAlign(HAlign_Center)
            .VAlign(VAlign_Center)
            [
                //SNew(STextBlock)
                //.Text(WidgetText)
                poo->WidgetTree
            ]
        ];
    */
}

// spawns the python interactive console
TSharedRef<SDockTab> FuepyEditorModule::OnSpawnConsole(const FSpawnTabArgs& SpawnTabArgs)
{
    return SNew(SDockTab)
           .TabRole(ETabRole::NomadTab)
           [
            SNew(SPythonConsole)
           ];
}

TSharedRef<SWidget> UTestEditorWidget::RebuildWidget()
{
    if (!GetRootWidget())
    {
        UWidget *root = WidgetTree->ConstructWidget<UWidget>(UCanvasPanel::StaticClass(), TEXT("RootWidget"));

        // try to configure it some if it has a canvas panel slot
        UCanvasPanelSlot *rootSlot = Cast<UCanvasPanelSlot>(root->Slot);
        if (rootSlot)
        {
            rootSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
            rootSlot->SetOffsets(FMargin(0.0f, 0.0f));
        }
        WidgetTree->RootWidget = root;
    }

    TSharedRef<SWidget> ret = Super::RebuildWidget();
    return ret;
}

void UTestEditorWidget::NativePreConstruct()
{
    Super::NativePreConstruct();
    UButton *b = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
    UCanvasPanel *root = Cast<UCanvasPanel>(WidgetTree->RootWidget);
    root->AddChildToCanvas(b);
    //UCanvasPanelSlot *rootSlot = Cast<UCanvasPanelSlot>(WidgetTree->RootWidget->Slot);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FuepyEditorModule, uepyEditor)

