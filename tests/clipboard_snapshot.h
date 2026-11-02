#pragma once
#include <windows.h>
#include <ole2.h>
#include <vector>
#include <utility>

// OleGetClipboard returns a live proxy, not a snapshot. Setting that proxy
// back as the clipboard owner makes GetData recurse into itself. Materialize
// independent handles before exercising the clipboard path instead.
struct ClipboardSnapshot {
    HWND owner;
    bool captured = false;
    std::vector<std::pair<UINT,HANDLE>> data;
    explicit ClipboardSnapshot(HWND hwnd) : owner(hwnd) {
        if(!OpenClipboard(hwnd))return;
        for(UINT format=EnumClipboardFormats(0);format;format=EnumClipboardFormats(format)) {
            if(format==CF_OWNERDISPLAY)continue;
            HANDLE value=GetClipboardData(format);
            if(value)if(HANDLE copy=OleDuplicateData(value,static_cast<CLIPFORMAT>(format),0))data.push_back({format,copy});
        }
        CloseClipboard();
        captured = true;
    }
    ~ClipboardSnapshot() {
        // Do not overwrite anything the user copied while tests were running.
        if(captured && GetClipboardOwner()==owner && OpenClipboard(owner)) {
            EmptyClipboard();
            for(auto& item:data)if(SetClipboardData(item.first,item.second))item.second=nullptr;
            CloseClipboard();
        }
        for(const auto& item:data)if(item.second) {
            if(item.first==CF_BITMAP||item.first==CF_DSPBITMAP||item.first==CF_PALETTE)DeleteObject(item.second);
            else if(item.first==CF_ENHMETAFILE||item.first==CF_DSPENHMETAFILE)DeleteEnhMetaFile((HENHMETAFILE)item.second);
            else {
                if(item.first==CF_METAFILEPICT||item.first==CF_DSPMETAFILEPICT) {
                    auto picture=(METAFILEPICT*)GlobalLock(item.second);if(picture){DeleteMetaFile(picture->hMF);GlobalUnlock(item.second);}
                }
                GlobalFree(item.second);
            }
        }
    }
};
