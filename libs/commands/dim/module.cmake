# DIMLINEAR, DIMALIGNED, DIMRADIUS, DIMDIAMETER, DIMANGULAR, LEADER and DIMSTYLE. The style panel
# needs Dear ImGui, which only exists in application builds, so the UI hook is declared only then.
if(TBCAD_BUILD_APP)
  tbcad_command_module(NAME dim_commands
                       REGISTER tbcad::dimcmd::registerCommands
                       UI_REGISTER tbcad::dimcmd::registerUi)
else()
  tbcad_command_module(NAME dim_commands
                       REGISTER tbcad::dimcmd::registerCommands)
endif()
