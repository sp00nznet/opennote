#ifndef DOCTREE_RTF_H
#define DOCTREE_RTF_H

// DocModel -> RTF, which is how a model reaches the rich text view.
// Caller frees with free(). Returns NULL on allocation failure.
char* DocRtf_Emit(const DocModel* doc);

#endif // DOCTREE_RTF_H
