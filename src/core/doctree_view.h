#ifndef DOCTREE_VIEW_H
#define DOCTREE_VIEW_H

// Capture the current contents of a rich text view as a document model.
// This is what saving uses, so it is the direction that must not lose
// structure. Caller frees with Doc_Free().
DocModel* DocView_Capture(HWND hRichEdit);

// The same, told which model the view was loaded from.
//
// The control holds a picture as an object it will not give the bytes of
// back -- what comes out is the object replacement character and nothing
// else. The pictures in `source` are therefore matched to those characters in
// order, which is how a document opened, edited and saved keeps its pictures.
DocModel* DocView_CaptureWith(HWND hRichEdit, const DocModel* source);

#endif // DOCTREE_VIEW_H
