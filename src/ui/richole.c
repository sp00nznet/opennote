// Letting the rich text view hold a picture.
//
// RichEdit stores an embedded picture as an OLE object, and it will not create
// one without somewhere to put it. That somewhere comes from the container: a
// callback the control asks for a storage every time it reads an object. With
// no callback, a `\pict` group in RTF is parsed, discarded, and nothing is
// reported -- the picture is simply not there afterwards, which is how every
// picture in a .docx was being lost on its way to the view.
//
// This is the smallest callback that satisfies it: a storage per object, built
// on a block of memory, and sensible answers to the questions a container is
// asked. WordPad's is the same shape.

#define COBJMACROS

#include "supernote.h"
#include "ui/richole.h"

#include <richole.h>
#include <objbase.h>

#pragma comment(lib, "ole32.lib")

typedef struct {
    IRichEditOleCallbackVtbl* lpVtbl;
    LONG refs;
} RichCallback;

static HRESULT STDMETHODCALLTYPE CB_QueryInterface(IRichEditOleCallback* self,
                                                   REFIID riid, void** out) {
    if (!out) return E_POINTER;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRichEditOleCallback)) {
        *out = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE CB_AddRef(IRichEditOleCallback* self) {
    RichCallback* cb = (RichCallback*)self;
    return (ULONG)InterlockedIncrement(&cb->refs);
}

static ULONG STDMETHODCALLTYPE CB_Release(IRichEditOleCallback* self) {
    RichCallback* cb = (RichCallback*)self;
    LONG left = InterlockedDecrement(&cb->refs);
    // The callback is a single static object for the process; there is nothing
    // to free when the last control lets go of it.
    return (ULONG)(left < 0 ? 0 : left);
}

// The one method that matters. Each embedded object gets a compound file of
// its own, held in memory: the control writes the picture into it and reads it
// back from there.
static HRESULT STDMETHODCALLTYPE CB_GetNewStorage(IRichEditOleCallback* self,
                                                  LPSTORAGE* out) {
    (void)self;
    if (!out) return E_POINTER;
    *out = NULL;

    ILockBytes* bytes = NULL;
    HRESULT hr = CreateILockBytesOnHGlobal(NULL, TRUE, &bytes);
    if (FAILED(hr)) return hr;

    hr = StgCreateDocfileOnILockBytes(bytes,
                                      STGM_SHARE_EXCLUSIVE | STGM_CREATE | STGM_READWRITE,
                                      0, out);
    ILockBytes_Release(bytes);
    return hr;
}

static HRESULT STDMETHODCALLTYPE CB_GetInPlaceContext(IRichEditOleCallback* self,
                                                      LPOLEINPLACEFRAME* frame,
                                                      LPOLEINPLACEUIWINDOW* doc,
                                                      LPOLEINPLACEFRAMEINFO info) {
    (void)self; (void)frame; (void)doc; (void)info;
    // No in-place activation: a picture in this view is shown, not edited in
    // its own application.
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CB_ShowContainerUI(IRichEditOleCallback* self, BOOL show) {
    (void)self; (void)show;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE CB_QueryInsertObject(IRichEditOleCallback* self,
                                                      LPCLSID clsid, LPSTORAGE stg,
                                                      LONG cp) {
    (void)self; (void)clsid; (void)stg; (void)cp;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE CB_DeleteObject(IRichEditOleCallback* self, LPOLEOBJECT obj) {
    (void)self; (void)obj;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE CB_QueryAcceptData(IRichEditOleCallback* self,
                                                    LPDATAOBJECT data, CLIPFORMAT* format,
                                                    DWORD reco, BOOL really, HGLOBAL metafile) {
    (void)self; (void)data; (void)format; (void)reco; (void)really; (void)metafile;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE CB_ContextSensitiveHelp(IRichEditOleCallback* self,
                                                         BOOL enter) {
    (void)self; (void)enter;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE CB_GetClipboardData(IRichEditOleCallback* self,
                                                     CHARRANGE* range, DWORD reco,
                                                     LPDATAOBJECT* out) {
    (void)self; (void)range; (void)reco; (void)out;
    // The control's own clipboard handling is what should happen.
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CB_GetDragDropEffect(IRichEditOleCallback* self,
                                                      BOOL drag, DWORD keyState,
                                                      LPDWORD effect) {
    (void)self; (void)drag; (void)keyState;
    if (effect) *effect = DROPEFFECT_COPY;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE CB_GetContextMenu(IRichEditOleCallback* self,
                                                   WORD seltype, LPOLEOBJECT obj,
                                                   CHARRANGE* range, HMENU* menu) {
    (void)self; (void)seltype; (void)obj; (void)range;
    if (menu) *menu = NULL;
    return E_NOTIMPL;
}

static IRichEditOleCallbackVtbl g_vtbl = {
    CB_QueryInterface,
    CB_AddRef,
    CB_Release,
    CB_GetNewStorage,
    CB_GetInPlaceContext,
    CB_ShowContainerUI,
    CB_QueryInsertObject,
    CB_DeleteObject,
    CB_QueryAcceptData,
    CB_ContextSensitiveHelp,
    CB_GetClipboardData,
    CB_GetDragDropEffect,
    CB_GetContextMenu
};

static RichCallback g_callback = { &g_vtbl, 1 };

void RichOle_Attach(HWND hRichEdit) {
    if (!hRichEdit) return;
    SendMessageW(hRichEdit, EM_SETOLECALLBACK, 0, (LPARAM)&g_callback);
}
