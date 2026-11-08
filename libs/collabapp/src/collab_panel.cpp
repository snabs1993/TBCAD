#include "collab_panel.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>

#include "collab_controller.h"

namespace tbcad::collabapp {

namespace {
struct Form {
    std::string name{};
    std::string address{};
    int port = 47800;
    bool guestsMayEdit = true;
    std::string error{};
    std::string chat{};
};
Form& form() {
    static Form f;
    return f;
}

void drawRemembered(CollabController& c) {
    const auto known = c.rememberedComputers();
    if (known.empty())
        return;
    ImGui::SeparatorText("Computers that join without asking");
    for (std::size_t i = 0; i < known.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(known[i].name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Forget")) {
            c.forgetComputer(known[i].id);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
}
}  // namespace

void drawCollabPanel(CollabController& c, ui::PanelContext&) {
    Form& f = form();
    ImGui::TextUnformatted(c.status().c_str());
    if (!f.error.empty())
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1), "%s", f.error.c_str());

    if (c.mode() == CollabController::Mode::Off) {
        ImGui::SeparatorText("Work on this drawing together");
        if (ImGui::Button("Share this drawing", ImVec2(-1, 0))) {
            f.error.clear();
            (void)c.shareSimple(f.error);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped(
            "Others on this network see it in their list and click Join. You let them in.");
        ImGui::PopStyleColor();

        ImGui::SeparatorText("Join someone");
        const auto hosts = c.nearbyHosts();
        if (hosts.empty())
            ImGui::TextDisabled("Looking for shared drawings on this network...");
        for (std::size_t i = 0; i < hosts.size(); ++i) {
            const Announcement& a = hosts[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s's drawing", a.name.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50);
            if (ImGui::Button("Join", ImVec2(60, 0))) {
                f.error.clear();
                (void)c.joinSimple(a.address, a.port, f.name, f.error);
            }
            ImGui::PopID();
        }
        if (ImGui::TreeNode("Not in the list? Type the address")) {
            ImGui::InputText("Address", &f.address);
            ImGui::InputInt("Port", &f.port);
            ImGui::BeginDisabled(f.address.empty());
            if (ImGui::Button("Join this address")) {
                f.error.clear();
                (void)c.joinSimple(f.address, f.port, f.name, f.error);
            }
            ImGui::EndDisabled();
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("More")) {
            ImGui::InputText("Your name", &f.name);
            ImGui::TextDisabled("Empty: the computer's user name.");
            ImGui::TextDisabled("Both computers must be on the same network (or the same VPN,");
            ImGui::TextDisabled(
                "such as Tailscale). Allow TBCAD when Windows asks about the firewall.");
            drawRemembered(c);
            ImGui::TreePop();
        }
    } else {
        if (c.mode() == CollabController::Mode::Hosting) {
            const auto waiting = c.requests();
            for (const auto& r : waiting) {
                ImGui::PushID(static_cast<int>(r.conn));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.95f, 0.75f, 0.2f, 0.18f));
                if (ImGui::BeginChild("##ask", ImVec2(0, 0),
                                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
                    ImGui::Text("%s wants to join", r.name.c_str());
                    if (ImGui::Button("Let in"))
                        c.answer(r.conn, true, collab::Role::Editor, true);
                    ImGui::SameLine();
                    if (ImGui::Button("View only"))
                        c.answer(r.conn, true, collab::Role::Viewer, true);
                    ImGui::SameLine();
                    if (ImGui::Button("Refuse"))
                        c.answer(r.conn, false, collab::Role::Viewer);
                    if (!r.computer.empty())
                        ImGui::TextDisabled("Next time this computer joins without asking.");
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            const std::string addr = c.localAddress();
            if (!addr.empty())
                ImGui::TextDisabled("This computer: %s  port %d", addr.c_str(), c.port());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Others: File > Work Together, then Join on your drawing.");
            ImGui::PopStyleColor();
            bool ask = c.askBeforeJoin();
            if (ImGui::Checkbox("Ask me before anyone joins", &ask))
                c.setAskBeforeJoin(ask);
            bool edit = c.guestsMayEdit();
            if (ImGui::Checkbox("New guests may edit", &edit))
                c.setGuestsMayEdit(edit);
            drawRemembered(c);
        }
        if (c.mode() == CollabController::Mode::Offline) {
            if (ImGui::Button("Reconnect")) {
                f.error.clear();
                (void)c.reconnect(f.error);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Stop"))
            c.stop();
        if (!c.conflicts().empty()) {
            ImGui::SeparatorText("Offline edits that clash");
            for (std::size_t i = 0; i < c.conflicts().size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextWrapped("Your edit to %s", c.conflicts()[i].what.c_str());
                if (ImGui::SmallButton("Keep mine")) {
                    c.keepMine(i);
                    ImGui::PopID();
                    break;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Keep theirs")) {
                    c.keepTheirs(i);
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }
        ImGui::SeparatorText("People");
        for (const collab::PeerInfo& p : c.peers()) {
            ImGui::PushID(static_cast<int>(p.id));
            const Rgba col = CollabController::colorOf(p.id);
            ImGui::ColorButton("##c", ImVec4(col.r / 255.f, col.g / 255.f, col.b / 255.f, 1),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
            ImGui::SameLine();
            ImGui::Text("%s%s", p.name.c_str(),
                        p.role == collab::Role::Viewer ? " (view only)" : "");
            if (c.mode() == CollabController::Mode::Hosting) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove"))
                    (void)c.kick(p.id);
                ImGui::SameLine();
                if (ImGui::SmallButton("Make host")) {
                    f.error.clear();
                    (void)c.handOver(p.id, f.error);
                }
            }
            ImGui::PopID();
        }
    }
    if (c.mode() == CollabController::Mode::Hosting || c.mode() == CollabController::Mode::Joined) {
        ImGui::SeparatorText("Layers kept for one person");
        if (const Document* d = c.sharedDocument()) {
            const LayerId current = d->header().currentLayer;
            const auto owner = c.claims().ownerOf(current);
            const Layer* cl = d->layer(current);
            const std::string cname = cl ? cl->name : std::string("?");
            if (!owner) {
                if (ImGui::Button(("Keep layer \"" + cname + "\" for me").c_str()))
                    c.setClaim(current, true);
            }
            for (const auto& [layer, peer] : c.claims().table()) {
                ImGui::PushID(static_cast<int>(layer));
                const Layer* l = d->layer(LayerId{layer});
                const Rgba col = CollabController::colorOf(peer);
                ImGui::ColorButton("##c", ImVec4(col.r / 255.f, col.g / 255.f, col.b / 255.f, 1),
                                   ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
                ImGui::SameLine();
                ImGui::Text("%s: %s", l ? l->name.c_str() : "?", c.nameOf(peer).c_str());
                if (peer == c.self()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Release"))
                        c.setClaim(LayerId{layer}, false);
                }
                ImGui::PopID();
            }
        }
        ImGui::SeparatorText("Chat");
        for (const ChatLine& line : c.chat())
            ImGui::TextWrapped("%s: %s", c.nameOf(line.from).c_str(), line.text.c_str());
        if (ImGui::InputText("##chat", &f.chat, ImGuiInputTextFlags_EnterReturnsTrue)) {
            c.sendChat(f.chat);
            f.chat.clear();
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Send")) {
            c.sendChat(f.chat);
            f.chat.clear();
        }
    }
    ImGui::SeparatorText("Activity");
    if (ImGui::BeginChild("##collab-log", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (const std::string& line : c.log())
            ImGui::TextWrapped("%s", line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

}  // namespace tbcad::collabapp
