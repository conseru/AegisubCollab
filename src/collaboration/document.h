// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace collab {
// comment, layer, start, end, style, actor, margin L/R/V, effect, text,
// collaboration author, collaboration last editor.
struct Line {
    std::string id;
    std::array<std::string, 13> fields;
    bool operator==(Line const& other) const { return id == other.id && fields == other.fields; }
    bool operator!=(Line const& other) const { return !(*this == other); }
};
struct Document {
    std::vector<Line> lines;
    std::map<std::string, std::string> settings, styles;
    bool operator==(Document const& o) const { return lines == o.lines && settings == o.settings && styles == o.styles; }
    bool operator!=(Document const& o) const { return !(*this == o); }
};
struct Conflict : std::runtime_error { using std::runtime_error::runtime_error; };
void Validate(Document const&);
Document Merge(Document const& base, Document const& local, Document const& remote,
               std::set<std::string> const& deleted = {});
class Room {
    std::map<uint64_t, Document> history;
    std::set<std::string> deleted;
    uint64_t revision = 1;
public:
    explicit Room(Document initial);
    uint64_t Revision() const { return revision; }
    Document const& Current() const { return history.at(revision); }
    bool Apply(uint64_t base, Document const& proposed);
};
// A bounded, length-prefixed wire format (no parsing of executable code).
class Writer {
    std::string bytes;
public:
    void Number(uint32_t n);
    void String(std::string const& s);
    void Doc(Document const& doc);
    std::string const& Bytes() const { return bytes; }
};
class Reader {
    std::string const& bytes;
    size_t pos = 0;
public:
    explicit Reader(std::string const& bytes) : bytes(bytes) {}
    uint32_t Number();
    std::string String();
    Document Doc();
    void End() const;
};
constexpr size_t MaxFrame = 8 * 1024 * 1024;
}
