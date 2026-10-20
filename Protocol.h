// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
namespace ArdorProtocol {
using Packet=std::array<unsigned char,65>;
inline std::array<Packet,5> Frame(const std::array<unsigned char,240>& rgb,unsigned short sequence)
{
    unsigned short crc=0xffff;
    for(auto value:rgb){crc^=unsigned(value)<<8;for(unsigned b=0;b<8;++b)crc=(crc<<1)^((crc&0x8000)?0x1021:0);}
    std::array<Packet,5> packets{};
    for(unsigned page=0;page<5;++page)
    {
        auto& p=packets[page];unsigned length=page==4?16:56;
        p[1]=0xe8;p[2]=1;p[3]=sequence&255;p[4]=sequence>>8;p[5]=page;p[6]=5;p[7]=length;
        unsigned sum=0;for(unsigned i=1;i<8;++i)sum+=p[i];p[8]=255-(sum&255);
        for(unsigned i=0;i<length;++i)p[9+i]=rgb[page*56+i];
        if(page==4){p[25]=crc&255;p[26]=crc>>8;}
    }
    return packets;
}
inline Packet Lighting(unsigned char effect,unsigned char speed,unsigned char brightness,
                       unsigned char options,unsigned char red,unsigned char green,unsigned char blue)
{
    Packet p{};p[1]=7;p[2]=effect;p[3]=speed;p[4]=brightness;p[5]=options;
    p[6]=red;p[7]=green;p[8]=blue;
    unsigned sum=0;for(unsigned i=1;i<=8;++i)sum+=p[i];p[9]=255-(sum&255);
    return p;
}
inline Packet CustomKey(unsigned char slot,unsigned char red,unsigned char green,unsigned char blue)
{
    Packet p{};p[1]=0x14;p[3]=slot;p[8]=255-((0x14+slot)&255);
    p[9]=red;p[10]=green;p[11]=blue;return p;
}
}
