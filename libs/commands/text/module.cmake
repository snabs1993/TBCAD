# TEXT, MTEXT, STYLE, TEXTEDIT and the text editor panel (W4-4). The module is called "textcmd"
# because the layout library already owns the target name tbcad_text.
tbcad_command_module(NAME textcmd
                     REGISTER tbcad::textcmd::registerCommands
                     UI_REGISTER tbcad::textcmd::registerUi)
