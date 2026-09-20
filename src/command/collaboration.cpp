// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#include "command.h"
#include "../collaboration_controller.h"
#include "../include/aegisub/context.h"

namespace {
CollaborationController* Controller(agi::Context* c) {
    if(!c->collaboration) c->collaboration=std::make_unique<CollaborationController>(c);
    return c->collaboration.get();
}

struct session final : cmd::Command {
    CMD_NAME("collab/session")
    STR_MENU("&Collaborate...")
    STR_DISP("Collaborate")
    STR_HELP("Host or join a live subtitle editing room")
    void operator()(agi::Context* c) override { Controller(c)->Show(); }
};

struct export_ytt final : cmd::Command {
    CMD_NAME("ytsub/export_ytt")
    STR_MENU("Export as YouTube YTT...")
    STR_DISP("Export as YouTube YTT")
    STR_HELP("Export the current subtitles directly to YouTube Timed Text using YTSubConverter")
    void operator()(agi::Context* c) override { Controller(c)->ExportYTT(); }
};

struct convert_ytsub final : cmd::Command {
    CMD_NAME("ytsub/convert")
    STR_MENU("Convert with YTSubConverter...")
    STR_DISP("Convert with YTSubConverter")
    STR_HELP("Convert ASS, YTT, SRV3, SRT or SBV subtitle files")
    void operator()(agi::Context* c) override { Controller(c)->ConvertYTFile(); }
};
}

namespace cmd {
void init_collaboration() {
    reg(std::make_unique<session>());
    reg(std::make_unique<export_ytt>());
    reg(std::make_unique<convert_ytsub>());
}
}
