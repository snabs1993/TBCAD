#include <set>

#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

const std::set<std::string>& deniedCommands() {
    // Developer commands and anything that would open a file dialog or leave the program; files
    // have their own tools.
    static const std::set<std::string> k{"DEVGEN", "OPEN", "SAVE", "SAVEAS", "QSAVE",
                                         "NEW",    "QNEW", "QUIT", "EXIT",   "CLOSE"};
    return k;
}

Result<cmd::Input> parseInput(const Json& j) {
    if (j.isString()) {
        const std::string& s = j.asString();
        if (s == "<Esc>")
            return cmd::Input::cancel();
        if (s.empty())
            return cmd::Input::enter();
        return cmd::Input::typed(s);
    }
    if (j.isObject()) {
        if (const Json* c = j.find("click")) {
            auto p = pointFromJson(*c);
            if (!p)
                return makeError(ErrorCode::InvalidArgument, "click needs [x, y]");
            return cmd::Input::click(p.value());
        }
        if (const Json* c = j.find("pick")) {
            auto id = parseId(*c);
            if (!id)
                return makeError(ErrorCode::InvalidArgument, "pick needs an entity id");
            Vec2 at{};
            if (const Json* a = j.find("at")) {
                auto p = pointFromJson(*a);
                if (p)
                    at = p.value();
            }
            return cmd::Input::pick(*id, at);
        }
        if (const Json* c = j.find("select")) {
            std::vector<EntityId> ids;
            for (const auto& e : c->asArray()) {
                auto id = parseId(e);
                if (!id)
                    return makeError(ErrorCode::InvalidArgument, "select needs entity ids");
                ids.push_back(*id);
            }
            return cmd::Input::select(std::move(ids));
        }
        if (const Json* c = j.find("text"); c && c->isString())
            return cmd::Input::typed(c->asString());
    }
    return makeError(ErrorCode::InvalidArgument,
                     "an input is a string, \"<Esc>\", {click:[x,y]}, {pick:id} or {select:[ids]}");
}

std::string describeInput(const Json& j) {
    std::string t = dump(j);
    if (t.size() > 60)
        t = t.substr(0, 57) + "...";
    return t;
}

}  // namespace

CommandRunReport runScripted(DrawingSession& s, const std::string& command,
                             const std::vector<Json>& inputs, bool allowIncomplete) {
    CommandRunReport rep;
    cmd::CommandRunner& runner = s.runner();
    cmd::Command* c = s.registry().find(command);
    if (!c) {
        rep.error = "unknown command \"" + command + "\". Use list_commands to see what exists.";
        return rep;
    }
    std::string name(c->name());
    if (c->description().empty() || deniedCommands().count(name)) {
        rep.error = "command " + name +
                    " cannot be run from here (use the dedicated tools for files, or do it in "
                    "TBCAD yourself)";
        return rep;
    }
    std::size_t mark = s.messageCount();
    auto notePrompt = [&] {
        if (const cmd::PromptInfo* p = runner.prompt())
            rep.prompts.push_back(p->display());
    };
    auto started = runner.start(name);
    if (!started) {
        rep.error = started.error().message;
        return rep;
    }
    notePrompt();
    for (std::size_t i = 0; i < inputs.size() && runner.active(); ++i) {
        auto in = parseInput(inputs[i]);
        if (!in) {
            rep.error = "inputs[" + std::to_string(i) + "]: " + in.error().message;
            break;
        }
        std::string promptBefore = runner.prompt() ? runner.prompt()->display() : std::string();
        cmd::SubmitResult r = runner.submit(in.value());
        if (r == cmd::SubmitResult::Rejected || r == cmd::SubmitResult::Ignored) {
            rep.error = "the input " + describeInput(inputs[i]) + " (inputs[" + std::to_string(i) +
                        "]) was not accepted at the prompt \"" + promptBefore + "\"";
            break;
        }
        notePrompt();
    }
    if (runner.active()) {
        if (const cmd::PromptInfo* p = runner.prompt())
            rep.pendingPrompt = p->display();
        if (!allowIncomplete) {
            for (int i = 0; i < 8 && runner.active(); ++i)
                (void)runner.submit(cmd::Input::cancel());
            if (rep.error.empty()) {
                rep.error = "command " + name + " is still waiting at \"" + rep.pendingPrompt +
                            "\" after the last input. Send more inputs, or set allow_incomplete.";
            }
        }
    }
    rep.finished = !runner.active();
    rep.messages = s.messagesSince(mark);
    return rep;
}

namespace {

Json stringsJson(const std::vector<std::string>& v) {
    Json::Array a;
    for (const auto& s : v)
        a.emplace_back(s);
    return Json(std::move(a));
}

Json limitedIds(const std::vector<EntityId>& ids, std::size_t cap = 1000) {
    std::vector<EntityId> part(
        ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(std::min(ids.size(), cap)));
    return idsJson(part);
}

std::string failureText(const CommandRunReport& rep) {
    std::string text = rep.error;
    if (!rep.prompts.empty()) {
        text += " Prompts seen: ";
        for (std::size_t i = 0; i < rep.prompts.size(); ++i)
            text += (i ? " | " : "") + rep.prompts[i];
        text += ".";
    }
    if (!rep.messages.empty()) {
        text += " Messages: ";
        for (std::size_t i = 0; i < rep.messages.size(); ++i)
            text += (i ? " | " : "") + rep.messages[i];
        text += ".";
    }
    return text;
}

ToolResult runWith(ToolContext& ctx, const std::string& command, const std::vector<Json>& inputs,
                   bool allowIncomplete) {
    DrawingSession& s = ctx.session;
    ChangeRecorder rec(s.document());
    CommandRunReport rep = runScripted(s, command, inputs, allowIncomplete);
    if (!rep.error.empty())
        return ToolResult::fail(failureText(rep));
    Json::Object o;
    o["finished"] = rep.finished;
    o["messages"] = stringsJson(rep.messages);
    o["prompts"] = stringsJson(rep.prompts);
    if (!rep.finished)
        o["waitingAt"] = rep.pendingPrompt;
    auto created = rec.created(), changed = rec.changed(), erased = rec.erased();
    o["created"] = limitedIds(created);
    o["changed"] = limitedIds(changed);
    o["erased"] = limitedIds(erased);
    o["createdCount"] = static_cast<double>(created.size());
    o["changedCount"] = static_cast<double>(changed.size());
    o["erasedCount"] = static_cast<double>(erased.size());
    return ToolResult::ok(Json(std::move(o)));
}

ToolResult runCommand(ToolContext& ctx, const Json& a) {
    std::vector<Json> inputs;
    if (const Json* in = a.find("inputs"))
        inputs = in->asArray();
    return runWith(ctx, argString(a, "command"), inputs, argBool(a, "allow_incomplete"));
}

bool pathEndsWith(const std::string& p, const char* ext) {
    std::string e = ext;
    if (p.size() < e.size())
        return false;
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(p[p.size() - e.size() + i])) != e[i])
            return false;
    }
    return true;
}

ToolResult exportPdf(ToolContext& ctx, const Json& a) {
    std::string path = argString(a, "path");
    if (!pathEndsWith(path, ".pdf"))
        return ToolResult::fail("path must end with .pdf");
    std::vector<Json> inputs;
    auto option = [&](const char* arg, const char* keyword) {
        if (const Json* v = a.find(arg); v && v->isString() && !v->asString().empty()) {
            inputs.emplace_back(keyword);
            inputs.emplace_back(v->asString());
        }
    };
    option("paper", "Paper");
    option("orientation", "Orientation");
    option("scale", "Scale");
    option("area", "Area");
    inputs.emplace_back("Export");
    inputs.emplace_back(path);
    return runWith(ctx, "EXPORTPDF", inputs, false);
}

ToolResult exportDxf(ToolContext& ctx, const Json& a) {
    std::string path = argString(a, "path");
    if (!pathEndsWith(path, ".dxf"))
        return ToolResult::fail("path must end with .dxf");
    std::vector<Json> inputs{Json(path)};
    std::string ver = argString(a, "version");
    inputs.emplace_back(ver);  // "" = Enter = the default version
    return runWith(ctx, ctx.session.registry().find("EXPORTDXF") ? "EXPORTDXF" : "DXFOUT", inputs,
                   false);
}

}  // namespace

void registerCommandTools(ToolSet& tools) {
    tools.add({"run_command", "Run a TBCAD command",
               "Runs any TBCAD command (see list_commands) exactly as if typed, feeding `inputs` "
               "to its prompts in order. Inputs: a string (\"0,0\", \"@100,0\", \"C\"; \"\" = "
               "Enter; \"<Esc>\"), {click:[x,y]}, {pick:id}, {select:[ids]}. If the command still "
               "waits after the last input it is cancelled, the work rolled back, and the error "
               "names the prompt it stopped at. One undo step. Returns messages, prompts seen and "
               "the ids created/changed/erased.",
               tObject({{"command", tString("command name or alias, e.g. LINE or L")},
                        {"inputs", tArray(tAny())},
                        {"allow_incomplete", tBool("leave the command waiting for the user")}},
                       {"command"}),
               outSchema({"finished", "messages", "prompts", "created", "changed", "erased"}),
               Edit::Document, false, false, true, runCommand});

    Tool pdf{"export_pdf",
             "Export PDF",
             "Writes the drawing to a PDF at the given scale (the EXPORTPDF command).",
             tObject({{"path", tString("absolute path ending in .pdf")},
                      {"paper", tString("A0..A4, ANSI A..E")},
                      {"orientation", tString("Portrait or Landscape")},
                      {"scale", tString("Fit or 1:N, e.g. 1:100")},
                      {"area", tString("Extents or Display")}},
                     {"path"}),
             outSchema({"finished", "messages"}),
             Edit::Session,
             false,
             false,
             true,
             exportPdf};
    pdf.available = [](DrawingSession& s) { return s.registry().find("EXPORTPDF") != nullptr; };
    tools.add(std::move(pdf));

    Tool dxf{"export_dxf",
             "Export DXF",
             "Writes the drawing to a DXF file (the EXPORTDXF command).",
             tObject({{"path", tString("absolute path ending in .dxf")},
                      {"version", tString("R12, R2000 or R2007; default R2000")}},
                     {"path"}),
             outSchema({"finished", "messages"}),
             Edit::Session,
             false,
             false,
             true,
             exportDxf};
    dxf.available = [](DrawingSession& s) {
        return s.registry().find("EXPORTDXF") != nullptr || s.registry().find("DXFOUT") != nullptr;
    };
    tools.add(std::move(dxf));
}

}  // namespace tbcad::mcp::detail
