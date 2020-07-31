// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "uepyStyle.h"

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
	TSharedPtr< FUICommandInfo > OpenPluginWindow;
};