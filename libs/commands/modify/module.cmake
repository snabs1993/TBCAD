# Modify commands: OFFSET TRIM EXTEND FILLET CHAMFER BREAK BREAKATPOINT JOIN ERASE.
# The target is tbcad_modify_commands because tbcad_modify is the algorithm library.
tbcad_command_module(NAME modify
                     TARGET tbcad_modify_commands
                     REGISTER tbcad::modifycmd::registerCommands)
