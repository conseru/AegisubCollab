// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <utility>
#include <vector>
namespace agi { struct Context; }
class AssDialogue;
class CollaborationController {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit CollaborationController(agi::Context*);
    ~CollaborationController();
    void Show();
    void ExportYTT();
    void ConvertYTFile();
    std::string AuthorFor(AssDialogue const*) const;
    std::string LastEditorFor(AssDialogue const*) const;
    std::string PresenceFor(AssDialogue const*) const;
    uint32_t PresenceColorFor(AssDialogue const*) const;
    uint32_t UserColor(std::string const&) const;
    std::vector<std::pair<std::string,int>> RemotePlayheads() const;
    int YTWarningCount(AssDialogue const*) const;
};
