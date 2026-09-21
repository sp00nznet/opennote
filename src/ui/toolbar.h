#ifndef TOOLBAR_H
#define TOOLBAR_H

// The formatting toolbar: font, size, bold/italic/underline, colour,
// alignment, lists and indent. Shown only while a rich text tab is active,
// because none of it means anything to the plain text view.

HWND FormatBar_Create(HWND hParent);

// Height in pixels, or 0 when the bar is hidden. MainWindow_OnSize needs this
// to know where the editor starts.
int  FormatBar_Height(void);

// Show or hide according to the active tab, and lay out at the given width.
// `pageLayout` is TRUE when the active tab is showing the laid-out view, in
// which case the character formatting is not applicable and the page button is
// the one that is latched.
void FormatBar_UpdateVisibility(HWND hEditor, BOOL pageLayout);
void FormatBar_Layout(int width);

// Pull the button and combo states from what is under the caret.
void FormatBar_SyncFromEditor(HWND hEditor);

// Handle a command from the bar's own controls. Returns TRUE if it was one.
BOOL FormatBar_OnCommand(HWND hEditor, int id, int notifyCode, HWND hCtl);

// Custom draw for the buttons, which carry no bitmap resources.
LRESULT FormatBar_OnCustomDraw(LPNMTBCUSTOMDRAW nm);

void FormatBar_OnGetTooltip(NMTTDISPINFOW* info);

HWND FormatBar_Handle(void);

#endif // TOOLBAR_H
