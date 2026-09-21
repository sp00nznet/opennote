#ifndef RICHOLE_H
#define RICHOLE_H

// Give a rich text control somewhere to keep embedded objects.
//
// Without this the control discards every picture it reads, silently. See
// richole.c for what the callback has to answer.
void RichOle_Attach(HWND hRichEdit);

#endif // RICHOLE_H
