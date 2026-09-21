#ifndef LAYOUTIMAGE_H
#define LAYOUTIMAGE_H

// Turning a picture's bytes into something Direct2D can draw.
//
// The model carries a picture exactly as the file had it -- a PNG stays a PNG
// -- so every renderer needs the same step: hand the bytes to WIC, get pixels,
// make a bitmap for the device being drawn on. The page view and the printer
// both do it, so it lives here rather than in either of them.
//
// A bitmap belongs to the device it was made for: when a render target goes,
// so do its bitmaps.

#ifdef __cplusplus

struct ID2D1RenderTarget;
struct ID2D1Bitmap;

// A bitmap for `image` on `target`. NULL if the bytes are not a picture WIC
// can read. Caller releases.
ID2D1Bitmap* LayoutImage_Create(ID2D1RenderTarget* target, const DocImage* image);

#endif

#endif // LAYOUTIMAGE_H
