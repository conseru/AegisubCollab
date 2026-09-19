// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "document.h"
#include <wx/socket.h>
#include <chrono>
#include <memory>
namespace collab {
using Clock = std::chrono::steady_clock;
struct SocketPeer {
    std::unique_ptr<wxSocketBase> socket;
    std::string input, output, name;
    uint32_t ack=0;
    bool authenticated=false, hello=false;
    Clock::time_point last=Clock::now();
    explicit SocketPeer(wxSocketBase* s):socket(s) {socket->SetFlags(wxSOCKET_NOWAIT); socket->Notify(false);}
    void Queue(collab::Writer const& w) {
        if(output.size()+w.Bytes().size()+4>2*collab::MaxFrame) throw collab::Conflict("Connection is too slow; local edits are kept.");
        collab::Writer header; header.Number(static_cast<uint32_t>(w.Bytes().size())); output+=header.Bytes(); output+=w.Bytes();
    }
    void Flush() {
        if(output.empty()) return;
        socket->Write(output.data(),static_cast<wxUint32>(output.size()));
        output.erase(0,socket->LastCount());
        if(socket->Error() && socket->LastError()!=wxSOCKET_WOULDBLOCK) throw collab::Conflict("Connection lost. Local edits are kept.");
    }
    std::vector<std::string> Read() {
        std::vector<std::string> messages;
        // Bound work per UI tick; partial frames stay in the buffer.
        for(int i=0;i<16 && socket->IsData();++i) {
            char bytes[65536]; socket->Read(bytes,sizeof bytes); input.append(bytes,socket->LastCount());
            while(input.size()>=4) {
                std::string header=input.substr(0,4); collab::Reader r(header); auto size=r.Number();
                if(!size || size>collab::MaxFrame) throw collab::Conflict("Invalid incoming message size.");
                if(input.size()<size+4) break;
                messages.push_back(input.substr(4,size)); input.erase(0,size+4);
                if(messages.size()>64) throw collab::Conflict("Too many incoming messages.");
            }
        }
        if(!messages.empty()) last=Clock::now();
        return messages;
    }
};

}
