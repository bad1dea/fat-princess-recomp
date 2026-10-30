#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fp {
struct KeyboardView {
    std::string title, text, controls, hint;
    unsigned limit=4;
    int row=1, column=0;
    bool upper=true, symbols=false, secret=false;
};
struct KeyboardKey { int x,y,w,h,row,column; std::string label; };
inline std::array<std::string_view,4> keyboard_rows(bool symbols) {
    return symbols ? std::array<std::string_view,4>{"1234567890","!@#$%&*()","-_=+:;?/.","[]{}\\'\",~"}
                   : std::array<std::string_view,4>{"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"};
}
inline int keyboard_columns(int row,bool symbols) { return row==4?6:int(keyboard_rows(symbols)[row].size()); }
inline std::vector<KeyboardKey> keyboard_keys(const KeyboardView& view) {
    std::vector<KeyboardKey> keys;
    const auto rows=keyboard_rows(view.symbols);
    for(int row=0;row<4;++row) {
        const int start=(960-(int(rows[row].size())*70-8))/2;
        for(int col=0;col<int(rows[row].size());++col) {
            char ch=rows[row][col];if(view.upper && ch>='a' && ch<='z')ch-=32;
            keys.push_back({start+col*70,145+row*48,62,42,row,col,std::string(1,ch)});
        }
    }
    const char* labels[]={view.upper?"Shift ON":"Shift",view.symbols?"ABC":"Symbols","Space","Delete","Cancel","Done"};
    const int widths[]={104,112,212,110,110,116};int x=78;
    for(int i=0;i<6;++i) { keys.push_back({x,344,widths[i],46,4,i,labels[i]});x+=widths[i]+8; }
    return keys;
}
inline void keyboard_vertical(int& row,int& col,int direction,bool symbols) {
    KeyboardView view;view.symbols=symbols;const auto keys=keyboard_keys(view);
    float x=0;for(const auto& key:keys)if(key.row==row && key.column==col)x=key.x+key.w*.5f;
    row=(row+5+direction)%5;float distance=10000;
    for(const auto& key:keys)if(key.row==row && std::abs(key.x+key.w*.5f-x)<distance) { distance=std::abs(key.x+key.w*.5f-x);col=key.column; }
}
KeyboardView settings_keyboard_view();
std::vector<uint8_t> keyboard_pixels(const KeyboardView&,unsigned& width,unsigned& height);
struct TrophyCard { std::string name; int grade=4; std::vector<uint8_t> png; std::string hint; };   // grade: 1 platinum .. 4 bronze
struct TrophyIcon { int w=0,h=0; std::vector<uint8_t> rgba; };
struct TrophyRow { int id=0,grade=4; bool hidden=false,unlocked=false; std::string name,detail,when_text; };
struct TrophyDetailView { TrophyRow row; const TrophyIcon* icon=nullptr; std::string status,title,controls; int earned=0,total=0; };
struct TrophyListView { std::vector<TrophyRow> rows; std::vector<const TrophyIcon*> icons; std::string title,controls; int earned=0,total=0,selected=0; };
std::vector<uint8_t> trophy_card_pixels(const TrophyCard&,float scale,unsigned& width,unsigned& height,unsigned& radius);
std::vector<uint8_t> trophy_detail_pixels(const TrophyDetailView&,float scale,unsigned& width,unsigned& height,unsigned& radius);
std::vector<uint8_t> trophy_list_pixels(const TrophyListView&,float scale,unsigned& width,unsigned& height,unsigned& radius);
}
