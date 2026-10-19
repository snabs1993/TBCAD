#include "app.h"
#include "tbcad/font/font_library.h"

int main(int argc, char** argv) {
    // Reading the installed fonts takes seconds on Windows the first time: start it now, so the
    // first drawing's texts do not wait for it.
    tbcad::font::FontLibrary::warmSystemFonts();
    tbcad::app::App app(tbcad::app::parseOptions(argc, argv));
    return app.run();
}
