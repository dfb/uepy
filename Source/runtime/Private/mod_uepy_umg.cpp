#include "mod_uepy_umg.h"
#include "common.h"
#include "Components/Button.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ComboBoxString.h"
#include "Components/ContentWidget.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBoxSlot.h"

/*
GRRR: should we subclass slate? subclass UWidget? UUserWidget? UUserWidget is commonly suggested, but it's geared towards
cases where you're trying to make something that is subclassable in BP. BUT, users have to explicitly reparent their BP
to the UUserWidget parent class - the widgets won't show up in the designer palette in the UI.

Subclassing UWidget (or a specific one, like UVerticalBox) and then adding child UWidgets didn't pan out - I may have been doing
something wrong, but it resulted in some sort of weird sharing across instances that we didn't want - I think that when you subclass
UWidget, the expectation is that internally you'll use Slate.

Subclassing UWidget and internally using Slate worked great in C++, but then translating it to Python gets iffy because Slate stuff isn't
UObject-based, and is instead all TSharedRef/TSharedPtr, which means we'd need to extend the object tracker to work with UE4's smart
pointers since UE4 doesn't use std::shared_ptr. Maybe at some point we should do that, but for now, it's easiest to just stick to
UObject-based stuff, which means subclassing UUserWidget I guess!


*/

TSharedRef<SWidget> UPyUserWidget::RebuildWidget()
{
    if (!GetRootWidget())
    {
        UWidget *root = WidgetTree->ConstructWidget<UWidget>(rootWidgetClass, TEXT("RootWidget"));

        // try to configure it some if it has a canvas panel slot
        UCanvasPanelSlot *rootSlot = Cast<UCanvasPanelSlot>(root->Slot);
        if (rootSlot)
        {
            rootSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
            rootSlot->SetOffsets(FMargin(0.0f, 0.0f));
        }
        WidgetTree->RootWidget = root;
    }

    return Super::RebuildWidget();
}

void UPyUserWidget::NativePreConstruct()
{
    Super::NativePreConstruct();
    try {
        pyInst.attr("Construct")(GetRootWidget());
    } catchpy;
}

// called on pre engine init
void _LoadModuleUMG(py::module& uepy)
{
    LOG("Creating Python module uepy.umg");

    py::module m = uepy.def_submodule("_umg");

    // e.g. "WidgetBlueprintGeneratedClass'/Game/Blueprints/Foo" --> UUserWidget UClass for that BP
    m.def("GetUserWidgetClassFromReference", [](std::string refPath) { return LoadClass<UUserWidget>(NULL, UTF8_TO_TCHAR(refPath.c_str())); });

    m.def("CreateWidget", [](UObject* owner, UClass *widgetClass, std::string name) { return NewObject<UWidget>(owner, widgetClass, name.c_str(), RF_Transactional); }, py::return_value_policy::reference);

    py::class_<UVisual, UObject, UnrealTracker<UVisual>>(m, "UVisual");

    py::class_<UWidget, UVisual, UnrealTracker<UWidget>>(m, "UWidget")
        .def_static("StaticClass", []() { return UWidget::StaticClass(); });
        ;

    py::class_<UUserWidget, UWidget, UnrealTracker<UUserWidget>>(m, "UUserWiget");
    py::class_<UPyUserWidget, UUserWidget, UnrealTracker<UPyUserWidget>>(m, "UPyUserWidget")
        .def_static("StaticClass", []() { return UPyUserWidget::StaticClass(); });
        ;

    m.def("AsUPyUserWidget", [](UObject *engineObj) -> UPyUserWidget* { return Cast<UPyUserWidget>(engineObj); }, py::return_value_policy::reference);

    py::class_<UPanelWidget, UWidget, UnrealTracker<UPanelWidget>>(m, "UPanelWidget")
        .def_static("StaticClass", []() { return UPanelWidget::StaticClass(); });
        ;

    py::class_<UVerticalBox, UPanelWidget, UnrealTracker<UVerticalBox>>(m, "UVerticalBox")
        .def_static("StaticClass", []() { return UVerticalBox::StaticClass(); })
        .def_static("Cast", [](UWidget *w) { return Cast<UVerticalBox>(w); })
        .def("AddChild", [](UVerticalBox& self, UWidget *child) { return self.AddChild(child); })
        ;

    py::class_<UHorizontalBox, UPanelWidget, UnrealTracker<UHorizontalBox>>(m, "UHorizontalBox")
        .def_static("StaticClass", []() { return UHorizontalBox::StaticClass(); })
        .def_static("Cast", [](UWidget *w) { return Cast<UHorizontalBox>(w); })
        .def("AddChild", [](UHorizontalBox& self, UWidget *child) { return self.AddChild(child); })
        ;

    py::class_<UPanelSlot, UVisual, UnrealTracker<UPanelSlot>>(m, "UPanelSlot");

    py::class_<UVerticalBoxSlot, UPanelSlot, UnrealTracker<UVerticalBoxSlot>>(m, "UVerticalBoxSlot")
        .def_static("StaticClass", []() { return UVerticalBoxSlot::StaticClass(); })
        .def_static("Cast", [](UPanelSlot *slot) { return Cast<UVerticalBoxSlot>(slot); })
        .def("SetPadding", [](UVerticalBoxSlot& self, FMargin& m) { self.SetPadding(m); })
        ;

    py::class_<UHorizontalBoxSlot, UPanelSlot, UnrealTracker<UHorizontalBoxSlot>>(m, "UHorizontalBoxSlot")
        .def_static("StaticClass", []() { return UHorizontalBoxSlot::StaticClass(); })
        .def_static("Cast", [](UPanelSlot *slot) { return Cast<UHorizontalBoxSlot>(slot); })
        .def("SetPadding", [](UHorizontalBoxSlot& self, FMargin& m) { self.SetPadding(m); })
        .def("SetVerticalAlignment", [](UHorizontalBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        ;

    py::class_<UTextBlock, UWidget, UnrealTracker<UTextBlock>>(m, "UTextBlock") // note: there is an unexposed intermediate type in between this and UWidget
        .def_static("StaticClass", []() { return UTextBlock::StaticClass(); })
        .def("SetText", [](UTextBlock& self, std::string newText) { self.SetText(FText::FromString(newText.c_str())); })
        .def_static("Cast", [](UWidget *obj) { return Cast<UTextBlock>(obj); })
        ;

    py::class_<UContentWidget, UPanelWidget, UnrealTracker<UContentWidget>>(m, "UContentWidget")
        .def("SetContent", [](UContentWidget& self, UWidget *obj) { return self.SetContent(obj); });

    py::class_<UButton, UContentWidget, UnrealTracker<UButton>>(m, "UButton")
        .def_static("StaticClass", []() { return UButton::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UButton>(obj); })
        .def("BindOnClicked", [](UButton& self, py::object callback)
        {
            UBasePythonDelegate* delegate = NewObject<UBasePythonDelegate>();
            delegate->callback = callback;
            self.OnClicked.AddDynamic(delegate, &UBasePythonDelegate::On);
            return (UObject*)delegate; // TODO: this is a temp hack so the caller can keep it alive by saving a ref to it :-/
        });

    py::class_<UComboBoxString, UWidget, UnrealTracker<UComboBoxString>>(m, "UComboBoxString")
        .def_static("StaticClass", []() { return UComboBoxString::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UComboBoxString>(obj); })
        .def("AddOption", [](UComboBoxString& self, std::string o) { self.AddOption(o.c_str()); })
        .def("ClearOptions", [](UComboBoxString& self) { self.ClearOptions(); })
        .def("RefreshOptions", [](UComboBoxString& self) { self.RefreshOptions(); })
        .def("SetSelectedOption", [](UComboBoxString& self, std::string o) { self.SetSelectedOption(o.c_str()); })
        .def("SetSelectedIndex", [](UComboBoxString& self, int i) { self.SetSelectedIndex(i); })
        .def("GetSelectedOption", [](UComboBoxString& self) { return *self.GetSelectedOption(); })
        .def("GetSelectedIndex", [](UComboBoxString& self) { return self.GetSelectedIndex(); })
        .def("BindOnSelectionChanged", [](UComboBoxString& self, py::object callback)
        {
            UBasePythonDelegate* delegate = NewObject<UBasePythonDelegate>();
            delegate->callback = callback;
            self.OnSelectionChanged.AddDynamic(delegate, &UBasePythonDelegate::OnUComboBoxString_HandleSelectionChanged);
            return (UObject*)delegate; // TODO: this is a temp hack so the caller can keep it alive by saving a ref to it :-/
        })
        ;

    py::class_<UCheckBox, UContentWidget, UnrealTracker<UCheckBox>>(m, "UCheckBox")
        .def_static("StaticClass", []() { return UCheckBox::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UCheckBox>(obj); })
        .def("IsChecked", [](UCheckBox& self) { return self.IsChecked(); })
        .def("SetIsChecked", [](UCheckBox& self, bool b) { self.SetIsChecked(b); })
        .def("BindOnCheckStateChanged", [](UCheckBox& self, py::object callback)
        {
            UBasePythonDelegate* delegate = NewObject<UBasePythonDelegate>();
            delegate->callback = callback;
            self.OnCheckStateChanged.AddDynamic(delegate, &UBasePythonDelegate::OnUCheckBox_OnCheckStateChanged);
            return (UObject*)delegate; // TODO: this is a temp hack so the caller can keep it alive by saving a ref to it :-/
        })
        ;

    py::class_<UEditableTextBox, UWidget, UnrealTracker<UEditableTextBox>>(m, "UEditableTextBox")
        .def_static("StaticClass", []() { return UEditableTextBox::StaticClass(); })
        .def_static("Cast", [](UObject *obj) { return Cast<UEditableTextBox>(obj); })
        ;
}

