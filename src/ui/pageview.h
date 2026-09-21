#ifndef PAGEVIEW_H
#define PAGEVIEW_H

// A scrollable window showing what the layout engine produced: real pages,
// with margins and page boundaries, rather than text flowed into a window.
//
// Captures the editor's current contents, lays them out and shows them, so it
// previews what is on screen rather than what was last saved.

BOOL PageView_Show(HWND hOwner, HWND hRichEdit, const WCHAR* docTitle);

#endif // PAGEVIEW_H
