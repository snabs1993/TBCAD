// GDI backend: lists printers, shows the system print dialog and draws each sheet through the
// shared drawSheet() onto the printer's device context.
// clang-format off
#include <windows.h>
#include <commdlg.h>
#include <winspool.h>
// clang-format on

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "tbcad/print/print.h"

namespace tbcad::print {

namespace {

std::string toUtf8(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::wstring toWide(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring defaultPrinterName() {
    DWORD n = 0;
    GetDefaultPrinterW(nullptr, &n);
    if (n == 0)
        return {};
    std::wstring name(n, L'\0');
    if (!GetDefaultPrinterW(name.data(), &n))
        return {};
    name.resize(n > 0 ? n - 1 : 0);
    return name;
}

COLORREF rgb(Rgba c) {
    return RGB(c.r, c.g, c.b);
}

LONG toLong(double v) {
    return static_cast<LONG>(std::lround(v));
}

// Draws on a printer device context.
class GdiCanvas final : public Canvas {
public:
    explicit GdiCanvas(HDC dc) : dc_(dc) {
        SetBkMode(dc_, TRANSPARENT);
        SetPolyFillMode(dc_, ALTERNATE);
    }

    void polyline(const std::vector<Vec2>& px, bool closed, Rgba color, double widthPx,
                  const std::vector<double>& dashPx) override {
        if (px.size() < 2)
            return;
        std::vector<POINT> pts;
        pts.reserve(px.size() + 1);
        for (const Vec2& p : px)
            pts.push_back({toLong(p.x), toLong(p.y)});
        if (closed)
            pts.push_back(pts.front());
        HPEN pen = makePen(color, widthPx, dashPx);
        HGDIOBJ old = SelectObject(dc_, pen);
        Polyline(dc_, pts.data(), static_cast<int>(pts.size()));
        SelectObject(dc_, old);
        DeleteObject(pen);
    }

    void fill(const std::vector<std::vector<Vec2>>& rings, Rgba color) override {
        std::vector<POINT> pts;
        std::vector<INT> counts;
        for (const auto& r : rings) {
            for (const Vec2& p : r)
                pts.push_back({toLong(p.x), toLong(p.y)});
            counts.push_back(static_cast<INT>(r.size()));
        }
        HBRUSH brush = CreateSolidBrush(rgb(color));
        HPEN pen = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ oldBrush = SelectObject(dc_, brush);
        HGDIOBJ oldPen = SelectObject(dc_, pen);
        PolyPolygon(dc_, pts.data(), counts.data(), static_cast<int>(counts.size()));
        SelectObject(dc_, oldBrush);
        SelectObject(dc_, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void dot(Vec2 px, Rgba color, double diameterPx) override {
        const LONG r = std::max<LONG>(1, toLong(diameterPx / 2));
        HBRUSH brush = CreateSolidBrush(rgb(color));
        HPEN pen = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ oldBrush = SelectObject(dc_, brush);
        HGDIOBJ oldPen = SelectObject(dc_, pen);
        Ellipse(dc_, toLong(px.x) - r, toLong(px.y) - r, toLong(px.x) + r + 1,
                toLong(px.y) + r + 1);
        SelectObject(dc_, oldBrush);
        SelectObject(dc_, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void pushClip(const BBox2& r) override {
        SaveDC(dc_);
        IntersectClipRect(dc_, toLong(r.min.x), toLong(r.min.y), toLong(r.max.x) + 1,
                          toLong(r.max.y) + 1);
    }

    void pushClipPolygon(const std::vector<Vec2>& px, const BBox2& box) override {
        SaveDC(dc_);
        IntersectClipRect(dc_, toLong(box.min.x), toLong(box.min.y), toLong(box.max.x) + 1,
                          toLong(box.max.y) + 1);
        std::vector<POINT> pts;
        pts.reserve(px.size());
        for (const Vec2& p : px)
            pts.push_back({toLong(p.x), toLong(p.y)});
        BeginPath(dc_);
        Polygon(dc_, pts.data(), static_cast<int>(pts.size()));
        EndPath(dc_);
        SelectClipPath(dc_, RGN_AND);
    }

    void popClip() override { RestoreDC(dc_, -1); }

    // The pixels are blended with white (the paper) here, so the device only copies colours; a
    // turned picture goes through a world transform that maps pixel (x, y) from the top-left.
    void image(const ImageData& d, const std::array<Vec2, 4>& c, double opacity) override {
        if (d.width <= 0 || d.height <= 0 ||
            d.rgba.size() < static_cast<std::size_t>(d.width) * d.height * 4)
            return;
        const double a = std::clamp(opacity, 0.0, 1.0);
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(d.width) * d.height * 4);
        for (std::size_t i = 0; i < static_cast<std::size_t>(d.width) * d.height; ++i) {
            const double k = a * d.rgba[4 * i + 3] / 255.0;
            auto mix = [&](std::uint8_t v) {
                return static_cast<std::uint8_t>(std::lround(255.0 * (1 - k) + v * k));
            };
            bgra[4 * i] = mix(d.rgba[4 * i + 2]);
            bgra[4 * i + 1] = mix(d.rgba[4 * i + 1]);
            bgra[4 * i + 2] = mix(d.rgba[4 * i]);
        }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = d.width;
        bmi.bmiHeader.biHeight = -d.height;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        const Vec2 tl = c[3], tr = c[2], bl = c[0];
        const Vec2 ex = (tr - tl) / static_cast<double>(d.width);
        const Vec2 ey = (bl - tl) / static_cast<double>(d.height);
        if (SetGraphicsMode(dc_, GM_ADVANCED) == 0)
            return;  // the driver cannot turn pictures
        SaveDC(dc_);
        XFORM xf{static_cast<FLOAT>(ex.x), static_cast<FLOAT>(ex.y), static_cast<FLOAT>(ey.x),
                 static_cast<FLOAT>(ey.y), static_cast<FLOAT>(tl.x), static_cast<FLOAT>(tl.y)};
        SetWorldTransform(dc_, &xf);
        SetStretchBltMode(dc_, HALFTONE);
        StretchDIBits(dc_, 0, 0, d.width, d.height, 0, 0, d.width, d.height, bgra.data(), &bmi,
                      DIB_RGB_COLORS, SRCCOPY);
        RestoreDC(dc_, -1);
    }

private:
    HPEN makePen(Rgba color, double widthPx, const std::vector<double>& dashPx) {
        LOGBRUSH lb{BS_SOLID, rgb(color), 0};
        const DWORD width = static_cast<DWORD>(std::max<LONG>(1, toLong(widthPx)));
        if (dashPx.empty())
            return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, width,
                                &lb, 0, nullptr);
        std::vector<DWORD> style;
        for (double d : dashPx)
            style.push_back(static_cast<DWORD>(std::max<LONG>(1, toLong(d))));
        if (style.size() % 2)
            style.push_back(style.back());
        return ExtCreatePen(PS_GEOMETRIC | PS_USERSTYLE | PS_ENDCAP_FLAT | PS_JOIN_ROUND, width,
                            &lb, static_cast<DWORD>(style.size()), style.data());
    }

    HDC dc_;
};

// Orientation and paper of one sheet, applied to a device mode (0.1 mm units).
void applySheetToDevMode(DEVMODEW& dm, const exportlib::PlotLayout& layout) {
    const bool landscape = layout.pageWidthMm > layout.pageHeightMm;
    dm.dmFields |= DM_ORIENTATION | DM_PAPERWIDTH | DM_PAPERLENGTH;
    dm.dmOrientation = landscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
    // Paper sizes are given in portrait terms; the driver rotates for landscape.
    dm.dmPaperWidth =
        static_cast<short>(std::lround(std::min(layout.pageWidthMm, layout.pageHeightMm) * 10));
    dm.dmPaperLength =
        static_cast<short>(std::lround(std::max(layout.pageWidthMm, layout.pageHeightMm) * 10));
    dm.dmPaperSize = 0;  // custom: width and length above
    dm.dmFields &= ~static_cast<DWORD>(DM_PAPERSIZE);
}

DeviceMetrics metricsOf(HDC dc) {
    DeviceMetrics m;
    m.dpiX = GetDeviceCaps(dc, LOGPIXELSX);
    m.dpiY = GetDeviceCaps(dc, LOGPIXELSY);
    m.pageWidthPx = GetDeviceCaps(dc, PHYSICALWIDTH);
    m.pageHeightPx = GetDeviceCaps(dc, PHYSICALHEIGHT);
    m.offsetXPx = GetDeviceCaps(dc, PHYSICALOFFSETX);
    m.offsetYPx = GetDeviceCaps(dc, PHYSICALOFFSETY);
    return m;
}

}  // namespace

bool available() {
    return true;
}

std::vector<PrinterInfo> listPrinters() {
    std::vector<PrinterInfo> out;
    DWORD needed = 0, count = 0;
    const DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
    EnumPrintersW(flags, nullptr, 4, nullptr, 0, &needed, &count);
    if (needed == 0)
        return out;
    std::vector<BYTE> buf(needed);
    if (!EnumPrintersW(flags, nullptr, 4, buf.data(), needed, &needed, &count))
        return out;
    const std::wstring def = defaultPrinterName();
    const auto* info = reinterpret_cast<const PRINTER_INFO_4W*>(buf.data());
    for (DWORD i = 0; i < count; ++i) {
        PrinterInfo p;
        p.name = toUtf8(info[i].pPrinterName);
        p.isDefault = def == info[i].pPrinterName;
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<PaperInfo> printerPapers(const std::string& printer) {
    std::vector<PaperInfo> out;
    const std::wstring name = printer.empty() ? defaultPrinterName() : toWide(printer);
    if (name.empty())
        return out;
    const int n = DeviceCapabilitiesW(name.c_str(), nullptr, DC_PAPERNAMES, nullptr, nullptr);
    if (n <= 0)
        return out;
    std::vector<wchar_t> names(static_cast<std::size_t>(n) * 64, L'\0');
    std::vector<POINT> sizes(static_cast<std::size_t>(n));
    if (DeviceCapabilitiesW(name.c_str(), nullptr, DC_PAPERNAMES, names.data(), nullptr) != n ||
        DeviceCapabilitiesW(name.c_str(), nullptr, DC_PAPERSIZE,
                            reinterpret_cast<LPWSTR>(sizes.data()), nullptr) != n)
        return out;
    for (int i = 0; i < n; ++i) {
        PaperInfo p;
        p.name = toUtf8(&names[static_cast<std::size_t>(i) * 64]);
        const double w = sizes[static_cast<std::size_t>(i)].x / 10.0;
        const double h = sizes[static_cast<std::size_t>(i)].y / 10.0;
        p.widthMm = std::min(w, h);
        p.heightMm = std::max(w, h);
        out.push_back(std::move(p));
    }
    return out;
}

Result<void> printSheets(const std::vector<exportlib::PlotSheet>& sheets, const PrintJob& job) {
    if (sheets.empty())
        return Error{ErrorCode::InvalidArgument, "There is nothing to print."};

    HDC dc = nullptr;
    DEVMODEW* dialogMode = nullptr;
    PRINTDLGW pd{};
    std::wstring printerName = toWide(job.printer);
    if (job.showDialog) {
        pd.lStructSize = sizeof pd;
        pd.hwndOwner = GetActiveWindow();
        pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS | PD_USEDEVMODECOPIESANDCOLLATE;
        pd.nCopies = static_cast<WORD>(std::clamp(job.copies, 1, 999));
        // Open the dialog on the printer chosen in the plot settings, not the system default.
        if (!printerName.empty()) {
            const std::wstring driver = L"winspool";
            const std::size_t head = sizeof(DEVNAMES) / sizeof(wchar_t);
            const std::size_t chars = head + driver.size() + 1 + printerName.size() + 1 + 1;
            HGLOBAL mem = GlobalAlloc(GHND, chars * sizeof(wchar_t));
            if (mem) {
                auto* dn = static_cast<DEVNAMES*>(GlobalLock(mem));
                auto* base = reinterpret_cast<wchar_t*>(dn);
                dn->wDriverOffset = static_cast<WORD>(head);
                dn->wDeviceOffset = static_cast<WORD>(head + driver.size() + 1);
                dn->wOutputOffset =
                    static_cast<WORD>(head + driver.size() + 1 + printerName.size() + 1);
                dn->wDefault = 0;
                std::copy(driver.begin(), driver.end(), base + dn->wDriverOffset);
                std::copy(printerName.begin(), printerName.end(), base + dn->wDeviceOffset);
                GlobalUnlock(mem);
                pd.hDevNames = mem;
            }
        }
        if (!PrintDlgW(&pd)) {
            const DWORD err = CommDlgExtendedError();
            if (pd.hDevNames)
                GlobalFree(pd.hDevNames);
            if (pd.hDevMode)
                GlobalFree(pd.hDevMode);
            if (err == 0)
                return Error{ErrorCode::Generic, "Printing cancelled."};
            return Error{ErrorCode::Io,
                         "The print dialog failed (error " + std::to_string(err) + ")."};
        }
        dc = pd.hDC;
        if (pd.hDevMode)
            dialogMode = static_cast<DEVMODEW*>(GlobalLock(pd.hDevMode));
    } else {
        if (printerName.empty())
            printerName = defaultPrinterName();
        if (printerName.empty())
            return Error{ErrorCode::NotFound, "No printer is installed."};
        dc = CreateDCW(L"WINSPOOL", printerName.c_str(), nullptr, nullptr);
    }
    if (!dc)
        return Error{ErrorCode::Io, "Cannot open the printer."};
    auto closeDc = [&] {
        if (pd.hDevMode) {
            GlobalUnlock(pd.hDevMode);
            GlobalFree(pd.hDevMode);
        }
        if (pd.hDevNames)
            GlobalFree(pd.hDevNames);
        DeleteDC(dc);
    };

    std::wstring title = toWide(job.title);
    DOCINFOW di{};
    di.cbSize = sizeof di;
    di.lpszDocName = title.c_str();
    if (StartDocW(dc, &di) <= 0) {
        closeDc();
        return Error{ErrorCode::Io, "The printer did not accept the job."};
    }
    const int copies = job.showDialog ? 1 : std::clamp(job.copies, 1, 999);
    Result<void> result = Result<void>();
    for (int copy = 0; copy < copies && result.ok(); ++copy) {
        for (const auto& sheet : sheets) {
            // Each page gets its own paper size and orientation.
            DEVMODEW local{};
            DEVMODEW* dm = dialogMode;
            if (!dm) {
                local.dmSize = sizeof local;
                dm = &local;
            }
            applySheetToDevMode(*dm, sheet.layout);
            ResetDCW(dc, dm);
            if (StartPage(dc) <= 0) {
                result = Error{ErrorCode::Io, "The printer stopped while starting a page."};
                break;
            }
            GdiCanvas canvas(dc);
            drawSheet(sheet, metricsOf(dc), canvas);
            if (EndPage(dc) <= 0) {
                result = Error{ErrorCode::Io, "The printer stopped while printing a page."};
                break;
            }
        }
    }
    if (result.ok())
        EndDoc(dc);
    else
        AbortDoc(dc);
    closeDc();
    return result;
}

}  // namespace tbcad::print
