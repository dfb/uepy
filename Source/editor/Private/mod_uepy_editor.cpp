#include "mod_uepy_editor.h"
#include "common.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Docking/TabManager.h"

// called on pre engine init
void _LoadModuleEditor(py::module& uepy)
{
    LOG("Creating Python module uepy.editor");

    py::module m = uepy.def_submodule("editor");

    m.def("GetWorld", []() { return GEditor->GetEditorWorldContext().World(); }, py::return_value_policy::reference);

    m.def("RegisterNomadTabSpawner", [](UClass *menuWidgetClass, std::string menuName)
    {
        auto tabMgr = FGlobalTabmanager::Get();
        FName tabName(menuName.c_str());
        tabMgr->UnregisterNomadTabSpawner(tabName);
        tabMgr->RegisterNomadTabSpawner(tabName, FOnSpawnTab::CreateLambda([menuWidgetClass](const FSpawnTabArgs& spawnTabArgs)
        {
            // TODO: maybe also have a param that tells the module (or we figure it out on our own) so we can reload the module on open

            // create a dock tab with a box inside it, and then put the widget inside that box
            TSharedPtr<SBox> box;
            TSharedRef<SDockTab> tab = SNew(SDockTab)
                .TabRole(ETabRole::NomadTab)
                [
                    SAssignNew(box, SBox)
                    .HAlign(HAlign_Center)
                    .VAlign(VAlign_Center)
                ];

			try {
	            UUserWidget* widget = (UUserWidget*)CreateWidget<UUserWidget>(GEditor->GetEditorWorldContext().World(), menuWidgetClass);
				box->SetContent(widget->TakeWidget());
            } catchpy;
            return tab;
        }))
        .SetDisplayName(FText::FromName(tabName))
        .SetMenuType(ETabSpawnerMenuType::Enabled)
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory());
    });

}

