# Declares how this module registers itself. Read by cmake/command_modules.cmake.
tbcad_command_module(NAME drafting
                     REGISTER tbcad::draftingcmds::registerCommands
                     UI_REGISTER tbcad::draftingcmds::registerUi
                     TARGET tbcad_drafting_cmds)
