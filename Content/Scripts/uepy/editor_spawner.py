# Creates a tab/window in the editor for spawning Python-based actors
import uepy, time
from uepy import umg, editor
from uepy import log, logTB

class SpawnerTab(uepy.UUserWidget_PGLUE):
    def __init__(self):
        self.num = int(time.time())

    def Construct(self, vboxRoot):
        log('editor_spawner.SpawnerTab.Construct:', vboxRoot)
        vboxRoot = umg.UVerticalBox.Cast(vboxRoot)

        margin = uepy.FMargin(5,5,5,5)

        # Row: combo box of class names + refresh button
        hb = umg.UHorizontalBox.Cast(umg.CreateWidget(vboxRoot, umg.UHorizontalBox, 'hb'))
        slot = umg.UVerticalBoxSlot.Cast(vboxRoot.AddChild(hb))
        slot.SetPadding(margin)

        self.comboBox = umg.UComboBoxString.Cast(umg.CreateWidget(hb, umg.UComboBoxString, 'comboBox'))
        self.comboBox.SetFontSize(11)
        umg.UHorizontalBoxSlot.Cast(hb.AddChild(self.comboBox)).SetPadding(margin)
        self.RepopulateClassList()
        self.comboBox.BindOnSelectionChanged(self.OnSelectionChanged)

        spawnButton = umg.UButton.Cast(umg.CreateWidget(hb, umg.UButton, 'spawnButton'))
        umg.UHorizontalBoxSlot.Cast(hb.AddChild(spawnButton)).SetPadding(margin)
        label = umg.UTextBlock.Cast(umg.CreateWidget(spawnButton, umg.UTextBlock, 'textblock'))
        label.SetText('Spawn')
        label.SetFontSize(11)
        spawnButton.SetContent(label)
        spawnButton.BindOnClicked(self.OnSpawnClicked)

        refreshButton = umg.UButton.Cast(umg.CreateWidget(hb, umg.UButton, 'refreshButton'))
        umg.UHorizontalBoxSlot.Cast(hb.AddChild(refreshButton)).SetPadding(margin)
        label = umg.UTextBlock.Cast(umg.CreateWidget(refreshButton, umg.UTextBlock, 'textblock'))
        label.SetText('Refresh')
        label.SetFontSize(11)
        refreshButton.SetContent(label)
        refreshButton.BindOnClicked(self.OnRefreshClicked)

        if 0:
            # Row: checkbox (delete old instances) + text
            hb = umg.UHorizontalBox.Cast(umg.CreateWidget(vboxRoot, umg.UHorizontalBox, 'hb2'))
            slot = umg.UVerticalBoxSlot.Cast(vboxRoot.AddChild(hb))
            slot.SetPadding(margin)

            self.locationCheckbox = umg.UCheckBox.Cast(umg.CreateWidget(hb, umg.UCheckBox, 'checkbox'))
            umg.UHorizontalBoxSlot.Cast(hb.AddChild(self.locationCheckbox)).SetPadding(margin)
            self.locationCheckbox.SetIsChecked(True)
            self.hackCheck = self.locationCheckbox.BindOnCheckStateChanged(self.OnCheckStateChanged)

            label = umg.UTextBlock.Cast(umg.CreateWidget(hb, umg.UTextBlock, 'label'))
            slot = umg.UHorizontalBoxSlot.Cast(hb.AddChild(label))
            slot.SetVerticalAlignment(uepy.enums.EVerticalAlignment.Center)
            slot.SetPadding(margin)
            label.SetText('Delete old instances before spawning')
        else:
            # msg telling them to use sourcewatcher
            hb = umg.UHorizontalBox.Cast(umg.CreateWidget(vboxRoot, umg.UHorizontalBox, 'hb2'))
            slot = umg.UVerticalBoxSlot.Cast(vboxRoot.AddChild(hb))
            label.SetFontSize(11)
            slot.SetPadding(margin)

            label = umg.UTextBlock.Cast(umg.CreateWidget(hb, umg.UTextBlock, 'label'))
            label.SetFontSize(11)
            slot = umg.UHorizontalBoxSlot.Cast(hb.AddChild(label))
            slot.SetVerticalAlignment(uepy.enums.EVerticalAlignment.Center)
            slot.SetPadding(margin)
            label.SetText('Instead of using this spawner, ask dave about using sourcewatcher!')

    def RepopulateClassList(self):
        self.comboBox.ClearOptions()
        _classes = list(uepy.GetPythonEngineSubclasses().values())
        glueClasses = uepy.GetAllGlueClasses() # TODO: filter out glue classes for widgets
        classes = []
        for klass in _classes: # TODO: there's got to be a better way than this
            foundGlueBase = False
            for gc in glueClasses:
                if issubclass(klass, gc):
                    foundGlueBase = True
                    break
            if foundGlueBase:
                classes.append(klass)

        classes.sort(key=lambda x:x.__name__.split('.')[-1])
        self.classes = classes
        for c in classes:
            self.comboBox.AddOption(c.__name__.split('.')[-1])
        self.comboBox.SetSelectedIndex(0)

    def OnRefreshClicked(self, *args, **kwargs):
        # TODO: we're only refreshing the list, not triggering any modules to reload
        self.RepopulateClassList()

    def OnSpawnClicked(self, *args, **kwargs):
        editor.DeselectAllActors()
        world = editor.GetWorld() # TODO: how about just uepy.GetWorld to allow spawning during PIE too?
        index = max(0,self.comboBox.GetSelectedIndex())
        klass = self.classes[index]
        actor = uepy.SpawnActor(world, klass)
        editor.SelectActor(actor)

    def OnSelectionChanged(self, *args, **kwargs):
        log('ON SELCH', self, args, kwargs)

    def OnCheckStateChanged(self, *args, **kwargs):
        log('ON CHECK', self, args, kwargs)

editor.RegisterNomadTabSpawner(SpawnerTab, 'uepy Spawner')

'''
UEditableTextBox
SEditableTextBox w/ hint_text
'''

