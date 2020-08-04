#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
//#include "uepyEditor.generated.h"

class FuepyEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /** This function will be bound to Command (by default it will bring up plugin window) */
    void PluginButtonClicked();

private:
    void AddToolbarExtension(FToolBarBuilder& Builder);
    void AddMenuExtension(FMenuBuilder& Builder);

    TSharedRef<class SDockTab> OnSpawnPluginTab(const class FSpawnTabArgs& SpawnTabArgs);
    TSharedPtr<class FUICommandList> PluginCommands;


};

