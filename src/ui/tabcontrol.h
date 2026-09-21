#ifndef TABCONTROL_H
#define TABCONTROL_H

// Tab structure
struct Tab {
    Document* document;
    HWND hEditor;

    // The laid-out view of the same document, when this tab is showing one.
    // The tab owns it; the text control stays alive underneath, holding what
    // was there when the view opened.
    HWND hPageView;

    int index;
};

// Tab control creation
HWND TabControl_Create(HWND hParent);

// Tab operations
int TabControl_AddTab(const WCHAR* title);
void TabControl_RemoveTab(int index);
void TabControl_SetActiveTab(int index);
int TabControl_GetActiveTab(void);
int TabControl_GetTabCount(void);

// Tab title management
void TabControl_SetTabTitle(int index, const WCHAR* title);
void TabControl_GetTabTitle(int index, WCHAR* buffer, int bufferSize);
void TabControl_UpdateTabTitle(int index);  // Updates based on document state

// Tab notifications
void TabControl_OnSelChange(void);
void TabControl_OnRightClick(int x, int y);

// Context menu
void TabControl_ShowContextMenu(int tabIndex, int x, int y);

#endif // TABCONTROL_H
