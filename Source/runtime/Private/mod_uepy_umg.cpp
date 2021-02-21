#include "mod_uepy_umg.h"
#include "common.h"

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

// called on pre engine init
void _LoadModuleUMG(py::module& uepy)
{
    LOG("Creating Python module uepy.umg");

    py::module m = uepy.def_submodule("_umg");
    py::object glueclasses = uepy.attr("glueclasses");

    // e.g. "WidgetBlueprintGeneratedClass'/Game/Blueprints/Foo" --> UUserWidget UClass for that BP
    m.def("GetUserWidgetClassFromReference", [](std::string refPath) { return LoadClass<UUserWidget>(NULL, UTF8_TO_TCHAR(refPath.c_str())); }, py::return_value_policy::reference);

    m.def("CreateWidget", [](UObject* owner, py::object& _widgetClass, py::object name) -> UWidget*
    {
        FName n = NAME_None;
        if (!name.is_none())
            n = name.cast<std::string>().c_str();
        UClass *widgetClass = PyObjectToUClass(_widgetClass);

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
    }, py::return_value_policy::reference, py::arg("owner"), py::arg("_widgetClass"), py::arg("name")=py::none());

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

    py::class_<UVisual, UObject, UnrealTracker<UVisual>>(m, "UVisual")
        .def_static("StaticClass", []() { return UVisual::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UVisual>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UWidget, UVisual, UnrealTracker<UWidget>>(m, "UWidget")
        .def_static("StaticClass", []() { return UWidget::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UWidget>(obj); }, py::return_value_policy::reference)
        .def("SetIsEnabled", [](UWidget& self, bool e) { self.SetIsEnabled(e); })
        .def("SetVisibility", [](UWidget& self, int v) { self.SetVisibility((ESlateVisibility)v); })
        .def("GetDesiredSize", [](UWidget& self) { return self.GetDesiredSize(); })
        .def("SetRenderTransformAngle", [](UWidget& self, float a) { self.SetRenderTransformAngle(a); })
        .def("IsHovered", [](UWidget& self) { return self.IsHovered(); })
        .def("GetRenderOpacity", [](UWidget& self) { return self.GetRenderOpacity(); })
        .def("SetRenderOpacity", [](UWidget& self, float o) { self.SetRenderOpacity(o); })
        ;

    py::class_<UImage, UWidget, UnrealTracker<UImage>>(m, "UImage")
        .def_static("StaticClass", []() { return UImage::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UImage>(obj); }, py::return_value_policy::reference)
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

    py::class_<UUserWidget, UWidget, UnrealTracker<UUserWidget>>(m, "UUserWidget")
        .def_static("StaticClass", []() { return UUserWidget::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UUserWidget>(obj); }, py::return_value_policy::reference)
        .def("AddToViewport", [](UUserWidget& self, int zOrder) { self.AddToViewport(zOrder); })
        .def("SetDesiredSizeInViewport", [](UUserWidget& self, FVector2D& size) { self.SetDesiredSizeInViewport(size); })
        ;

    py::class_<UUserWidget_CGLUE, UUserWidget, UnrealTracker<UUserWidget_CGLUE>>(glueclasses, "UUserWidget_CGLUE")
        .def_static("StaticClass", []() { return UUserWidget_CGLUE::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UUserWidget_CGLUE>(w); }, py::return_value_policy::reference)
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

    py::class_<UPanelWidget, UWidget, UnrealTracker<UPanelWidget>>(m, "UPanelWidget")
        .def_static("StaticClass", []() { return UPanelWidget::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UPanelWidget>(obj); }, py::return_value_policy::reference)
        .def("AddChild", [](UPanelWidget& self, UWidget *child) { return self.AddChild(child); }, py::return_value_policy::reference)
        ;

    py::class_<UVerticalBox, UPanelWidget, UnrealTracker<UVerticalBox>>(m, "UVerticalBox")
        .def_static("StaticClass", []() { return UVerticalBox::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UVerticalBox>(w); }, py::return_value_policy::reference)
        .def("AddChild", [](UVerticalBox& self, UWidget *child) { return self.AddChild(child); }, py::return_value_policy::reference)
        ;

    py::class_<UHorizontalBox, UPanelWidget, UnrealTracker<UHorizontalBox>>(m, "UHorizontalBox")
        .def_static("StaticClass", []() { return UHorizontalBox::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UHorizontalBox>(w); }, py::return_value_policy::reference)
        .def("AddChild", [](UHorizontalBox& self, UWidget *child) { return self.AddChild(child); }, py::return_value_policy::reference)
        ;

    py::class_<UGridPanel, UPanelWidget, UnrealTracker<UGridPanel>>(m, "UGridPanel")
        .def_static("StaticClass", []() { return UGridPanel::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *w) { return Cast<UGridPanel>(w); }, py::return_value_policy::reference)
        .def("AddChildToGrid", [](UGridPanel& self, UWidget* content, int row, int col) { return self.AddChildToGrid(content, row, col); }, py::return_value_policy::reference)
        .def("SetColumnFill", [](UGridPanel& self, int index, float coefficient) { self.SetColumnFill(index, coefficient); })
        .def("SetRowFill", [](UGridPanel& self, int index, float coefficient) { self.SetRowFill(index, coefficient); })
        ;

    py::class_<UPanelSlot, UVisual, UnrealTracker<UPanelSlot>>(m, "UPanelSlot")
        .def_static("StaticClass", []() { return UPanelSlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UPanelSlot>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UBorderSlot, UPanelSlot, UnrealTracker<UBorderSlot>>(m, "UBorderSlot")
        .def_static("StaticClass", []() { return UBorderSlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UBorderSlot>(obj); }, py::return_value_policy::reference)
        .def("SetPadding", [](UBorderSlot& self, FMargin& m) { self.SetPadding(m); })
        .def("SetVerticalAlignment", [](UBorderSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        .def("SetHorizontalAlignment", [](UBorderSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        ;

    py::class_<UCanvasPanelSlot, UPanelSlot, UnrealTracker<UCanvasPanelSlot>>(m, "UCanvasPanelSlot")
        .def_static("StaticClass", []() { return UCanvasPanelSlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UCanvasPanelSlot>(obj); }, py::return_value_policy::reference)
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

    py::class_<UGridSlot, UPanelSlot, UnrealTracker<UGridSlot>>(m, "UGridSlot")
        .def_static("StaticClass", []() { return UGridSlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UGridSlot>(obj); }, py::return_value_policy::reference)
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

    py::class_<UVerticalBoxSlot, UPanelSlot, UnrealTracker<UVerticalBoxSlot>>(m, "UVerticalBoxSlot")
        .def_static("StaticClass", []() { return UVerticalBoxSlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *slot) { return Cast<UVerticalBoxSlot>(slot); }, py::return_value_policy::reference)
        .def("SetPadding", [](UVerticalBoxSlot& self, FMargin& m) { self.SetPadding(m); })
        .def("SetSize", [](UVerticalBoxSlot& self, FSlateChildSize& size) { self.SetSize(size); })
        .def("SetVerticalAlignment", [](UVerticalBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        .def("SetHorizontalAlignment", [](UVerticalBoxSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        ;

    py::class_<UHorizontalBoxSlot, UPanelSlot, UnrealTracker<UHorizontalBoxSlot>>(m, "UHorizontalBoxSlot")
        .def_static("StaticClass", []() { return UHorizontalBoxSlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *slot) { return Cast<UHorizontalBoxSlot>(slot); }, py::return_value_policy::reference)
        .def("SetPadding", [](UHorizontalBoxSlot& self, FMargin& m) { self.SetPadding(m); })
        .def("SetSize", [](UHorizontalBoxSlot& self, FSlateChildSize& size) { self.SetSize(size); })
        .def("SetVerticalAlignment", [](UHorizontalBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        .def("SetHorizontalAlignment", [](UHorizontalBoxSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        ;

    py::class_<UTextBlock, UWidget, UnrealTracker<UTextBlock>>(m, "UTextBlock") // note: there is an unexposed intermediate type in between this and UWidget
        .def_static("StaticClass", []() { return UTextBlock::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UTextBlock>(obj); }, py::return_value_policy::reference)
        .def("SetText", [](UTextBlock& self, std::string newText) { self.SetText(FText::FromString(newText.c_str())); })
        .def("SetFontSize", [](UTextBlock& self, int newSize)
        {
            FSlateFontInfo& sfi = self.Font;
            sfi.Size = newSize;
            self.SetFont(sfi);
        })
        ;

    py::class_<UContentWidget, UPanelWidget, UnrealTracker<UContentWidget>>(m, "UContentWidget")
        .def_static("StaticClass", []() { return UContentWidget::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *slot) { return Cast<UContentWidget>(slot); }, py::return_value_policy::reference)
        .def("SetContent", [](UContentWidget& self, UWidget *obj) { return self.SetContent(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UScaleBox, UContentWidget, UnrealTracker<UScaleBox>>(m, "UScaleBox")
        .def_static("StaticClass", []() { return UScaleBox::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *slot) { return Cast<UScaleBox>(slot); }, py::return_value_policy::reference)
        .def("SetStretch", [](UScaleBox& self, int type) { self.SetStretch((EStretch::Type)type); })
        .def("SetStretchDirection", [](UScaleBox& self, int dir) { self.SetStretchDirection((EStretchDirection::Type)dir); })
        .def("SetUserSpecifiedScale", [](UScaleBox& self, float scale) { self.SetUserSpecifiedScale(scale); })
        .def("SetIgnoreInheritedScale", [](UScaleBox& self, bool ignore) { self.SetIgnoreInheritedScale(ignore); })
        ;

    py::class_<UCanvasPanel, UPanelWidget, UnrealTracker<UCanvasPanel>>(m, "UCanvasPanel")
        .def_static("StaticClass", []() { return UCanvasPanel::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *slot) { return Cast<UCanvasPanel>(slot); }, py::return_value_policy::reference)
        ;

    py::class_<UOverlay, UPanelWidget, UnrealTracker<UOverlay>>(m, "UOverlay")
        .def_static("StaticClass", []() { return UOverlay::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *slot) { return Cast<UOverlay>(slot); }, py::return_value_policy::reference)
        .def("AddChildToOverlay", [](UOverlay& self, UWidget *child) { return self.AddChildToOverlay(child); }, py::return_value_policy::reference)
        ;

    py::class_<UButton, UContentWidget, UnrealTracker<UButton>>(m, "UButton")
        .def_static("StaticClass", []() { return UButton::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UButton>(obj); }, py::return_value_policy::reference)
        // TODO: event-specific Bind APIs might no longer be needed and might go away
        .def("BindOnClicked", [](UButton* self, py::object callback) { UEPY_BIND(self, OnClicked, On, callback); })
        .def("UnbindOnClicked", [](UButton* self, py::object callback) { UEPY_UNBIND(self, OnClicked, On, callback); })
        ;

    py::class_<UComboBoxString, UWidget, UnrealTracker<UComboBoxString>>(m, "UComboBoxString")
        .def_static("StaticClass", []() { return UComboBoxString::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UComboBoxString>(obj); }, py::return_value_policy::reference)
        .def("AddOption", [](UComboBoxString& self, std::string o) { self.AddOption(o.c_str()); })
        .def("ClearOptions", [](UComboBoxString& self) { self.ClearOptions(); })
        .def("RefreshOptions", [](UComboBoxString& self) { self.RefreshOptions(); })
        .def("SetSelectedOption", [](UComboBoxString& self, std::string o) { self.SetSelectedOption(o.c_str()); })
        .def("SetSelectedIndex", [](UComboBoxString& self, int i) { self.SetSelectedIndex(i); })
        .def("GetSelectedOption", [](UComboBoxString& self) { return PYSTR(self.GetSelectedOption()); })
        .def("GetSelectedIndex", [](UComboBoxString& self) { return self.GetSelectedIndex(); })
        .def("SetFontSize", [](UComboBoxString& self, int newSize)
        {
            FSlateFontInfo& sfi = self.Font;
            sfi.Size = newSize;
        })
        // TODO: event-specific Bind APIs might no longer be needed and might go away
        .def("BindOnSelectionChanged", [](UComboBoxString* self, py::object callback) { UEPY_BIND(self, OnSelectionChanged, UComboBoxString_OnHandleSelectionChanged, callback); })
        .def("UnbindOnSelectionChanged", [](UComboBoxString* self, py::object callback) { UEPY_UNBIND(self, OnSelectionChanged, UComboBoxString_OnHandleSelectionChanged, callback); })
        ;

    py::class_<UCheckBox, UContentWidget, UnrealTracker<UCheckBox>>(m, "UCheckBox")
        .def_static("StaticClass", []() { return UCheckBox::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UCheckBox>(obj); }, py::return_value_policy::reference)
        .def("IsChecked", [](UCheckBox& self) { return self.IsChecked(); })
        .def("SetIsChecked", [](UCheckBox& self, bool b) { self.SetIsChecked(b); })
        // TODO: event-specific Bind APIs might no longer be needed and might go away
        .def("BindOnCheckStateChanged", [](UCheckBox* self, py::object callback) { UEPY_BIND(self, OnCheckStateChanged, UCheckBox_OnCheckStateChanged, callback); })
        .def("UnbindOnCheckStateChanged", [](UCheckBox* self, py::object callback) { UEPY_UNBIND(self, OnCheckStateChanged, UCheckBox_OnCheckStateChanged, callback); })
        ;

    py::class_<UEditableTextBox, UWidget, UnrealTracker<UEditableTextBox>>(m, "UEditableTextBox")
        .def_static("StaticClass", []() { return UEditableTextBox::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UEditableTextBox>(obj); }, py::return_value_policy::reference)
        ;

    py::class_<UWrapBoxSlot, UPanelSlot, UnrealTracker<UWrapBoxSlot>>(m, "UWrapBoxSlot")
        .def_static("StaticClass", []() { return UWrapBoxSlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UWrapBoxSlot>(obj); }, py::return_value_policy::reference)
        .def("SetPadding", [](UWrapBoxSlot& self, FMargin& padding) { self.SetPadding(padding); })
        .def("SetFillEmptySpace", [](UWrapBoxSlot& self, bool b) { self.SetFillEmptySpace(b); })
        .def("SetFillSpanWhenLessThan", [](UWrapBoxSlot& self, float f) { self.SetFillSpanWhenLessThan(f); })
        .def("SetHorizontalAlignment", [](UWrapBoxSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](UWrapBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        ;

    py::class_<UScaleBoxSlot, UPanelSlot, UnrealTracker<UScaleBoxSlot>>(m, "UScaleBoxSlot")
        .def_static("StaticClass", []() { return UScaleBoxSlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UScaleBoxSlot>(obj); }, py::return_value_policy::reference)
        .def("SetPadding", [](UScaleBoxSlot& self, FMargin& padding) { self.SetPadding(padding); })
        .def("SetHorizontalAlignment", [](UScaleBoxSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](UScaleBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        ;

    py::class_<UOverlaySlot, UPanelSlot, UnrealTracker<UOverlaySlot>>(m, "UOverlaySlot")
        .def_static("StaticClass", []() { return UOverlaySlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UOverlaySlot>(obj); }, py::return_value_policy::reference)
        .def("SetPadding", [](UOverlaySlot& self, FMargin& padding) { self.SetPadding(padding); })
        .def("SetHorizontalAlignment", [](UOverlaySlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](UOverlaySlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        ;

    py::class_<UWrapBox, UPanelWidget, UnrealTracker<UWrapBox>>(m, "UWrapBox")
        .def_static("StaticClass", []() { return UWrapBox::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UWrapBox>(obj); }, py::return_value_policy::reference)
        .def("SetInnerSlotPadding", [](UWrapBox& self, FVector2D& v) { self.SetInnerSlotPadding(v); })
        .def_readwrite("WrapWidth", &UWrapBox::WrapWidth)
        ;

    py::class_<USizeBoxSlot, UPanelSlot, UnrealTracker<USizeBoxSlot>>(m, "USizeBoxSlot")
        .def_static("StaticClass", []() { return USizeBoxSlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<USizeBoxSlot>(obj); }, py::return_value_policy::reference)
        .def("SetPadding", [](USizeBoxSlot& self, FMargin& padding) { self.SetPadding(padding); })
        .def("SetHorizontalAlignment", [](USizeBoxSlot& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](USizeBoxSlot& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        ;

    py::class_<USizeBox, UContentWidget, UnrealTracker<USizeBox>>(m, "USizeBox")
        .def_static("StaticClass", []() { return USizeBox::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<USizeBox>(obj); }, py::return_value_policy::reference)
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

    py::class_<USpacer, UWidget, UnrealTracker<USpacer>>(m, "USpacer")
        .def_static("StaticClass", []() { return USpacer::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<USpacer>(obj); }, py::return_value_policy::reference)
        .def("SetSize", [](USpacer& self, FVector2D& size) { self.SetSize(size); })
        ;

    py::class_<UBorder, UContentWidget, UnrealTracker<UBorder>>(m, "UBorder")
        .def_static("StaticClass", []() { return UBorder::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UBorder>(obj); }, py::return_value_policy::reference)
        .def("SetPadding", [](UBorder& self, FMargin& pad) { self.SetPadding(pad); })
        .def("SetHorizontalAlignment", [](UBorder& self, int a) { self.SetHorizontalAlignment((EHorizontalAlignment)a); })
        .def("SetVerticalAlignment", [](UBorder& self, int a) { self.SetVerticalAlignment((EVerticalAlignment)a); })
        .def("SetBrushColor", [](UBorder& self, FLinearColor& c) { self.SetBrushColor(c); })
        //.def("SetBrush", [](UBorder& self, (const FSlateBrush& InBrush);
        ;

    py::class_<UWidgetLayoutLibrary, UObject, UnrealTracker<UWidgetLayoutLibrary>>(m, "UWidgetLayoutLibrary")
        .def_static("RemoveAllWidgets", [](UObject *worldCtx) { UWidgetLayoutLibrary::RemoveAllWidgets(worldCtx); })
        .def_static("GetViewportSize", [](UObject *worldCtx) { return UWidgetLayoutLibrary::GetViewportSize(worldCtx); })
        ;

    py::class_<UNamedSlot, UContentWidget, UnrealTracker<UNamedSlot>>(m, "UNamedSlot")
        .def_static("StaticClass", []() { return UNamedSlot::StaticClass(); }, py::return_value_policy::reference)
        .def_static("Cast", [](UObject *obj) { return Cast<UNamedSlot>(obj); }, py::return_value_policy::reference)
        ;
}

//#pragma optimize("", on)
