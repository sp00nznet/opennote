#ifndef IMAGEDIB_H
#define IMAGEDIB_H

// Decoding a picture, for the one reader that cannot do it itself.
//
// The model carries a picture as the bytes the file held -- a PNG stays a PNG
// -- because that is the only way one survives a round trip without being
// re-encoded. The rich text view is the exception: RichEdit's RTF reader takes
// `\wmetafile` and `\dibitmap` and silently drops `\pngblip`, so a picture on
// its way to that view has to be decoded first.
//
// WIC does the decoding. It ships with Windows, reads every format anybody
// pastes into a document, and is already a dependency of the printing path.

// Decode `bytes` into a 24-bit bottom-up DIB: the header is filled in and the
// pixels are returned. Caller frees. NULL if the format is not one WIC reads.
BYTE* ImageDib_Decode(const BYTE* bytes, size_t len,
                      BITMAPINFOHEADER* headerOut, size_t* pixelsLenOut);

// ...and wrap that DIB in a Windows metafile, which is the only picture
// RichEdit's RTF reader will actually take. Caller frees.
BYTE* ImageDib_ToMetafile(const BYTE* pixels, const BITMAPINFOHEADER* header,
                          size_t* lenOut);

#endif // IMAGEDIB_H
