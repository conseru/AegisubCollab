// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#include "document.h"
#include <algorithm>
#include <limits>
#include <utility>
namespace collab {
namespace {
template<class T> T Choose(T const& b, T const& l, T const& r, std::string const& label) {
    if (l == b) return r;
    if (r == b || l == r) return l;
    throw Conflict("Both editors changed " + label + ". Local edits are kept; resolve the conflict before reconnecting.");
}
using Map = std::map<std::string, std::string>;
Map MergeMap(Map const& b, Map const& l, Map const& r, std::string const& label) {
    Map result;
    std::set<std::string> keys;
    for (auto const* m : {&b, &l, &r}) for (auto const& kv : *m) keys.insert(kv.first);
    using Value = std::pair<bool, std::string>;
    auto get = [](Map const& m, std::string const& k) -> Value {
        auto it = m.find(k); return it == m.end() ? Value{false, ""} : Value{true, it->second};
    };
    for (auto const& k : keys) {
        auto v = Choose(get(b,k), get(l,k), get(r,k), label + " " + k);
        if (v.first) result[k] = v.second;
    }
    return result;
}
int64_t Integer(std::string const& s) {
    if (s.empty() || s.size() > 10 || s.find_first_not_of("0123456789") != std::string::npos)
        throw Conflict("Invalid numeric subtitle field.");
    auto n = std::stoll(s);
    if (n > std::numeric_limits<int32_t>::max()) throw Conflict("Subtitle field out of range.");
    return n;
}
}
void Validate(Document const& doc) {
    if (doc.lines.size() > 20000 || doc.styles.size() > 2000 || doc.settings.size() > 32)
        throw Conflict("Project exceeds collaboration limits.");
    size_t bytes=12;
    auto size=[&](std::string const& value) {bytes+=4+value.size(); if(bytes>MaxFrame-64) throw Conflict("Project exceeds the 8 MB room limit.");};
    for(auto const& line:doc.lines) {size(line.id); for(auto const& field:line.fields) size(field);}
    for(auto const* map:{&doc.settings,&doc.styles}) for(auto const& kv:*map) {size(kv.first); size(kv.second);}
    std::set<std::string> ids;
    for (auto const& l : doc.lines) {
        if (l.id.size() != 32 || l.id.find_first_not_of("0123456789abcdef") != std::string::npos || !ids.insert(l.id).second)
            throw Conflict("Invalid or duplicate line identity.");
        for (auto const& f : l.fields) if (f.size() > 100000 || f.find('\0') != std::string::npos || f.find_first_of("\r\n") != std::string::npos)
            throw Conflict("Invalid subtitle text.");
        if (l.fields[0] != "0" && l.fields[0] != "1") throw Conflict("Invalid comment flag.");
        for (size_t i : {1u,2u,3u,6u,7u,8u}) Integer(l.fields[i]);
        if (Integer(l.fields[2]) > Integer(l.fields[3])) throw Conflict("A line ends before it starts.");
        for (size_t i : {4u,5u,9u}) if (l.fields[i].find(',') != std::string::npos) throw Conflict("Commas are not supported in style/actor/effect fields.");
    }
    static std::set<std::string> const settings = {"PlayResX","PlayResY","LayoutResX","LayoutResY","WrapStyle","ScaledBorderAndShadow","YCbCr Matrix"};
    for (auto const& kv : doc.settings) {
        if (!settings.count(kv.first) || kv.second.size() > 128 || kv.second.find_first_of("\r\n") != std::string::npos || kv.second.find('\0') != std::string::npos)
            throw Conflict("Invalid script setting.");
        if (kv.first.find("Res") != std::string::npos && !kv.second.empty() && (Integer(kv.second) < 1 || Integer(kv.second) > 100000))
            throw Conflict("Invalid script resolution.");
    }
    for (auto const& kv : doc.styles) if (kv.first.empty() || kv.first.size()>1024 || kv.second.size()>100000 ||
        kv.first.find('\0')!=std::string::npos || kv.first.find_first_of(",\r\n")!=std::string::npos || kv.second.find_first_of("\r\n")!=std::string::npos || kv.second.find('\0')!=std::string::npos)
        throw Conflict("Invalid style definition.");
}
Document Merge(Document const& base, Document const& local, Document const& remote, std::set<std::string> const& deleted) {
    Validate(base); Validate(local); Validate(remote);
    std::map<std::string, Line> b,l,r,result;
    std::map<std::string,size_t> rank;
    for (size_t i=0;i<base.lines.size();++i) { b.emplace(base.lines[i].id,base.lines[i]); rank[base.lines[i].id]=i+1; }
    size_t previous=0;
    for (auto const& x:local.lines) {
        l.emplace(x.id,x);
        if (rank.count(x.id)) {
            if (rank[x.id]<previous) throw Conflict("Moving/sorting existing lines during collaboration is not supported yet.");
            previous=rank[x.id];
        }
    }
    for (auto const& x:remote.lines) r.emplace(x.id,x);
    for (auto const& kv:b) {
        auto const& id=kv.first; auto const& old=kv.second;
        if (!l.count(id) || !r.count(id)) {
            if ((l.count(id) && l.at(id)!=old) || (r.count(id) && r.at(id)!=old))
                throw Conflict("A deletion conflicts with an edit to line " + std::to_string(rank[id]) + ". Local edits are kept.");
            continue;
        }
        Line x=old;
        for (size_t i=0;i<11;++i) x.fields[i]=Choose(old.fields[i],l.at(id).fields[i],r.at(id).fields[i],"line " + std::to_string(rank[id]));
        auto pair=[](Line const& a){return std::make_pair(a.fields[2],a.fields[3]);};
        auto timing=Choose(pair(old),pair(l.at(id)),pair(r.at(id)),"timing on line " + std::to_string(rank[id]));
        x.fields[2]=timing.first; x.fields[3]=timing.second; result.emplace(id,std::move(x));
    }
    for (auto const& kv:r) if (!b.count(kv.first)) result.emplace(kv);
    for (auto const& kv:l) if (!b.count(kv.first)) {
        if (deleted.count(kv.first)) throw Conflict("A stale addition was already deleted remotely. Local edits are kept.");
        if (r.count(kv.first) && r.at(kv.first)!=kv.second) throw Conflict("A newly added line has conflicting edits.");
        result[kv.first]=kv.second;
    }
    std::vector<std::string> order, pending;
    std::set<std::string> present;
    for (auto const& x:remote.lines) if (result.count(x.id)) {order.push_back(x.id); present.insert(x.id);}
    for (auto const& x:local.lines) {
        if (!result.count(x.id)) continue;
        if (present.count(x.id)) {
            auto at=std::find(order.begin(),order.end(),x.id);
            order.insert(at,pending.begin(),pending.end()); present.insert(pending.begin(),pending.end()); pending.clear();
        }
        else pending.push_back(x.id);
    }
    order.insert(order.end(),pending.begin(),pending.end());
    Document out;
    for (auto const& id:order) out.lines.push_back(result.at(id));
    // Treat coordinate settings together to prevent mismatched width/height pairs.
    out.settings=Choose(base.settings,local.settings,remote.settings,"script resolution/settings");
    out.styles=MergeMap(base.styles,local.styles,remote.styles,"style");
    Validate(out); return out;
}
Room::Room(Document initial) { Validate(initial); history.emplace(1,std::move(initial)); }
bool Room::Apply(uint64_t base,Document const& proposed) {
    auto it=history.find(base);
    if (it==history.end()) throw Conflict("Unknown base revision; reconnect using a saved copy.");
    auto merged=Merge(it->second,proposed,Current(),deleted);
    if (merged==Current()) return false;
    std::set<std::string> surviving; for(auto const& x:merged.lines) surviving.insert(x.id);
    auto next_deleted=deleted;
    for(auto const& x:Current().lines) if(!surviving.count(x.id)) next_deleted.insert(x.id);
    // Publish only after all validation and allocations have succeeded.
    history.emplace(revision+1,std::move(merged)); deleted.swap(next_deleted); ++revision;
    // Bound revision history. Slow clients fail safely instead of merging against a guessed base.
    while(history.size()>32) history.erase(history.begin());
    return true;
}
void Writer::Number(uint32_t n) { for (int i=3;i>=0;--i) bytes.push_back(static_cast<char>((n>>(i*8))&255)); if(bytes.size()>MaxFrame) throw Conflict("Message exceeds 8 MB."); }
void Writer::String(std::string const& s) { if(s.size()>MaxFrame || bytes.size()+4+s.size()>MaxFrame) throw Conflict("Message exceeds 8 MB."); Number(static_cast<uint32_t>(s.size())); bytes+=s; }
void Writer::Doc(Document const& doc) {
    Validate(doc); Number(static_cast<uint32_t>(doc.lines.size()));
    for(auto const& l:doc.lines) {String(l.id); for(auto const& f:l.fields) String(f);}
    for(auto const* map:{&doc.settings,&doc.styles}) {Number(static_cast<uint32_t>(map->size())); for(auto const& kv:*map){String(kv.first); String(kv.second);}}
}
uint32_t Reader::Number() { if(bytes.size()>MaxFrame || pos+4>bytes.size()) throw Conflict("Truncated message."); uint32_t n=0; for(int i=0;i<4;++i) n=(n<<8)|static_cast<unsigned char>(bytes[pos++]); return n; }
std::string Reader::String() {auto n=Number(); if(n>MaxFrame || n>bytes.size()-pos) throw Conflict("Invalid message field length."); auto s=bytes.substr(pos,n); pos+=n; return s;}
Document Reader::Doc() {
    Document doc; auto count=Number(); if(count>20000) throw Conflict("Too many subtitle lines.");
    for(uint32_t i=0;i<count;++i) {Line l; l.id=String(); for(auto& f:l.fields) f=String(); doc.lines.push_back(std::move(l));}
    for(auto* m:{&doc.settings,&doc.styles}) {auto n=Number(); if(n>2000) throw Conflict("Too many settings/styles."); for(uint32_t i=0;i<n;++i) {auto k=String(),v=String(); if(!m->emplace(k,v).second) throw Conflict("Duplicate map key.");}}
    Validate(doc); return doc;
}
void Reader::End() const {if(pos!=bytes.size()) throw Conflict("Unexpected trailing message data.");}
}
