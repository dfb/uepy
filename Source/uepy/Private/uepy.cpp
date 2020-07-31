// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "uepy.h"
#include "common.h"
#include "py_module.h"
#include "uepyStyle.h"
#include "uepyCommands.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

DEFINE_LOG_CATEGORY(UEPY);

static const FName uepyTabName("uepy");

#define LOCTEXT_NAMESPACE "FuepyModule"

void FuepyModule::StartupModule()
{
    // This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
    py::initialize_interpreter();

    FuepyStyle::Initialize();
    FuepyStyle::ReloadTextures();

    FuepyCommands::Register();
    
    PluginCommands = MakeShareable(new FUICommandList);

    PluginCommands->MapAction(
        FuepyCommands::Get().OpenPluginWindow,
        FExecuteAction::CreateRaw(this, &FuepyModule::PluginButtonClicked),
        FCanExecuteAction());
        
    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
    
    {
        TSharedPtr<FExtender> MenuExtender = MakeShareable(new FExtender());
        MenuExtender->AddMenuExtension("WindowLayout", EExtensionHook::After, PluginCommands, FMenuExtensionDelegate::CreateRaw(this, &FuepyModule::AddMenuExtension));

        LevelEditorModule.GetMenuExtensibilityManager()->AddExtender(MenuExtender);
    }
    
    {
        TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
        ToolbarExtender->AddToolBarExtension("Settings", EExtensionHook::After, PluginCommands, FToolBarExtensionDelegate::CreateRaw(this, &FuepyModule::AddToolbarExtension));
        
        LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);
    }
    
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(uepyTabName, FOnSpawnTab::CreateRaw(this, &FuepyModule::OnSpawnPluginTab))
        .SetDisplayName(LOCTEXT("FuepyTabTitle", "uepy"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);

    // we need the engine to start up before we do much more Python stuff
    FCoreDelegates::OnPostEngineInit.AddLambda([]() -> void { LoadPyModule(); });
}

void FuepyModule::ShutdownModule()
{
    // This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
    // we call this function before unloading the module.
    FuepyStyle::Shutdown();

    FuepyCommands::Unregister();

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(uepyTabName);
    py::finalize_interpreter();
}

TSharedRef<SDockTab> FuepyModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
    FText WidgetText = FText::Format(
        LOCTEXT("WindowWidgetText", "Add code to {0} in {1} to override this window's contents"),
        FText::FromString(TEXT("FuepyModule::OnSpawnPluginTab")),
        FText::FromString(TEXT("uepy.cpp"))
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

void FuepyModule::PluginButtonClicked()
{
    FGlobalTabmanager::Get()->InvokeTab(uepyTabName);
}

void FuepyModule::AddMenuExtension(FMenuBuilder& Builder)
{
    Builder.AddMenuEntry(FuepyCommands::Get().OpenPluginWindow);
}

void FuepyModule::AddToolbarExtension(FToolBarBuilder& Builder)
{
    Builder.AddToolBarButton(FuepyCommands::Get().OpenPluginWindow);
}

#undef LOCTEXT_NAMESPACE
    
IMPLEMENT_MODULE(FuepyModule, uepy)

