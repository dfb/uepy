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
#include "mod_uepy_editor.h"

static const FName consoleTabName("uepy Console");

#define LOCTEXT_NAMESPACE "FuepyEditorModule"

DEFINE_LOG_CATEGORY(UEPYED);

void FuepyEditorModule::StartupModule()
{
    FuepyStyle::Initialize();
    FuepyStyle::ReloadTextures();

    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(consoleTabName, FOnSpawnTab::CreateRaw(this, &FuepyEditorModule::OnSpawnConsole))
        .SetDisplayName(FText::FromName(consoleTabName)).SetMenuType(ETabSpawnerMenuType::Enabled)
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory());

    FUEPyDelegates::LaunchInit.AddStatic(&_LoadModuleEditor);
}

void FuepyEditorModule::ShutdownModule()
{
    // This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
    // we call this function before unloading the module.
    FuepyStyle::Shutdown();

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(consoleTabName);
}

// spawns the python interactive console
TSharedRef<SDockTab> FuepyEditorModule::OnSpawnConsole(const FSpawnTabArgs& SpawnTabArgs)
{
    return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SPythonConsole)];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FuepyEditorModule, uepyEditor)

