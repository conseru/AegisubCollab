// Copyright (c) 2026 OpenAI. SPDX-License-Identifier: BSD-3-Clause
#include "sha256.h"
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace collab {
namespace {
inline uint32_t R(uint32_t x,int n){return (x>>n)|(x<<(32-n));}
constexpr std::array<uint32_t,64> K={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
struct Sha256 {
    std::array<uint32_t,8> h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<unsigned char,64> block{};
    uint64_t total=0; size_t used=0;
    void Transform(unsigned char const* p){
        uint32_t w[64];
        for(int i=0;i<16;++i) w[i]=(uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|(uint32_t(p[i*4+2])<<8)|p[i*4+3];
        for(int i=16;i<64;++i){auto s0=R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3);auto s1=R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
        auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int i=0;i<64;++i){auto S1=R(e,6)^R(e,11)^R(e,25);auto ch=(e&f)^((~e)&g);auto t1=hh+S1+ch+K[i]+w[i];auto S0=R(a,2)^R(a,13)^R(a,22);auto maj=(a&b)^(a&c)^(b&c);auto t2=S0+maj;hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    void Update(unsigned char const* p,size_t n){
        total+=n;
        while(n){auto take=std::min(n,64-used);std::copy(p,p+take,block.begin()+used);used+=take;p+=take;n-=take;if(used==64){Transform(block.data());used=0;}}
    }
    std::string Finish(){
        uint64_t bits=total*8;
        block[used++]=0x80;
        if(used>56){while(used<64)block[used++]=0;Transform(block.data());used=0;}
        while(used<56)block[used++]=0;
        for(int i=7;i>=0;--i)block[used++]=static_cast<unsigned char>((bits>>(i*8))&255);
        Transform(block.data());
        std::ostringstream out; out<<std::hex<<std::setfill('0');
        for(auto x:h) out<<std::setw(8)<<x;
        return out.str();
    }
};
}
std::string FileSha256Hex(agi::fs::path const& path){
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error("Could not read file for SHA-256 verification.");
    Sha256 sha; std::vector<unsigned char> buf(1024*1024);
    while(in){in.read(reinterpret_cast<char*>(buf.data()),static_cast<std::streamsize>(buf.size()));auto n=in.gcount();if(n>0)sha.Update(buf.data(),static_cast<size_t>(n));}
    if(!in.eof()) throw std::runtime_error("Could not finish SHA-256 verification.");
    return sha.Finish();
}
}
