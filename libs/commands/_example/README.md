# Template command module

Copy this folder to `libs/commands/<yourmodule>/` and rename:

1. `CMakeLists.txt`: `tbcad_add_library(<yourmodule> ...)` (target `tbcad_<yourmodule>`); link
   `tbcad::cmd` plus whatever else you need. Add tests with `tbcad_add_tests`.
2. `module.cmake`: `tbcad_command_module(NAME <yourmodule> REGISTER tbcad::<yourmodule>::registerCommands)`,
   and `UI_REGISTER tbcad::<yourmodule>::registerUi` if you have panels. Drop `EXAMPLE`.
3. `include/tbcad/<yourmodule>/<yourmodule>.h`: declares `registerCommands(cmd::Registry&)`.
4. One `cmd::Command` subclass per command; `run()` is a coroutine that `co_await`s prompts.

Nothing outside your folder needs editing: CMake finds `module.cmake` and generates the
registration (`tbcad::registerAllCommands`) for the app. Test commands without a UI with
`cmd::ScriptHost` (see `tests/example_tests.cpp`).
