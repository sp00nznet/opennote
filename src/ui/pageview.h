#ifndef PAGEVIEW_H
#define PAGEVIEW_H

// The page view: the laid-out document, drawn and edited.
//
// A child window, not a window of its own. A tab shows either its text control
// or this, in the same rectangle, and switching between them is what View >
// Page Layout does -- so the document does not move house to be looked at on
// its pages, and every tab can be in a different view.
//
// It works on a copy of what the control holds: `source` is the model the
// document was read from, for the things a control cannot give back -- a
// picture's bytes, a header, the page setup. What is edited here goes back to
// the control when the view closes or when the document is saved.

HWND PageView_Create(HWND hParent, HWND hRichEdit, const DocModel* source,
                     const WCHAR* docTitle);

// Which paragraph the caret is in, as an index into the document. Used by the
// commands that attach something to a paragraph -- a comment, for one -- so
// that they mean the paragraph the user is looking at.
BOOL PageView_CaretPara(HWND hPageView, int* paraIndexOut);

// Push whatever has been edited back into the text control it came from.
// Called before saving or printing, and when the view goes away.
void PageView_Apply(HWND hPageView);

#endif // PAGEVIEW_H
