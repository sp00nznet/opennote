#ifndef DOCTREE_VIEW_H
#define DOCTREE_VIEW_H

// Capture the current contents of a rich text view as a document model.
// This is what saving uses, so it is the direction that must not lose
// structure. Caller frees with Doc_Free().
DocModel* DocView_Capture(HWND hRichEdit);

#endif // DOCTREE_VIEW_H
