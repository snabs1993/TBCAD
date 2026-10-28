#pragma once

#include <string>

namespace tbcad {

// The one plain message shown when the graphics driver cannot give OpenGL 3.3 (no window
// could be created). `renderer` / `version` are what a basic probe context reported, "" if none.
inline std::string openGlProblemText(const std::string& renderer, const std::string& version) {
    std::string t =
        "TBCAD needs OpenGL 3.3 to draw, and this computer's graphics driver does not "
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
    t += "\n\nTo draw without a graphics driver (slower), start tbcad.exe with --software-gl "
         "(needs the software-gl folder next to tbcad.exe).";
    return t;
}

// A message box on Windows; nothing elsewhere (the text is also printed to stderr).
// When `offerSoftware` is set the box asks whether to restart with software drawing and returns
// true on "Yes".
bool showOpenGlProblem(const std::string& text, bool offerSoftware = false);

// Optional software OpenGL (Mesa llvmpipe, MIT): if the installer folder holds
// software-gl/opengl32.dll, load it instead of the system driver. Windows only; false elsewhere.
bool softwareGlAvailable();
bool useSoftwareGl();
// Start this program again with --software-gl (same arguments). Windows only.
bool relaunchWithSoftwareGl();

}  // namespace tbcad
