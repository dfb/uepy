// creates the uepy.umg builtin module

#pragma once
#include "uepy.h"
#include "Components/VerticalBox.h"
#include "Blueprint/WidgetTree.h"
#include "mod_uepy_umg.generated.h"

// base UMG widget from which others can derive
UCLASS()
class UEPY_API UUserWidget_CGLUE : public UUserWidget, public IUEPYGlueMixin
{
    GENERATED_BODY()
    
public:
    TSubclassOf<UWidget> rootWidgetClass = UVerticalBox::StaticClass();
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativePreConstruct() override;

    // Hack: UUserWidget::Initialize is convinced that py-based configurators implement INamedSlotInterface, and the default
    // implementation of GetSlotNames accesses WidgetTree while it is still NULL. This prevents that.
    // Also, the default impl of GetSlotNames is a NOP for non-BP widgets until UE4.24 it looks like (it omitted a 'SlotNames.Append(NamedSlots);'
    // line at the end until Sept 15, 2019), so maybe this isn't too bad.
    virtual void GetSlotNames(TArray<FName>& SlotNames) const override {};

};

void _LoadModuleUMG(py::module& uepy);


