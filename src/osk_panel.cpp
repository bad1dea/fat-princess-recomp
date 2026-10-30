#include "osk_panel.h"
#include "fp_ui_font_data.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/ps3recomp/libs/font/stb_truetype.h"
#include <map>
#include "../third_party/ps3recomp/libs/codec/stb_image.h"

namespace {
using Color=std::array<unsigned char,3>;
struct Font {
    std::array<unsigned char,1024*512> atlas{};
    std::array<stbtt_bakedchar,224> glyphs{};
    explicit Font(unsigned size) { stbtt_BakeFontBitmap(fp_ui_font_data,0,float(size),atlas.data(),1024,512,32,224,glyphs.data()); }
};
Font& font(unsigned size) { static std::map<unsigned,Font> fonts;return fonts.try_emplace(size,size).first->second; }
struct Canvas {
    unsigned w=960,h=460;
    Canvas() = default;
    Canvas(unsigned width,unsigned height):w(width),h(height) {}
    std::vector<uint8_t> rgba=std::vector<uint8_t>(w*h*4);
    void blend(int x,int y,Color color,unsigned alpha=255) {
        if(x<0||y<0||unsigned(x)>=w||unsigned(y)>=h)return;
        const auto at=(size_t(y)*w+x)*4;
        for(unsigned c=0;c<3;++c)rgba[at+c]=uint8_t((rgba[at+c]*(255-alpha)+color[c]*alpha+127)/255);
        rgba[at+3]=255;
    }
    void box(int x,int y,int width,int height,Color color,int radius=0) {
        for(int dy=0;dy<height;++dy)for(int dx=0;dx<width;++dx) {
            const float corner_x=std::max(float(radius)-dx-.5f,dx+.5f-(width-radius));
            const float corner_y=std::max(float(radius)-dy-.5f,dy+.5f-(height-radius));
            if(radius && corner_x>0 && corner_y>0) {
                const float distance=std::hypot(corner_x,corner_y);
                if(distance>=radius+.5f)continue;
                blend(x+dx,y+dy,color,unsigned(std::clamp(radius+.5f-distance,0.f,1.f)*255));
            } else blend(x+dx,y+dy,color);
        }
    }
    float text_width(std::string_view text,unsigned size) {
        auto& f=font(size);float width=0;
        for(unsigned char c:text)width+=f.glyphs[c>=32?c-32:'?'-32].xadvance;
        return width;
    }
    void text(std::string_view text,int x,int baseline,unsigned size,Color color,int clip_right=940) {
        auto& f=font(size);float pen=float(x);
        for(unsigned char c:text) {
            const auto& g=f.glyphs[c>=32?c-32:'?'-32];
            if(pen+g.xadvance>clip_right)break;
            for(int y=0;y<g.y1-g.y0;++y)for(int px=0;px<g.x1-g.x0;++px)
                blend(int(pen+g.xoff)+px,baseline+int(g.yoff)+y,color,f.atlas[(g.y0+y)*1024+g.x0+px]);
            pen+=g.xadvance;
        }
    }
};
}
namespace fp {
std::vector<uint8_t> keyboard_pixels(const KeyboardView& view,unsigned& width,unsigned& height) {
    Canvas c;width=c.w;height=c.h;
    const Color bg{23,28,38},ink{245,239,228},muted{156,170,187},accent{238,145,174},gold{247,194,96};
    c.box(0,0,int(c.w),int(c.h),bg);c.box(0,0,int(c.w),3,accent);
    c.text(view.title.empty()?"Enter text":view.title,32,35,28,ink,760);
    const auto count=std::to_string(view.text.size())+" / "+std::to_string(view.limit);
    c.text(count,928-int(c.text_width(count,20)),32,20,view.text.size()>=view.limit?gold:muted);
    c.box(31,65,898,66,{62,72,91},10);c.box(32,66,896,64,{13,18,27},9);
    if(view.limit<=8) {
        for(unsigned i=0;i<view.limit;++i) {
            const int x=48+int(i)*48;c.box(x,78,40,40,{32,42,57},6);
            if(i<view.text.size()) {
                const std::string ch(1,view.secret?'*':view.text[i]);
                c.text(ch,x+20-int(c.text_width(ch,32)/2),109,32,ink);
            } else if(i==view.text.size()) c.box(x+11,109,18,2,accent);
        }
        if(view.text.empty()) c.text("Use the keyboard below or type",int(80+view.limit*48),105,18,muted);
    } else {
        std::string visible=view.secret?std::string(view.text.size(),'*'):view.text;
        while(c.text_width(visible,28)>830 && !visible.empty())visible.erase(0,1);
        c.text(visible,48,108,28,ink);c.box(50+int(c.text_width(visible,28)),84,2,29,accent);
    }
    const bool full=view.text.size()>=view.limit;
    for(const auto& key:keyboard_keys(view)) {
        const bool selected=key.row==view.row && key.column==view.column;
        const bool done=key.row==4 && key.column==5;
        const Color fill=selected?accent:done?gold:full&&key.row<4?Color{30,36,47}:Color{42,52,68};
        c.box(key.x,key.y+2,key.w,key.h,{9,13,19},8);c.box(key.x,key.y,key.w,key.h,fill,8);
        if(selected)c.box(key.x+10,key.y+key.h-5,key.w-20,2,{255,217,231});
        const unsigned size=key.row<4?25:20;
        c.text(key.label,key.x+key.w/2-int(c.text_width(key.label,size)/2),key.y+key.h/2+int(size*.3f),size,
            selected||done?Color{23,28,38}:full&&key.row<4?muted:ink,key.x+key.w-4);
    }
    c.text(view.controls,32,422,18,ink,930);c.text(view.hint,32,447,16,muted,930);
    return std::move(c.rgba);
}

// ---- trophies: the toast card, the details panel and the trophy list (PS5-style; scale 1 is 720p) ----
namespace {
const Color kTrophyBg{33,34,38},kTrophyInk{240,240,242},kTrophyMuted{168,170,176},kTrophyAccent{98,160,232};
Color trophy_metal(int grade) {
    return grade==1?Color{186,214,236}:grade==2?Color{232,190,72}:grade==3?Color{200,204,212}:Color{205,128,62};
}
const char* trophy_grade_name(int grade) { return grade==1?"Platinum":grade==2?"Gold":grade==3?"Silver":"Bronze"; }
// An RGBA icon, area-averaged into a size x size square with rounded corners; a missing icon is a plain tile.
void trophy_icon(Canvas& c,const TrophyIcon* icon,int x0,int y0,int size,int radius,bool dim=false) {
    for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
        const float cx=std::max(float(radius)-x-.5f,x+.5f-(size-radius)),cy=std::max(float(radius)-y-.5f,y+.5f-(size-radius));
        float cover=1;if(cx>0&&cy>0)cover=std::clamp(radius+.5f-std::hypot(cx,cy),0.f,1.f);
        if(cover<=0)continue;
        Color col{70,74,84};unsigned a=255;
        if(icon && !icon->rgba.empty()) {
            const int iw=icon->w,ih=icon->h;
            const int sx0=x*iw/size,sx1=std::max(sx0+1,(x+1)*iw/size),sy0=y*ih/size,sy1=std::max(sy0+1,(y+1)*ih/size);
            unsigned sum[4]={0,0,0,0},count=0;
            for(int sy=sy0;sy<sy1;++sy)for(int sx=sx0;sx<sx1;++sx) { const uint8_t* p=&icon->rgba[(size_t(sy)*iw+sx)*4];for(int k=0;k<4;++k)sum[k]+=p[k];++count; }
            col={uint8_t(sum[0]/count),uint8_t(sum[1]/count),uint8_t(sum[2]/count)};a=sum[3]/count;
            if(dim) { const unsigned l=(col[0]*3+col[1]*6+col[2])/10/2+20;col={uint8_t(l),uint8_t(l),uint8_t(l)}; }
        }
        c.blend(x0+x,y0+y,col,unsigned(a*cover));
    }
}
// The grade cup: bowl, handles, stem and foot, 4x4 supersampled.
void trophy_cup(Canvas& c,int x0,int y0,int size,Color metal) {
    const float S=float(size);
    const auto inside=[S](float x,float y) {
        const float u=x/S-.5f,v=y/S;
        if(v>=0 && v<=.55f && std::abs(u)<=.34f*std::sqrt(std::max(0.f,1-(v/.55f)*(v/.55f))))return true;
        const float hx=std::abs(u)-.34f,hy=v-.2f,d=std::hypot(hx,hy);
        if(std::abs(u)>.2f && d<=.15f && d>=.07f)return true;
        if(std::abs(u)<=.06f && v>.5f && v<=.8f)return true;
        return std::abs(u)<=.26f && v>.8f && v<=.95f;
    };
    for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
        int hits=0;for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx)hits+=inside(x+(sx+.5f)/4,y+(sy+.5f)/4);
        if(hits)c.blend(x0+x,y0+y,metal,unsigned(hits*255/16));
    }
}
std::vector<std::string> trophy_wrap(Canvas& c,const std::string& text,unsigned size,int width) {
    std::vector<std::string> lines;std::string line,word;
    auto flush_word=[&] {
        if(word.empty())return;
        const std::string trial=line.empty()?word:line+" "+word;
        if(c.text_width(trial,size)>width && !line.empty()) { lines.push_back(line);line=word; } else line=trial;
        word.clear();
    };
    for(char ch:text) { if(ch==' ')flush_word(); else word+=ch; }
    flush_word();if(!line.empty())lines.push_back(line);
    return lines;
}
void trophy_bar(Canvas& c,int x,int y,int w,int h,float fraction) {
    c.box(x,y,w,h,{58,60,68},h/2);
    const int fw=int(std::lround(w*std::clamp(fraction,0.f,1.f)));
    if(fw>=h)c.box(x,y,fw,h,kTrophyAccent,h/2);
}
}

// The toast: icon, grade cup, trophy name, "Trophy earned!" and the details hint.
std::vector<uint8_t> trophy_card_pixels(const TrophyCard& card,float scale,unsigned& width,unsigned& height,unsigned& radius) {
    const auto px=[scale](float v){return int(std::lround(v*scale));};
    const unsigned name_size=unsigned(px(19)),note_size=unsigned(px(15)),hint_size=unsigned(px(13));
    const int icon=px(46),pad=px(10),text_x=pad+icon+px(12),cup=px(16);
    std::string name=card.name.empty()?"Trophy":card.name;
    const std::string hint=card.hint;
    {
        Canvas probe(1,1);
        while(name.size()>3 && text_x+cup+px(6)+probe.text_width(name,name_size)>px(560)) name=name.substr(0,name.size()-4)+"...";
        const int note_w=int(probe.text_width("Trophy earned!",note_size))+(hint.empty()?0:px(16)+int(probe.text_width(hint,hint_size)));
        width=unsigned(std::max({px(300),text_x+cup+px(6)+int(probe.text_width(name,name_size))+px(18),text_x+note_w+px(18)}));
    }
    height=unsigned(icon+2*pad);radius=unsigned(px(10));
    Canvas c(width,height);
    c.box(0,0,int(width),int(height),kTrophyBg);
    TrophyIcon decoded;
    if(!card.png.empty()) {
        int iw=0,ih=0,n=0;
        if(unsigned char* png=stbi_load_from_memory(card.png.data(),int(card.png.size()),&iw,&ih,&n,4)) {
            decoded.w=iw;decoded.h=ih;decoded.rgba.assign(png,png+size_t(iw)*ih*4);stbi_image_free(png);
        }
    }
    trophy_icon(c,card.png.empty()?nullptr:&decoded,pad,pad,icon,px(5));
    trophy_cup(c,text_x,pad+px(4),cup,trophy_metal(card.grade));
    c.text(name,text_x+cup+px(6),pad+px(17),name_size,kTrophyInk,int(width)-px(8));
    c.text("Trophy earned!",text_x,pad+px(39),note_size,kTrophyMuted,int(width)-px(8));
    if(!hint.empty()) c.text(hint,int(width)-px(12)-int(c.text_width(hint,hint_size)),pad+px(39),hint_size,{120,124,132},int(width)-px(4));
    return std::move(c.rgba);
}

// The details panel for one trophy (opened from the toast).
std::vector<uint8_t> trophy_detail_pixels(const TrophyDetailView& v,float scale,unsigned& width,unsigned& height,unsigned& radius) {
    const auto px=[scale](float x){return int(std::lround(x*scale));};
    width=unsigned(px(440));height=unsigned(px(400));radius=unsigned(px(14));
    Canvas c(width,height);
    c.box(0,0,int(width),int(height),{28,32,40});
    const int W=int(width),pad=px(22);
    c.text("Details",pad,pad+px(14),unsigned(px(15)),kTrophyMuted,W-pad);
    const int icon=px(72),top=pad+px(34);
    const bool secret=v.row.hidden && !v.row.unlocked;
    trophy_icon(c,secret?nullptr:v.icon,pad,top,icon,px(8),!v.row.unlocked);
    const int tx=pad+icon+px(16);
    trophy_cup(c,tx,top+px(6),px(18),trophy_metal(v.row.grade));
    std::string name=secret?"Hidden Trophy":v.row.name;
    while(name.size()>3 && c.text_width(name,unsigned(px(19)))>W-tx-px(26)-pad) name=name.substr(0,name.size()-4)+"...";
    c.text(name,tx+px(24),top+px(21),unsigned(px(19)),kTrophyInk,W-pad);
    c.text(std::string(trophy_grade_name(v.row.grade))+" trophy  -  "+v.status,tx,top+px(48),unsigned(px(14)),kTrophyMuted,W-pad);
    int y=top+icon+px(30);
    for(const auto& line:trophy_wrap(c,secret?"Keep playing to reveal this trophy.":v.row.detail,unsigned(px(15)),W-2*pad)) {
        c.text(line,pad,y,unsigned(px(15)),{200,204,212},W-pad);y+=px(21);
    }
    y=std::max(y+px(14),int(height)-px(150));
    c.text("Total trophy progress",pad,y,unsigned(px(14)),kTrophyMuted,W-pad);
    y+=px(12);
    trophy_bar(c,pad,y,W-2*pad-px(60),px(8),v.total?float(v.earned)/float(v.total):0.f);
    const std::string pct=std::to_string(v.total?v.earned*100/v.total:0)+"%";
    c.text(pct,W-pad-int(c.text_width(pct,unsigned(px(15)))),y+px(9),unsigned(px(15)),kTrophyInk,W);
    y+=px(26);
    c.text(std::to_string(v.earned)+" of "+std::to_string(v.total)+" earned  -  "+v.title,pad,y,unsigned(px(13)),kTrophyMuted,W-pad);
    const int by=int(height)-px(70);
    c.box(pad,by,W-2*pad,px(38),{220,233,237},px(19));
    const std::string cta="View Trophies";
    c.text(cta,W/2-int(c.text_width(cta,unsigned(px(15)))/2),by+px(25),unsigned(px(15)),{17,24,32},W);
    c.text(v.controls,pad,int(height)-px(14),unsigned(px(12)),kTrophyMuted,W-pad);
    return std::move(c.rgba);
}

// Every trophy of the game with its state; locked hidden trophies show no name or description.
std::vector<uint8_t> trophy_list_pixels(const TrophyListView& v,float scale,unsigned& width,unsigned& height,unsigned& radius) {
    const auto px=[scale](float x){return int(std::lround(x*scale));};
    width=unsigned(px(760));height=unsigned(px(600));radius=unsigned(px(14));
    Canvas c(width,height);
    c.box(0,0,int(width),int(height),{16,29,51});
    const int W=int(width),pad=px(26);
    c.text(v.title.empty()?"Trophies":v.title,pad,pad+px(24),unsigned(px(26)),kTrophyInk,W-pad);
    c.text("Trophy collection",pad,pad+px(46),unsigned(px(14)),{170,185,210},W-pad);
    int y=pad+px(66);
    c.box(pad,y,W-2*pad,1,{60,74,96});
    trophy_cup(c,pad,y+px(14),px(30),trophy_metal(v.earned==v.total && v.total?1:2));
    const std::string pct=std::to_string(v.total?v.earned*100/v.total:0)+"%";
    c.text(pct,pad+px(44),y+px(36),unsigned(px(24)),kTrophyInk,W);
    c.text(std::to_string(v.earned)+" of "+std::to_string(v.total)+" trophies earned",pad+px(120),y+px(25),unsigned(px(14)),{170,185,210},W-pad);
    trophy_bar(c,pad+px(120),y+px(33),W-2*pad-px(120),px(8),v.total?float(v.earned)/float(v.total):0.f);
    y+=px(56);c.box(pad,y,W-2*pad,1,{60,74,96});y+=px(12);
    const int row_h=px(74),visible=std::max(1,(int(height)-y-px(34))/row_h);
    const int first=std::clamp(v.selected-visible/2,0,std::max(0,int(v.rows.size())-visible));
    for(int i=first;i<int(v.rows.size()) && i<first+visible;++i) {
        const auto& r=v.rows[size_t(i)];
        const bool sel=i==v.selected, secret=r.hidden && !r.unlocked;
        const int ry=y+(i-first)*row_h;
        c.box(pad,ry,W-2*pad,row_h-px(8),sel?Color{44,62,92}:Color{24,38,62},px(10));
        if(sel) { c.box(pad,ry,px(4),row_h-px(8),{220,238,255},px(2)); }
        const int icon=row_h-px(22);
        trophy_icon(c,secret?nullptr:(size_t(i)<v.icons.size()?v.icons[size_t(i)]:nullptr),pad+px(10),ry+px(7),icon,px(6),!r.unlocked);
        const int tx=pad+px(20)+icon;
        trophy_cup(c,tx,ry+px(9),px(15),r.unlocked?trophy_metal(r.grade):Color{110,114,124});
        const std::string status=r.unlocked?r.when_text:"Locked";
        const int sw=int(c.text_width(status,unsigned(px(13))));
        c.text(secret?"Hidden Trophy":r.name,tx+px(20),ry+px(22),unsigned(px(17)),r.unlocked?kTrophyInk:Color{196,200,208},W-pad-sw-px(20));
        c.text(status,W-pad-px(12)-sw,ry+px(22),unsigned(px(13)),r.unlocked?Color{150,210,170}:kTrophyMuted,W);
        std::string detail=secret?"Keep playing to reveal this trophy.":r.detail;
        while(detail.size()>3 && c.text_width(detail,unsigned(px(13)))>W-pad-px(14)-tx) detail=detail.substr(0,detail.size()-4)+"...";
        c.text(detail,tx,ry+px(46),unsigned(px(13)),{180,190,206},W-pad-px(10));
    }
    c.text(v.controls,pad,int(height)-px(14),unsigned(px(12)),{150,165,190},W-pad);
    return std::move(c.rgba);
}
}
