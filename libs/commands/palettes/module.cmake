# Tool palettes: one-click tools that insert a block, start a hatch or run a command with saved
# settings (PALETTES panel, -PALETTE command).
tbcad_command_module(NAME palettes
                     REGISTER tbcad::palettes::registerCommands
                     UI_REGISTER tbcad::palettes::registerUi)
