#ifndef SIGNPAD_H
#define SIGNPAD_H

// Drawing a signature.
//
// A small window, a black pen, and the mouse. That is the whole thing -- and
// it is the difference between "sign this PDF" meaning "find a scanner" and
// meaning "sign it".
//
// What comes out is a PNG with a transparent background, which is what goes
// onto a page: a signature on a white rectangle would paint over the line it
// is supposed to sit on.

#ifdef __cplusplus
extern "C" {
#endif

// Show the pad. Returns the PNG the user drew, which the caller frees, or
// NULL if they cancelled or drew nothing. `saveOut` says whether they asked
// for it to be kept for next time.
BYTE* SignPad_Draw(HWND owner, size_t* lenOut, BOOL* saveOut, WCHAR* nameOut,
                   size_t nameChars);

#ifdef __cplusplus
}
#endif

#endif // SIGNPAD_H
