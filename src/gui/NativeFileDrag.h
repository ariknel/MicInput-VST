#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// NativeFileDrag — Windows OLE file drag using CF_HDROP.
//
// This is the only correct way to drag files into DAWs (Bitwig, Reaper,
// Ableton, etc.) and Windows Explorer. JUCE's DragAndDropContainer is
// component-to-component only and does not interop with the OS shell.
//
// Usage (call from mouse drag handler, blocks until drop or cancel):
//   NativeFileDrag::perform(filePath);
//
// Thread: must be called on the message thread.
// ─────────────────────────────────────────────────────────────────────────────

#if JUCE_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <string>

namespace NativeFileDrag
{

// ── IDropSource implementation ───────────────────────────────────────────────
class DropSource : public IDropSource
{
public:
    ULONG   refs = 1;
    bool    cancelled = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropSource)
            { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override
    {
        if (escapePressed) { cancelled = true; return DRAGDROP_S_CANCEL; }
        if (!(keyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

// ── IDataObject with CF_HDROP ────────────────────────────────────────────────
class DataObject : public IDataObject
{
public:
    ULONG refs = 1;
    HGLOBAL m_hDrop = nullptr;

    DataObject(const std::wstring& path)
    {
        // Build DROPFILES structure
        size_t pathLen  = path.size() + 1;           // +1 for null terminator
        size_t totalLen = pathLen + 1;                // +1 for double-null at end
        size_t bufSize  = sizeof(DROPFILES) + totalLen * sizeof(wchar_t);

        m_hDrop = GlobalAlloc(GHND, bufSize);
        if (!m_hDrop) return;

        auto* df = static_cast<DROPFILES*>(GlobalLock(m_hDrop));
        df->pFiles = sizeof(DROPFILES);
        df->fWide  = TRUE;
        df->pt.x   = 0; df->pt.y = 0;
        df->fNC    = FALSE;

        auto* dest = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
        memcpy(dest, path.c_str(), pathLen * sizeof(wchar_t));
        dest[pathLen] = L'\0';  // double null
        GlobalUnlock(m_hDrop);
    }

    ~DataObject() { if (m_hDrop) GlobalFree(m_hDrop); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDataObject)
            { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --refs; if (r == 0) delete this; return r;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* stg) override
    {
        if (!fe || !stg) return E_INVALIDARG;
        if (fe->cfFormat != CF_HDROP) return DV_E_FORMATETC;
        if (!(fe->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        if (!m_hDrop) return E_UNEXPECTED;

        // Duplicate the HGLOBAL so the caller can free it independently
        SIZE_T sz    = GlobalSize(m_hDrop);
        HGLOBAL dup  = GlobalAlloc(GHND, sz);
        if (!dup) return E_OUTOFMEMORY;
        void* src = GlobalLock(m_hDrop);
        void* dst = GlobalLock(dup);
        memcpy(dst, src, sz);
        GlobalUnlock(m_hDrop);
        GlobalUnlock(dup);

        stg->tymed          = TYMED_HGLOBAL;
        stg->hGlobal        = dup;
        stg->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override
    {
        if (!fe) return E_INVALIDARG;
        if (fe->cfFormat != CF_HDROP) return DV_E_FORMATETC;
        if (!(fe->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
        { if (out) out->ptd = nullptr; return DATA_S_SAMEFORMATETC; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override
    {
        if (dir != DATADIR_GET) return E_NOTIMPL;
        FORMATETC fe { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return SHCreateStdEnumFmtEtc(1, &fe, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

// ── Public entry point ────────────────────────────────────────────────────────
// filePath: full Windows path, e.g. C:\Users\Arik\Music\MicInput\Saves\take.wav
// Blocks on DoDragDrop until user drops or cancels.
inline void perform(const juce::String& filePath)
{
    std::wstring wpath = filePath.toWideCharPointer();

    auto* dataObj  = new DataObject(wpath);
    auto* dropSrc  = new DropSource();

    DWORD effect = 0;
    ::DoDragDrop(dataObj, dropSrc, DROPEFFECT_COPY, &effect);

    dataObj->Release();
    dropSrc->Release();
}

} // namespace NativeFileDrag

#endif // JUCE_WINDOWS
