#pragma once

// Drawing templates. A template is an ordinary .tbcad file with the extension .tbt; NEW starts a
// drawing from a copy of one. Without a template the built-in default is used: the usual layers,
// linetypes, a text style and two dimension styles, in millimetres.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tbcad/core/document.h"
#include "tbcad/core/result.h"

namespace tbcad::tmpl {

inline constexpr const char* kTemplateExtension = ".tbt";
// The name under which the built-in drawing is offered next to the user's templates.
inline constexpr const char* kBuiltinTemplate = "Built-in";

struct TemplateInfo {
    std::string name{};  // file name without the extension
    std::filesystem::path path{};
};

// The default drawing for NEW when the user has not chosen a template.
std::unique_ptr<Document> builtinTemplate();

// The .tbt files directly inside `dir`, sorted by name (case-insensitive). A missing folder gives
// an empty list.
std::vector<TemplateInfo> listTemplates(const std::filesystem::path& dir);

// Reads a template (or any .tbcad file) into a new document. The result has no file of its own, so
// saving it asks for a name.
Result<std::unique_ptr<Document>> loadTemplate(const std::filesystem::path& path);

// Writes `doc` as a template. `path` gets the .tbt extension when it has none. Creates the folder
// when needed. Returns the path written.
Result<std::filesystem::path> saveTemplate(const Document& doc, std::filesystem::path path);

// True for a path with the template extension (any case).
bool isTemplatePath(const std::filesystem::path& path);

// A file name made safe for a template: letters, digits, space, dash, underscore and non-ASCII
// characters stay; everything else becomes '_'. Empty input gives "template".
std::string sanitizeTemplateName(const std::string& name);

}  // namespace tbcad::tmpl
