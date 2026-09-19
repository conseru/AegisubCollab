// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <memory>
#include <string>
namespace agi { struct Context; }
class AssDialogue;
class CollaborationController {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit CollaborationController(agi::Context*);
    ~CollaborationController();
    void Show();
    std::string AuthorFor(AssDialogue const*) const;
    std::string LastEditorFor(AssDialogue const*) const;
    std::string PresenceFor(AssDialogue const*) const;
};
