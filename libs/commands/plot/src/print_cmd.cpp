#include "tbcad/plot/plot.h"
#include "tbcad/print/print.h"

namespace tbcad::plot {

namespace {

constexpr const char* kPrinterKey = "plot.printer";
constexpr const char* kCopiesKey = "plot.copies";

int copiesOf(const cmd::IHost& host) {
    try {
        return std::clamp(std::stoi(host.setting(kCopiesKey).value_or("1")), 1, 99);
    } catch (...) {
        return 1;
    }
}

// PRINTER [PRN]: sends the sheet the plot settings describe to a Windows printer, through the
// system print dialog (default) or straight to a named printer.
class PrinterCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PRINTER"; }
    std::vector<std::string> defaultAliases() const override { return {"PRN"}; }
    std::string_view description() const override {
        return "Print the drawing on a printer (uses the plot settings)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        if (!print::available()) {
            ctx.message(
                "Printing is only available in the Windows version. Use PLOT to make a PDF.");
            co_return;
        }
        for (;;) {
            const std::string chosen = ctx.host().setting(kPrinterKey).value_or("");
            cmd::KeywordOptions menu;
            menu.keywords = {"Dialog", "Direct", "Printer", "Copies", "List"};
            menu.defaultKeyword = "Dialog";
            auto choice = co_await ctx.getKeyword(
                "Print to " + (chosen.empty() ? std::string("the default printer") : chosen) +
                    ", " + std::to_string(copiesOf(ctx.host())) +
                    " copy [Dialog/Direct/Printer/Copies/List]",
                menu);
            if (!choice.ok())
                co_return;
            const std::string& kw = choice.keyword;
            if (kw == "List") {
                for (const auto& p : print::listPrinters())
                    ctx.message(p.name + (p.isDefault ? " (default)" : ""));
            } else if (kw == "Printer") {
                cmd::StringOptions so;
                so.defaultValue = chosen;
                auto r = co_await ctx.getString("Printer name (empty = default printer)", so);
                if (r.ok())
                    ctx.host().setSetting(kPrinterKey, r.text);
            } else if (kw == "Copies") {
                cmd::RealOptions ro;
                ro.min = 1;
                ro.max = 99;
                ro.defaultValue = copiesOf(ctx.host());
                auto r = co_await ctx.getReal("Number of copies", ro);
                if (r.ok())
                    ctx.host().setSetting(kCopiesKey, std::to_string(static_cast<int>(r.value)));
            } else {  // Dialog or Direct
                auto sheet = buildPlotSheet(ctx.doc(), ctx.host(), loadSettings(ctx.host()));
                if (!sheet) {
                    ctx.message(sheet.error().message);
                    continue;
                }
                print::PrintJob job;
                job.printer = chosen;
                job.copies = copiesOf(ctx.host());
                job.showDialog = kw == "Dialog";
                const std::string file = ctx.host().setting("document.file").value_or("");
                if (!file.empty())
                    job.title = file;
                auto r = print::printSheet(sheet.value(), job);
                ctx.message(r ? "Sent " + std::to_string(sheet.value().entityCount) +
                                    " objects to the printer."
                              : r.error().message);
                co_return;
            }
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makePrinterCommand() {
    return std::make_unique<PrinterCommand>();
}

}  // namespace tbcad::plot
