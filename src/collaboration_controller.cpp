// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#include "collaboration_controller.h"
#include "collaboration/document.h"
#include "collaboration/transport.h"
#include "include/aegisub/context.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "subtitle_format_ass.h"
#include <libaegisub/vfr.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/socket.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <random>

namespace {
using Clock = std::chrono::steady_clock;
constexpr char IdKey[] = "aegisub-collab-id";
constexpr int Port = 49215;
const std::vector<std::string> Settings = {"PlayResX","PlayResY","LayoutResX","LayoutResY","WrapStyle","ScaledBorderAndShadow","YCbCr Matrix"};
std::string Utf8(wxString const& s) { return s.ToStdString(wxConvUTF8); }
wxString Wx(std::string const& s) { return wxString::FromUTF8(s); }
std::string NewId() {
    static std::random_device rng;
    const char* hex = "0123456789abcdef";
    std::string id;
    for (int i=0;i<32;++i) id += hex[rng() & 15];
    return id;
}
void CheckName(std::string const& s) {
    if (s.empty() || s.size()>40 || s.find_first_of(",\r\n[]")!=std::string::npos || s.find('\0')!=std::string::npos)
        throw collab::Conflict("Enter a name (1–40 bytes, without commas or brackets).");
}
std::string Identity(AssFile* file, AssDialogue const& line) {
    for (auto const& e:file->GetExtradata(line.ExtradataIds)) if(e.key==IdKey) return e.value;
    return {};
}
void SetIdentity(AssFile* file, AssDialogue& line, std::string const& id) {
    std::vector<uint32_t> ids;
    for (auto const& e:file->GetExtradata(line.ExtradataIds)) if(e.key!=IdKey) ids.push_back(e.id);
    ids.push_back(file->AddExtradata(IdKey,id)); line.ExtradataIds=ids;
}
collab::Line ReadLine(AssDialogue const& x, std::string id) {
    return {std::move(id), {x.Comment?"1":"0", std::to_string(x.Layer),std::to_string(int(x.Start)),std::to_string(int(x.End)),
        x.Style.get(),x.Actor.get(),std::to_string(x.Margin[0]),std::to_string(x.Margin[1]),std::to_string(x.Margin[2]),x.Effect.get(),x.Text.get()}};
}
void WriteLine(AssDialogue& x, collab::Line const& l) {
    auto const& f=l.fields;
    x.Comment=f[0]=="1"; x.Layer=std::stoi(f[1]); x.Start=std::stoi(f[2]); x.End=std::stoi(f[3]);
    x.Style=f[4]; x.Actor=f[5]; for(int i=0;i<3;++i) x.Margin[i]=std::stoi(f[6+i]); x.Effect=f[9]; x.Text=f[10];
}
// All peers parse styles before accepting a state, including the room owner.
void CheckStyles(collab::Document const& doc) {
    if(doc.lines.empty() || doc.styles.empty()) throw collab::Conflict("Aegisub rooms need at least one line and one style.");
    std::set<wxString> names;
    for(auto const& kv:doc.styles) {
        AssStyle style(kv.second);
        if(style.name!=kv.first || !names.insert(Wx(kv.first).Lower()).second)
            throw collab::Conflict("Invalid or duplicate style name.");
    }
}
using Peer = collab::SocketPeer;}

struct CollaborationController::Impl : wxEvtHandler {
    agi::Context* c;
    wxTimer timer{this};
    wxWeakRef<wxDialog> window;
    wxTextCtrl *nameBox=nullptr,*addressBox=nullptr,*passwordBox=nullptr;
    wxStaticText *status=nullptr,*people=nullptr;
    wxButton *hostButton=nullptr,*joinButton=nullptr,*leaveButton=nullptr;
    std::unique_ptr<wxSocketServer> listener;
    std::vector<std::unique_ptr<Peer>> peers;
    std::unique_ptr<collab::Room> room;
    collab::Document base, sent;
    uint32_t revision=0, sequence=0;
    bool ignoring=false, connected=false, inFlight=false, ticking=false, dirty=false;
    std::map<int,std::string> nativeIds;
    std::string name,password;
    Clock::time_point changed=Clock::now(), heartbeat=Clock::now(), checked=Clock::now();
    agi::signal::Connection commit, fileOpened;

    explicit Impl(agi::Context* context):c(context) {
        Bind(wxEVT_TIMER,[this](wxTimerEvent&) {Tick();});
        commit=c->ass->AddCommitListener([this](int type,const AssDialogue*) {
            if(ignoring) return;
            (void)type; // Undo also uses COMMIT_NEW; the file-open signal handles real file changes.
            changed=Clock::now(); dirty=true;
        });
        fileOpened=c->subsController->AddFileOpenListener([this](agi::fs::path const&) {Stop("Disconnected: another subtitle file was opened.");});
    }
    ~Impl() {timer.Stop(); peers.clear(); listener.reset(); if(window) {window->Unbind(wxEVT_CLOSE_WINDOW,&Impl::OnClose,this); window->Destroy();}}
    void OnClose(wxCloseEvent& e) {if(e.CanVeto()) {window->Hide(); e.Veto();} else e.Skip();}
    void Status(std::string const& text) {if(status) {status->SetLabel(Wx(text)); status->Wrap(430); window->Layout();}}
    void Buttons() {
        bool active=listener || !peers.empty();
        hostButton->Enable(!active); joinButton->Enable(!active); leaveButton->Enable(active);
        nameBox->Enable(!active); addressBox->Enable(!active); passwordBox->Enable(!active);
    }
    void Stop(std::string const& reason) {
        timer.Stop(); peers.clear(); listener.reset(); room.reset(); connected=false; inFlight=false; ignoring=false; nativeIds.clear(); dirty=false;
        if(window) {Status(reason); people->SetLabel("Nobody connected"); Buttons();}
    }
    void Guard(std::function<void()> fn) {
        try {fn();}
        catch(std::exception const& e) {Stop(e.what()); if(window) {window->Show(); window->Raise();}}
        catch(...) {Stop("Could not sync this project. Local edits are kept."); if(window) window->Show();}
    }
    void Show() {
        if(!window) {
            window=new wxDialog(c->parent,wxID_ANY,"Collaborate",wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE);
            auto root=new wxBoxSizer(wxVERTICAL);
            auto title=new wxStaticText(window,wxID_ANY,"Edit subtitles together");
            auto font=title->GetFont(); font.SetPointSize(font.GetPointSize()+3); font.SetWeight(wxFONTWEIGHT_BOLD); title->SetFont(font);
            root->Add(title,0,wxALL,16);
            auto hint=new wxStaticText(window,wxID_ANY,"Connect both PCs to the same Hamachi network first."); root->Add(hint,0,wxLEFT|wxRIGHT|wxBOTTOM,16);
            auto field=[&](char const* label,wxTextCtrl*& box,long style=0) {
                root->Add(new wxStaticText(window,wxID_ANY,label),0,wxLEFT|wxRIGHT,16);
                box=new wxTextCtrl(window,wxID_ANY,"",wxDefaultPosition,wxSize(430,-1),style);
                root->Add(box,0,wxEXPAND|wxLEFT|wxRIGHT|wxTOP|wxBOTTOM,8);
            };
            field("Your name",nameBox); nameBox->SetMaxLength(40);
            field("Hamachi IPv4 address — yours to host, your friend's to join",addressBox);
            field("Room password — agree on this with your friend",passwordBox,wxTE_PASSWORD);
            auto buttons=new wxBoxSizer(wxHORIZONTAL);
            hostButton=new wxButton(window,wxID_ANY,"Host room"); joinButton=new wxButton(window,wxID_ANY,"Join room"); leaveButton=new wxButton(window,wxID_ANY,"Disconnect");
            for(auto b:{hostButton,joinButton,leaveButton}) buttons->Add(b,1,wxRIGHT,8);
            root->Add(buttons,0,wxEXPAND|wxALL,16);
            status=new wxStaticText(window,wxID_ANY,"Open the subtitles you want to share, then host a room."); status->Wrap(430);
            people=new wxStaticText(window,wxID_ANY,"Nobody connected");
            root->Add(status,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,16); root->Add(people,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,16);
            window->SetSizerAndFit(root); window->CentreOnParent();
            window->Bind(wxEVT_CLOSE_WINDOW,&Impl::OnClose,this);
            hostButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Guard([this]{Start(true);});});
            joinButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Guard([this]{Start(false);});});
            leaveButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Stop("Disconnected. Your subtitles stay open.");});
            Buttons();
        }
        window->Show(); window->Raise();
    }
    collab::Document Snapshot() {
        collab::Document doc; std::set<std::string> seen;
        std::vector<std::pair<AssDialogue*,collab::Line>> assigned;
        std::map<std::string,int> owners;
        for(auto const& x:c->ass->Events) {
            auto it=nativeIds.find(x.Id);
            if(it!=nativeIds.end()) owners[it->second]=x.Id;
        }
        for(auto& x:c->ass->Events) {
            auto id=Identity(c->ass.get(),x);
            bool fresh=connected && !nativeIds.count(x.Id);
            auto record=ReadLine(x,id);
            if(id.empty() || seen.count(id) || fresh || (owners.count(id) && owners.at(id)!=x.Id)) {
                id=NewId();
                auto actor=x.Actor.get(); auto suffix=actor.rfind(" [by ");
                if(suffix!=std::string::npos && actor.back()==']') actor.resize(suffix);
                if(actor.rfind("[by ",0)==0) actor.clear();
                record.id=id;
                record.fields[5]=actor+(actor.empty()?"":" ")+"[by "+name+"]";
                assigned.emplace_back(&x,record);
            }
            seen.insert(id); doc.lines.push_back(record);
        }
        for(auto const& k:Settings) {auto v=c->ass->GetScriptInfo(k); if(!v.empty()) doc.settings[k]=std::string(v);}
        for(auto const& style:c->ass->Styles) if(!doc.styles.emplace(style.name,style.GetEntryData()).second) throw collab::Conflict("Duplicate style names.");
        collab::Validate(doc); CheckStyles(doc);
        if(!assigned.empty()) {
            for(auto const& pair:assigned) {SetIdentity(c->ass.get(),*pair.first,pair.second.id); pair.first->Actor=pair.second.fields[5];}
            ignoring=true;
            c->ass->Commit("Assign collaboration authors",AssFile::COMMIT_EXTRADATA|AssFile::COMMIT_DIAG_META);
            ignoring=false;
        }
        size_t index=0; for(auto const& x:c->ass->Events) nativeIds[x.Id]=doc.lines[index++].id;
        return doc;
    }
    void Apply(collab::Document const& doc) {
        collab::Validate(doc); CheckStyles(doc);
        // Prepare allocations and parse all incoming rows before touching the grid.
        std::map<std::string,AssDialogue*> existing;
        for(auto& x:c->ass->Events) existing[Identity(c->ass.get(),x)]=&x;
        std::vector<std::unique_ptr<AssDialogue>> additions;
        std::vector<AssDialogue*> order;
        for(auto const& line:doc.lines) {
            auto it=existing.find(line.id);
            if(it!=existing.end()) order.push_back(it->second);
            else {auto x=std::make_unique<AssDialogue>(); WriteLine(*x,line); SetIdentity(c->ass.get(),*x,line.id); order.push_back(x.get()); additions.push_back(std::move(x));}
        }
        std::vector<std::unique_ptr<AssStyle>> styles;
        for(auto const& kv:doc.styles) styles.push_back(std::make_unique<AssStyle>(kv.second));
        Selection surviving(order.begin(),order.end()), selection;
        for(auto x:c->selectionController->GetSelectedSet()) if(surviving.count(x)) selection.insert(x);
        auto active=c->selectionController->GetActiveLine();
        if(!surviving.count(active)) active=order.empty()?nullptr:order.front();
        if(selection.empty() && active) selection.insert(active);
        ignoring=true;
        // Unlink first; retain objects for unchanged rows, so editing focus survives.
        std::vector<AssDialogue*> removed;
        for(auto& x:c->ass->Events) if(!surviving.count(&x)) removed.push_back(&x);
        c->ass->Events.clear();
        for(size_t i=0;i<order.size();++i) {WriteLine(*order[i],doc.lines[i]); order[i]->Row=static_cast<int>(i); c->ass->Events.push_back(*order[i]);}
        for(auto& x:additions) x.release();
        nativeIds.clear(); for(size_t i=0;i<order.size();++i) nativeIds[order[i]->Id]=doc.lines[i].id;
        c->selectionController->SetSelectionAndActive(selection,active);
        for(auto x:removed) delete x;
        c->ass->Styles.clear_and_dispose([](AssStyle* s){delete s;});
        for(auto& s:styles) c->ass->Styles.push_back(*s.release());
        for(auto const& k:Settings) {auto it=doc.settings.find(k); c->ass->SetScriptInfo(k,it==doc.settings.end()?"":it->second);}
        c->ass->Commit("Receive collaboration edits",AssFile::COMMIT_DIAG_FULL|AssFile::COMMIT_DIAG_ADDREM|AssFile::COMMIT_ORDER|AssFile::COMMIT_STYLES|AssFile::COMMIT_SCRIPTINFO|AssFile::COMMIT_EXTRADATA);
        ignoring=false;
    }
    void Backup() {
        auto dir=wxStandardPaths::Get().GetUserLocalDataDir()+"/collaboration-backups";
        if(!wxFileName::Mkdir(dir,wxS_DIR_DEFAULT,wxPATH_MKDIR_FULL) && !wxDirExists(dir)) throw collab::Conflict("Could not create the backup folder.");
        auto path=dir+"/before-join-"+Wx(NewId())+".ass";
        AssSubtitleFormat().WriteFile(c->ass.get(),agi::fs::path(Utf8(path)),agi::vfr::Framerate(),"UTF-8");
    }
    void Start(bool host) {
        name=Utf8(nameBox->GetValue().Strip(wxString::both)); CheckName(name);
        password=Utf8(passwordBox->GetValue());
        if(password.size()<6 || password.size()>128) throw collab::Conflict("Use a room password between 6 and 128 bytes.");
        auto ip=addressBox->GetValue().Strip(wxString::both);
        // Numeric IPv4 only, avoiding a blocking DNS lookup on the editor thread.
        auto address=Utf8(ip); unsigned a,b,d,e; char rest;
        if(std::sscanf(address.c_str(),"%u.%u.%u.%u%c",&a,&b,&d,&e,&rest)!=4 || a>255 || b>255 || d>255 || e>255 || a==0 || a>=224)
            throw collab::Conflict("Enter the Hamachi IPv4 address, for example 25.12.34.56.");
        wxIPV4address addr; addr.Hostname(ip); addr.Service(Port);
        if(host) {
            listener=std::make_unique<wxSocketServer>(addr,wxSOCKET_NOWAIT); listener->Notify(false);
            if(!listener->IsOk()) throw collab::Conflict("Could not host on that address. Check your Hamachi IP and whether a room is already open.");
            base=Snapshot(); room=std::make_unique<collab::Room>(base); revision=1; connected=true;
            Status("Hosting — edits sync automatically. Share your IP and password."); People();
        }
        else {
            if(wxMessageBox("Joining opens the host's subtitles in this window. A backup of your current subtitles will be saved first. Continue?","Join room",wxYES_NO|wxICON_QUESTION,window)!=wxYES) return;
            Backup();
            auto socket=new wxSocketClient(wxSOCKET_NOWAIT); peers.push_back(std::make_unique<Peer>(socket)); socket->Connect(addr,false);
            Status("Connecting…");
        }
        changed=heartbeat=Clock::now(); timer.Start(50); Buttons();
    }
    void People() {
        std::string names=name+" (host)";
        for(auto const& p:peers) if(p->authenticated) names+="\n"+p->name;
        people->SetLabel(Wx(names)); window->Layout();
        collab::Writer w; w.String("people"); w.String(names);
        for(auto& p:peers) if(p->authenticated) p->Queue(w);
    }
    void State(Peer& p) {
        collab::Writer w; w.String("state"); w.Number(static_cast<uint32_t>(room->Revision())); w.Number(p.ack); w.Doc(room->Current()); p.Queue(w);
    }
    void Broadcast() {for(auto& p:peers) if(p->authenticated) State(*p);}
    void Sync() {
        if(!connected) return;
        auto current=Snapshot();
        if(room) {
            if(room->Apply(revision,current)) {base=room->Current(); revision=static_cast<uint32_t>(room->Revision()); if(current!=base) Apply(base); Broadcast();}
        }
        else if(!inFlight && current!=base) {
            collab::Writer w; w.String("update"); w.Number(revision); w.Number(++sequence); w.Doc(current);
            peers.front()->Queue(w); sent=current; inFlight=true; Status("Sending edits…");
        }
    }
    void Handle(Peer& p,std::string const& message) {
        collab::Reader r(message); auto kind=r.String();
        if(room) {
            if(!p.authenticated) {
                if(kind!="hello" || r.Number()!=1) throw collab::Conflict("Incompatible collaboration build.");
                auto username=r.String(); auto secret=r.String(); r.End(); CheckName(username);
                if(secret!=password) throw collab::Conflict("Room password did not match.");
                if(username==name || std::any_of(peers.begin(),peers.end(),[&](auto const& q){return q.get()!=&p && q->authenticated && q->name==username;})) throw collab::Conflict("That name is already in the room.");
                p.name=username; p.authenticated=true; State(p); People(); return;
            }
            if(kind=="update") {
                auto rev=r.Number(),seq=r.Number(); auto doc=r.Doc(); r.End(); CheckStyles(doc);
                if(seq!=p.ack+1) throw collab::Conflict("Unexpected update sequence.");
                Sync(); // Publish the host's local changes before applying a peer update.
                room->Apply(rev,doc); p.ack=seq; auto current=Snapshot(); base=room->Current(); revision=static_cast<uint32_t>(room->Revision());
                if(current!=base) Apply(base);
                Broadcast(); Status("Connected — all received edits are synced."); return;
            }
        }
        else {
            if(kind=="state") {
                auto rev=r.Number(),ack=r.Number(); auto doc=r.Doc(); r.End(); CheckStyles(doc);
                if(inFlight && ack<sequence) return; // Our ordered acknowledgement includes these earlier states.
                if(ack>sequence || (connected && rev<revision)) throw collab::Conflict("Invalid room revision.");
                if(!connected) {Apply(doc); connected=true;}
                else {
                    auto current=Snapshot(); auto merged=collab::Merge(inFlight?sent:base,current,doc);
                    if(merged!=current) Apply(merged);
                }
                base=doc; revision=rev; inFlight=false; dirty=true; Status("Connected — edits sync automatically."); return;
            }
            if(kind=="people") {auto names=r.String(); r.End(); if(names.size()>2048) throw collab::Conflict("Invalid room list."); people->SetLabel(Wx(names)); window->Layout(); return;}
            if(kind=="error") {auto why=r.String(); r.End(); throw collab::Conflict(why);}
        }
        if(kind=="ping") {r.End(); collab::Writer w; w.String("pong"); p.Queue(w); return;}
        if(kind=="pong") {r.End(); return;}
        throw collab::Conflict("Unexpected room message.");
    }
    void Tick() {
        if(ticking) return;
        ticking=true;
        Guard([this] {
            if(listener) {
                if(auto socket=listener->Accept(false)) {
                    if(peers.size()>=8) delete socket;
                    else peers.push_back(std::make_unique<Peer>(socket));
                }
            }
            auto now=Clock::now();
            for(auto it=peers.begin();it!=peers.end();) {
                auto& p=**it;
                try {
                    if(!room && !p.hello && p.socket->IsConnected()) {
                        collab::Writer w; w.String("hello"); w.Number(1); w.String(name); w.String(password); p.Queue(w); p.hello=true;
                    }
                    for(auto const& message:p.Read()) Handle(p,message);
                    p.Flush();
                    if(now-p.last>std::chrono::seconds(20)) throw collab::Conflict("Connection timed out. Local edits are kept.");
                    if((p.authenticated || connected) && !p.socket->IsConnected()) throw collab::Conflict("Connection lost. Local edits are kept.");
                    ++it;
                }
                catch(std::exception const& e) {
                    if(!room) throw;
                    collab::Writer error; error.String("error"); error.String(e.what());
                    try {p.Queue(error); p.Flush();} catch(...) {}
                    it=peers.erase(it); People(); Status(std::string("A guest disconnected: ")+e.what());
                }
            }
            if((dirty && now-changed>=std::chrono::milliseconds(250) && now-checked>=std::chrono::milliseconds(250)) || now-checked>=std::chrono::seconds(10)) {
                Sync(); checked=now;
                if(!inFlight) dirty=false;
            }
            if(now-heartbeat>std::chrono::seconds(5)) {
                collab::Writer w; w.String("ping"); for(auto& p:peers) if(p->authenticated || connected) p->Queue(w); heartbeat=now;
            }
        });
        ignoring=false; ticking=false;
    }
};
CollaborationController::CollaborationController(agi::Context* c):impl(std::make_unique<Impl>(c)) {}
CollaborationController::~CollaborationController()=default;
void CollaborationController::Show() {impl->Show();}
