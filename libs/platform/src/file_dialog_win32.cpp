// Windows IFileDialog wrapper. The only place outside apps/ that includes Windows headers.
// clang-format off
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
// clang-format on

#include "tbcad/platform/file_dialog.h"

namespace tbcad::platform {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Owns the strings the COMDLG_FILTERSPEC entries point to.
struct FilterStorage {
    std::vector<std::wstring> names;
    std::vector<std::wstring> patterns;
    std::vector<COMDLG_FILTERSPEC> specs;
    explicit FilterStorage(const std::vector<FileFilter>& filters) {
        names.reserve(filters.size() + 1);
        patterns.reserve(filters.size() + 1);
        for (const auto& f : filters) {
            names.push_back(widen(f.name));
            patterns.push_back(widen(f.pattern));
        }
        names.push_back(L"All files");
        patterns.push_back(L"*.*");
        for (std::size_t i = 0; i < names.size(); ++i)
            specs.push_back({names[i].c_str(), patterns[i].c_str()});
    }
};

template <class Dialog>
std::optional<std::filesystem::path> runDialog(Dialog* dlg, HWND owner) {
    if (FAILED(dlg->Show(owner)))
        return std::nullopt;  // cancelled
    IShellItem* item = nullptr;
    if (FAILED(dlg->GetResult(&item)) || item == nullptr)
        return std::nullopt;
    PWSTR path = nullptr;
    std::optional<std::filesystem::path> result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
        result = std::filesystem::path(path);
        CoTaskMemFree(path);
    }
    item->Release();
    return result;
}

// Opens the dialog in `folder` when it exists, else in the user's Documents folder.
void startIn(IFileDialog* dlg, const std::filesystem::path& folder) {
    IShellItem* item = nullptr;
    std::error_code ec;
    if (!folder.empty() && std::filesystem::is_directory(folder, ec))
        SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (item == nullptr) {
        PWSTR docs = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
            SHCreateItemFromParsingName(docs, nullptr, IID_PPV_ARGS(&item));
            CoTaskMemFree(docs);
        }
    }
    if (item != nullptr) {
        dlg->SetFolder(item);
        item->Release();
    }
}

// COM must be initialised on the calling thread; the GUI thread normally has it from GLFW/OLE,
// but initialising twice is harmless.
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope() {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
};

}  // namespace

bool nativeDialogsAvailable() {
    return true;
}

std::optional<std::filesystem::path> showOpenDialog(void* ownerWindow,
                                                    const std::vector<FileFilter>& filters,
                                                    const std::string& title,
                                                    const std::filesystem::path& startFolder) {
    ComScope com;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return std::nullopt;
    FilterStorage fs(filters);
    dlg->SetFileTypes(static_cast<UINT>(fs.specs.size()), fs.specs.data());
    dlg->SetFileTypeIndex(1);
    startIn(dlg, startFolder);
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    std::wstring wtitle = widen(title);
    if (!wtitle.empty())
        dlg->SetTitle(wtitle.c_str());
    auto result = runDialog(dlg, static_cast<HWND>(ownerWindow));
    dlg->Release();
    return result;
}

std::optional<std::filesystem::path> showSaveDialog(void* ownerWindow,
                                                    const std::vector<FileFilter>& filters,
                                                    const std::string& title,
                                                    const std::string& defaultExtension,
                                                    const std::string& suggestedName,
                                                    const std::filesystem::path& startFolder) {
    ComScope com;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return std::nullopt;
    FilterStorage fs(filters);
    dlg->SetFileTypes(static_cast<UINT>(fs.specs.size()), fs.specs.data());
    dlg->SetFileTypeIndex(1);
    startIn(dlg, startFolder);
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    std::wstring wext = widen(defaultExtension);
    if (!wext.empty())
        dlg->SetDefaultExtension(wext.c_str());
    std::wstring wname = widen(suggestedName);
    if (!wname.empty())
        dlg->SetFileName(wname.c_str());
    std::wstring wtitle = widen(title);
    if (!wtitle.empty())
        dlg->SetTitle(wtitle.c_str());
    auto result = runDialog(dlg, static_cast<HWND>(ownerWindow));
    dlg->Release();
    return result;
}

}  // namespace tbcad::platform
