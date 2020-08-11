// a logging console with an interactive python prompt
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Runtime/SlateCore/Public/SlateCore.h"
#include "EditorStyle.h"

class SPythonConsole : public SCompoundWidget, public FOutputDevice
{
public:

	SLATE_BEGIN_ARGS(SPythonConsole)
	{
	}

	SLATE_END_ARGS()

    SPythonConsole();
    ~SPythonConsole();

    void Construct(const FArguments& InArgs);
    void OnTextCommitted(const FText& text, ETextCommit::Type type);

protected:
	virtual void Serialize( const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category ) override;
    TSharedPtr<class FTextMarshaller> marshaller;
	TArray<TSharedPtr<struct FLogMessage>> messages;
	TSharedPtr< SMultiLineEditableTextBox > messagesBox;

private:
	TSharedPtr<SEditableText> replText;

};


