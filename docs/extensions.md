# Extension modules

TBCAD can be built together with an extra source tree that is kept outside this repository (for
example the private tree of paid add-ons). Nothing in this repository depends on it.

    cmake -S . -B build -DTBCAD_PRO_DIR=/path/to/extension

The folder must contain a `CMakeLists.txt` that defines the static target `tbcad_pro`, and the header
`<tbcad/pro/pro.h>` declaring

    namespace tbcad::pro {
    // `wake` may be called from any thread: it makes the window run one more loop turn.
    // The returned function (may be empty) runs on the main thread once per loop turn.
    std::function<void()> attach(ui::AppCore& core, cmd::Registry& commands, ui::UiRegistry& ui,
                                 std::function<void()> wake);
    }

The program defines `TBCAD_HAS_PRO` and calls `attach` once at start-up, after the built-in commands and
panels are registered. The extension uses the same public interfaces as any module: commands, panels,
overlay drawers, `AppCore::adoptDocument` and the `DocumentObserver` hooks of the core library.
