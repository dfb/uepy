#include "mod_uepy_umg.h"
#include "common.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/ContentWidget.h"
#include "Components/EditableTextBox.h"
#include "Components/GridPanel.h"
#include "Components/GridSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/NamedSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Widget.h"
#include "Components/WidgetSwitcher.h"
#include "Components/WidgetSwitcherSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "Paper2D/Classes/PaperSprite.h"
#include "PaperSprite.h"
#include "WebBrowser.h"

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

//#pragma optimize("", off)

TSharedRef<SWidget> UUserWidget_CGLUE::RebuildWidget()
{
    if (!GetRootWidget())
    {
        Initialize();
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

void UUserWidget_CGLUE::NativePreConstruct()
{
    Super::NativePreConstruct();
    try {
        UWidget *root = GetRootWidget();
        pyInst.attr("Construct")(root);

        // for performance, determine up front if this widget ever wants its Tick method called
        ticks = py::hasattr(pyInst, "Tick");
    } catchpy;
}

void UUserWidget_CGLUE::NativeTick(const FGeometry& geo, float dt)
{
    Super::NativeTick(geo, dt);
    try {
        // Most widgets don't implement a Tick function so we skip the overhead of calling into
        // Python unless one is defined
        if (ticks)
            pyInst.attr("Tick")(geo, dt);
    } catchpy;
}

void UUserWidget_CGLUE::BeginDestroy()
{
    try {
        if (!pyInst.is(py::none()) && pyInst.ptr())
            pyInst.attr("BeginDestroy")();
    } catchpy;
    Super::BeginDestroy();
}

// called on pre engine init
void _LoadModuleUMG(py::module& uepy)
{
    LOG("Creating Python module uepy.umg");

    py::module m = uepy.def_submodule("_umg");
    py::object glueclasses = uepy.attr("glueclasses");

    // e.g. "WidgetBlueprintGeneratedClass'/Game/Blueprints/Foo" --> UUserWidget UClass for that BP
    m.def("GetUserWidgetClassFromReference", [](std::string& refPath) { return LoadClass<UUserWidget>(NULL, FSTR(refPath)); }, py::return_value_policy::reference);

    m.def("CreateWidget_", [](UObject* owner, py::object& _widgetClass, std::string& name, py::dict kwargs) -> UWidget*
    {
        FName n = NAME_None;
        if (name.length() > 0)
            n = FSTR(name);
        UClass *widgetClass = PyObjectToUClass(_widgetClass);
        SetInternalSpawnArgs(kwargs);

        TSubclassOf<UUserWidget> userWidgetClass = widgetClass;
        if (userWidgetClass)
        {
            UWidget *wowner = Cast<UWidget>(owner);
            if (wowner)
                return UUserWidget::CreateWidgetInstance(*wowner, userWidgetClass, n);
            APlayerController *pcowner = Cast<APlayerController>(owner);
            if (pcowner)
                return UUserWidget::CreateWidgetInstance(*pcowner, userWidgetClass, n);
            UWorld *worldowner = Cast<UWorld>(owner);
            if (worldowner)
                return UUserWidget::CreateWidgetInstance(*worldowner, userWidgetClass, n);

            // TODO: there are a couple of additional owner types that are allowed here
            LERROR("Invalid widget owner %s", *owner->GetName());
            return nullptr;
        }

        return NewObject<UWidget>(owner, widgetClass, n, RF_Transactional);
    }, py::return_value_policy::reference);

    py::class_<FAnchors>(m, "FAnchors")
        .def(py::init<FAnchors>())
        .def(py::init<>())
        .def(py::init<float>())
        .def(py::init<float,float>())
        .def(py::init<float,float,float,float>())
        .def_readwrite("Minimum", &FAnchors::Minimum)
        .def_readwrite("Maximum", &FAnchors::Maximum)
        ;

    py::class_<FSlateColor>(m, "FSlateColor")
        .def(py::init<FSlateColor>())
        .def(py::init<>())
        .def(py::init<FLinearColor>())
        ;

    py::class_<FGeometry>(m, "FGeometry")
        .def(py::init<>())
        .def("GetLocalSize", [](FGeometry& self) { return self.GetLocalSize(); })
        .def("GetAbsolutePosition", [](FGeometry& self) { return self.GetAbsolutePosition(); })
        .def("GetAbsoluteSize", [](FGeometry& self) { return self.GetAbsoluteSize(); })
        ;

    UEPY_EXPOSE_CLASS(UVisual, UObject, m)
        ;

    UEPY_EXPOSE_CLASS(UWidget, UVisual, m)
        .def("GetIsEnabled", [](UWidget& self) { return self.GetIsEnabled(); })
        .def("SetIsEnabled", [](UWidget& self, bool e) { self.SetIsEnabled(e); })
        .def("SetVisibility", [](UWidget& self, int v) { self.SetVisibility((ESlateVisibility)v); })
        .def("GetDesiredSize", [](UWidget& self) { return self.GetDesiredSize(); })
        .def("SetRenderTransformAngle", [](UWidget& self, float a) { self.SetRenderTransformAngle(a); })
        .def("IsHovered", [](UWidget& self) { return self.IsHovered(); })
        .def("GetRenderOpacity", [](UWidget& self) { return self.GetRenderOpacity(); })
        .def("SetRenderOpacity", [](UWidget& self, float o) { self.SetRenderOpacity(o); })
        .def("GetParent", [](UWidget& self) { return self.GetParent(); }, py::return_value_policy::reference)
        .def("RemoveFromParent", [](UWidget& self) { self.RemoveFromParent(); })
        ;

    UEPY_EXPOSE_CLASS(UImage, UWidget, m)
        .def("SetColorAndOpacity", [](UImage& self, FLinearColor& c) { self.SetColorAndOpacity(c); })
        .def("SetOpacity", [](UImage& self, float o) { self.SetOpacity(o); })
        .def("SetBrushSize", [](UImage& self, FVector2D& size) { self.SetBrushSize(size); })
        .def("SetBrushTintColor", [](UImage& self, FSlateColor c) { self.SetBrushTintColor(c); })
        .def("SetBrush", [](UImage& self, FSlateBrush& b) { self.SetBrush(b); })
        .def("SetBrushImageSize", [](UImage& self, FVector2D& size) { self.Brush.ImageSize = size; }) // hack until we expose FSlateBrush to Python
        .def("SetBrushFromTexture", [](UImage& self, UTexture2D* t, bool matchSize) { self.SetBrushFromTexture(t, matchSize); })
        .def("SetBrushFromMaterial", [](UImage& self, UMaterialInterface* m) { self.SetBrushFromMaterial(m); })
        .def("SetBrushFromSprite", [](UImage& self, UPaperSprite* sprite, bool matchSize) { self.SetBrushFromAtlasInterface(sprite, matchSize); }) // this is a made-up method since we don't currently expose ISlateTextureAtlastInterface to Python
        .def("SetBrushResourceObject", [](UImage& self, UObject* resourceObj) { self.SetBrushResourceObject(resourceObj); })
        ;

    UEPY_EXPOSE_CLASS(UUserWidget, UWidget, m)
        .def_readonly("WidgetTree", &UUserWidget::WidgetTree, py::return_value_policy::reference)
        .def("AddToViewport", [](UUserWidget& self, int zOrder) { self.AddToViewport(zOrder); })
        .def("RemoveFromViewport", [](UUserWidget& self) { self.RemoveFromViewport(); })
        .def("SetDesiredSizeInViewport", [](UUserWidget& self, FVector2D& size) { self.SetDesiredSizeInViewport(size); })
        .def("SetPadding", [](UUserWidget& self, FMargin& padding) { self.SetPadding(padding); })
        ;

    UEPY_EXPOSE_CLASS(UUserWidget_CGLUE, UUserWidget, glueclasses)
        .def("SetRootWidgetClass", [](UUserWidget_CGLUE& self, py::object& klass)
        {
            UClass *k = PyObjectToUClass(klass);
            if (k)
                self.rootWidgetClass = k;
            else
            {
                std::string s = py::repr(klass);
                LERROR("Invalid object %s for root widget class on object %s", s.c_str(), *self.GetName());
            }
        })
        ;

    UEPY_EXPOSE_CLASS(UPanelWidget, UWidget, m)
        .def("AddChild", [](UPanelWidget& self, UWidget *child) { return self.AddChild(child); }, py::return_value_policy::reference)
        .def("GetChildrenCount", [](UPanelWidget& self) { return self.GetChildrenCount(); })
        .def("GetChildAt", [](UPanelWidget& self, int index) { return self.GetChildAt(index); }, py::return_value_policy::reference)
        .def("ClearChildren", [](UPanelWidget& self) { self.ClearChildren(); })
        .def("RemoveChildAt", [](UPanelWidget& self, int i) { return self.RemoveChildAt(i); })
        ;

    UEPY_EXPOSE_CLASS(UVerticalBox, UPanelWidget, m)
        .def("AddChild", [](UVerticalBox& self, UWidget *child) { return self.AddChild(child); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(UHorizontalBox, UPanelWidget, m)
        .def("AddChild", [](UHorizontalBox& self, UWidget *child) { return self.AddChild(child); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(UGridPanel, UPanelWidget, m)
        .def("AddChildToGrid", [](UGridPanel& self, UWidget* content, int row, int col) { return self.AddChildToGrid(content, row, col); }, py::return_value_policy::reference)
        .def("SetColumnFill", [](UGridPanel& self, int index, float coefficient) { self.SetColumnFill(index, coefficient); })
        .def("SetRowFill", [](UGridPanel& self, int index, float coefficient) { self.SetRowFill(index, coefficient); })
        ;

    UEPY_EXPOSE_CLASS(UPanelSlot, UVisual, m)
        ;

    UEPY_EXPOSE_CLASS(UBorderSlot, UPanelSlot, m)
        .def("SetPadding", [](UBorderSlot& self, FMargin& m) { self.SetPadding(m); })
        .def("SetVerticalAlignment", [](UBorderSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        .def("SetHorizontalAlignment", [](UBorderSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        ;

    UEPY_EXPOSE_CLASS(UCanvasPanelSlot, UPanelSlot, m)
        // GetLayout / SetLayout
        .def("GetPosition", [](UCanvasPanelSlot& self) { return self.GetPosition(); })
        .def("SetPosition", [](UCanvasPanelSlot& self, FVector2D& pos) { self.SetPosition(pos); })
        .def("GetSize", [](UCanvasPanelSlot& self) { return self.GetSize(); })
        .def("SetSize", [](UCanvasPanelSlot& self, FVector2D& size) { self.SetSize(size); })
        .def("GetAutoSize", [](UCanvasPanelSlot& self) { return self.GetAutoSize(); })
        .def("SetAutoSize", [](UCanvasPanelSlot& self, bool a) { self.SetAutoSize(a); })
        .def("GetAlignment", [](UCanvasPanelSlot& self) { return self.GetAlignment(); })
        .def("SetAlignment", [](UCanvasPanelSlot& self, FVector2D& a) { self.SetAlignment(a); })
        .def("GetZOrder", [](UCanvasPanelSlot& self) { return self.GetZOrder(); })
        .def("SetZOrder", [](UCanvasPanelSlot& self, int z) { self.SetZOrder(z); })
        .def("GetOffsets", [](UCanvasPanelSlot& self) { return self.GetOffsets(); })
        .def("SetOffsets", [](UCanvasPanelSlot& self, FMargin& m) { self.SetOffsets(m); })
        .def("GetAnchors", [](UCanvasPanelSlot& self) { return self.GetAnchors(); })
        .def("SetAnchors", [](UCanvasPanelSlot& self, FAnchors& a) { self.SetAnchors(a); })
        ;

    UEPY_EXPOSE_CLASS(UGridSlot, UPanelSlot, m)
        .def("SetPadding", [](UGridSlot& self, FMargin& pad) { self.SetPadding(pad); })
        .def("SetRow", [](UGridSlot& self, int row) { self.SetRow(row); })
        .def("SetRowSpan", [](UGridSlot& self, int span) { self.SetRowSpan(span); })
        .def("SetColumn", [](UGridSlot& self, int col) { self.SetColumn(col); })
        .def("SetColumnSpan", [](UGridSlot& self, int span) { self.SetColumnSpan(span); })
        .def("SetLayer", [](UGridSlot& self, int layer) { self.SetLayer(layer); })
        .def("SetNudge", [](UGridSlot& self, FVector2D& nudge) { self.SetNudge(nudge); })
        .def("SetHorizontalAlignment", [](UGridSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](UGridSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        ;

    py::class_<FSlateChildSize>(m, "FSlateChildSize")
        .def(py::init<>())
        .def_readwrite("Value", &FSlateChildSize::Value)
        ENUM_PROP(SizeRule, ESlateSizeRule::Type, FSlateChildSize)
        ;

    UEPY_EXPOSE_CLASS(UVerticalBoxSlot, UPanelSlot, m)
        .def("SetPadding", [](UVerticalBoxSlot& self, FMargin& m) { self.SetPadding(m); })
        .def("SetSize", [](UVerticalBoxSlot& self, FSlateChildSize& size) { self.SetSize(size); })
        .def("SetVerticalAlignment", [](UVerticalBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        .def("SetHorizontalAlignment", [](UVerticalBoxSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        ;

    UEPY_EXPOSE_CLASS(UHorizontalBoxSlot, UPanelSlot, m)
        .def("SetPadding", [](UHorizontalBoxSlot& self, FMargin& m) { self.SetPadding(m); })
        .def("SetSize", [](UHorizontalBoxSlot& self, FSlateChildSize& size) { self.SetSize(size); })
        .def("SetVerticalAlignment", [](UHorizontalBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        .def("SetHorizontalAlignment", [](UHorizontalBoxSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        ;

    UEPY_EXPOSE_CLASS(UWidgetSwitcherSlot, UPanelSlot, m)
        .def("SetPadding", [](UWidgetSwitcherSlot& self, FMargin& m) { self.SetPadding(m); })
        .def("SetVerticalAlignment", [](UWidgetSwitcherSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        .def("SetHorizontalAlignment", [](UWidgetSwitcherSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        ;

    UEPY_EXPOSE_CLASS(UTextBlock, UWidget, m)
        .def("SetText", [](UTextBlock& self, std::string& newText) { self.SetText(FText::FromString(FSTR(newText))); })
        .def("SetJustification", [](UTextBlock& self, int j) { self.SetJustification((ETextJustify::Type)j); })
        .def("SetFontSize", [](UTextBlock& self, int newSize)
        {
            FSlateFontInfo& sfi = self.Font;
            sfi.Size = newSize;
            self.SetFont(sfi);
        })
        ;

    UEPY_EXPOSE_CLASS(UContentWidget, UPanelWidget, m)
        .def("SetContent", [](UContentWidget& self, UWidget *obj) { return self.SetContent(obj); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(UScaleBox, UContentWidget, m)
        .def("SetStretch", [](UScaleBox& self, int type) { self.SetStretch((EStretch::Type)type); })
        .def("SetStretchDirection", [](UScaleBox& self, int dir) { self.SetStretchDirection((EStretchDirection::Type)dir); })
        .def("SetUserSpecifiedScale", [](UScaleBox& self, float scale) { self.SetUserSpecifiedScale(scale); })
        .def("SetIgnoreInheritedScale", [](UScaleBox& self, bool ignore) { self.SetIgnoreInheritedScale(ignore); })
        ;

    UEPY_EXPOSE_CLASS(UCanvasPanel, UPanelWidget, m)
        .def("AddChildToCanvas", [](UCanvasPanel& self, UWidget* c) { return self.AddChildToCanvas(c); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(UOverlay, UPanelWidget, m)
        .def("AddChildToOverlay", [](UOverlay& self, UWidget *child) { return self.AddChildToOverlay(child); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(UButton, UContentWidget, m)
        ;

    UEPY_EXPOSE_CLASS(UComboBoxString, UWidget, m)
        .def("AddOption", [](UComboBoxString& self, std::string& o) { self.AddOption(FSTR(o)); })
        .def("ClearOptions", [](UComboBoxString& self) { self.ClearOptions(); })
        .def("RefreshOptions", [](UComboBoxString& self) { self.RefreshOptions(); })
        .def("SetSelectedOption", [](UComboBoxString& self, std::string o) { self.SetSelectedOption(FSTR(o)); })
        .def("SetSelectedIndex", [](UComboBoxString& self, int i) { self.SetSelectedIndex(i); })
        .def("GetSelectedOption", [](UComboBoxString& self) { return PYSTR(self.GetSelectedOption()); })
        .def("GetSelectedIndex", [](UComboBoxString& self) { return self.GetSelectedIndex(); })
        .def("SetFontSize", [](UComboBoxString& self, int newSize)
        {
            FSlateFontInfo& sfi = self.Font;
            sfi.Size = newSize;
        })
        ;

    UEPY_EXPOSE_CLASS(UCheckBox, UContentWidget, m)
        .def("IsChecked", [](UCheckBox& self) { return self.IsChecked(); })
        .def("SetIsChecked", [](UCheckBox& self, bool b) { self.SetIsChecked(b); })
        ;

    UEPY_EXPOSE_CLASS(UEditableTextBox, UWidget, m)
        ;

    UEPY_EXPOSE_CLASS(UWrapBoxSlot, UPanelSlot, m)
        .def("SetPadding", [](UWrapBoxSlot& self, FMargin& padding) { self.SetPadding(padding); })
        .def("SetFillEmptySpace", [](UWrapBoxSlot& self, bool b) { self.SetFillEmptySpace(b); })
        .def("SetFillSpanWhenLessThan", [](UWrapBoxSlot& self, float f) { self.SetFillSpanWhenLessThan(f); })
        .def("SetHorizontalAlignment", [](UWrapBoxSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](UWrapBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        ;

    UEPY_EXPOSE_CLASS(UScaleBoxSlot, UPanelSlot, m)
        .def("SetPadding", [](UScaleBoxSlot& self, FMargin& padding) { self.SetPadding(padding); })
        .def("SetHorizontalAlignment", [](UScaleBoxSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](UScaleBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        ;

    UEPY_EXPOSE_CLASS(UOverlaySlot, UPanelSlot, m)
        .def("SetPadding", [](UOverlaySlot& self, FMargin& padding) { self.SetPadding(padding); })
        .def("SetHorizontalAlignment", [](UOverlaySlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](UOverlaySlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        ;

    UEPY_EXPOSE_CLASS(UWrapBox, UPanelWidget, m)
        .def("SetInnerSlotPadding", [](UWrapBox& self, FVector2D& v) { self.SetInnerSlotPadding(v); })
        .def_readwrite("WrapSize", &UWrapBox::WrapSize)
        ;

    UEPY_EXPOSE_CLASS(UWidgetSwitcher, UPanelWidget, m)
        .def("GetNumWidgets", [](UWidgetSwitcher& self) { return self.GetNumWidgets(); })
        .def("GetActiveWidgetIndex", [](UWidgetSwitcher& self) { return self.GetActiveWidgetIndex(); })
        .def("SetActiveWidgetIndex", [](UWidgetSwitcher& self, int i) { self.SetActiveWidgetIndex(i); })
        .def("GetWidgetAtIndex", [](UWidgetSwitcher& self, int i) { return self.GetWidgetAtIndex(i); }, py::return_value_policy::reference)
        ;

    UEPY_EXPOSE_CLASS(USizeBoxSlot, UPanelSlot, m)
        .def("SetPadding", [](USizeBoxSlot& self, FMargin& padding) { self.SetPadding(padding); })
        .def("SetHorizontalAlignment", [](USizeBoxSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](USizeBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        ;

    UEPY_EXPOSE_CLASS(USizeBox, UContentWidget, m)
        .def("SetWidthOverride", [](USizeBox& self, float n) { self.SetWidthOverride(n); })
        .def("SetHeightOverride", [](USizeBox& self, float n) { self.SetHeightOverride(n); })
        .def("SetMinDesiredWidth", [](USizeBox& self, float n) { self.SetMinDesiredWidth(n); })
        .def("SetMinDesiredHeight", [](USizeBox& self, float n) { self.SetMinDesiredHeight(n); })
        .def("SetMaxDesiredWidth", [](USizeBox& self, float n) { self.SetMaxDesiredWidth(n); })
        .def("SetMaxDesiredHeight", [](USizeBox& self, float n) { self.SetMaxDesiredHeight(n); })
        .def("SetMinAspectRatio", [](USizeBox& self, float n) { self.SetMinAspectRatio(n); })
        .def("SetMaxAspectRatio", [](USizeBox& self, float n) { self.SetMaxAspectRatio(n); })
        .def("ClearWidthOverride", [](USizeBox& self) { self.ClearWidthOverride(); })
        .def("ClearHeightOverride", [](USizeBox& self) { self.ClearHeightOverride(); })
        .def("ClearMinDesiredWidth", [](USizeBox& self) { self.ClearMinDesiredWidth(); })
        .def("ClearMinDesiredHeight", [](USizeBox& self) { self.ClearMinDesiredHeight(); })
        .def("ClearMaxDesiredWidth", [](USizeBox& self) { self.ClearMaxDesiredWidth(); })
        .def("ClearMaxDesiredHeight", [](USizeBox& self) { self.ClearMaxDesiredHeight(); })
        .def("ClearMinAspectRatio", [](USizeBox& self) { self.ClearMinAspectRatio(); })
        .def("ClearMaxAspectRatio", [](USizeBox& self) { self.ClearMaxAspectRatio(); })
        ;

    UEPY_EXPOSE_CLASS(USpacer, UWidget, m)
        .def("SetSize", [](USpacer& self, FVector2D& size) { self.SetSize(size); })
        ;

    UEPY_EXPOSE_CLASS(UBorder, UContentWidget, m)
        .def("SetPadding", [](UBorder& self, FMargin& pad) { self.SetPadding(pad); })
        .def("SetHorizontalAlignment", [](UBorder& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](UBorder& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        .def("SetBrushColor", [](UBorder& self, FLinearColor& c) { self.SetBrushColor(c); })
        //.def("SetBrush", [](UBorder& self, (const FSlateBrush& InBrush);
        ;

    UEPY_EXPOSE_CLASS(UWidgetLayoutLibrary, UObject, m)
        .def_static("RemoveAllWidgets", [](UObject *worldCtx) { UWidgetLayoutLibrary::RemoveAllWidgets(worldCtx); })
        .def_static("GetViewportSize", [](UObject *worldCtx) { return UWidgetLayoutLibrary::GetViewportSize(worldCtx); })
        .def_static("SlotAsCanvasSlot", [](UWidget *widget) { return UWidgetLayoutLibrary::SlotAsCanvasSlot(widget); }, py::return_value_policy::reference)
        .def_static("ProjectWorldLocationToWidgetPosition", [](APlayerController* pc, FVector worldLoc, bool viewportRelative)
        {
            FVector2D screenPos;
            bool hitScreen = UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(pc, worldLoc, screenPos, viewportRelative);
            return py::make_tuple(hitScreen, screenPos);
        })
        ;

    UEPY_EXPOSE_CLASS(UNamedSlot, UContentWidget, m)
        ;

    UEPY_EXPOSE_CLASS(UWidgetTree, UObject, m)
        .def("FindWidget", [](UWidgetTree& self, std::string& name) { return self.FindWidget(FSTR(name)); }, py::return_value_policy::reference)
        .def_property("RootWidget", [](UWidgetTree& self) { return self.RootWidget; }, [](UWidgetTree& self, UWidget* w) { self.RootWidget = w; }, py::return_value_policy::reference)
        .def("RemoveWidget", [](UWidgetTree& self, UWidget* w) { return self.RemoveWidget(w); })
        ;

    UEPY_EXPOSE_CLASS(UProgressBar, UWidget, m)
        .def("SetPercent", [](UProgressBar& self, float p) { self.SetPercent(p); })
        ;

    UEPY_EXPOSE_CLASS(UWebBrowser, UWidget, m)
        .def("LoadURL", [](UWebBrowser& self, std::string& url) { self.LoadURL(FSTR(url)); })
        .def("LoadString", [](UWebBrowser& self, std::string& contents, std::string& dummyURL) { self.LoadString(FSTR(contents), FSTR(dummyURL)); })
        .def("ExecuteJavascript", [](UWebBrowser& self, std::string& js) { self.ExecuteJavascript(FSTR(js)); })
        .def("GetTitleText", [](UWebBrowser& self) { return self.GetTitleText(); })
        .def("GetUrl", [](UWebBrowser& self) { return self.GetUrl(); })
        .def("GetURL", [](UWebBrowser& self) { return self.GetUrl(); })
        ;
}

//#pragma optimize("", on)
