// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "uepyCommands.h"

#define LOCTEXT_NAMESPACE "FuepyModule"

void FuepyCommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "uepy", "Bring up uepy window", EUserInterfaceActionType::Button, FInputGesture());
}

#undef LOCTEXT_NAMESPACE
