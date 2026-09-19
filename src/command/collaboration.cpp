// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#include "command.h"
#include "../collaboration_controller.h"
#include "../include/aegisub/context.h"
namespace {
struct session final : cmd::Command {
    CMD_NAME("collab/session")
    STR_MENU("&Collaborate…")
    STR_DISP("Collaborate")
    STR_HELP("Host or join a live subtitle editing room")
    void operator()(agi::Context* c) override {
        if(!c->collaboration) c->collaboration=std::make_unique<CollaborationController>(c);
        c->collaboration->Show();
    }
};
}
namespace cmd { void init_collaboration() {reg(std::make_unique<session>());} }
