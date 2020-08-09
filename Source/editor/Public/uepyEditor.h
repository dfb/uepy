#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
//#include "uepyEditor.generated.h"
#include "Framework/Commands/Commands.h"
#include "uepyStyle.h"

class FuepyEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    void OnTextCommitted(const FText& text, ETextCommit::Type type);
	TSharedPtr<class SEditableText> replText;

private:
    TSharedRef<class SDockTab> OnSpawnConsole(const class FSpawnTabArgs& SpawnTabArgs);
    TSharedRef<class SDockTab> OnSpawnSpawner(const class FSpawnTabArgs& SpawnTabArgs);
    TSharedPtr<class FUICommandList> PluginCommands;



};

class FuepyCommands : public TCommands<FuepyCommands>
{
public:

	FuepyCommands()
		: TCommands<FuepyCommands>(TEXT("uepy"), NSLOCTEXT("Contexts", "uepy", "uepy Plugin"), NAME_None, FuepyStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > OpenConsole;
	TSharedPtr< FUICommandInfo > OpenSpawner;
};

