// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <memory>
namespace agi { struct Context; }
class CollaborationController {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit CollaborationController(agi::Context*);
    ~CollaborationController();
    void Show();
};
