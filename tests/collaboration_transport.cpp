// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#include "../src/collaboration/transport.h"
#include <wx/init.h>
#include <iostream>
#include <thread>
using namespace collab;
void Check(bool value,char const* why) {if(!value) throw std::runtime_error(why);}
std::string Receive(SocketPeer& peer) {
    auto deadline=Clock::now()+std::chrono::seconds(3);
    while(Clock::now()<deadline) {
        auto messages=peer.Read();
        if(!messages.empty()) {Check(messages.size()==1,"expected one frame"); return messages.front();}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("loopback receive timed out");
}
int main() {
    try {
        wxInitializer init; Check(init.IsOk(),"wx initialization");
        wxIPV4address address; address.Hostname("127.0.0.1"); address.Service(0);
        wxSocketServer server(address,wxSOCKET_NOWAIT); server.Notify(false); Check(server.IsOk(),"listen");
        server.GetLocal(address);
        auto client=new wxSocketClient(wxSOCKET_NOWAIT); SocketPeer guest(client);
        client->Connect(address,false);
        Check(server.WaitForAccept(3),"accept timeout");
        SocketPeer host(server.Accept(false));
        Check(client->WaitOnConnect(3),"connect timeout");
        Writer hello; hello.String("hello"); hello.Number(3); hello.String("字幕 editor"); hello.String("test password");
        guest.Queue(hello); guest.Flush(); Check(Receive(host)==hello.Bytes(),"hello round trip");
        Document doc; Line line{std::string(32,'a'),{"0","0","0","1000","Default","[by Jake]","0","0","0","","こんにちは {\\i1}world"}};
        doc.lines.push_back(line); doc.settings={{"PlayResX","1920"},{"PlayResY","1080"}};
        Writer state; state.String("state"); state.Number(1); state.Number(0); state.Doc(doc);
        host.Queue(state); host.Flush(); auto bytes=Receive(guest); Reader r(bytes);
        Check(r.String()=="state" && r.Number()==1 && r.Number()==0 && r.Doc()==doc,"state round trip"); r.End();
        // A frame split across multiple TCP writes must not be applied early.
        Writer header; header.Number(static_cast<uint32_t>(state.Bytes().size())); auto frame=header.Bytes()+state.Bytes();
        host.socket->Write(frame.data(),2); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(guest.Read().empty(),"partial header accepted");
        host.socket->Write(frame.data()+2,7); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(guest.Read().empty(),"partial payload accepted");
        host.socket->Write(frame.data()+9,static_cast<wxUint32>(frame.size()-9));
        Check(Receive(guest)==state.Bytes(),"fragmented frame");
        Writer invalid; invalid.Number(MaxFrame+1); host.socket->Write(invalid.Bytes().data(),4);
        bool rejected=false; try {Receive(guest);} catch(Conflict const&) {rejected=true;}
        Check(rejected,"oversized frame accepted");
        std::cout<<"Native TCP checks passed (connection, UTF-8, document round trip, fragmentation, size limits).\n";
    }
    catch(std::exception const& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
