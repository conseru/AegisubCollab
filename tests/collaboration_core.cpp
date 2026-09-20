// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#include "../src/collaboration/document.h"
#include <iostream>
#include <cstdlib>
using namespace collab;
void Check(bool v) {if(!v) throw std::runtime_error("test failed");}
template<class F> void Fails(F f) {bool failed=false; try{f();}catch(Conflict const&){failed=true;} Check(failed);}
Line L(char id,std::string text) {return {std::string(32,id),{"0","0","1000","2000","Default","[by Jake]","0","0","0","",std::move(text)}};}
int main() {
 try {
 Document b{{L('a',"{\\pos(960,540)}日本語"),L('b',"second")},{{"PlayResX","1920"},{"PlayResY","1080"}},{{"Default","Style: Default"}}};
 Room room(b); auto a=b,c=b; a.lines[0].fields[10]="A"; c.lines[1].fields[10]="B";
 room.Apply(1,a); room.Apply(1,c); Check(room.Current().lines[0].fields[10]=="A" && room.Current().lines[1].fields[10]=="B");
 auto revision=room.Revision(); Check(!room.Apply(1,c)); Check(room.Revision()==revision);
 c.lines[0].fields[10]="same-line-latest"; room.Apply(1,c); Check(room.Current().lines[0].fields[10]=="same-line-latest");
 a=b;c=b; a.lines.insert(a.lines.begin()+1,L('c',"same")); c.lines.insert(c.lines.begin()+1,L('d',"same"));
 auto merged=Merge(b,c,a); Check(merged.lines.size()==4 && merged.lines[1].id==std::string(32,'c') && merged.lines[2].id==std::string(32,'d'));
 a=b;a.lines.erase(a.lines.begin()); Check(Merge(b,b,a).lines.size()==1);
 c=b;c.lines[0].fields[10]="edited"; Fails([&]{Merge(b,c,a);});
 a=b;a.lines.clear(); Check(Merge(b,b,a).lines.empty());
 a=b;c=b;a.lines[0].fields[5]="actor"; c.lines[0].fields[10]="text"; merged=Merge(b,c,a); Check(merged.lines[0].fields[5]=="actor" && merged.lines[0].fields[10]=="text");
 a=b;c=b;a.settings["PlayResX"]="1280";a.settings["PlayResY"]="720";c.lines[0].fields[10]="text"; Check(Merge(b,c,a).settings==a.settings);
 c.settings["PlayResX"]="3840"; Fails([&]{Merge(b,c,a);});
 a=b;c=b; a.styles["Another"]="style A"; c.styles["Third"]="style B"; Check(Merge(b,c,a).styles.size()==3);
 a=b;std::swap(a.lines[0],a.lines[1]); Fails([&]{Merge(b,a,b);});
 a=b;a.lines.push_back(a.lines[0]);Fails([&]{Validate(a);});
 a=b;a.lines[0].fields[3]="0";Fails([&]{Validate(a);});
 Room replay(b);a=b;a.lines.push_back(L('e',"new"));replay.Apply(1,a);auto created=replay.Revision();replay.Apply(created,b);Fails([&]{replay.Apply(1,a);});
 Room bounded(b); for(int i=0;i<40;++i) {auto next=bounded.Current(); next.lines[0].fields[10]=std::to_string(i); bounded.Apply(bounded.Revision(),next);} Fails([&]{bounded.Apply(1,b);});
 a=b; a.styles[std::string("bad\0name",8)]="Style: Default"; Fails([&]{Validate(a);});
 a=b; a.lines[0].fields[10]=std::string(100001,'x'); Fails([&]{Validate(a);});
 Writer w;w.Doc(b);Reader r(w.Bytes());Check(r.Doc()==b);r.End();
 for(size_t n=0;n<w.Bytes().size();++n){auto truncated=w.Bytes().substr(0,n);Fails([&]{Reader rr(truncated);rr.Doc();rr.End();});}
 auto trailing=w.Bytes()+"x";Fails([&]{Reader rr(trailing);rr.Doc();rr.End();});
 std::string enormous(4,'\xff');Fails([&]{Reader rr(enormous);rr.Doc();});
 std::cout<<"Native collaboration core checks passed (merge, same-line edits, structural conflicts, deletion, replay, styles/resolution, bounded wire format).\n";
 }catch(std::exception const& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
}
