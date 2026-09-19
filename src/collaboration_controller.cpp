// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#include "collaboration_controller.h"
#include "collaboration/document.h"
#include "collaboration/transport.h"
#include "include/aegisub/context.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "base_grid.h"
#include "frame_main.h"
#include "selection_controller.h"
#include "project.h"
#include "subs_controller.h"
#include "subtitle_format_ass.h"
#include <libaegisub/fs.h>
#include <libaegisub/vfr.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/notebook.h>
#include <wx/panel.h>
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
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>

namespace {
using Clock = std::chrono::steady_clock;
constexpr char IdKey[] = "aegisub-collab-id";
constexpr char AuthorKey[] = "aegisub-collab-author";
constexpr char LastEditorKey[] = "aegisub-collab-last-editor";
constexpr int Port = 49215;
constexpr uint64_t MaxMediaBytes = 32ull * 1024 * 1024 * 1024;
constexpr size_t MediaChunkBytes = 1024 * 1024;
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
void Write64(collab::Writer& w, uint64_t n) {
    w.Number(static_cast<uint32_t>(n >> 32));
    w.Number(static_cast<uint32_t>(n));
}
uint64_t Read64(collab::Reader& r) {
    return (static_cast<uint64_t>(r.Number()) << 32) | r.Number();
}
std::string SafeMediaFilename(std::string const& source) {
    std::string clean;
    clean.reserve(std::min<size_t>(source.size(), 120));
    for (unsigned char ch : source) {
        if (clean.size() >= 120) break;
        if (std::isalnum(ch) || ch=='.' || ch=='_' || ch=='-' || ch==' ' || ch=='(' || ch==')' || ch=='[' || ch==']')
            clean.push_back(static_cast<char>(ch));
        else clean.push_back('_');
    }
    if (clean.empty() || clean=="." || clean=="..") clean="video.bin";
    return "shared-" + NewId().substr(0,8) + "-" + clean;
}
std::string HumanBytes(uint64_t bytes) {
    char out[64];
    double value=static_cast<double>(bytes);
    const char* unit="B";
    if(bytes>=1024ull*1024*1024) {value/=1024.0*1024.0*1024.0; unit="GB";}
    else if(bytes>=1024ull*1024) {value/=1024.0*1024.0; unit="MB";}
    else if(bytes>=1024ull) {value/=1024.0; unit="KB";}
    std::snprintf(out,sizeof out,"%.2f %s",value,unit);
    return out;
}
void CheckName(std::string const& s) {
    if (s.empty() || s.size()>40 || s.find_first_of(",\r\n[]")!=std::string::npos || s.find('\0')!=std::string::npos)
        throw collab::Conflict("Enter a name (1-40 bytes, without commas or brackets).");
}
std::string Extra(AssFile* file, AssDialogue const& line, char const* key) {
    for (auto const& e:file->GetExtradata(line.ExtradataIds)) if(e.key==key) return e.value;
    return {};
}
std::string Identity(AssFile* file, AssDialogue const& line) { return Extra(file,line,IdKey); }
bool SetExtra(AssFile* file, AssDialogue& line, char const* key, std::string const& value) {
    std::vector<uint32_t> ids;
    std::string old;
    bool found=false;
    for (auto const& e:file->GetExtradata(line.ExtradataIds)) {
        if(e.key==key) {if(!found) old=e.value; found=true;}
        else ids.push_back(e.id);
    }
    if((found ? old : std::string())==value && found==!value.empty()) return false;
    if(!value.empty()) ids.push_back(file->AddExtradata(key,value));
    line.ExtradataIds=ids;
    return true;
}
bool SetIdentity(AssFile* file, AssDialogue& line, std::string const& id) {return SetExtra(file,line,IdKey,id);}
collab::Line ReadLine(AssFile* file, AssDialogue const& x, std::string id) {
    return {std::move(id), {x.Comment?"1":"0", std::to_string(x.Layer),std::to_string(int(x.Start)),std::to_string(int(x.End)),
        x.Style.get(),x.Actor.get(),std::to_string(x.Margin[0]),std::to_string(x.Margin[1]),std::to_string(x.Margin[2]),x.Effect.get(),x.Text.get(),
        Extra(file,x,AuthorKey),Extra(file,x,LastEditorKey)}};
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
    wxStaticText *status=nullptr,*people=nullptr,*recent=nullptr;
    wxButton *hostButton=nullptr,*joinButton=nullptr,*leaveButton=nullptr;
    wxCheckBox *autoMediaBox=nullptr;
    wxChoice *followChoice=nullptr;
    struct PresenceInfo {std::string lineId; int frame=-1;};
    std::map<Peer*,PresenceInfo> peerPresence;
    std::map<std::string,PresenceInfo> roomPresence;
    std::string localLineId;
    int localFrame=-1;
    bool presenceDirty=false;
    Clock::time_point presenceSent=Clock::now();
    struct MediaSend {
        std::ifstream file;
        uint64_t size=0, sent=0;
        int lastPercent=-1;
        std::string name;
        bool active=false;
    };
    struct MediaReceive {
        std::ofstream file;
        agi::fs::path partPath, finalPath;
        uint64_t expected=0, received=0;
        int lastPercent=-1;
        std::string name;
        bool active=false;
    };
    std::map<Peer*,MediaSend> mediaSends;
    MediaReceive mediaReceive;
    std::string offeredMediaName;
    uint64_t offeredMediaSize=0;
    std::unique_ptr<wxSocketServer> listener;
    std::vector<std::unique_ptr<Peer>> peers;
    std::unique_ptr<collab::Room> room;
    collab::Document base, sent;
    uint32_t revision=0, sequence=0;
    bool ignoring=false, connected=false, inFlight=false, ticking=false, dirty=false;
    std::map<int,std::string> nativeIds;
    std::string name,password;
    Clock::time_point changed=Clock::now(), heartbeat=Clock::now(), checked=Clock::now();
    agi::signal::Connection commit, fileOpened, videoOpened, activeLineChanged, videoSeek;

    explicit Impl(agi::Context* context):c(context) {
        Bind(wxEVT_TIMER,[this](wxTimerEvent&) {Tick();});
        commit=c->ass->AddCommitListener([this](int type,const AssDialogue*) {
            if(ignoring) return;
            (void)type; // Undo also uses COMMIT_NEW; the file-open signal handles real file changes.
            changed=Clock::now(); dirty=true;
        });
        fileOpened=c->subsController->AddFileOpenListener([this](agi::fs::path const&) {Stop("Disconnected: another subtitle file was opened.");});
        videoOpened=c->project->AddVideoProviderListener([this](AsyncVideoProvider*) {
            if(room) for(auto& p:peers) if(p->authenticated) SendMediaOffer(*p);
        });
        activeLineChanged=c->selectionController->AddActiveLineListener([this](AssDialogue* line) {
            localLineId=line?Identity(c->ass.get(),*line):std::string();
            presenceDirty=true;
            if(c->subsGrid) c->subsGrid->Refresh(false);
        });
        videoSeek=c->videoController->AddSeekListener([this](int frame) {
            localFrame=frame; presenceDirty=true;
        });
    }
    ~Impl() {Goodbye(); timer.Stop(); ClearMediaReceive(false); mediaSends.clear(); peers.clear(); listener.reset(); if(window) {window->Unbind(wxEVT_CLOSE_WINDOW,&Impl::OnClose,this); window->Destroy();}}
    void OnClose(wxCloseEvent& e) {if(e.CanVeto()) {window->Hide(); e.Veto();} else e.Skip();}
    void Status(std::string const& text) {if(status) {status->SetLabel(Wx(text)); status->Wrap(500); window->Layout();}}
    void Notice(std::string const& text) {
        if(recent) {recent->SetLabel(Wx("Recent: "+text)); recent->Wrap(500); if(window) window->Layout();}
        if(c->frame) c->frame->StatusTimeout(Wx("Collaboration: "+text),8000);
    }
    void BroadcastNotice(std::string const& text) {
        Notice(text);
        if(!room) return;
        collab::Writer w; w.String("notice"); w.String(text);
        for(auto& p:peers) if(p->authenticated) p->Queue(w);
    }
    void Buttons() {
        bool active=listener || !peers.empty();
        hostButton->Enable(!active); joinButton->Enable(!active); leaveButton->Enable(active);
        nameBox->Enable(!active); addressBox->Enable(!active); passwordBox->Enable(!active);
        if(autoMediaBox) autoMediaBox->Enable(!active);
    }
    void ResetFollow() {
        roomPresence.clear(); peerPresence.clear(); localLineId.clear(); localFrame=-1; presenceDirty=false;
        if(followChoice) {followChoice->Clear(); followChoice->Append("Do not follow"); followChoice->SetSelection(0);}
        if(c->subsGrid) c->subsGrid->Refresh(false);
    }
    void Stop(std::string const& reason) {
        timer.Stop(); ClearMediaReceive(true); mediaSends.clear(); offeredMediaName.clear(); offeredMediaSize=0;
        peers.clear(); listener.reset(); room.reset(); connected=false; inFlight=false; ignoring=false; nativeIds.clear(); dirty=false;
        ResetFollow();
        if(window) {Status(reason); people->SetLabel("Not in a room"); Buttons();}
    }
    void Goodbye() noexcept {
        try {
            if(room) {
                collab::Writer w; w.String("room-closed"); w.String(name);
                for(auto& p:peers) if(p->authenticated) {p->Queue(w); p->Flush();}
            }
            else if(connected && !peers.empty()) {
                collab::Writer w; w.String("leave"); peers.front()->Queue(w); peers.front()->Flush();
            }
        } catch(...) {}
    }
    void Guard(std::function<void()> fn) {
        try {fn();}
        catch(std::exception const& e) {Stop(e.what()); if(window) {window->Show(); window->Raise();}}
        catch(...) {Stop("Could not sync this project. Local edits are kept."); if(window) window->Show();}
    }
    void Show() {
        if(!window) {
            window=new wxDialog(c->parent,wxID_ANY,"Collaborate",wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
            auto root=new wxBoxSizer(wxVERTICAL);
            auto tabs=new wxNotebook(window,wxID_ANY);

            auto collabPage=new wxPanel(tabs);
            auto collabRoot=new wxBoxSizer(wxVERTICAL);
            auto title=new wxStaticText(collabPage,wxID_ANY,"Edit subtitles together");
            auto font=title->GetFont(); font.SetPointSize(font.GetPointSize()+3); font.SetWeight(wxFONTWEIGHT_BOLD); title->SetFont(font);
            collabRoot->Add(title,0,wxALL,16);
            auto hint=new wxStaticText(collabPage,wxID_ANY,"Connect both PCs to the same Hamachi network first."); collabRoot->Add(hint,0,wxLEFT|wxRIGHT|wxBOTTOM,16);
            auto field=[&](char const* label,wxTextCtrl*& box,long style=0) {
                collabRoot->Add(new wxStaticText(collabPage,wxID_ANY,label),0,wxLEFT|wxRIGHT,16);
                box=new wxTextCtrl(collabPage,wxID_ANY,"",wxDefaultPosition,wxSize(500,-1),style);
                collabRoot->Add(box,0,wxEXPAND|wxLEFT|wxRIGHT|wxTOP|wxBOTTOM,8);
            };
            field("Your name",nameBox); nameBox->SetMaxLength(40);
            field("Hamachi IPv4 address - yours to host, your friend's to join",addressBox);
            field("Room password - agree on this with your friend",passwordBox,wxTE_PASSWORD);
            autoMediaBox=new wxCheckBox(collabPage,wxID_ANY,"Automatically download and open the host's video if mine does not match");
            autoMediaBox->SetValue(true);
            collabRoot->Add(autoMediaBox,0,wxLEFT|wxRIGHT|wxBOTTOM,16);
            auto buttons=new wxBoxSizer(wxHORIZONTAL);
            hostButton=new wxButton(collabPage,wxID_ANY,"Host room"); joinButton=new wxButton(collabPage,wxID_ANY,"Join room"); leaveButton=new wxButton(collabPage,wxID_ANY,"Disconnect");
            for(auto b:{hostButton,joinButton,leaveButton}) buttons->Add(b,1,wxRIGHT,8);
            collabRoot->Add(buttons,0,wxEXPAND|wxALL,16);
            status=new wxStaticText(collabPage,wxID_ANY,"Open the subtitles you want to share, then host a room."); status->Wrap(500);
            people=new wxStaticText(collabPage,wxID_ANY,"Not in a room");
            recent=new wxStaticText(collabPage,wxID_ANY,"Recent: none");
            collabRoot->Add(status,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,16);
            collabRoot->Add(people,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,12);
            collabRoot->Add(new wxStaticText(collabPage,wxID_ANY,"Follow collaborator playhead"),0,wxLEFT|wxRIGHT,16);
            followChoice=new wxChoice(collabPage,wxID_ANY); followChoice->Append("Do not follow"); followChoice->SetSelection(0);
            collabRoot->Add(followChoice,0,wxEXPAND|wxLEFT|wxRIGHT|wxTOP|wxBOTTOM,8);
            collabRoot->Add(recent,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,16);
            collabPage->SetSizer(collabRoot);
            tabs->AddPage(collabPage,"Collaborate",true);

            auto ytPage=new wxPanel(tabs);
            auto ytRoot=new wxBoxSizer(wxVERTICAL);
            auto ytTitle=new wxStaticText(ytPage,wxID_ANY,"YTSubConverter supported ASS features");
            auto ytFont=ytTitle->GetFont(); ytFont.SetPointSize(ytFont.GetPointSize()+2); ytFont.SetWeight(wxFONTWEIGHT_BOLD); ytTitle->SetFont(ytFont);
            ytRoot->Add(ytTitle,0,wxALL,12);
            auto reference=new wxTextCtrl(ytPage,wxID_ANY,wxString::FromUTF8(R"YTREF(YTSubConverter / YouTube ASS quick reference

STYLE FEATURES
- Font name: YouTube only accepts its supported font list. Unsupported fonts fall back to Roboto.
- Font size: Default (or the first style) becomes YouTube's standard size. Other styles are relative to it.
  Minimum effective size is 75%. Android ignores custom relative sizes.
- Bold, italic, underline.
- Primary, secondary, outline and shadow colors.
- Alignment 1-9. Top aligned captions move down on hover, middle stay in place, bottom move up.
- Outline thickness and shadow distance: YTSubConverter checks only whether each value is zero or greater than zero.

SUPPORTED OVERRIDE TAGS
{\\b} / {\\i} / {\\u} - bold / italic / underline
{\\fnFont Name} - font (unsupported names fall back to Roboto)
{\\fs30} - relative font size
{\\c&H...&} or {\\1c&H...&} - primary text color
{\\2c&H...&} - unsung karaoke color
{\\3c&H...&} - outline color
{\\4c&H...&} - shadow color
{\\1a&H...&} - text alpha
{\\2a&H...&} - unsung karaoke alpha; fully transparent enables native YouTube karaoke hiding
{\\3a&H...&} - background alpha
{\\4a&H...&} - shadow alpha (limited by YouTube; works with &H222222& and matching text alpha)
{\\alpha&H...&} - set all alpha values
{\\pos(x,y)} - position
{\\an1} ... {\\an9} - alignment
{\\kN} - karaoke segment duration
{\\r} / {\\rStyle} - reset formatting
{\\fad(in,out)} - simple fade; outline/shadow fade is limited unless color is &H222222&
{\\fade(...)} - complex fade with the same outline/shadow limitation
{\\move(x1,y1,x2,y2)} - move
{\\t(...)} - animate colors, alpha and font size

YOUTUBE-SPECIFIC TAGS
{\\ytsub} - subscript (PC only)
{\\ytsup} - superscript (PC only)
{\\ytsur} - return to regular script
{\\ytruby} - ruby text. Syntax uses [base/reading] pairs, for example [kanji/kana].
{\\ytruby8} - ruby above (default); {\\ytruby2} - ruby below. PC only; mobile shows parenthesized reading.
{\\ytvert9} - vertical columns right-to-left (PC only)
{\\ytvert7} - vertical columns left-to-right (PC only)
{\\ytvert1} - rotate subtitle 90 degrees counter-clockwise (PC only)
{\\ytvert3} - rotate and invert line order (PC only)
{\\ytdir4} - force right-to-left inside a left-to-right subtitle language
{\\ytpack1} / {\\ytpack0} - start/stop full-width-character packing in vertical text (PC only)

YT SHAKE
{\\ytshake} - 20 px radius for the whole line
{\\ytshake(radius)}
{\\ytshake(radiusX,radiusY)}
{\\ytshake(radius,t1,t2)}
{\\ytshake(radiusX,radiusY,t1,t2)}

YT CHROMA
{\\ytchroma} - default RGB chromatic entrance/exit
{\\ytchroma(intime,outtime)}
{\\ytchroma(offsetX,offsetY,intime,outtime)}
{\\ytchroma(color1,color2,...,alpha,offsetX,offsetY,intime,outtime)}

ADVANCED KARAOKE
{\\ytktFade} - fading karaoke (can generate very large files and lag on some devices)
{\\ytktGlitch} - randomized glitch karaoke for Latin/CJK/Korean text
{\\ytkt(Cursor,text)} - cursor after the active word
{\\ytkt(Cursor,formatting tags,text)} - formatted cursor
{\\ytkt(Cursor,interval,tags1,text1,tags2,text2,...)} - animated cursor
{\\ytkt(LCursor,text)} - cursor before the active word; formatting/animation variants also apply

Anything not listed above is not supported by YTSubConverter and will not have an effect on YouTube.

Font chart:
https://github.com/arcusmaximus/YTSubConverter/blob/master/images/fonts.png

YTSubConverter:
https://github.com/arcusmaximus/YTSubConverter
)YTREF"),
                wxDefaultPosition,wxSize(660,520),wxTE_MULTILINE|wxTE_READONLY|wxTE_RICH2);
            auto mono=reference->GetFont(); mono.SetFamily(wxFONTFAMILY_TELETYPE); reference->SetFont(mono);
            ytRoot->Add(reference,1,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,12);
            ytPage->SetSizer(ytRoot);
            tabs->AddPage(ytPage,"YTSub Reference",false);

            root->Add(tabs,1,wxEXPAND|wxALL,6);
            window->SetSizerAndFit(root); window->SetMinSize(wxSize(560,620)); window->CentreOnParent();
            window->Bind(wxEVT_CLOSE_WINDOW,&Impl::OnClose,this);
            hostButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Guard([this]{Start(true);});});
            joinButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Guard([this]{Start(false);});});
            leaveButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Goodbye(); Stop("Disconnected. Your subtitles stay open.");});
            Buttons();
        }
        window->Show(); window->Raise();
    }
    collab::Document Snapshot() {
        collab::Document doc; std::set<std::string> seen;
        struct Update {AssDialogue* line; std::string id,author,last;};
        std::vector<Update> updates;
        std::map<std::string,int> owners;
        std::map<std::string,collab::Line const*> baseLines;
        for(auto const& line:base.lines) baseLines[line.id]=&line;
        for(auto const& x:c->ass->Events) {
            auto it=nativeIds.find(x.Id);
            if(it!=nativeIds.end()) owners[it->second]=x.Id;
        }
        for(auto& x:c->ass->Events) {
            auto id=Identity(c->ass.get(),x);
            bool fresh=connected && !nativeIds.count(x.Id);
            auto record=ReadLine(c->ass.get(),x,id);
            bool update=false;
            if(id.empty() || seen.count(id) || fresh || (owners.count(id) && owners.at(id)!=x.Id)) {
                id=NewId(); record.id=id;
                if(connected) {record.fields[11]=name; record.fields[12]=name;}
                update=true;
            }
            else if(connected) {
                auto old=baseLines.find(id);
                if(old!=baseLines.end()) {
                    bool realChanged=false;
                    for(size_t i=0;i<11;++i) realChanged |= record.fields[i]!=old->second->fields[i];
                    if(realChanged && record.fields[12]!=name) {record.fields[12]=name; update=true;}
                }
            }
            if(update) updates.push_back({&x,id,record.fields[11],record.fields[12]});
            seen.insert(id); doc.lines.push_back(record);
        }
        for(auto const& k:Settings) {auto v=c->ass->GetScriptInfo(k); if(!v.empty()) doc.settings[k]=std::string(v);}
        for(auto const& style:c->ass->Styles) if(!doc.styles.emplace(style.name,style.GetEntryData()).second) throw collab::Conflict("Duplicate style names.");
        collab::Validate(doc); CheckStyles(doc);
        if(!updates.empty()) {
            for(auto const& u:updates) {
                SetIdentity(c->ass.get(),*u.line,u.id);
                SetExtra(c->ass.get(),*u.line,AuthorKey,u.author);
                SetExtra(c->ass.get(),*u.line,LastEditorKey,u.last);
            }
            ignoring=true;
            c->ass->Commit("Update collaboration authorship",AssFile::COMMIT_EXTRADATA|AssFile::COMMIT_DIAG_META);
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
            else {auto x=std::make_unique<AssDialogue>(); WriteLine(*x,line); order.push_back(x.get()); additions.push_back(std::move(x));}
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
        for(size_t i=0;i<order.size();++i) {
            WriteLine(*order[i],doc.lines[i]);
            SetIdentity(c->ass.get(),*order[i],doc.lines[i].id);
            SetExtra(c->ass.get(),*order[i],AuthorKey,doc.lines[i].fields[11]);
            SetExtra(c->ass.get(),*order[i],LastEditorKey,doc.lines[i].fields[12]);
            order[i]->Row=static_cast<int>(i); c->ass->Events.push_back(*order[i]);
        }
        for(auto& x:additions) x.release();
        nativeIds.clear(); for(size_t i=0;i<order.size();++i) nativeIds[order[i]->Id]=doc.lines[i].id;
        c->selectionController->SetSelectionAndActive(selection,active);
        for(auto x:removed) delete x;
        c->ass->Styles.clear_and_dispose([](AssStyle* s){delete s;});
        for(auto& s:styles) c->ass->Styles.push_back(*s.release());
        for(auto const& k:Settings) {auto it=doc.settings.find(k); c->ass->SetScriptInfo(k,it==doc.settings.end()?"":it->second);}
        c->ass->Commit("Receive collaboration edits",AssFile::COMMIT_DIAG_FULL|AssFile::COMMIT_DIAG_META|AssFile::COMMIT_DIAG_ADDREM|AssFile::COMMIT_ORDER|AssFile::COMMIT_STYLES|AssFile::COMMIT_SCRIPTINFO|AssFile::COMMIT_EXTRADATA);
        ignoring=false;
    }
    void Backup() {
        auto dir=wxStandardPaths::Get().GetUserLocalDataDir()+"/collaboration-backups";
        if(!wxFileName::Mkdir(dir,wxS_DIR_DEFAULT,wxPATH_MKDIR_FULL) && !wxDirExists(dir)) throw collab::Conflict("Could not create the backup folder.");
        auto path=dir+"/before-join-"+Wx(NewId())+".ass";
        AssSubtitleFormat().WriteFile(c->ass.get(),agi::fs::path(Utf8(path)),agi::vfr::Framerate(),"UTF-8");
    }
    void ClearMediaReceive(bool removePartial) {
        if(mediaReceive.file.is_open()) mediaReceive.file.close();
        if(removePartial && !mediaReceive.partPath.empty()) {
            try {agi::fs::Remove(mediaReceive.partPath);} catch(...) {}
        }
        mediaReceive=MediaReceive{};
    }
    bool CurrentMedia(std::string& filename,uint64_t& size,agi::fs::path& path) {
        path=c->project->VideoName();
        if(path.empty()) return false;
        try {
            if(!agi::fs::FileExists(path)) return false;
            auto bytes=agi::fs::Size(path);
            if(!bytes || bytes>MaxMediaBytes) return false;
            filename=path.filename().string();
            size=static_cast<uint64_t>(bytes);
            return !filename.empty();
        }
        catch(...) {return false;}
    }
    bool LocalMediaMatches(std::string const& filename,uint64_t size) {
        auto const& path=c->project->VideoName();
        if(path.empty() || path.filename().string()!=filename) return false;
        try {return agi::fs::FileExists(path) && agi::fs::Size(path)==size;}
        catch(...) {return false;}
    }
    void SendMediaOffer(Peer& p) {
        std::string filename; uint64_t size=0; agi::fs::path path;
        if(!CurrentMedia(filename,size,path)) return;
        collab::Writer w; w.String("media-offer"); w.String(filename); Write64(w,size); p.Queue(w);
    }
    void RequestMedia(Peer& p,std::string const& filename,uint64_t size) {
        if(filename.empty() || filename.size()>512 || !size || size>MaxMediaBytes) throw collab::Conflict("Invalid shared video offer.");
        auto dirWx=wxStandardPaths::Get().GetUserLocalDataDir()+"/collaboration-media";
        agi::fs::path dir(Utf8(dirWx));
        agi::fs::CreateDirectory(dir);
        if(agi::fs::FreeSpace(dir)<size+64ull*1024*1024) throw collab::Conflict("Not enough free disk space for the shared video.");
        auto localName=SafeMediaFilename(filename);
        ClearMediaReceive(true);
        mediaReceive.name=filename; mediaReceive.expected=size;
        mediaReceive.finalPath=dir/agi::fs::path(localName);
        mediaReceive.partPath=dir/agi::fs::path(localName+".part");
        offeredMediaName=filename; offeredMediaSize=size;
        collab::Writer w; w.String("media-request"); w.String(filename); Write64(w,size); p.Queue(w);
        Status("Requesting host video " + filename + " (" + HumanBytes(size) + ")...");
    }
    void BeginMediaReceive(std::string const& filename,uint64_t size) {
        if(filename!=offeredMediaName || size!=offeredMediaSize || filename!=mediaReceive.name || size!=mediaReceive.expected)
            throw collab::Conflict("The host video changed while the download was starting.");
        mediaReceive.file.open(mediaReceive.partPath,std::ios::binary|std::ios::trunc);
        if(!mediaReceive.file) throw collab::Conflict("Could not create the shared video download file.");
        mediaReceive.received=0; mediaReceive.lastPercent=-1; mediaReceive.active=true;
        Status("Downloading " + filename + " - 0% of " + HumanBytes(size));
    }
    void BeginMediaSend(Peer& p,std::string const& requestedName,uint64_t requestedSize) {
        std::string filename; uint64_t size=0; agi::fs::path path;
        if(!CurrentMedia(filename,size,path) || filename!=requestedName || size!=requestedSize)
            throw collab::Conflict("The host video changed. Reopen Collaborate and try again.");
        MediaSend transfer;
        transfer.file.open(path,std::ios::binary);
        if(!transfer.file) throw collab::Conflict("Could not read the host video.");
        transfer.size=size; transfer.name=filename; transfer.active=true;
        mediaSends[&p]=std::move(transfer);
        collab::Writer w; w.String("media-start"); w.String(filename); Write64(w,size); p.Queue(w);
        Status("Sending " + filename + " to " + p.name + "...");
    }
    void PumpMedia(Peer& p) {
        auto it=mediaSends.find(&p);
        if(it==mediaSends.end() || !it->second.active || p.output.size()>1024*1024) return;
        auto& transfer=it->second;
        if(transfer.sent>=transfer.size) return;
        auto want=static_cast<size_t>(std::min<uint64_t>(MediaChunkBytes,transfer.size-transfer.sent));
        std::string bytes(want,'\0');
        transfer.file.read(bytes.data(),static_cast<std::streamsize>(want));
        auto got=transfer.file.gcount();
        if(got<=0) throw collab::Conflict("Could not finish reading the host video.");
        bytes.resize(static_cast<size_t>(got));
        collab::Writer w; w.String("media-chunk"); Write64(w,transfer.sent); w.String(bytes); p.Queue(w);
        transfer.sent+=static_cast<uint64_t>(got);
        int percent=static_cast<int>((transfer.sent*100)/transfer.size);
        if(percent!=transfer.lastPercent) {
            transfer.lastPercent=percent;
            Status("Sending " + transfer.name + " to " + p.name + " - " + std::to_string(percent) + "%");
        }
        if(transfer.sent==transfer.size) {
            collab::Writer done; done.String("media-done"); Write64(done,transfer.size); p.Queue(done);
            transfer.file.close(); transfer.active=false;
        }
    }
    void FinishMediaReceive(uint64_t size) {
        if(!mediaReceive.active || size!=mediaReceive.expected || mediaReceive.received!=mediaReceive.expected)
            throw collab::Conflict("Shared video download was incomplete.");
        mediaReceive.file.close(); mediaReceive.active=false;
        if(agi::fs::Size(mediaReceive.partPath)!=mediaReceive.expected) throw collab::Conflict("Shared video size check failed.");
        agi::fs::Rename(mediaReceive.partPath,mediaReceive.finalPath);
        auto finalPath=mediaReceive.finalPath; auto displayName=mediaReceive.name;
        mediaReceive=MediaReceive{};
        c->project->LoadVideo(finalPath);
        if(c->project->VideoName()==finalPath) Status("Connected - downloaded and opened " + displayName + ".");
        else Status("Connected - video downloaded, but Aegisub could not open it automatically.");
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
            Status("Hosting - edits sync automatically. Share your IP and password."); People();
        }
        else {
            if(wxMessageBox("Joining opens the host's subtitles in this window. A backup of your current subtitles will be saved first. Continue?","Join room",wxYES_NO|wxICON_QUESTION,window)!=wxYES) return;
            Backup();
            auto socket=new wxSocketClient(wxSOCKET_NOWAIT); peers.push_back(std::make_unique<Peer>(socket)); socket->Connect(addr,false);
            Status("Connecting...");
        }
        localFrame=c->project->VideoProvider()?c->videoController->GetFrameN():-1;
        localLineId=RowLineId(c->selectionController->GetActiveLine());
        presenceDirty=true;
        changed=heartbeat=Clock::now(); timer.Start(50); Buttons();
    }
    std::string RowLineId(AssDialogue const* line) const {
        return line?Identity(c->ass.get(),*line):std::string();
    }
    void UpdateFollowChoices() {
        if(!followChoice) return;
        auto selected=followChoice->GetStringSelection();
        std::set<std::string> names;
        if(room) {
            for(auto const& p:peers) if(p->authenticated && p->name!=name) names.insert(p->name);
        }
        else {
            for(auto const& kv:roomPresence) if(kv.first!=name) names.insert(kv.first);
        }
        followChoice->Clear(); followChoice->Append("Do not follow");
        for(auto const& n:names) followChoice->Append(Wx(n));
        auto found=followChoice->FindString(selected);
        followChoice->SetSelection(found==wxNOT_FOUND?0:found);
    }
    void MaybeFollow(std::string const& who,int frame) {
        if(frame<0 || !followChoice || followChoice->GetSelection()<=0 || Utf8(followChoice->GetStringSelection())!=who) return;
        if(c->project->VideoProvider() && c->videoController->GetFrameN()!=frame) c->videoController->JumpToFrame(frame);
    }
    void BroadcastPresence() {
        if(!room) return;
        collab::Writer w; w.String("presence-list");
        uint32_t count=1;
        for(auto const& p:peers) if(p->authenticated) ++count;
        w.Number(count);
        w.String(name); w.String(localLineId); w.Number(localFrame<0?0:static_cast<uint32_t>(localFrame+1));
        for(auto const& p:peers) if(p->authenticated) {
            auto it=peerPresence.find(p.get());
            w.String(p->name);
            w.String(it==peerPresence.end()?std::string():it->second.lineId);
            auto frame=it==peerPresence.end()?-1:it->second.frame;
            w.Number(frame<0?0:static_cast<uint32_t>(frame+1));
        }
        for(auto& p:peers) if(p->authenticated) p->Queue(w);
        UpdateFollowChoices();
        if(c->subsGrid) c->subsGrid->Refresh(false);
    }
    void SendPresence() {
        if(!connected) return;
        if(room) {BroadcastPresence(); return;}
        if(peers.empty()) return;
        collab::Writer w; w.String("presence"); w.String(localLineId); w.Number(localFrame<0?0:static_cast<uint32_t>(localFrame+1));
        peers.front()->Queue(w);
    }
    void People() {
        size_t count=1;
        for(auto const& p:peers) if(p->authenticated) ++count;
        std::string names="Connected users ("+std::to_string(count)+")\n\n"+name+" (Host)";
        for(auto const& p:peers) if(p->authenticated) names+="\n"+p->name;
        people->SetLabel(Wx(names)); window->Layout();
        collab::Writer w; w.String("people"); w.String(names);
        for(auto& p:peers) if(p->authenticated) p->Queue(w);
        UpdateFollowChoices();
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
            peers.front()->Queue(w); sent=current; inFlight=true; Status("Sending edits...");
        }
    }
    void Handle(Peer& p,std::string const& message) {
        collab::Reader r(message); auto kind=r.String();
        if(room) {
            if(!p.authenticated) {
                if(kind!="hello" || r.Number()!=3) throw collab::Conflict("Incompatible collaboration build.");
                auto username=r.String(); auto secret=r.String(); r.End(); CheckName(username);
                if(secret!=password) throw collab::Conflict("Room password did not match.");
                if(username==name || std::any_of(peers.begin(),peers.end(),[&](auto const& q){return q.get()!=&p && q->authenticated && q->name==username;})) throw collab::Conflict("That name is already in the room.");
                p.name=username; p.authenticated=true; State(p); People(); SendMediaOffer(p);
                BroadcastNotice(username+" joined the room."); presenceDirty=true; return;
            }
            if(kind=="leave") {r.End(); throw collab::Conflict("Client disconnected.");}
            if(kind=="presence") {
                auto lineId=r.String(); auto encodedFrame=r.Number(); r.End();
                if(!lineId.empty() && (lineId.size()!=32 || lineId.find_first_not_of("0123456789abcdef")!=std::string::npos))
                    throw collab::Conflict("Invalid collaboration presence.");
                peerPresence[&p]={lineId,encodedFrame?static_cast<int>(encodedFrame-1):-1};
                MaybeFollow(p.name,peerPresence[&p].frame); presenceDirty=true;
                if(c->subsGrid) c->subsGrid->Refresh(false);
                return;
            }
            if(kind=="media-request") {
                auto requestedName=r.String(); auto requestedSize=Read64(r); r.End();
                BeginMediaSend(p,requestedName,requestedSize); return;
            }
            if(kind=="media-received") {
                r.End(); Status(p.name + " finished downloading the host video."); return;
            }
            if(kind=="update") {
                auto rev=r.Number(),seq=r.Number(); auto doc=r.Doc(); r.End(); CheckStyles(doc);
                if(seq!=p.ack+1) throw collab::Conflict("Unexpected update sequence.");
                Sync(); // Publish the host's local changes before applying a peer update.
                room->Apply(rev,doc); p.ack=seq; auto current=Snapshot(); base=room->Current(); revision=static_cast<uint32_t>(room->Revision());
                if(current!=base) Apply(base);
                Broadcast(); Status("Connected - all received edits are synced."); return;
            }
        }
        else {
            if(kind=="notice") {auto text=r.String(); r.End(); if(text.size()>512) throw collab::Conflict("Invalid room notice."); Notice(text); return;}
            if(kind=="room-closed") {
                auto hostName=r.String(); r.End(); Notice(hostName+" disconnected."); throw collab::Conflict("Room closed by host.");
            }
            if(kind=="presence-list") {
                auto count=r.Number(); if(count>9) throw collab::Conflict("Invalid presence list.");
                roomPresence.clear();
                for(uint32_t i=0;i<count;++i) {
                    auto who=r.String(), lineId=r.String(); auto encodedFrame=r.Number();
                    if(who.empty() || who.size()>40 || (!lineId.empty() && (lineId.size()!=32 || lineId.find_first_not_of("0123456789abcdef")!=std::string::npos)))
                        throw collab::Conflict("Invalid collaboration presence.");
                    roomPresence[who]={lineId,encodedFrame?static_cast<int>(encodedFrame-1):-1};
                }
                r.End(); UpdateFollowChoices();
                for(auto const& kv:roomPresence) MaybeFollow(kv.first,kv.second.frame);
                if(c->subsGrid) c->subsGrid->Refresh(false);
                return;
            }
            if(kind=="media-offer") {
                auto filename=r.String(); auto size=Read64(r); r.End();
                if(filename.empty() || filename.size()>512 || !size || size>MaxMediaBytes) throw collab::Conflict("Invalid shared video offer.");
                offeredMediaName=filename; offeredMediaSize=size;
                if(LocalMediaMatches(filename,size)) {
                    Status("Connected - matching host video is already open.");
                    return;
                }
                bool accept=autoMediaBox && autoMediaBox->GetValue();
                if(!accept) {
                    auto message=wxString("The host is using ")+Wx(filename)+" ("+Wx(HumanBytes(size))+").\n\nDownload and open it now?";
                    accept=wxMessageBox(message,"Shared video",wxYES_NO|wxICON_QUESTION,window)==wxYES;
                }
                if(accept) RequestMedia(p,filename,size);
                else Status("Connected - host video was not downloaded.");
                return;
            }
            if(kind=="media-start") {
                auto filename=r.String(); auto size=Read64(r); r.End();
                BeginMediaReceive(filename,size); return;
            }
            if(kind=="media-chunk") {
                auto offset=Read64(r); auto bytes=r.String(); r.End();
                if(!mediaReceive.active || offset!=mediaReceive.received || bytes.empty() || bytes.size()>MediaChunkBytes ||
                   mediaReceive.received+bytes.size()>mediaReceive.expected) throw collab::Conflict("Invalid shared video chunk.");
                mediaReceive.file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
                if(!mediaReceive.file) throw collab::Conflict("Could not write the shared video to disk.");
                mediaReceive.received+=bytes.size();
                int percent=static_cast<int>((mediaReceive.received*100)/mediaReceive.expected);
                if(percent!=mediaReceive.lastPercent) {
                    mediaReceive.lastPercent=percent;
                    Status("Downloading " + mediaReceive.name + " - " + std::to_string(percent) + "% of " + HumanBytes(mediaReceive.expected));
                }
                return;
            }
            if(kind=="media-done") {
                auto size=Read64(r); r.End(); FinishMediaReceive(size);
                collab::Writer ack; ack.String("media-received"); p.Queue(ack); return;
            }
            if(kind=="state") {
                auto rev=r.Number(),ack=r.Number(); auto doc=r.Doc(); r.End(); CheckStyles(doc);
                if(inFlight && ack<sequence) return; // Our ordered acknowledgement includes these earlier states.
                if(ack>sequence || (connected && rev<revision)) throw collab::Conflict("Invalid room revision.");
                if(!connected) {Apply(doc); connected=true;}
                else {
                    auto current=Snapshot(); auto merged=collab::Merge(inFlight?sent:base,current,doc);
                    if(merged!=current) Apply(merged);
                }
                base=doc; revision=rev; inFlight=false; dirty=true;
                localLineId=RowLineId(c->selectionController->GetActiveLine());
                localFrame=c->project->VideoProvider()?c->videoController->GetFrameN():-1;
                presenceDirty=true; Status("Connected - edits sync automatically."); return;
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
                        collab::Writer w; w.String("hello"); w.Number(3); w.String(name); w.String(password); p.Queue(w); p.hello=true;
                    }
                    for(auto const& message:p.Read()) Handle(p,message);
                    if(room && p.authenticated) PumpMedia(p);
                    p.Flush();
                    if(now-p.last>std::chrono::seconds(20)) throw collab::Conflict("Connection timed out. Local edits are kept.");
                    if((p.authenticated || connected) && !p.socket->IsConnected()) throw collab::Conflict("Connection lost. Local edits are kept.");
                    ++it;
                }
                catch(std::exception const& e) {
                    if(!room) throw;
                    collab::Writer error; error.String("error"); error.String(e.what());
                    try {p.Queue(error); p.Flush();} catch(...) {}
                    auto departed=p.name;
                    mediaSends.erase(&p); peerPresence.erase(&p);
                    it=peers.erase(it); People();
                    if(!departed.empty()) BroadcastNotice(departed+" disconnected.");
                    Status("Connected - a guest disconnected."); presenceDirty=true;
                }
            }
            if((dirty && now-changed>=std::chrono::milliseconds(250) && now-checked>=std::chrono::milliseconds(250)) || now-checked>=std::chrono::seconds(10)) {
                Sync(); checked=now;
                if(!inFlight) dirty=false;
            }
            if(presenceDirty && now-presenceSent>=std::chrono::milliseconds(200)) {
                SendPresence(); presenceDirty=false; presenceSent=now;
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
std::string CollaborationController::AuthorFor(AssDialogue const* line) const {
    return line?Extra(impl->c->ass.get(),*line,AuthorKey):std::string();
}
std::string CollaborationController::LastEditorFor(AssDialogue const* line) const {
    return line?Extra(impl->c->ass.get(),*line,LastEditorKey):std::string();
}
std::string CollaborationController::PresenceFor(AssDialogue const* line) const {
    if(!line || !impl->connected) return {};
    auto id=Identity(impl->c->ass.get(),*line); if(id.empty()) return {};
    std::vector<std::string> names;
    if(impl->localLineId==id && !impl->name.empty()) names.push_back(impl->name);
    if(impl->room) {
        for(auto const& p:impl->peers) if(p->authenticated) {
            auto it=impl->peerPresence.find(p.get());
            if(it!=impl->peerPresence.end() && it->second.lineId==id) names.push_back(p->name);
        }
    } else {
        for(auto const& kv:impl->roomPresence) if(kv.first!=impl->name && kv.second.lineId==id) names.push_back(kv.first);
    }
    std::string out;
    for(auto const& n:names) {if(!out.empty()) out+=", "; out+=n;}
    return out;
}
