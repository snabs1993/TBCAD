#include "tbcad/print/print.h"

namespace tbcad::print {

bool available() {
    return false;
}

std::vector<PrinterInfo> listPrinters() {
    return {};
}

std::vector<PaperInfo> printerPapers(const std::string&) {
    return {};
}

Result<void> printSheets(const std::vector<exportlib::PlotSheet>&, const PrintJob&) {
    return Error{ErrorCode::Unsupported,
                 "Printing to a printer is only available on Windows. Export a PDF instead."};
}

}  // namespace tbcad::print
