#pragma once

#include <string>

namespace tbcad {

// The one plain message shown when the graphics driver cannot give OpenGL 3.3 (no window
// could be created). `renderer` / `version` are what a basic probe context reported, "" if none.
inline std::string openGlProblemText(const std::string& renderer, const std::string& version) {
    std::string t = "TBCAD needs OpenGL 3.3 to draw, and this computer's graphics driver does not "
                    "provide it.\n\n";
    t += "Graphics: " + (renderer.empty() ? std::string("unknown") : renderer) + "\n";
    t += "OpenGL version: " + (version.empty() ? std::string("none") : version) + "\n\n";
    const bool noDriver = renderer.find("GDI Generic") != std::string::npos ||
                          renderer.find("Microsoft") != std::string::npos;
    if (noDriver)
        t += "Windows is using its basic display driver. Install the driver from the maker of "
             "your graphics card (Intel, NVIDIA or AMD), then start TBCAD again.";
    else
        t += "Update the graphics driver from the maker of your graphics card (Intel, NVIDIA or "
             "AMD), then start TBCAD again. If this is a remote desktop or virtual machine, run "
             "TBCAD on the computer itself.";
    return t;
}

// A message box on Windows; nothing elsewhere (the text is also printed to stderr).
void showOpenGlProblem(const std::string& text);

}  // namespace tbcad
