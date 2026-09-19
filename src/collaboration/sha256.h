// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>
#include <libaegisub/fs.h>

namespace collab {
std::string FileSha256Hex(agi::fs::path const& path);
}
