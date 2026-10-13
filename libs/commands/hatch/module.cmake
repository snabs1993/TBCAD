# HATCH and HATCHEDIT. The panel and the property section need Dear ImGui, which only exists in
# application builds, so the UI hook is declared only then.
if(TBCAD_BUILD_APP)
  tbcad_command_module(NAME hatch_commands
                       REGISTER tbcad::hatchcmd::registerCommands
                       UI_REGISTER tbcad::hatchcmd::registerUi)
else()
  tbcad_command_module(NAME hatch_commands
                       REGISTER tbcad::hatchcmd::registerCommands)
endif()
