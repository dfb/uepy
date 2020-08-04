#include "uepyEditor.h"
#include "uepyStyle.h"
#include "uepyCommands.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

static const FName uepyTabName("uepy");

#define LOCTEXT_NAMESPACE "FuepyEditorModule"

void FuepyEditorModule::StartupModule()
{
    FuepyStyle::Initialize();
    FuepyStyle::ReloadTextures();

    FuepyCommands::Register();
    
    PluginCommands = MakeShareable(new FUICommandList);
    PluginCommands->MapAction(
        FuepyCommands::Get().OpenPluginWindow,
        FExecuteAction::CreateRaw(this, &FuepyEditorModule::PluginButtonClicked),
        FCanExecuteAction());
        
    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
    
    {
        TSharedPtr<FExtender> MenuExtender = MakeShareable(new FExtender());
        MenuExtender->AddMenuExtension("WindowLayout", EExtensionHook::After, PluginCommands, FMenuExtensionDelegate::CreateRaw(this, &FuepyEditorModule::AddMenuExtension));

        LevelEditorModule.GetMenuExtensibilityManager()->AddExtender(MenuExtender);
    }
    
    {
        TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
        ToolbarExtender->AddToolBarExtension("Settings", EExtensionHook::After, PluginCommands, FToolBarExtensionDelegate::CreateRaw(this, &FuepyEditorModule::AddToolbarExtension));
        
        LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);
    }
    
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(uepyTabName, FOnSpawnTab::CreateRaw(this, &FuepyEditorModule::OnSpawnPluginTab))
        .SetDisplayName(LOCTEXT("FuepyTabTitle", "uepy"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FuepyEditorModule::ShutdownModule()
{
    // This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
    // we call this function before unloading the module.
    FuepyStyle::Shutdown();

    FuepyCommands::Unregister();

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(uepyTabName);
}

TSharedRef<SDockTab> FuepyEditorModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
    FText WidgetText = FText::Format(
        LOCTEXT("WindowWidgetText", "Add code to {0} in {1} to override this window's contents"),
        FText::FromString(TEXT("FuepyEditorModule::OnSpawnPluginTab")),
        FText::FromString(TEXT("uepyEditor.cpp"))
        );

    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            // Put your tab content here!
            SNew(SBox)
            .HAlign(HAlign_Center)
            .VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(WidgetText)
            ]
        ];
}

void FuepyEditorModule::PluginButtonClicked()
{
    FGlobalTabmanager::Get()->InvokeTab(uepyTabName);
}

void FuepyEditorModule::AddMenuExtension(FMenuBuilder& Builder)
{
    Builder.AddMenuEntry(FuepyCommands::Get().OpenPluginWindow);
}

void FuepyEditorModule::AddToolbarExtension(FToolBarBuilder& Builder)
{
    Builder.AddToolBarButton(FuepyCommands::Get().OpenPluginWindow);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FuepyEditorModule, uepyEditor)

