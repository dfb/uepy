/* Note: a lot of this comes from UnrealEnginePython's much-better version
Basically a quick hack job so I could get a Python REPL up and running.

TODO
- highlight rows that match filter text
- right click menu to clear
- impl copy support
- let user scroll away from the bottom
- prefix python input with '>>>'
- handle multiline input
- and much more...
*/

#include "SPythonConsole.h"
#include "common.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Text/BaseTextLayoutMarshaller.h"

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION

struct FLogMessage
{
    TSharedRef<FString> message;
    FName style;

    FLogMessage(const TSharedRef<FString>& m, FName s = NAME_None) : message(m), style(s) {}
};

class FTextMarshaller : public FBaseTextLayoutMarshaller
{
public:

    FTextMarshaller() : FBaseTextLayoutMarshaller() {};
    virtual void SetText(const FString& SourceString, FTextLayout& targetTextLayout) override
    {
        textLayout = &targetTextLayout;
    };

    virtual void GetText(FString& TargetString, const FTextLayout& SourceTextLayout) override {};

    void AddMessage(const TCHAR* msg, const ELogVerbosity::Type verbosity, const FName& category, TSharedPtr<SMultiLineEditableTextBox> messagesBox)

    {
        if (verbosity == ELogVerbosity::SetColor)
            return;
		if (!wcscmp(msg, L"") || !wcscmp(msg, L"\n"))
			return;

		FName style;
		if (category == NAME_Cmd)
			style = FName(TEXT("Log.Command"));
		else if (verbosity == ELogVerbosity::Error)
			style = FName(TEXT("Log.Error"));
		else if (verbosity == ELogVerbosity::Warning)
			style = FName(TEXT("Log.Warning"));
		else
			style = FName(TEXT("Log.Normal"));
        TSharedPtr<FLogMessage> newMsg = MakeShareable(new FLogMessage(MakeShareable(new FString(msg)), style));
        messages.Emplace(newMsg);

        if (textLayout)
        {
            TArray<FTextLayout::FNewLineData> newLines;
            const FTextBlockStyle& MessageTextStyle = FEditorStyle::Get().GetWidgetStyle<FTextBlockStyle>(newMsg->style);

            TArray<TSharedRef<IRun>> Runs;
            Runs.Add(FSlateTextRun::Create(FRunInfo(), newMsg->message, MessageTextStyle));

            newLines.Emplace(MoveTemp(newMsg->message), MoveTemp(Runs));
            textLayout->AddLines(newLines);
            messagesBox->ScrollTo(FTextLocation(messages.Num()));
        }
    }

    void ClearMessages();
    TArray<TSharedPtr<FLogMessage>> messages;

protected:

    FTextMarshaller(TArray<TSharedPtr<FLogMessage>> InMessages);

    void AppendMessageToTextLayout(const TSharedPtr<FLogMessage>& InMessage) {}
    void AppendMessagesToTextLayout(const TArray<TSharedPtr<FLogMessage>>& InMessages) {}

    FTextLayout* textLayout;
};


SPythonConsole::SPythonConsole()
{
    GLog->AddOutputDevice(this);
    GLog->SerializeBacklog(this);
}

SPythonConsole::~SPythonConsole()
{
    if (GLog) GLog->RemoveOutputDevice(this); // can already be null'd at this point
}

void SPythonConsole::Construct(const FArguments& InArgs)
{
    marshaller = MakeShareable(new FTextMarshaller());
    ChildSlot
    [
       SNew(SVerticalBox)
       +SVerticalBox::Slot()
       .FillHeight(1)
       [
            SAssignNew(messagesBox, SMultiLineEditableTextBox)
            .Style(FEditorStyle::Get(), "Log.TextBox")
            .TextStyle(FEditorStyle::Get(), "Log.Normal")
            .ForegroundColor(FLinearColor::Gray)
            .Marshaller(marshaller)
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
                   .OnTextCommitted(FOnTextCommitted::CreateRaw(this, &SPythonConsole::OnTextCommitted))
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

void SPythonConsole::OnTextCommitted(const FText& text, ETextCommit::Type type)
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

void SPythonConsole::Serialize(const TCHAR* msg, ELogVerbosity::Type verbosity, const class FName& category)
{
    marshaller->AddMessage(msg, verbosity, category, messagesBox);
}

END_SLATE_FUNCTION_BUILD_OPTIMIZATION

