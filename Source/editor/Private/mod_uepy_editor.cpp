#include "mod_uepy_editor.h"
#include "common.h"
#include "LevelEditor.h"
#include "Blueprint/UserWidget.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Docking/TabManager.h"
#include "SPythonConsole.h"
#include "Runtime/AssetRegistry/Public/AssetRegistryModule.h"

// called on pre engine init
void _LoadModuleEditor(py::module& uepy)
{
    LOG("Creating Python module uepy.editor");

    py::module m = uepy.def_submodule("_editor");

    m.def("GetWorld", []() { return GEditor->GetEditorWorldContext().World(); }, py::return_value_policy::reference);

    m.def("RegisterNomadTabSpawner", [](py::object& _menuWidgetClass, std::string menuName)
    {
        UClass *menuWidgetClass = PyObjectToUClass(_menuWidgetClass);
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

    m.def("DeselectAllActors", []() { GEditor->SelectNone(true, true, false); });
    m.def("SelectActor", [](AActor *actor) { GEditor->SelectActor(actor, true, true); });

    py::class_<FAssetData>(m, "FAssetData")
        .def_property_readonly("AssetName", [](FAssetData& self) { return PYSTR(self.AssetName.ToString()); })
        .def_property_readonly("AssetClass", [](FAssetData& self) { return PYSTR(self.AssetClass.ToString()); })
        .def_property_readonly("ObjectPath", [](FAssetData& self) { return PYSTR(self.ObjectPath.ToString()); })
        .def_property_readonly("PackageName", [](FAssetData& self) { return PYSTR(self.PackageName.ToString()); })
        .def_property_readonly("PackagePath", [](FAssetData& self) { return PYSTR(self.PackagePath.ToString()); })
        .def("IsUAsset", [](FAssetData& self) { return self.IsUAsset(); })
        .def("IsRedirector", [](FAssetData& self) { return self.IsRedirector(); })
        .def("GetFullName", [](FAssetData& self) { return self.GetFullName(); })
        .def("GetAsset", [](FAssetData& self) { return self.GetAsset(); }, py::return_value_policy::reference)
        ;

    // this is an ugly hack and likely quite slow (it loads *all* assets of a given class name), but
    // for in-editor scripting, not too bad. We could add filtering or a assets-by-class thing or ...
    m.def("GetAssetsByClass", [](std::string& path)
    {
        TArray<FAssetData> assets;
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");
        AssetRegistryModule.Get().GetAssetsByClass(FSTR(path), assets, true);
        py::list ret;
        for (FAssetData asset : assets)
        {
            if (!asset.IsValid())
                continue;
            ret.append(asset.GetAsset());
        }
        return ret;
    }, py::return_value_policy::reference);

    UEPY_EXPOSE_CLASS(UPythonConsole, UWidget, m)
        ;
}

