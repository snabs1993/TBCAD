#include "app.h"

int main(int argc, char** argv) {
    tbcad::app::App app(tbcad::app::parseOptions(argc, argv));
    return app.run();
}
