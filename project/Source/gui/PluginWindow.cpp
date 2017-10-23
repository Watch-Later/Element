/*
    PluginWindow.cpp - This file is part of Element
    Copyright (C) 2016 Kushview, LLC.  All rights reserved.
*/

#include "engine/GraphNode.h"
#include "gui/PluginWindow.h"

namespace Element {
static Array <PluginWindow*> activePluginWindows;

class PluginWindowToolbar : public Toolbar
{
public:
    enum Items {
        BypassPlugin = 1
    };
    
    PluginWindowToolbar() { }
    ~PluginWindowToolbar() { }
};

class PluginWindowContent : public Component,
                            public ComponentListener,
                            public ButtonListener
{
public:
    PluginWindowContent (Component* const _editor, GraphNode* _node)
        : editor (_editor), node (_node)
    {
        addAndMakeVisible (toolbar = new PluginWindowToolbar());
        toolbar->setBounds (0, 0, getWidth(), 24);
        
        addAndMakeVisible (editor);
        
        addAndMakeVisible (bypassButton);
        
        bypassButton.setButtonText ("Bypass");
        bypassButton.setToggleState (_node->getAudioProcessor()->isSuspended(), dontSendNotification);
        bypassButton.setColour (TextButton::buttonOnColourId, Colours::red);
        bypassButton.addListener (this);
        
        setSize (editor->getWidth(), editor->getHeight() + toolbar->getHeight());
        resized();
    }
    
    ~PluginWindowContent()
    {
        bypassButton.removeListener (this);
        editor = nullptr;
        toolbar = nullptr;
        leftPanel = nullptr;
        rightPanel = nullptr;
    }
    
    void resized() override
    {
        Rectangle<int> r (getLocalBounds());
        
        if (toolbar->getThickness())
        {
            auto r2 = r.removeFromTop (toolbar->getThickness());
            toolbar->setBounds (r2);
            r2.removeFromRight(4);
            bypassButton.changeWidthToFitText();
            bypassButton.setBounds (r2.removeFromRight(bypassButton.getWidth()).reduced (1));
        }
        
        editor->setBounds (r);
    }
    
    void buttonClicked (Button*) override
    {
        const bool desiredBypassState = !node->getAudioProcessor()->isSuspended();
        node->getAudioProcessor()->suspendProcessing (desiredBypassState);
        bypassButton.setToggleState (node->getAudioProcessor()->isSuspended(),
                                     dontSendNotification);
    }
    
    void componentMovedOrResized (Component&, bool wasMoved, bool wasResized) override
    {

    }
    
    Toolbar* getToolbar() const { return toolbar.get(); }
    
private:
    ScopedPointer<PluginWindowToolbar> toolbar;
    TextButton bypassButton;
    ScopedPointer<Component> editor, leftPanel, rightPanel;
    GraphNodePtr node;
};

PluginWindow::PluginWindow (Component* const ui, GraphNode* node)
    : DocumentWindow (ui->getName(), Colours::lightgrey,
                      DocumentWindow::minimiseButton | DocumentWindow::closeButton, false),
      owner (node)
{
    setUsingNativeTitleBar (true);
    setSize (400, 300);
    setContentOwned (new PluginWindowContent (ui, owner), true);
    setTopLeftPosition (owner->properties.getWithDefault ("windowLastX", Random::getSystemRandom().nextInt (500)),
                        owner->properties.getWithDefault ("windowLastY", Random::getSystemRandom().nextInt (500)));
    owner->properties.set ("windowVisible", true);
    setVisible (true);
    addToDesktop();
    
    activePluginWindows.add (this);
}

PluginWindow::~PluginWindow()
{
    activePluginWindows.removeFirstMatchingValue (this);
    clearContentComponent();
}

void PluginWindow::closeCurrentlyOpenWindowsFor (GraphNode* const node)
{
    if (node)
        closeCurrentlyOpenWindowsFor (node->nodeId);
}

void PluginWindow::closeCurrentlyOpenWindowsFor (const uint32 nodeId)
{
    for (int i = activePluginWindows.size(); --i >= 0;)
        if (activePluginWindows.getUnchecked(i)->owner->nodeId == nodeId)
            { delete activePluginWindows.getUnchecked(i); break; }
}

void PluginWindow::closeAllCurrentlyOpenWindows()
{
    if (activePluginWindows.size() > 0)
    {
        for (int i = activePluginWindows.size(); --i >= 0;)
            delete activePluginWindows.getUnchecked(i);
        MessageManager::getInstance()->runDispatchLoopUntil (50);
    }
}

PluginWindow* PluginWindow::getOrCreateWindowFor (GraphNode* node)
{
    if (PluginWindow* win = getWindowFor (node))
        return win;
    return createWindowFor (node);
}

Toolbar* PluginWindow::getToolbar() const
{
    if (PluginWindowContent* pwc = dynamic_cast<PluginWindowContent*> (getContentComponent()))
        return pwc->getToolbar();
    return nullptr;
}

void PluginWindow::resized()
{
    DocumentWindow::resized();
}

PluginWindow* PluginWindow::getWindowFor (GraphNode* node)
{
    for (int i = activePluginWindows.size(); --i >= 0;)
        if (activePluginWindows.getUnchecked(i)->owner == node)
            return activePluginWindows.getUnchecked(i);

    return nullptr;
}

PluginWindow* PluginWindow::getFirstWindow()
{
    return activePluginWindows.getFirst();
}
    
void PluginWindow::updateGraphNode (GraphNode *newNode, Component *newEditor)
{
    jassert(nullptr != newNode && nullptr != newEditor);
    owner = newNode;
    setContentOwned (newEditor, true);
}
    
PluginWindow* PluginWindow::createWindowFor (GraphNode* node)
{
    AudioPluginInstance* plug (node->getAudioPluginInstance());
    if (! plug->hasEditor())
        return nullptr;
    
    AudioProcessorEditor* editor = plug->createEditorIfNeeded();
    return (editor != nullptr) ? new PluginWindow (editor, node) : nullptr;
}

PluginWindow* PluginWindow::createWindowFor (GraphNode* node, Component* ed)
{
    return new PluginWindow (ed, node);
}

void PluginWindow::moved()
{
    owner->properties.set ("windowLastX", getX());
    owner->properties.set ("windowLastY", getY());
}

void PluginWindow::closeButtonPressed()
{
    if (owner) {
        owner->properties.set ("windowVisible", false);
    }
    delete this;
}

}
