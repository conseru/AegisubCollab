// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#include "collaboration_controller.h"
#include "collaboration/document.h"
#include "collaboration/sha256.h"
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
#include "text_selection_controller.h"
#include "video_controller.h"
#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/vfr.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clrpicker.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/socket.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
#include <optional>

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
std::string SafeMediaFilename(std::string const& source,std::string const& hash) {
    std::string clean;
    clean.reserve(std::min<size_t>(source.size(), 120));
    for (unsigned char ch : source) {
        if (clean.size() >= 120) break;
        if (std::isalnum(ch) || ch=='.' || ch=='_' || ch=='-' || ch==' ' || ch=='(' || ch==')' || ch=='[' || ch==']')
            clean.push_back(static_cast<char>(ch));
        else clean.push_back('_');
    }
    if (clean.empty() || clean=="." || clean=="..") clean="video.bin";
    return "shared-" + hash.substr(0,8) + "-" + clean;
}
std::string AssColor(wxColour const& color) {
    char out[16];
    std::snprintf(out,sizeof out,"&H%02X%02X%02X&",color.Blue(),color.Green(),color.Red());
    return out;
}
wxString QuoteCommandArg(wxString value) {
    value.Replace(""","\\"");
    return """ + value + """;
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

std::vector<std::string> const& YTFonts() {
    static std::vector<std::string> fonts={"Roboto","Courier New","Times New Roman","Lucida Console","Comic Sans MS","Monotype Corsiva","Arial","Carrois Gothic SC"};
    return fonts;
}
bool YTFontAllowed(std::string font) {
    while(!font.empty() && std::isspace(static_cast<unsigned char>(font.front()))) font.erase(font.begin());
    while(!font.empty() && std::isspace(static_cast<unsigned char>(font.back()))) font.pop_back();
    return std::any_of(YTFonts().begin(),YTFonts().end(),[&](auto const& f){
        if(f.size()!=font.size()) return false;
        for(size_t i=0;i<f.size();++i) if(std::tolower(static_cast<unsigned char>(f[i]))!=std::tolower(static_cast<unsigned char>(font[i]))) return false;
        return true;
    });
}
std::string YTTagName(std::string const& afterSlash) {
    static std::vector<std::string> const exact={"ytktGlitch","ytktFade","ytchroma","ytshake","ytruby","ytvert","ytdir","ytpack","ytsub","ytsup","ytsur","alpha","fade","move","fad","pos","an","fs","1c","2c","3c","4c","1a","2a","3a","4a","ytkt","b","i","u","c","k","t"};
    if(afterSlash.rfind("fn",0)==0) return "fn";
    if(afterSlash.rfind("r",0)==0) return "r";
    for(auto const& tag:exact) if(afterSlash.rfind(tag,0)==0) {
        if(tag.size()==1 && afterSlash.size()>1 && std::isalpha(static_cast<unsigned char>(afterSlash[1]))) continue;
        return tag;
    }
    size_t n=0; while(n<afterSlash.size() && (std::isalnum(static_cast<unsigned char>(afterSlash[n]))||afterSlash[n]=='_')) ++n;
    return afterSlash.substr(0,n);
}
std::vector<std::string> YTLineWarnings(std::string const& text) {
    static std::set<std::string> const supported={"b","i","u","fn","fs","c","1c","2c","3c","4c","1a","2a","3a","4a","alpha","pos","an","k","r","fad","fade","move","t","ytsub","ytsup","ytsur","ytruby","ytvert","ytdir","ytpack","ytshake","ytchroma","ytkt","ytktFade","ytktGlitch"};
    std::vector<std::string> warnings; std::set<std::string> seen; bool inBlock=false;
    for(size_t i=0;i<text.size();++i) {
        if(text[i]=='{') inBlock=true;
        else if(text[i]=='}') inBlock=false;
        else if(inBlock && text[i]=='\\') {
            auto tag=YTTagName(text.substr(i+1)); if(tag.empty()) continue;
            if(!supported.count(tag) && seen.insert(tag).second) warnings.push_back("unsupported tag \\"+tag);
            if(tag=="fn") {
                size_t a=i+3,b=a; while(b<text.size() && text[b]!='\\' && text[b]!='}') ++b;
                auto font=text.substr(a,b-a);
                if(!font.empty() && !YTFontAllowed(font) && seen.insert("font:"+font).second) warnings.push_back("unsupported YouTube font "+font);
            }
            if((tag=="ytruby"||tag=="ytvert"||tag=="ytpack"||tag=="ytsub"||tag=="ytsup") && seen.insert("mobile:"+tag).second)
                warnings.push_back("\\"+tag+" is PC-only or has a mobile fallback");
        }
    }
    return warnings;
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
    wxTextCtrl *nameBox=nullptr,*addressBox=nullptr,*passwordBox=nullptr,*chatLog=nullptr,*chatInput=nullptr;
    wxStaticText *status=nullptr,*people=nullptr,*recent=nullptr,*ytCurrentStatus=nullptr;
    wxTextCtrl *ytScanOutput=nullptr,*ytTagValue=nullptr;
    wxChoice *ytTagChoice=nullptr;
    wxColourPickerCtrl *ytTextColor=nullptr,*ytOutlineColor=nullptr,*ytShadowColor=nullptr;
    wxButton *ytInsertButton=nullptr,*ytScanButton=nullptr,*ytExportButton=nullptr,*ytConvertButton=nullptr;
    wxButton *hostButton=nullptr,*joinButton=nullptr,*leaveButton=nullptr,*undoMineButton=nullptr,*chatSendButton=nullptr,*lineNoteButton=nullptr,*cancelMediaButton=nullptr,*transferHostButton=nullptr;
    wxCheckBox *autoMediaBox=nullptr;
    wxChoice *followChoice=nullptr,*roleChoice=nullptr;
    struct PresenceInfo {std::string lineId; int frame=-1; bool typing=false;};
    std::map<Peer*,PresenceInfo> peerPresence;
    std::map<std::string,PresenceInfo> roomPresence;
    std::string localLineId;
    int localFrame=-1;
    bool presenceDirty=false, localTyping=false;
    Clock::time_point presenceSent=Clock::now();
    std::optional<collab::Document> localUndo;
    uint32_t localUndoRevision=0;
    wxIPV4address guestAddress;
    bool reconnecting=false, manualDisconnect=false;
    int reconnectAttempts=0;
    Clock::time_point reconnectAt=Clock::now(), reconnectStarted=Clock::now();
    bool hostTransferPending=false;
    std::string hostTransferTarget;
    collab::Document hostTransferDocument;
    struct MediaSend {
        std::ifstream file;
        uint64_t size=0, sent=0;
        int lastPercent=-1;
        std::string name,hash,kind;
        bool active=false;
    };
    struct MediaReceive {
        std::ofstream file;
        agi::fs::path partPath, finalPath;
        uint64_t expected=0, received=0;
        int lastPercent=-1;
        std::string name,hash,kind;
        bool active=false;
    };
    std::map<Peer*,MediaSend> mediaSends;
    MediaReceive mediaReceive;
    std::string offeredMediaName,offeredMediaHash,offeredMediaKind;
    uint64_t offeredMediaSize=0;
    std::string mediaHashCachePath,mediaHashCache;
    uint64_t mediaHashCacheSize=0;
    std::unique_ptr<wxSocketServer> listener;
    std::vector<std::unique_ptr<Peer>> peers;
    std::unique_ptr<collab::Room> room;
    collab::Document base, sent;
    uint32_t revision=0, sequence=0;
    bool ignoring=false, connected=false, inFlight=false, ticking=false, dirty=false;
    std::map<int,std::string> nativeIds;
    std::string name,password;
    Clock::time_point changed=Clock::now(), heartbeat=Clock::now(), checked=Clock::now();
    agi::signal::Connection commit, fileOpened, videoOpened, audioOpened, activeLineChanged, videoSeek;

    explicit Impl(agi::Context* context):c(context) {
        Bind(wxEVT_TIMER,[this](wxTimerEvent&) {Tick();});
        commit=c->ass->AddCommitListener([this](int type,const AssDialogue*) {
            if(ignoring) return;
            (void)type; // Undo also uses COMMIT_NEW; the file-open signal handles real file changes.
            changed=Clock::now(); dirty=true;
            if(connected) {localTyping=true; presenceDirty=true;}
        });
        fileOpened=c->subsController->AddFileOpenListener([this](agi::fs::path const&) {Stop("Disconnected: another subtitle file was opened.");});
        videoOpened=c->project->AddVideoProviderListener([this](AsyncVideoProvider*) {
            if(room) {
                for(auto& p:peers) {
                    if(p->authenticated) SendMediaOffer(*p);
                }
            }
        });
        audioOpened=c->project->AddAudioProviderListener([this](agi::AudioProvider*) {
            if(room && c->project->VideoName().empty()) {
                for(auto& p:peers) {
                    if(p->authenticated) SendMediaOffer(*p);
                }
            }
        });
        activeLineChanged=c->selectionController->AddActiveLineListener([this](AssDialogue* line) {
            localLineId=line?Identity(c->ass.get(),*line):std::string();
            UpdateYTCurrentLine();
            presenceDirty=true;
            if(c->subsGrid) c->subsGrid->Refresh(false);
            if(c->videoSlider) c->videoSlider->Refresh(false);
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
        if(roleChoice) roleChoice->Enable(!active);
        if(undoMineButton) undoMineButton->Enable(active && localUndo.has_value());
        if(chatSendButton) chatSendButton->Enable(active);
        if(lineNoteButton) lineNoteButton->Enable(active);
        if(cancelMediaButton) cancelMediaButton->Enable(active && (mediaReceive.active || !mediaSends.empty()));
        if(transferHostButton) {
            size_t editableGuests=0;
            if(room) {
                for(auto const& p:peers) {
                    if(p->authenticated && p->role=="Editor") ++editableGuests;
                }
            }
            transferHostButton->Enable(room && editableGuests==1 && peers.size()==1 && !hostTransferPending);
        }
    }
    void ResetFollow() {
        roomPresence.clear(); peerPresence.clear(); localLineId.clear(); localFrame=-1; presenceDirty=false; localTyping=false;
        reconnecting=false; reconnectAttempts=0;
        if(followChoice) {followChoice->Clear(); followChoice->Append("Do not follow"); followChoice->SetSelection(0);}
        if(c->subsGrid) c->subsGrid->Refresh(false);
            if(c->videoSlider) c->videoSlider->Refresh(false);
    }
    void Stop(std::string const& reason) {
        timer.Stop(); ClearMediaReceive(false); mediaSends.clear(); offeredMediaName.clear(); offeredMediaHash.clear(); offeredMediaKind.clear(); offeredMediaSize=0;
        peers.clear(); listener.reset(); room.reset(); connected=false; inFlight=false; ignoring=false; nativeIds.clear(); dirty=false;
        ResetFollow();
        if(window) {Status(reason); people->SetLabel("Not in a room"); Buttons();}
    }
    void Goodbye() noexcept {
        manualDisconnect=true;
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
    uint32_t ColorFor(std::string const& who) const {
        static uint32_t const palette[]={0x2D7FF9,0xD94FD5,0x27A96B,0xE67E22,0x8E5BD9,0xE74C3C,0x16A2B8,0xB58B00};
        uint32_t hash=2166136261u; for(unsigned char ch:who) {hash^=ch; hash*=16777619u;}
        return palette[hash%(sizeof(palette)/sizeof(palette[0]))];
    }
    void AppendChat(std::string const& who,std::string const& text,std::string const& lineId={}) {
        if(!chatLog) return;
        std::string where;
        if(!lineId.empty()) {
            int row=-1;
            for(auto const& line:c->ass->Events) if(Identity(c->ass.get(),line)==lineId) {row=line.Row+1; break;}
            if(row>0) where=" [line "+std::to_string(row)+"]";
        }
        chatLog->AppendText(Wx(who+where+": "+text+"\n"));
        if(!name.empty() && who!=name && text.find("@"+name)!=std::string::npos)
            Notice(who+" mentioned you.");
    }
    void SendChat(bool lineNote) {
        if(!connected || !chatInput) return;
        auto text=Utf8(chatInput->GetValue().Strip(wxString::both));
        if(text.empty()) return;
        if(text.size()>1000) {Notice("Chat message is too long."); return;}
        auto lineId=lineNote?RowLineId(c->selectionController->GetActiveLine()):std::string();
        if(room) {
            AppendChat(name,text,lineId);
            collab::Writer w; w.String("chat-event"); w.String(name); w.String(text); w.String(lineId);
            for(auto& p:peers) if(p->authenticated) p->Queue(w);
        }
        else if(!peers.empty()) {
            collab::Writer w; w.String("chat"); w.String(text); w.String(lineId); peers.front()->Queue(w);
        }
        chatInput->Clear();
    }
    void UndoMine() {
        if(!connected || !localUndo) {Notice("Nothing safe to undo yet."); return;}
        if(revision!=localUndoRevision+1) {Notice("Your last edit can no longer be undone safely because the room changed."); localUndo.reset(); Buttons(); return;}
        auto target=*localUndo; localUndo.reset();
        Apply(target); dirty=true; changed=Clock::now(); localTyping=true; presenceDirty=true; Buttons();
        Status("Undoing your last synced edit...");
    }
    void BeginReconnect(std::string const& why) {
        if(manualDisconnect || room) {Stop(why); return;}
        if(!reconnecting) Notice("Connection lost - reconnecting...");
        reconnecting=true; connected=false; inFlight=false; peers.clear();
        reconnectAt=Clock::now()+std::chrono::seconds(1); reconnectStarted=Clock::now();
        Status("Connection lost - reconnecting...");
    }
    void TryReconnect() {
        if(!reconnecting || !peers.empty()) return;
        if(++reconnectAttempts>10) {reconnecting=false; Stop("Could not reconnect after 10 attempts. Local edits are kept."); return;}
        auto socket=new wxSocketClient(wxSOCKET_NOWAIT);
        peers.push_back(std::make_unique<Peer>(socket));
        socket->Connect(guestAddress,false);
        reconnectStarted=Clock::now();
        Status("Reconnecting... attempt "+std::to_string(reconnectAttempts)+"/10");
    }
    void UpdateYTCurrentLine() {
        if(!ytCurrentStatus) return;
        auto line=c->selectionController->GetActiveLine();
        if(!line) {ytCurrentStatus->SetLabel("Current line: none"); return;}
        auto warnings=YTLineWarnings(line->Text.get());
        ytCurrentStatus->SetLabel(warnings.empty()?"Current line: YouTube compatible":"Current line: "+std::to_string(warnings.size())+" YouTube warning(s)");
        if(window) window->Layout();
    }
    void InsertOverride(std::string const& tag) {
        if(tag.empty()) return;
        c->textSelectionController->ReplaceSelection("{"+tag+"}");
        Notice("Applied "+tag+" to the current subtitle.");
    }
    void InsertYTTag() {
        if(!ytTagChoice || ytTagChoice->GetSelection()<=0) {
            Notice("Choose a YouTube/ASS tag first.");
            return;
        }
        auto tag=Utf8(ytTagChoice->GetStringSelection());
        auto value=ytTagValue?Utf8(ytTagValue->GetValue().Strip(wxString::both)):std::string();
        static std::set<std::string> const parenTags={"\\pos","\\fad","\\fade","\\move","\\t","\\ytshake","\\ytchroma","\\ytkt"};
        static std::set<std::string> const valueTags={"\\b","\\i","\\u","\\fn","\\fs","\\alpha","\\an","\\k","\\r","\\ytdir","\\ytpack","\\ytvert"};
        std::string built=tag;
        if(parenTags.count(tag)) {
            if(value.empty() && tag!="\\ytshake" && tag!="\\ytchroma") {Notice("Enter the values for "+tag+" first."); return;}
            if(!value.empty()) built+="("+value+")";
        }
        else if(valueTags.count(tag)) {
            if(value.empty() && tag!="\\r") {Notice("Enter a value for "+tag+" first."); return;}
            built+=value;
        }
        else if(tag=="Custom") {
            if(value.empty()) {Notice("Enter a custom tag first."); return;}
            built=value[0]=='\\'?value:"\\"+value;
        }
        InsertOverride(built);
    }
    void ApplyYTColor(int channel,wxColourPickerCtrl* picker) {
        if(!picker) return;
        InsertOverride("\\"+std::to_string(channel)+"c"+AssColor(picker->GetColour()));
    }
    agi::fs::path YTSubConverterPath() const {
        for(auto const& candidate:{
            c->path->Decode("?data/YTSubConverter.exe"),
            c->path->Decode("?data/YTSubConverter/YTSubConverter.exe")
        }) if(agi::fs::FileExists(candidate)) return candidate;
        return {};
    }
    bool RunYTSubConverter(agi::fs::path const& input,agi::fs::path const& output) {
#ifdef __WXMSW__
        auto exe=YTSubConverterPath();
        if(exe.empty()) {
            wxMessageBox("YTSubConverter.exe is not installed with this build. Create the portable package again or place YTSubConverter.exe next to aegisub.exe.",
                         "YTSubConverter",wxOK|wxICON_ERROR,c->parent);
            return false;
        }
        wxString command=QuoteCommandArg(wxString(exe.wstring()))+" "+QuoteCommandArg(wxString(input.wstring()))+" "+QuoteCommandArg(wxString(output.wstring()));
        long result=wxExecute(command,wxEXEC_SYNC);
        if(result!=0) {
            wxMessageBox("YTSubConverter could not convert this subtitle file. Check the YT compatibility scan for unsupported data.",
                         "YTSubConverter",wxOK|wxICON_ERROR,c->parent);
            return false;
        }
        return agi::fs::FileExists(output);
#else
        wxMessageBox("Bundled YTSubConverter integration is currently available in the Windows build.","YTSubConverter",wxOK|wxICON_INFORMATION,c->parent);
        return false;
#endif
    }
    void ExportYTT() {
        wxFileDialog save(c->parent,"Export YouTube Timed Text","","","YouTube Timed Text (*.ytt)|*.ytt",
                          wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(save.ShowModal()!=wxID_OK) return;
        agi::fs::path output(from_wx(save.GetPath()));
        if(output.extension().empty()) output.replace_extension(".ytt");
        auto tempDir=agi::fs::path(from_wx(wxStandardPaths::Get().GetTempDir()));
        auto input=tempDir/agi::fs::path("aegisub-ytsub-"+NewId()+".ass");
        try {
            AssSubtitleFormat().ExportFile(c->ass.get(),input,c->project->Timecodes(),"UTF-8");
            bool ok=RunYTSubConverter(input,output);
            try {agi::fs::Remove(input);} catch(...) {}
            if(ok) {
                Notice("Exported YouTube YTT: "+output.filename().string());
                if(c->frame) c->frame->StatusTimeout("Exported YTT: "+wxString(output.wstring()),8000);
            }
        }
        catch(std::exception const& e) {
            try {agi::fs::Remove(input);} catch(...) {}
            wxMessageBox(Wx(std::string("Could not export YTT: ")+e.what()),"YTSubConverter",wxOK|wxICON_ERROR,c->parent);
        }
    }
    void ConvertYTFile() {
        wxFileDialog open(c->parent,"Convert subtitle with YTSubConverter","","",
            "Subtitle files (*.ass;*.ytt;*.srv3;*.srt;*.sbv)|*.ass;*.ytt;*.srv3;*.srt;*.sbv|All files (*.*)|*.*",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
        if(open.ShowModal()!=wxID_OK) return;
        wxFileDialog save(c->parent,"Choose converted subtitle","","",
            "YouTube Timed Text (*.ytt)|*.ytt|YouTube SRV3 (*.srv3)|*.srv3|Advanced SubStation Alpha (*.ass)|*.ass|SubRip (*.srt)|*.srt|YouTube SBV (*.sbv)|*.sbv",
            wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(save.ShowModal()!=wxID_OK) return;
        agi::fs::path input(from_wx(open.GetPath())),output(from_wx(save.GetPath()));
        if(output.extension().empty()) {
            static char const* ext[]={".ytt",".srv3",".ass",".srt",".sbv"};
            output.replace_extension(ext[std::min<int>(save.GetFilterIndex(),4)]);
        }
        if(RunYTSubConverter(input,output)) {
            Notice("Converted subtitle: "+output.filename().string());
            if(c->frame) c->frame->StatusTimeout("YTSubConverter finished: "+wxString(output.wstring()),8000);
        }
    }
    void ScanYT() {
        if(!ytScanOutput) return;
        std::string out; int warningCount=0;
        for(auto const& style:c->ass->Styles) {
            if(!YTFontAllowed(style.font)) {out+="Style "+style.name+": unsupported YouTube font "+style.font+" (YTSubConverter will fall back to Roboto)\n"; ++warningCount;}
            if(style.strikeout || style.scalex!=100. || style.scaley!=100. || style.spacing!=0. || style.angle!=0.) {
                out+="Style "+style.name+": uses ASS style features outside the documented YTSubConverter supported set\n"; ++warningCount;
            }
        }
        for(auto const& line:c->ass->Events) {
            auto warnings=YTLineWarnings(line.Text.get());
            for(auto const& w:warnings) {out+="Line "+std::to_string(line.Row+1)+": "+w+"\n"; ++warningCount;}
        }
        if(!warningCount) out="No YTSubConverter compatibility warnings found.\n";
        else out="YTSubConverter scan: "+std::to_string(warningCount)+" warning(s)\n\n"+out;
        ytScanOutput->SetValue(Wx(out));
        Status("YTSubConverter compatibility scan finished.");
    }
    void TransferHost() {
        if(!room) {Notice("Only the host can transfer host."); return;}
        Peer* target=nullptr;
        for(auto const& p:peers) if(p->authenticated && p->role=="Editor") {
            if(target) {Notice("Host transfer currently requires exactly one connected editor."); return;}
            target=p.get();
        }
        if(!target || peers.size()!=1) {Notice("Host transfer currently requires exactly one connected editor."); return;}
        hostTransferPending=true; hostTransferTarget=target->name; hostTransferDocument=room->Current();
        collab::Writer w; w.String("become-host"); w.String(name); w.Doc(hostTransferDocument); target->Queue(w);
        Status("Transferring host to "+hostTransferTarget+"...");
        Buttons();
    }
    void BecomeHostFromPeer(Peer& oldHost,std::string const& oldHostName,collab::Document const& doc) {
        wxIPV4address local;
        if(!oldHost.socket->GetLocal(local)) throw collab::Conflict("Could not determine your Hamachi address for host transfer.");
        wxIPV4address listen; listen.Hostname(local.IPAddress()); listen.Service(Port);
        auto newListener=std::make_unique<wxSocketServer>(listen,wxSOCKET_NOWAIT); newListener->Notify(false);
        if(!newListener->IsOk()) {
            collab::Writer fail; fail.String("host-transfer-failed"); fail.String("Could not listen on the transferred host address."); oldHost.Queue(fail);
            return;
        }
        oldHost.name=oldHostName; oldHost.role="Editor"; oldHost.authenticated=true; oldHost.hello=true; oldHost.ack=0;
        room=std::make_unique<collab::Room>(doc); base=doc; revision=1; sequence=0; inFlight=false; connected=true;
        listener=std::move(newListener); reconnecting=false; hostTransferPending=false;
        collab::Writer ready; ready.String("host-ready"); ready.String(name); oldHost.Queue(ready); oldHost.Flush();
        People(); BroadcastPresence(); Notice("You are now the room host."); Status("Hosting - host transfer completed."); Buttons();
    }
    void FinishHostTransfer(Peer& newHost,std::string const& newHostName) {
        if(!hostTransferPending || newHostName!=hostTransferTarget) throw collab::Conflict("Unexpected host transfer acknowledgement.");
        listener.reset(); room.reset(); mediaSends.clear(); peerPresence.clear();
        base=hostTransferDocument; revision=1; sequence=0; inFlight=false; connected=true; reconnecting=false;
        newHost.name=newHostName; newHost.role="Host"; newHost.authenticated=true; newHost.hello=true; newHost.ack=0;
        wxIPV4address newHostAddress;
        if(newHost.socket->GetPeer(newHostAddress)) {newHostAddress.Service(Port); guestAddress=newHostAddress;}
        hostTransferPending=false; hostTransferTarget.clear();
        people->SetLabel(Wx("Connected users (2)\n\n"+newHostName+" (Host)\n"+name+" (Editor)"));
        Notice(newHostName+" is now the room host."); Status("Connected - host transfer completed."); presenceDirty=true; Buttons();
    }
    void Guard(std::function<void()> fn) {
        try {fn();}
        catch(std::exception const& e) {Stop(e.what()); if(window) {window->Show(); window->Raise();}}
        catch(...) {Stop("Could not sync this project. Local edits are kept."); if(window) window->Show();}
    }
    void Show() {
        if(!window) {
            window=new wxDialog(c->parent,wxID_ANY,"Aegisub Collaboration",wxDefaultPosition,wxSize(760,760),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
            auto root=new wxBoxSizer(wxVERTICAL);
            auto tabs=new wxNotebook(window,wxID_ANY);

            // Room tab
            auto collabPage=new wxScrolledWindow(tabs,wxID_ANY);
            collabPage->SetScrollRate(0,12);
            auto collabRoot=new wxBoxSizer(wxVERTICAL);
            auto title=new wxStaticText(collabPage,wxID_ANY,"Live subtitle collaboration");
            auto font=title->GetFont(); font.SetPointSize(font.GetPointSize()+4); font.SetWeight(wxFONTWEIGHT_BOLD); title->SetFont(font);
            collabRoot->Add(title,0,wxLEFT|wxRIGHT|wxTOP,18);
            auto hint=new wxStaticText(collabPage,wxID_ANY,"Work in the same subtitle file over Hamachi. Changes, presence, chat and media stay in sync.");
            hint->Wrap(680); collabRoot->Add(hint,0,wxLEFT|wxRIGHT|wxTOP|wxBOTTOM,18);

            auto connectionBox=new wxStaticBoxSizer(wxVERTICAL,collabPage,"Connection");
            auto fields=new wxFlexGridSizer(2,8,12); fields->AddGrowableCol(1,1);
            auto addField=[&](char const* label,wxTextCtrl*& box,long style=0) {
                fields->Add(new wxStaticText(collabPage,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL|wxLEFT,4);
                box=new wxTextCtrl(collabPage,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,style);
                fields->Add(box,1,wxEXPAND);
            };
            addField("Display name",nameBox); nameBox->SetMaxLength(40);
            addField("Hamachi IPv4",addressBox);
            addField("Room password",passwordBox,wxTE_PASSWORD);
            fields->Add(new wxStaticText(collabPage,wxID_ANY,"Join as"),0,wxALIGN_CENTER_VERTICAL|wxLEFT,4);
            roleChoice=new wxChoice(collabPage,wxID_ANY); roleChoice->Append("Editor"); roleChoice->Append("Viewer"); roleChoice->SetSelection(0);
            fields->Add(roleChoice,1,wxEXPAND);
            connectionBox->Add(fields,0,wxEXPAND|wxALL,12);
            autoMediaBox=new wxCheckBox(collabPage,wxID_ANY,"Automatically receive matching host video or audio");
            autoMediaBox->SetValue(true); connectionBox->Add(autoMediaBox,0,wxLEFT|wxRIGHT|wxBOTTOM,12);
            auto buttons=new wxBoxSizer(wxHORIZONTAL);
            hostButton=new wxButton(collabPage,wxID_ANY,"Host room");
            joinButton=new wxButton(collabPage,wxID_ANY,"Join room");
            leaveButton=new wxButton(collabPage,wxID_ANY,"Disconnect");
            buttons->Add(hostButton,1,wxRIGHT,8); buttons->Add(joinButton,1,wxRIGHT,8); buttons->Add(leaveButton,1);
            connectionBox->Add(buttons,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,12);
            collabRoot->Add(connectionBox,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,18);

            auto roomBox=new wxStaticBoxSizer(wxVERTICAL,collabPage,"Room status");
            status=new wxStaticText(collabPage,wxID_ANY,"Open the subtitles you want to share, then host or join a room."); status->Wrap(650);
            people=new wxStaticText(collabPage,wxID_ANY,"Not in a room");
            recent=new wxStaticText(collabPage,wxID_ANY,"Recent: none");
            roomBox->Add(status,0,wxEXPAND|wxALL,10);
            roomBox->Add(people,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
            auto followRow=new wxBoxSizer(wxHORIZONTAL);
            followRow->Add(new wxStaticText(collabPage,wxID_ANY,"Follow playhead"),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,10);
            followChoice=new wxChoice(collabPage,wxID_ANY); followChoice->Append("Do not follow"); followChoice->SetSelection(0);
            followRow->Add(followChoice,1);
            roomBox->Add(followRow,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
            auto actionButtons=new wxBoxSizer(wxHORIZONTAL);
            undoMineButton=new wxButton(collabPage,wxID_ANY,"Undo my edit");
            cancelMediaButton=new wxButton(collabPage,wxID_ANY,"Cancel transfer");
            transferHostButton=new wxButton(collabPage,wxID_ANY,"Transfer host");
            actionButtons->Add(undoMineButton,1,wxRIGHT,8); actionButtons->Add(cancelMediaButton,1,wxRIGHT,8); actionButtons->Add(transferHostButton,1);
            roomBox->Add(actionButtons,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
            roomBox->Add(recent,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
            collabRoot->Add(roomBox,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,18);

            auto chatBox=new wxStaticBoxSizer(wxVERTICAL,collabPage,"Chat and line notes");
            chatLog=new wxTextCtrl(collabPage,wxID_ANY,"",wxDefaultPosition,wxSize(-1,120),wxTE_MULTILINE|wxTE_READONLY);
            chatInput=new wxTextCtrl(collabPage,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxTE_PROCESS_ENTER);
            auto chatButtons=new wxBoxSizer(wxHORIZONTAL);
            chatSendButton=new wxButton(collabPage,wxID_ANY,"Send message");
            lineNoteButton=new wxButton(collabPage,wxID_ANY,"Attach note to current line");
            chatButtons->Add(chatSendButton,1,wxRIGHT,8); chatButtons->Add(lineNoteButton,1);
            chatBox->Add(chatLog,0,wxEXPAND|wxALL,10);
            chatBox->Add(chatInput,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
            chatBox->Add(chatButtons,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
            collabRoot->Add(chatBox,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,18);
            collabPage->SetSizer(collabRoot);
            tabs->AddPage(collabPage,"Room",true);

            // YouTube tools tab
            auto ytPage=new wxScrolledWindow(tabs,wxID_ANY);
            ytPage->SetScrollRate(0,12);
            auto ytRoot=new wxBoxSizer(wxVERTICAL);
            auto ytTitle=new wxStaticText(ytPage,wxID_ANY,"YouTube subtitle tools");
            auto ytFont=ytTitle->GetFont(); ytFont.SetPointSize(ytFont.GetPointSize()+4); ytFont.SetWeight(wxFONTWEIGHT_BOLD); ytTitle->SetFont(ytFont);
            ytRoot->Add(ytTitle,0,wxLEFT|wxRIGHT|wxTOP,18);
            auto ytHint=new wxStaticText(ytPage,wxID_ANY,"Build YTSubConverter tags without memorizing syntax, scan the file, or export the current subtitles directly to YTT.");
            ytHint->Wrap(680); ytRoot->Add(ytHint,0,wxLEFT|wxRIGHT|wxTOP|wxBOTTOM,18);
            ytCurrentStatus=new wxStaticText(ytPage,wxID_ANY,"Current line: not checked");
            ytRoot->Add(ytCurrentStatus,0,wxLEFT|wxRIGHT|wxBOTTOM,18);

            auto exportBox=new wxStaticBoxSizer(wxVERTICAL,ytPage,"YTSubConverter");
            auto exportRow=new wxBoxSizer(wxHORIZONTAL);
            ytExportButton=new wxButton(ytPage,wxID_ANY,"Export current subtitles as .ytt");
            ytConvertButton=new wxButton(ytPage,wxID_ANY,"Convert another subtitle file...");
            exportRow->Add(ytExportButton,1,wxRIGHT,8); exportRow->Add(ytConvertButton,1);
            exportBox->Add(exportRow,0,wxEXPAND|wxALL,10);
            ytRoot->Add(exportBox,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,18);

            auto tagBox=new wxStaticBoxSizer(wxVERTICAL,ytPage,"Apply tags to the current line");
            auto tagHelp=new wxStaticText(ytPage,wxID_ANY,"Choose the tag, type your own value or arguments, then apply. No values are pre-filled.");
            tagHelp->Wrap(650); tagBox->Add(tagHelp,0,wxEXPAND|wxALL,10);
            auto tagRow=new wxFlexGridSizer(2,8,10); tagRow->AddGrowableCol(1,1);
            tagRow->Add(new wxStaticText(ytPage,wxID_ANY,"Tag"),0,wxALIGN_CENTER_VERTICAL);
            ytTagChoice=new wxChoice(ytPage,wxID_ANY);
            for(auto const* tag:{"Choose a tag...","\\b","\\i","\\u","\\fn","\\fs","\\1a","\\2a","\\3a","\\4a","\\alpha","\\2c","\\pos","\\an","\\k","\\r","\\fad","\\fade","\\move","\\t","\\ytsub","\\ytsup","\\ytsur","\\ytruby","\\ytruby8","\\ytruby2","\\ytvert9","\\ytvert7","\\ytvert1","\\ytvert3","\\ytdir","\\ytpack","\\ytshake","\\ytchroma","\\ytktFade","\\ytktGlitch","\\ytkt","Custom"}) ytTagChoice->Append(tag);
            ytTagChoice->SetSelection(0); tagRow->Add(ytTagChoice,1,wxEXPAND);
            tagRow->Add(new wxStaticText(ytPage,wxID_ANY,"Value / arguments"),0,wxALIGN_CENTER_VERTICAL);
            ytTagValue=new wxTextCtrl(ytPage,wxID_ANY,""); tagRow->Add(ytTagValue,1,wxEXPAND);
            tagBox->Add(tagRow,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
            ytInsertButton=new wxButton(ytPage,wxID_ANY,"Apply tag at subtitle cursor");
            tagBox->Add(ytInsertButton,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
            ytRoot->Add(tagBox,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,18);

            auto colorBox=new wxStaticBoxSizer(wxVERTICAL,ytPage,"YouTube colors");
            auto colors=new wxFlexGridSizer(3,8,10); colors->AddGrowableCol(1,1);
            auto textColorButton=new wxButton(ytPage,wxID_ANY,"Apply text color");
            auto outlineColorButton=new wxButton(ytPage,wxID_ANY,"Apply outline color");
            auto shadowColorButton=new wxButton(ytPage,wxID_ANY,"Apply shadow color");
            colors->Add(new wxStaticText(ytPage,wxID_ANY,"Text"),0,wxALIGN_CENTER_VERTICAL); ytTextColor=new wxColourPickerCtrl(ytPage,wxID_ANY,*wxWHITE); colors->Add(ytTextColor,1,wxEXPAND); colors->Add(textColorButton);
            colors->Add(new wxStaticText(ytPage,wxID_ANY,"Outline"),0,wxALIGN_CENTER_VERTICAL); ytOutlineColor=new wxColourPickerCtrl(ytPage,wxID_ANY,*wxBLACK); colors->Add(ytOutlineColor,1,wxEXPAND); colors->Add(outlineColorButton);
            colors->Add(new wxStaticText(ytPage,wxID_ANY,"Shadow"),0,wxALIGN_CENTER_VERTICAL); ytShadowColor=new wxColourPickerCtrl(ytPage,wxID_ANY,*wxBLACK); colors->Add(ytShadowColor,1,wxEXPAND); colors->Add(shadowColorButton);
            colorBox->Add(colors,0,wxEXPAND|wxALL,10);
            ytRoot->Add(colorBox,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,18);

            auto scanBox=new wxStaticBoxSizer(wxVERTICAL,ytPage,"Compatibility");
            ytScanButton=new wxButton(ytPage,wxID_ANY,"Scan current subtitles for YouTube issues");
            ytScanOutput=new wxTextCtrl(ytPage,wxID_ANY,"",wxDefaultPosition,wxSize(-1,130),wxTE_MULTILINE|wxTE_READONLY);
            scanBox->Add(ytScanButton,0,wxEXPAND|wxALL,10); scanBox->Add(ytScanOutput,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
            ytRoot->Add(scanBox,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,18);
            ytPage->SetSizer(ytRoot);
            tabs->AddPage(ytPage,"YouTube Tools",false);

            // Reference tab stays intentionally read-only.
            auto refPage=new wxPanel(tabs);
            auto refRoot=new wxBoxSizer(wxVERTICAL);
            auto reference=new wxTextCtrl(refPage,wxID_ANY,wxString::FromUTF8(R"YTREF(YTSubConverter quick reference

STANDARD TAGS
\\b / \\i / \\u          bold / italic / underline
\\fn                     font name
\\fs                     font size
\\1c / \\2c / \\3c / \\4c text, karaoke, outline, shadow colors
\\1a / \\2a / \\3a / \\4a alpha channels
\\alpha                  all alpha channels
\\pos(x,y)               position
\\an1 ... \\an9         alignment
\\k                      karaoke duration
\\r                      reset formatting
\\fad / \\fade          fades
\\move                   movement
\\t                      animation

YOUTUBE-SPECIFIC TAGS
\\ytsub / \\ytsup / \\ytsur
\\ytruby / \\ytruby8 / \\ytruby2
\\ytvert9 / \\ytvert7 / \\ytvert1 / \\ytvert3
\\ytdir4
\\ytpack1 / \\ytpack0
\\ytshake(...)
\\ytchroma(...)
\\ytktFade
\\ytktGlitch
\\ytkt(...)

NOTES
- Unsupported fonts fall back to Roboto.
- Some ruby, vertical, packing, subscript and superscript behavior is PC-only or has a mobile fallback.
- Fully transparent secondary karaoke alpha enables native YouTube karaoke hiding.
- Outline/shadow behavior on YouTube is more limited than full ASS rendering.
- The compatibility scanner flags unsupported tags and known mobile caveats.

The Windows portable build includes the official YTSubConverter tool for direct .ytt export.
)YTREF"),wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_READONLY|wxTE_RICH2);
            auto mono=reference->GetFont(); mono.SetFamily(wxFONTFAMILY_TELETYPE); reference->SetFont(mono);
            refRoot->Add(reference,1,wxEXPAND|wxALL,12); refPage->SetSizer(refRoot);
            tabs->AddPage(refPage,"Reference",false);

            root->Add(tabs,1,wxEXPAND|wxALL,8);
            window->SetSizer(root); window->SetMinSize(wxSize(650,600)); window->SetSize(wxSize(760,760)); window->CentreOnParent();
            window->Bind(wxEVT_CLOSE_WINDOW,&Impl::OnClose,this);
            hostButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Guard([this]{Start(true);});});
            joinButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Guard([this]{Start(false);});});
            leaveButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Goodbye(); Stop("Disconnected. Your subtitles stay open.");});
            undoMineButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Guard([this]{UndoMine();});});
            transferHostButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {Guard([this]{TransferHost();});});
            cancelMediaButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
                if(!connected) return;
                if(!room && !peers.empty()) {collab::Writer w; w.String("media-cancel"); peers.front()->Queue(w);}
                for(auto& kv:mediaSends) {kv.second.file.close(); kv.second.active=false;}
                mediaSends.clear();
                if(mediaReceive.file.is_open()) mediaReceive.file.close();
                mediaReceive.active=false;
                Status("Media transfer canceled. Partial download kept for resume.");
                Buttons();
            });
            chatSendButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {SendChat(false);});
            lineNoteButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {SendChat(true);});
            chatInput->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&) {SendChat(false);});
            ytInsertButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {InsertYTTag();});
            ytScanButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {ScanYT();});
            ytExportButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {ExportYTT();});
            ytConvertButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {ConvertYTFile();});
            textColorButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {ApplyYTColor(1,ytTextColor);});
            outlineColorButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {ApplyYTColor(3,ytOutlineColor);});
            shadowColorButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {ApplyYTColor(4,ytShadowColor);});
            UpdateYTCurrentLine();
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
    bool MediaForKind(std::string const& kind,std::string& filename,uint64_t& size,agi::fs::path& path,std::string& hash) {
        if(kind=="video") path=c->project->VideoName();
        else if(kind=="audio") path=c->project->AudioName();
        else return false;
        if(path.empty()) return false;
        try {
            if(!agi::fs::FileExists(path)) return false;
            auto bytes=agi::fs::Size(path);
            if(!bytes || bytes>MaxMediaBytes) return false;
            filename=path.filename().string(); size=static_cast<uint64_t>(bytes);
            auto key=kind+":"+path.string();
            if(mediaHashCachePath!=key || mediaHashCacheSize!=size || mediaHashCache.empty()) {
                Status("Calculating "+kind+" SHA-256...");
                mediaHashCache=collab::FileSha256Hex(path); mediaHashCachePath=key; mediaHashCacheSize=size;
            }
            hash=mediaHashCache;
            return !filename.empty() && hash.size()==64;
        }
        catch(...) {return false;}
    }
    bool CurrentMedia(std::string& kind,std::string& filename,uint64_t& size,agi::fs::path& path,std::string& hash) {
        if(!c->project->VideoName().empty()) {
            kind="video";
            if(MediaForKind(kind,filename,size,path,hash)) return true;
        }
        if(!c->project->AudioName().empty()) {
            kind="audio";
            if(MediaForKind(kind,filename,size,path,hash)) return true;
        }
        return false;
    }
    bool LocalMediaMatches(std::string const& kind,std::string const& filename,uint64_t size,std::string const& hash) {
        auto const& path=kind=="audio"?c->project->AudioName():c->project->VideoName();
        if(path.empty() || path.filename().string()!=filename) return false;
        try {
            if(!agi::fs::FileExists(path) || agi::fs::Size(path)!=size) return false;
            return collab::FileSha256Hex(path)==hash;
        }
        catch(...) {return false;}
    }
    void SendMediaOffer(Peer& p) {
        std::string kind,filename,hash; uint64_t size=0; agi::fs::path path;
        if(!CurrentMedia(kind,filename,size,path,hash)) return;
        collab::Writer w; w.String("media-offer"); w.String(kind); w.String(filename); Write64(w,size); w.String(hash); p.Queue(w);
    }
    void RequestMedia(Peer& p,std::string const& kind,std::string const& filename,uint64_t size,std::string const& hash) {
        if((kind!="video" && kind!="audio") || filename.empty() || filename.size()>512 || !size || size>MaxMediaBytes || hash.size()!=64 || hash.find_first_not_of("0123456789abcdef")!=std::string::npos)
            throw collab::Conflict("Invalid shared media offer.");
        auto dirWx=wxStandardPaths::Get().GetUserLocalDataDir()+"/collaboration-media";
        agi::fs::path dir(Utf8(dirWx)); agi::fs::CreateDirectory(dir);
        if(agi::fs::FreeSpace(dir)<size+64ull*1024*1024) throw collab::Conflict("Not enough free disk space for the shared media.");
        auto localName=SafeMediaFilename(filename,hash);
        ClearMediaReceive(false);
        mediaReceive.name=filename; mediaReceive.expected=size; mediaReceive.hash=hash; mediaReceive.kind=kind;
        mediaReceive.finalPath=dir/agi::fs::path(localName);
        mediaReceive.partPath=dir/agi::fs::path(localName+".part");
        offeredMediaName=filename; offeredMediaSize=size; offeredMediaHash=hash; offeredMediaKind=kind;
        uint64_t offset=0;
        try {if(agi::fs::FileExists(mediaReceive.partPath)) offset=std::min<uint64_t>(agi::fs::Size(mediaReceive.partPath),size);} catch(...) {offset=0;}
        mediaReceive.received=offset;
        collab::Writer w; w.String("media-request"); w.String(kind); w.String(filename); Write64(w,size); w.String(hash); Write64(w,offset); p.Queue(w);
        Status((offset?"Resuming ":"Requesting ")+kind+" "+filename+" at "+HumanBytes(offset)+" / "+HumanBytes(size)+"...");
        Buttons();
    }
    void BeginMediaReceive(std::string const& kind,std::string const& filename,uint64_t size,std::string const& hash,uint64_t offset) {
        if(kind!=offeredMediaKind || filename!=offeredMediaName || size!=offeredMediaSize || hash!=offeredMediaHash ||
           kind!=mediaReceive.kind || filename!=mediaReceive.name || size!=mediaReceive.expected || hash!=mediaReceive.hash || offset!=mediaReceive.received)
            throw collab::Conflict("The host media changed while the download was starting.");
        if(offset==0) {
            std::ofstream reset(mediaReceive.partPath,std::ios::binary|std::ios::trunc);
            if(!reset) throw collab::Conflict("Could not create the shared media download file.");
        }
        mediaReceive.file.open(mediaReceive.partPath,std::ios::binary|std::ios::app);
        if(!mediaReceive.file) throw collab::Conflict("Could not open the shared media download file.");
        mediaReceive.lastPercent=-1; mediaReceive.active=true;
        Status("Downloading "+kind+" "+filename+" - "+std::to_string(size?offset*100/size:0)+"% of "+HumanBytes(size));
        Buttons();
    }
    void BeginMediaSend(Peer& p,std::string const& kind,std::string const& requestedName,uint64_t requestedSize,std::string const& requestedHash,uint64_t offset) {
        std::string filename,hash; uint64_t size=0; agi::fs::path path;
        if(!MediaForKind(kind,filename,size,path,hash) || filename!=requestedName || size!=requestedSize || hash!=requestedHash || offset>size)
            throw collab::Conflict("The host media changed. Request the transfer again.");
        MediaSend transfer; transfer.file.open(path,std::ios::binary);
        if(!transfer.file) throw collab::Conflict("Could not read the host media.");
        transfer.file.seekg(static_cast<std::streamoff>(offset),std::ios::beg);
        if(!transfer.file) throw collab::Conflict("Could not resume the host media transfer.");
        transfer.size=size; transfer.sent=offset; transfer.name=filename; transfer.hash=hash; transfer.kind=kind; transfer.active=true;
        mediaSends[&p]=std::move(transfer);
        collab::Writer w; w.String("media-start"); w.String(kind); w.String(filename); Write64(w,size); w.String(hash); Write64(w,offset); p.Queue(w);
        Status("Sending "+kind+" "+filename+" to "+p.name+(offset?" (resumed)...":"...")); Buttons();
    }
    void PumpMedia(Peer& p) {
        auto it=mediaSends.find(&p);
        if(it==mediaSends.end() || !it->second.active || p.output.size()>1024*1024) return;
        auto& transfer=it->second;
        if(transfer.sent>=transfer.size) return;
        auto want=static_cast<size_t>(std::min<uint64_t>(MediaChunkBytes,transfer.size-transfer.sent));
        std::string bytes(want,'\0'); transfer.file.read(bytes.data(),static_cast<std::streamsize>(want));
        auto got=transfer.file.gcount(); if(got<=0) throw collab::Conflict("Could not finish reading the host media.");
        bytes.resize(static_cast<size_t>(got));
        collab::Writer w; w.String("media-chunk"); Write64(w,transfer.sent); w.String(bytes); p.Queue(w);
        transfer.sent+=static_cast<uint64_t>(got);
        int percent=static_cast<int>((transfer.sent*100)/transfer.size);
        if(percent!=transfer.lastPercent) {transfer.lastPercent=percent; Status("Sending "+transfer.kind+" "+transfer.name+" to "+p.name+" - "+std::to_string(percent)+"%");}
        if(transfer.sent==transfer.size) {
            collab::Writer done; done.String("media-done"); done.String(transfer.kind); Write64(done,transfer.size); done.String(transfer.hash); p.Queue(done);
            transfer.file.close(); transfer.active=false; Buttons();
        }
    }
    void FinishMediaReceive(std::string const& kind,uint64_t size,std::string const& hash) {
        if(!mediaReceive.active || kind!=mediaReceive.kind || size!=mediaReceive.expected || hash!=mediaReceive.hash || mediaReceive.received!=mediaReceive.expected)
            throw collab::Conflict("Shared media download was incomplete.");
        mediaReceive.file.close(); mediaReceive.active=false;
        if(agi::fs::Size(mediaReceive.partPath)!=mediaReceive.expected) throw collab::Conflict("Shared media size check failed.");
        Status("Verifying downloaded "+kind+" SHA-256...");
        if(collab::FileSha256Hex(mediaReceive.partPath)!=mediaReceive.hash) throw collab::Conflict("Shared media SHA-256 verification failed.");
        if(agi::fs::FileExists(mediaReceive.finalPath)) agi::fs::Remove(mediaReceive.finalPath);
        agi::fs::Rename(mediaReceive.partPath,mediaReceive.finalPath);
        auto finalPath=mediaReceive.finalPath; auto displayName=mediaReceive.name; mediaReceive=MediaReceive{};
        if(kind=="audio") {
            c->project->LoadAudio(finalPath);
            if(c->project->AudioName()==finalPath) Status("Connected - verified and opened shared audio "+displayName+".");
            else Status("Connected - audio verified, but Aegisub could not open it automatically.");
        }
        else {
            c->project->LoadVideo(finalPath);
            if(c->project->VideoName()==finalPath) Status("Connected - verified and opened shared video "+displayName+".");
            else Status("Connected - video verified, but Aegisub could not open it automatically.");
        }
        Buttons();
    }
    void Start(bool host) {
        manualDisconnect=false; reconnecting=false; reconnectAttempts=0;
        name=Utf8(nameBox->GetValue().Strip(wxString::both)); CheckName(name);
        password=Utf8(passwordBox->GetValue());
        if(password.size()<6 || password.size()>128) throw collab::Conflict("Use a room password between 6 and 128 bytes.");
        auto ip=addressBox->GetValue().Strip(wxString::both);
        // Numeric IPv4 only, avoiding a blocking DNS lookup on the editor thread.
        auto address=Utf8(ip); unsigned a,b,d,e; char rest;
        if(std::sscanf(address.c_str(),"%u.%u.%u.%u%c",&a,&b,&d,&e,&rest)!=4 || a>255 || b>255 || d>255 || e>255 || a==0 || a>=224)
            throw collab::Conflict("Enter the Hamachi IPv4 address, for example 25.12.34.56.");
        wxIPV4address addr; addr.Hostname(ip); addr.Service(Port);
        guestAddress=addr;
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
        w.String(name); w.String(localLineId); w.Number(localFrame<0?0:static_cast<uint32_t>(localFrame+1)); w.Number(localTyping?1:0);
        for(auto const& p:peers) if(p->authenticated) {
            auto it=peerPresence.find(p.get());
            w.String(p->name);
            w.String(it==peerPresence.end()?std::string():it->second.lineId);
            auto frame=it==peerPresence.end()?-1:it->second.frame;
            w.Number(frame<0?0:static_cast<uint32_t>(frame+1));
            w.Number(it!=peerPresence.end() && it->second.typing ? 1:0);
        }
        for(auto& p:peers) if(p->authenticated) p->Queue(w);
        UpdateFollowChoices();
        if(c->subsGrid) c->subsGrid->Refresh(false);
            if(c->videoSlider) c->videoSlider->Refresh(false);
    }
    void SendPresence() {
        if(!connected) return;
        if(room) {BroadcastPresence(); return;}
        if(peers.empty()) return;
        collab::Writer w; w.String("presence"); w.String(localLineId); w.Number(localFrame<0?0:static_cast<uint32_t>(localFrame+1)); w.Number(localTyping?1:0);
        peers.front()->Queue(w);
    }
    void People() {
        size_t count=1;
        for(auto const& p:peers) if(p->authenticated) ++count;
        std::string names="Connected users ("+std::to_string(count)+")\n\n"+name+" (Host)";
        for(auto const& p:peers) if(p->authenticated) {
            auto it=peerPresence.find(p.get());
            auto state=(it!=peerPresence.end() && it->second.typing)?"typing":"viewing";
            names+="\n"+p->name+" ("+p->role+", "+state+")";
        }
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
            if(current!=base) {localUndo=base; localUndoRevision=revision;}
            if(room->Apply(revision,current)) {
                base=room->Current(); revision=static_cast<uint32_t>(room->Revision()); localTyping=false; presenceDirty=true;
                if(current!=base) Apply(base);
                Broadcast();
                Buttons();
            }
        }
        else if(!inFlight && current!=base && !peers.empty()) {
            localUndo=base; localUndoRevision=revision;
            collab::Writer w; w.String("update"); w.Number(revision); w.Number(++sequence); w.Doc(current);
            peers.front()->Queue(w); sent=current; inFlight=true; Status("Sending edits..."); Buttons();
        }
    }
    void Handle(Peer& p,std::string const& message) {
        collab::Reader r(message); auto kind=r.String();
        if(room) {
            if(!p.authenticated) {
                if(kind!="hello" || r.Number()!=7) throw collab::Conflict("Incompatible collaboration build.");
                auto username=r.String(); auto secret=r.String(); auto requestedRole=r.String(); r.End(); CheckName(username);
                if(requestedRole!="Editor" && requestedRole!="Viewer") throw collab::Conflict("Invalid collaboration role.");
                if(secret!=password) throw collab::Conflict("Room password did not match.");
                if(username==name || std::any_of(peers.begin(),peers.end(),[&](auto const& q){return q.get()!=&p && q->authenticated && q->name==username;})) throw collab::Conflict("That name is already in the room.");
                p.name=username; p.role=requestedRole; p.authenticated=true; State(p); People(); SendMediaOffer(p);
                BroadcastNotice(username+" joined the room."); presenceDirty=true; return;
            }
            if(kind=="host-ready") {
                auto newHostName=r.String(); r.End(); FinishHostTransfer(p,newHostName); return;
            }
            if(kind=="host-transfer-failed") {
                auto why=r.String(); r.End(); hostTransferPending=false; Notice("Host transfer failed: "+why); Buttons(); return;
            }
            if(kind=="leave") {r.End(); throw collab::Conflict("Client disconnected.");}
            if(kind=="presence") {
                auto lineId=r.String(); auto encodedFrame=r.Number(); auto typing=r.Number(); r.End();
                if(typing>1 || (!lineId.empty() && (lineId.size()!=32 || lineId.find_first_not_of("0123456789abcdef")!=std::string::npos)))
                    throw collab::Conflict("Invalid collaboration presence.");
                peerPresence[&p]={lineId,encodedFrame?static_cast<int>(encodedFrame-1):-1,typing==1};
                MaybeFollow(p.name,peerPresence[&p].frame); presenceDirty=true;
                if(c->subsGrid) c->subsGrid->Refresh(false);
            if(c->videoSlider) c->videoSlider->Refresh(false);
                return;
            }
            if(kind=="chat") {
                auto text=r.String(), lineId=r.String(); r.End();
                if(text.empty() || text.size()>1000 || lineId.size()>32) throw collab::Conflict("Invalid chat message.");
                AppendChat(p.name,text,lineId);
                collab::Writer w; w.String("chat-event"); w.String(p.name); w.String(text); w.String(lineId);
                for(auto& q:peers) if(q->authenticated) q->Queue(w);
                return;
            }
            if(kind=="media-request") {
                auto mediaKind=r.String(); auto requestedName=r.String(); auto requestedSize=Read64(r); auto requestedHash=r.String(); auto offset=Read64(r); r.End();
                BeginMediaSend(p,mediaKind,requestedName,requestedSize,requestedHash,offset); return;
            }
            if(kind=="media-cancel") {
                r.End(); auto it=mediaSends.find(&p); if(it!=mediaSends.end()) {it->second.file.close(); mediaSends.erase(it);}
                Status(p.name+" canceled the media transfer."); Buttons(); return;
            }
            if(kind=="media-received") {
                r.End(); Status(p.name + " finished downloading the host video."); return;
            }
            if(kind=="update") {
                auto rev=r.Number(),seq=r.Number(); auto doc=r.Doc(); r.End(); CheckStyles(doc);
                if(p.role=="Viewer") throw collab::Conflict("Viewer accounts cannot edit subtitles.");
                if(seq!=p.ack+1) throw collab::Conflict("Unexpected update sequence.");
                Sync(); // Publish the host's local changes before applying a peer update.
                try {
                    room->Apply(rev,doc); p.ack=seq; auto current=Snapshot(); base=room->Current(); revision=static_cast<uint32_t>(room->Revision());
                    if(current!=base) Apply(base);
                    Broadcast(); Status("Connected - all received edits are synced.");
                }
                catch(collab::Conflict const& e) {
                    p.ack=seq;
                    collab::Writer w; w.String("conflict"); w.String(e.what()); w.Number(static_cast<uint32_t>(room->Revision())); w.Doc(room->Current()); p.Queue(w);
                }
                return;
            }
        }
        else {
            if(kind=="become-host") {
                auto oldHostName=r.String(); auto doc=r.Doc(); r.End(); CheckStyles(doc);
                BecomeHostFromPeer(p,oldHostName,doc); return;
            }
            if(kind=="chat-event") {
                auto who=r.String(), text=r.String(), lineId=r.String(); r.End();
                if(who.empty() || who.size()>40 || text.empty() || text.size()>1000 || lineId.size()>32) throw collab::Conflict("Invalid chat message.");
                AppendChat(who,text,lineId); return;
            }
            if(kind=="conflict") {
                auto why=r.String(); auto rev=r.Number(); auto roomDoc=r.Doc(); r.End(); CheckStyles(roomDoc);
                inFlight=false; revision=rev;
                wxString message=Wx("Both editors changed the same subtitle data.\n\n"+why+
                    "\n\nYes = keep your version\nNo = use the room version\nCancel = disconnect and keep your local file");
                int choice=wxMessageBox(message,"Collaboration conflict",wxYES_NO|wxCANCEL|wxICON_WARNING,window);
                if(choice==wxYES) {base=roomDoc; dirty=true; changed=Clock::now(); localTyping=true; presenceDirty=true; Status("Keeping your version - resyncing...");}
                else if(choice==wxNO) {Apply(roomDoc); base=roomDoc; dirty=false; localTyping=false; presenceDirty=true; Status("Using the room version.");}
                else {Goodbye(); Stop("Disconnected after conflict. Your local subtitles are kept.");}
                return;
            }
            if(kind=="notice") {auto text=r.String(); r.End(); if(text.size()>512) throw collab::Conflict("Invalid room notice."); Notice(text); return;}
            if(kind=="room-closed") {
                auto hostName=r.String(); r.End(); Notice(hostName+" disconnected."); throw collab::Conflict("Room closed by host.");
            }
            if(kind=="presence-list") {
                auto count=r.Number(); if(count>9) throw collab::Conflict("Invalid presence list.");
                roomPresence.clear();
                for(uint32_t i=0;i<count;++i) {
                    auto who=r.String(), lineId=r.String(); auto encodedFrame=r.Number(); auto typing=r.Number();
                    if(typing>1 || who.empty() || who.size()>40 || (!lineId.empty() && (lineId.size()!=32 || lineId.find_first_not_of("0123456789abcdef")!=std::string::npos)))
                        throw collab::Conflict("Invalid collaboration presence.");
                    roomPresence[who]={lineId,encodedFrame?static_cast<int>(encodedFrame-1):-1,typing==1};
                }
                r.End(); UpdateFollowChoices();
                for(auto const& kv:roomPresence) MaybeFollow(kv.first,kv.second.frame);
                if(c->subsGrid) c->subsGrid->Refresh(false);
            if(c->videoSlider) c->videoSlider->Refresh(false);
                return;
            }
            if(kind=="media-offer") {
                auto mediaKind=r.String(); auto filename=r.String(); auto size=Read64(r); auto hash=r.String(); r.End();
                if((mediaKind!="video" && mediaKind!="audio") || filename.empty() || filename.size()>512 || !size || size>MaxMediaBytes || hash.size()!=64) throw collab::Conflict("Invalid shared media offer.");
                offeredMediaKind=mediaKind; offeredMediaName=filename; offeredMediaSize=size; offeredMediaHash=hash;
                if(LocalMediaMatches(mediaKind,filename,size,hash)) {
                    Status("Connected - matching host "+mediaKind+" is already open.");
                    return;
                }
                bool accept=autoMediaBox && autoMediaBox->GetValue();
                if(!accept) {
                    auto message=wxString("The host is using ")+Wx(mediaKind)+" "+Wx(filename)+" ("+Wx(HumanBytes(size))+").\n\nDownload and open it now?";
                    accept=wxMessageBox(message,"Shared media",wxYES_NO|wxICON_QUESTION,window)==wxYES;
                }
                if(accept) RequestMedia(p,mediaKind,filename,size,hash);
                else Status("Connected - host "+mediaKind+" was not downloaded.");
                return;
            }
            if(kind=="media-start") {
                auto mediaKind=r.String(); auto filename=r.String(); auto size=Read64(r); auto hash=r.String(); auto offset=Read64(r); r.End();
                BeginMediaReceive(mediaKind,filename,size,hash,offset); return;
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
                auto mediaKind=r.String(); auto size=Read64(r); auto hash=r.String(); r.End(); FinishMediaReceive(mediaKind,size,hash);
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
                base=doc; revision=rev; inFlight=false; dirty=true; localTyping=false;
                if(reconnecting) {reconnecting=false; reconnectAttempts=0; Notice("Reconnected.");}
                localLineId=RowLineId(c->selectionController->GetActiveLine());
                localFrame=c->project->VideoProvider()?c->videoController->GetFrameN():-1;
                presenceDirty=true; Status("Connected - edits sync automatically."); Buttons(); return;
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
            if(reconnecting && peers.empty() && now>=reconnectAt) TryReconnect();
            if(reconnecting && !peers.empty() && !peers.front()->socket->IsConnected() && now-reconnectStarted>std::chrono::seconds(3)) {
                peers.clear(); reconnectAt=now+std::chrono::seconds(1);
            }
            for(auto it=peers.begin();it!=peers.end();) {
                auto& p=**it;
                try {
                    if(!room && !p.hello && p.socket->IsConnected()) {
                        collab::Writer w; w.String("hello"); w.Number(7); w.String(name); w.String(password); w.String(roleChoice?Utf8(roleChoice->GetStringSelection()):std::string("Editor")); p.Queue(w); p.hello=true;
                    }
                    for(auto const& message:p.Read()) Handle(p,message);
                    if(room && p.authenticated) PumpMedia(p);
                    p.Flush();
                    if(now-p.last>std::chrono::seconds(20)) throw collab::Conflict("Connection timed out. Local edits are kept.");
                    if((p.authenticated || connected) && !p.socket->IsConnected()) throw collab::Conflict("Connection lost. Local edits are kept.");
                    ++it;
                }
                catch(std::exception const& e) {
                    if(!room) {
                        BeginReconnect(e.what());
                        break;
                    }
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
                if(room) People();
            }
            if(now-heartbeat>std::chrono::seconds(5)) {
                collab::Writer w; w.String("ping");
                for(auto& p:peers) {
                    if(p->authenticated || connected) p->Queue(w);
                }
                heartbeat=now;
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
    std::vector<std::pair<std::string,bool>> names;
    if(impl->localLineId==id && !impl->name.empty()) names.push_back({impl->name,impl->localTyping});
    if(impl->room) {
        for(auto const& p:impl->peers) if(p->authenticated) {
            auto it=impl->peerPresence.find(p.get());
            if(it!=impl->peerPresence.end() && it->second.lineId==id) names.push_back({p->name,it->second.typing});
        }
    } else {
        for(auto const& kv:impl->roomPresence) if(kv.first!=impl->name && kv.second.lineId==id) names.push_back({kv.first,kv.second.typing});
    }
    std::string out;
    for(auto const& n:names) {
        if(!out.empty()) out+=", ";
        out+=n.first+(n.second?" (typing)":" (viewing)");
    }
    return out;
}
uint32_t CollaborationController::UserColor(std::string const& who) const {return impl->ColorFor(who);}
uint32_t CollaborationController::PresenceColorFor(AssDialogue const* line) const {
    if(!line) return 0;
    auto text=PresenceFor(line); if(text.empty()) return 0;
    auto pos=text.find(" ("); return impl->ColorFor(text.substr(0,pos));
}
std::vector<std::pair<std::string,int>> CollaborationController::RemotePlayheads() const {
    std::vector<std::pair<std::string,int>> out;
    if(!impl->connected) return out;
    if(impl->room) {
        for(auto const& p:impl->peers) if(p->authenticated) {
            auto it=impl->peerPresence.find(p.get()); if(it!=impl->peerPresence.end() && it->second.frame>=0) out.emplace_back(p->name,it->second.frame);
        }
    } else {
        for(auto const& kv:impl->roomPresence) if(kv.first!=impl->name && kv.second.frame>=0) out.emplace_back(kv.first,kv.second.frame);
    }
    return out;
}

int CollaborationController::YTWarningCount(AssDialogue const* line) const {
    return line?static_cast<int>(YTLineWarnings(line->Text.get()).size()):0;
}
