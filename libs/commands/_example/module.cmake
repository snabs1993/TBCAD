# Declares how this module registers its commands. Read by cmake/command_modules.cmake.
# EXAMPLE: only registered when TBCAD_REGISTER_EXAMPLE_COMMANDS is ON. Real modules omit it.
tbcad_command_module(NAME example
                     REGISTER tbcad::example::registerCommands
                     EXAMPLE)
