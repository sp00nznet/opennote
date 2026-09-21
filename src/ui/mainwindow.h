#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// Window registration and creation
BOOL MainWindow_RegisterClass(HINSTANCE hInstance);
HWND MainWindow_Create(HINSTANCE hInstance);

// Window procedure
LRESULT CALLBACK MainWindow_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Command handlers
void MainWindow_OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify);
void MainWindow_OnSize(HWND hwnd, UINT state, int cx, int cy);
void MainWindow_OnClose(HWND hwnd);
BOOL MainWindow_OnQueryEndSession(HWND hwnd);

// UI updates
void MainWindow_UpdateTitle(void);

// Show a document in a tab -- reusing an untouched one where that fits, and
// giving a PDF a view of its own. Takes ownership of `newDoc`.
void MainWindow_OpenDocument(Document* newDoc);
void MainWindow_UpdateMenuState(void);
void MainWindow_UpdateNewTabButton(void);

// Context menus
void MainWindow_ShowEditorContextMenu(HWND hwnd, int x, int y);

#endif // MAINWINDOW_H
