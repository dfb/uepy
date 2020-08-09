#include "uepyEditor.h"
#include "uepyStyle.h"
#include "common.h"
//#include "uepyCommands.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"

#pragma warning(push)
#pragma warning (disable : 4686 4191 340)
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#pragma warning(pop)
namespace py = pybind11;

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
    
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(consoleTabName, FOnSpawnTab::CreateRaw(this, &FuepyEditorModule::OnSpawnConsole))
        .SetDisplayName(FText::FromName(consoleTabName)).SetMenuType(ETabSpawnerMenuType::Hidden);
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(spawnerTabName, FOnSpawnTab::CreateRaw(this, &FuepyEditorModule::OnSpawnSpawner))
        .SetDisplayName(FText::FromName(spawnerTabName)).SetMenuType(ETabSpawnerMenuType::Hidden);
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

void FuepyEditorModule::OnTextCommitted(const FText& text, ETextCommit::Type type)
{
    if (type == ETextCommit::OnEnter && !text.IsEmpty())
    {
        FString stext = text.ToString();
        UE_LOG(UEPYED, Log, TEXT("%s"), *stext);
        replText->SetText(FText::FromString(TEXT("")));

        try {
            //py::exec(TCHAR_TO_UTF8(*stext)); <-- doesn't cause the output to be printed
            PyObject* res = PyRun_String(TCHAR_TO_UTF8(*stext), Py_single_input, py::globals().ptr(), py::object().ptr());
            if (!res)
                throw py::error_already_set();
        } catch (std::exception e)
        {
            UE_LOG(LogTemp, Error, TEXT("%s"), UTF8_TO_TCHAR(e.what()));
        }
    }
}

// spawns the actor spawner utility panel
TSharedRef<SDockTab> FuepyEditorModule::OnSpawnSpawner(const FSpawnTabArgs& SpawnTabArgs)
{
    FText WidgetText = FText::Format(
        LOCTEXT("WindowWidgetText", "Add code to {0} in {1} to override this window's contents"),
        FText::FromString(TEXT("FuepyEditorModule::OnSpawnPluginTab")),
        FText::FromString(TEXT("uepyEditor.cpp"))
        );

    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SBox)
            .HAlign(HAlign_Center)
            .VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(WidgetText)
            ]
        ];
}

// spawns the python interactive console
TSharedRef<SDockTab> FuepyEditorModule::OnSpawnConsole(const FSpawnTabArgs& SpawnTabArgs)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SVerticalBox)
            +SVerticalBox::Slot()
            .FillHeight(1)
            [
                SNew(SMultiLineEditableTextBox)
                    .Style(FEditorStyle::Get(), "Log.TextBox")
                    .TextStyle(FEditorStyle::Get(), "Log.Normal")
                    .ForegroundColor(FLinearColor::Gray)
                    .IsReadOnly(true)
                    .AlwaysShowScrollbars(true)
            ]

            +SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SHorizontalBox)
                +SHorizontalBox::Slot()
                .FillWidth(3)
                [
                    SNew(SBorder)
                    [
                        SAssignNew(replText, SEditableText)
                        .ClearKeyboardFocusOnCommit(false)
                        .OnTextCommitted(FOnTextCommitted::CreateRaw(this, &FuepyEditorModule::OnTextCommitted))
                    ]
                ]
                +SHorizontalBox::Slot()
                .FillWidth(1)
                [
                    SNew(SBorder)
                    [
                        SNew(SEditableText)
                    ]
                ]
            ]
        ];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FuepyEditorModule, uepyEditor)

