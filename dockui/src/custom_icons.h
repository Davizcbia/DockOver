#pragma once
#include <windows.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "json_lite.h"

namespace dock {
// Versioned application namespace leaves other Dock item kinds independent.
std::string ApplicationItemId(const std::wstring& target, const std::wstring& arguments);
struct IconFileStamp {
    bool exists = false;
    uint64_t modified = 0, size = 0;
    bool operator==(const IconFileStamp& other) const { return exists==other.exists && modified==other.modified && size==other.size; }
};
IconFileStamp ReadIconFileStamp(const std::wstring& path) noexcept;
// PNG/ICO via WIC, SVG via Direct2D; square premultiplied BGRA with aspect ratio preserved.
bool LoadCustomIconPixels(const std::wstring& path, std::vector<uint32_t>& pixels,
                          UINT& width, UINT& height, std::wstring& error);

// Per-icon DIY parameters (V0.2 icon editor). Stored in the optional top-level "edits" object of
// custom-icons.json (format version 2); files without edits stay version 1. Unknown fields of an edit
// entry are kept in `extra` and written back unchanged.
struct IconEdit {
    double scale = 1.0;    // 0.5 .. 1.5, about the icon box centre
    double offsetX = 0.0;  // -0.25 .. 0.25 of the icon box side (positive = right)
    double offsetY = 0.0;  // -0.25 .. 0.25 of the icon box side (positive = down)
    int mask = -1;         // -1 inherit the Dock setting, 0 off, 1 on
    int shadow = -1;       // -1 inherit (on), 0 off, 1 on
    json::Value extra = json::Value::MakeObject();
    void Clamp();
    bool IsDefault() const;
};
// Scale/offset applied on a canvas of the same size (layout and hit areas never change). Bilinear.
std::vector<uint32_t> ApplyIconEdit(const std::vector<uint32_t>& pixels, UINT w, UINT h, const IconEdit& edit);

class CustomIconStore {
public:
    bool Load(const std::wstring& path, std::wstring& error);
    std::wstring Label(const std::string& id) const;
    bool SetLabel(const std::string& id, const std::wstring& label, std::wstring& error);
    bool Available() const { return available_; }
    const std::wstring* Find(const std::string& id) const;
    bool Set(const std::string& id, const std::wstring& path, std::wstring& error);
    bool Remove(const std::string& id, std::wstring& error);  // removes the image override and all edits
    const IconEdit* FindEdit(const std::string& id) const;
    // Atomically sets the edit (default edit = removed) and optionally replaces the image path.
    bool SetEdit(const std::string& id, const IconEdit& edit, const std::wstring* newImage, std::wstring& error);
private:
    bool Save(const std::map<std::string, std::wstring>& next, const std::map<std::string, IconEdit>& nextEdits, std::wstring& error);
    std::map<std::string, IconEdit> edits_;
    json::Value extraRoot_ = json::Value::MakeObject();  // unknown top-level keys, preserved
    std::wstring path_;
    std::map<std::string, std::wstring> icons_;
    bool available_ = false;
    bool hadFile_ = false;
    std::string loadedBytes_;
};
}
