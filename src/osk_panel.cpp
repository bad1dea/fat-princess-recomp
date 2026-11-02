#include "osk_panel.h"
#include "game_art.h"
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
    // ---- the game's own art (src/game_art.h) ----
    // Area-averaged copy of a source rectangle into a destination rectangle; tint multiplies, gray desaturates and darkens.
    void image(const fp::ArtImage& im,int sx,int sy,int sw,int sh,int dx,int dy,int dw,int dh,Color tint={255,255,255},bool gray=false,unsigned opacity=255) {
        if(dw<=0||dh<=0||sw<=0||sh<=0)return;
        for(int y=0;y<dh;++y)for(int x=0;x<dw;++x) {
            const int x0=sx+x*sw/dw,x1=std::max(x0+1,sx+(x+1)*sw/dw),y0=sy+y*sh/dh,y1=std::max(y0+1,sy+(y+1)*sh/dh);
            unsigned long r=0,g=0,b=0,a=0;unsigned n=0;
            for(int yy=y0;yy<y1;++yy)for(int xx=x0;xx<x1;++xx) {
                if(xx<0||yy<0||xx>=im.w||yy>=im.h){++n;continue;}
                const uint8_t* p=&im.rgba[(size_t(yy)*im.w+xx)*4];
                r+=p[0]*p[3];g+=p[1]*p[3];b+=p[2]*p[3];a+=p[3];++n;
            }
            if(!a||!n)continue;
            Color col{uint8_t(r/a),uint8_t(g/a),uint8_t(b/a)};
            if(gray){const unsigned l=(col[0]*3+col[1]*6+col[2])/10/2+20;col={uint8_t(l),uint8_t(l),uint8_t(l)};}
            for(int k=0;k<3;++k)col[k]=uint8_t(col[k]*tint[k]/255);
            blend(dx+x,dy+y,col,unsigned(a/n)*opacity/255);
        }
    }
    // Copy keeping the source alpha (a base layer with soft edges, such as the book).
    void copy_rgba(const fp::ArtImage& im,int sx,int sy,int sw,int sh,int dx,int dy,int dw,int dh) {
        for(int y=0;y<dh;++y)for(int x=0;x<dw;++x) {
            const int px=dx+x,py=dy+y;if(px<0||py<0||unsigned(px)>=w||unsigned(py)>=h)continue;
            const int x0=sx+x*sw/dw,x1=std::max(x0+1,sx+(x+1)*sw/dw),y0=sy+y*sh/dh,y1=std::max(y0+1,sy+(y+1)*sh/dh);
            unsigned long r=0,g=0,b=0,a=0;unsigned n=0;
            for(int yy=y0;yy<y1;++yy)for(int xx=x0;xx<x1;++xx) {
                ++n;if(xx<0||yy<0||xx>=im.w||yy>=im.h)continue;
                const uint8_t* p=&im.rgba[(size_t(yy)*im.w+xx)*4];r+=p[0]*p[3];g+=p[1]*p[3];b+=p[2]*p[3];a+=p[3];
            }
            uint8_t* o=&rgba[(size_t(py)*w+px)*4];
            if(a){o[0]=uint8_t(r/a);o[1]=uint8_t(g/a);o[2]=uint8_t(b/a);}
            o[3]=uint8_t(a/n);
        }
    }
    void image(const fp::ArtImage& im,int dx,int dy,int dw,int dh,Color tint={255,255,255}) { image(im,0,0,im.w,im.h,dx,dy,dw,dh,tint); }
    // A TrophyIcon (RGBA) the same way.
    void rgba_image(const std::vector<uint8_t>& px,int iw,int ih,int dx,int dy,int size,bool gray=false) {
        fp::ArtImage im;im.w=iw;im.h=ih;im.rgba=px;image(im,0,0,iw,ih,dx,dy,size,size,{255,255,255},gray);
    }
    // Text in a game bitmap font; `scale` 1 draws the atlas pixels. y is the top of the line. Returns the pen end.
    int art_text(const fp::ArtFont& f,std::string_view text,int x,int y,float scale,Color tint,int clip_right=1<<30) {
        float pen=float(x);
        for(unsigned char ch:text) {
            if(ch==' '){pen+=f.space*scale;continue;}
            const fp::ArtGlyph* g=f.find_glyph(ch);if(!g)continue;
            const int adv=int(std::lround((g->w+f.delta)*scale));
            if(int(pen)+adv>clip_right)break;
            image(f.atlas,g->x,g->y,g->w,g->h,int(std::lround(pen)),y+int(std::lround(g->yoffset*scale)),std::max(1,int(std::lround(g->w*scale))),std::max(1,int(std::lround(g->h*scale))),tint);
            pen+=(g->w+f.delta)*scale;
        }
        return int(pen);
    }
    // Paper card: brown border around a parchment gradient.
    void paper(int x,int y,int width,int height,int radius,int border) {
        box(x,y,width,height,{122,84,32},radius);
        const int r=std::max(0,radius-border);
        for(int dy=0;dy<height-2*border;++dy) {
            const float t=float(dy)/float(std::max(1,height-2*border));
            const Color col{uint8_t(244-13*t),uint8_t(230-18*t),uint8_t(174-27*t)};
            for(int dx=0;dx<width-2*border;++dx) {
                const int w2=width-2*border,h2=height-2*border;
                const float cx=std::max(float(r)-dx-.5f,dx+.5f-(w2-r)),cy=std::max(float(r)-dy-.5f,dy+.5f-(h2-r));
                float cover=1;if(r&&cx>0&&cy>0)cover=std::clamp(r+.5f-std::hypot(cx,cy),0.f,1.f);
                if(cover>0)blend(x+border+dx,y+border+dy,col,unsigned(cover*255));
            }
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
static std::vector<uint8_t> plain_card_pixels(const TrophyCard& card,float scale,unsigned& width,unsigned& height,unsigned& radius) {
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
static std::vector<uint8_t> plain_detail_pixels(const TrophyDetailView& v,float scale,unsigned& width,unsigned& height,unsigned& radius) {
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
static std::vector<uint8_t> plain_list_pixels(const TrophyListView& v,float scale,unsigned& width,unsigned& height,unsigned& radius) {
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

// ---- the same screens in the game's own look: paper cards, gold menu boxes, its fonts and princess ----
// Everything is read from the player's siege.qrk at run time (game_art.h); without it the plain screens above are used.
namespace {
struct Skin { const ArtFont *bold=nullptr,*boo=nullptr; const ArtImage *tip=nullptr,*boxes=nullptr,*bar=nullptr,*wall=nullptr,*book=nullptr; };
bool load_skin(Skin& k) {
    k.bold=art_font("fatprincessbold");k.boo=art_font("boopee");
    k.tip=art_image("data/textures/menus/princesstipred");k.boxes=art_image("data/textures/menus/menutextboxes");
    k.bar=art_image("data/textures/menus/menubackbar");k.wall=art_image("data/textures/menus/guysbackbrown");
    k.book=art_image("data/textures/menus/storybook");
    return k.bold&&k.boo&&k.tip;
}
float font_em(const ArtFont& f) { return f.line>0?f.line:57.f; }
float font_scale(const ArtFont& f,float pixels) { return pixels/font_em(f); }
const Color kGold{255,229,106},kBrown{90,58,18},kBrownMid{138,92,10},kGreen{60,122,28};
Color grade_ink(int g) { return g==1?Color{186,214,236}:g==2?Color{255,213,74}:g==3?Color{230,230,238}:Color{232,160,90}; }
const char* grade_word(int g) { return g==1?"PLATINUM":g==2?"GOLD":g==3?"SILVER":"BRONZE"; }
std::vector<std::string> art_wrap(const ArtFont& f,const std::string& text,float scale,int width) {
    std::vector<std::string> lines;std::string line,word;
    auto flush=[&] {
        if(word.empty())return;
        const std::string trial=line.empty()?word:line+" "+word;
        if(f.width(trial,scale)>width && !line.empty()) { lines.push_back(line);line=word; } else line=trial;
        word.clear();
    };
    for(char ch:text) { if(ch==' ')flush(); else word+=ch; }
    flush();if(!line.empty())lines.push_back(line);
    return lines;
}
std::string art_fit(const ArtFont& f,std::string text,float scale,int width) {
    while(text.size()>3 && f.width(text,scale)>width) text=text.substr(0,text.size()-4)+"...";
    return text;
}
// "7 of 10 kills" (or "7 / 10"): the counter text for a row, "" when the trophy has none.
std::string progress_text(const TrophyRow& r) {
    if(r.target<=0)return {};
    return std::to_string(std::min(r.progress,r.target))+" of "+std::to_string(r.target)+(r.unit.empty()?"":" "+r.unit);
}
void paper_bar(Canvas& c,int x,int y,int w,int h,float fraction) {
    c.box(x,y,w,h,{122,84,32},h/2);c.box(x+2,y+2,w-4,h-4,{216,194,138},std::max(0,(h-4)/2));
    const int fw=int(std::lround((w-4)*std::clamp(fraction,0.f,1.f)));
    if(fw>=h-4)c.box(x+2,y+2,fw,h-4,kGreen,std::max(0,(h-4)/2));
}
TrophyIcon decode_png(const std::vector<uint8_t>& png) {
    TrophyIcon out;int iw=0,ih=0,n=0;
    if(!png.empty()) if(unsigned char* px=stbi_load_from_memory(png.data(),int(png.size()),&iw,&ih,&n,4)) {
        out.w=iw;out.h=ih;out.rgba.assign(px,px+size_t(iw)*ih*4);stbi_image_free(px);
    }
    return out;
}
}

std::vector<uint8_t> trophy_card_pixels(const TrophyCard& card,float scale,unsigned& width,unsigned& height,unsigned& radius) {
    Skin k;
    if(!load_skin(k))return plain_card_pixels(card,scale,width,height,radius);
    const auto px=[scale](float v){return int(std::lround(v*scale));};
    const float kick=font_scale(*k.bold,float(px(22))),namef=font_scale(*k.bold,float(px(32))),desc=font_scale(*k.boo,float(px(20)));
    const int W=px(560),icon=px(96),left=px(124),right=W-px(26)-icon-px(10);
    const std::string kicker=std::string(grade_word(card.grade))+" TROPHY UNLOCKED";
    const std::string name=art_fit(*k.bold,card.name.empty()?"Trophy":card.name,namef,right-left);
    const auto lines=art_wrap(*k.boo,card.detail,desc,right-left);
    const int line_h=px(26),shown=int(std::min<size_t>(lines.size(),5));
    int H=std::max(px(150),px(88)+shown*line_h+px(18)+(card.hint.empty()?0:px(14)));
    width=unsigned(W);height=unsigned(H);radius=unsigned(px(12));
    Canvas c(width,height);
    c.paper(0,0,W,H,px(12),px(6));
    const int ph=px(132),pw=ph*156/200;
    c.image(*k.tip,100,0,156,200,px(12),H-px(6)-ph,pw,ph);
    c.art_text(*k.bold,kicker,left,px(16),kick,kBrownMid,right);
    c.art_text(*k.bold,name,left,px(42),namef,kGold,right);
    for(int i=0;i<shown;++i)c.art_text(*k.boo,lines[size_t(i)],left,px(84)+i*line_h,desc,kBrown,right+px(8));
    const TrophyIcon decoded=decode_png(card.png);
    if(!decoded.rgba.empty())c.rgba_image(decoded.rgba,decoded.w,decoded.h,W-px(26)-icon,px(18),icon);
    if(!card.hint.empty()) {
        const float hs=font_scale(*k.boo,float(px(15)));
        c.art_text(*k.boo,card.hint,W-px(20)-int(k.boo->width(card.hint,hs)),H-px(32),hs,kBrownMid);
    }
    return std::move(c.rgba);
}

// One trophy on a paper card (the details screen).
std::vector<uint8_t> trophy_detail_pixels(const TrophyDetailView& v,float scale,unsigned& width,unsigned& height,unsigned& radius) {
    Skin k;
    if(!load_skin(k))return plain_detail_pixels(v,scale,width,height,radius);
    const auto px=[scale](float x){return int(std::lround(x*scale));};
    const bool secret=v.row.hidden && !v.row.unlocked;
    const float namef=font_scale(*k.bold,float(px(36))),gf=font_scale(*k.bold,float(px(22))),df=font_scale(*k.boo,float(px(26))),sf=font_scale(*k.boo,float(px(20)));
    const int W=px(520),pad=px(26),icon=px(128),tx=pad+icon+px(18);
    const auto text=secret?std::string("Keep playing to reveal this trophy."):v.row.detail;
    const auto lines=art_wrap(*k.boo,text,df,W-2*pad);
    const std::string ptext=secret?std::string():progress_text(v.row);
    int H=pad+icon+px(24)+int(lines.size())*px(34)+px(20)+(ptext.empty()?0:px(70))+px(80);
    H=std::max(H,px(430));
    width=unsigned(W);height=unsigned(H);radius=unsigned(px(14));
    Canvas c(width,height);
    c.paper(0,0,W,H,px(14),px(6));
    const TrophyIcon* ic=secret?nullptr:v.icon;
    if(ic)c.rgba_image(ic->rgba,ic->w,ic->h,pad,pad,icon,!v.row.unlocked);
    c.art_text(*k.bold,art_fit(*k.bold,secret?"Hidden Trophy":v.row.name,namef,W-tx-pad),tx,pad+px(4),namef,kGold,W-pad);
    c.art_text(*k.bold,std::string(grade_word(v.row.grade))+" TROPHY",tx,pad+px(56),gf,kBrownMid,W-pad);
    c.art_text(*k.boo,v.status,tx,pad+px(88),sf,v.row.unlocked?kGreen:kBrown,W-pad);
    int y=pad+icon+px(20);
    for(const auto& line:lines) { c.art_text(*k.boo,line,pad,y,df,kBrown,W-pad);y+=px(34); }
    if(!ptext.empty()) {
        y+=px(10);
        const float frac=float(std::min(v.row.progress,v.row.target))/float(v.row.target);
        c.art_text(*k.boo,ptext,pad,y,df,v.row.unlocked?kGreen:kBrown,W-pad);y+=px(36);
        paper_bar(c,pad,y,W-2*pad,px(18),v.row.unlocked?1.f:frac);y+=px(26);
    }
    const float hf=font_scale(*k.boo,float(px(18)));
    const std::string summary=std::to_string(v.earned)+" of "+std::to_string(v.total)+" trophies earned";
    c.art_text(*k.boo,summary,pad,int(height)-px(66),hf,kBrownMid,W-pad);
    c.art_text(*k.boo,v.controls,pad,int(height)-px(40),hf,kBrownMid,W-pad);
    return std::move(c.rgba);
}

// All trophies as a menu page: the gold boxes with an icon each, a paper card for the highlighted one.
static std::vector<uint8_t> menu_list_pixels(const TrophyListView& v,float scale,unsigned& width,unsigned& height,unsigned& radius) {
    Skin k;
    if(!load_skin(k) || !k.boxes || !k.bar || !k.wall)return plain_list_pixels(v,scale,width,height,radius);
    const auto px=[scale](float x){return int(std::lround(x*scale));};
    const int W=px(1240),H=px(660),bar_h=px(32);
    width=unsigned(W);height=unsigned(H);radius=unsigned(px(10));
    Canvas c(width,height);
    c.box(0,0,W,H,{98,64,30});
    // wall pattern over the brown, as the game's pages show it
    for(int y=bar_h;y<H-bar_h;++y)for(int x=0;x<W;++x) {
        const uint8_t* p=&k.wall->rgba[(size_t(y*k.wall->h/H)*k.wall->w+size_t(x*k.wall->w/W))*4];
        c.blend(x,y,{uint8_t(p[0]),uint8_t(p[1]),uint8_t(p[2])},unsigned(p[3])*100/255);
    }
    for(int x=0;x<W;x+=px(512)) { c.image(*k.bar,x,0,px(512),bar_h);c.image(*k.bar,x,H-bar_h,px(512),bar_h); }
    const float tabf=font_scale(*k.bold,float(px(40))),rowf=font_scale(*k.bold,float(px(30))),gradef=font_scale(*k.bold,float(px(20)));
    const float hintf=font_scale(*k.boo,float(px(20)));
    const std::string tab=(v.title.empty()?std::string("TROPHIES"):std::string("TROPHIES"))+"  "+std::to_string(v.earned)+" / "+std::to_string(v.total);
    const int tw=int(k.bold->width(tab,tabf))+px(48);
    c.box(px(40),px(46),tw,px(60),{122,84,32});c.box(px(43),px(49),tw-6,px(54),{31,20,9});
    c.art_text(*k.bold,tab,px(64),px(54),tabf,{238,238,238});
    const int list_x=px(40),list_y=px(122),row_h=px(81),visible=std::max(1,(H-bar_h-list_y-px(50))/row_h);
    const int first=std::clamp(v.selected-visible/2,0,std::max(0,int(v.rows.size())-visible));
    const int box_w=px(710),box_h=px(85);
    for(int i=first;i<int(v.rows.size()) && i<first+visible;++i) {
        const auto& r=v.rows[size_t(i)];
        const bool sel=i==v.selected,secret=r.hidden && !r.unlocked;
        const int ry=list_y+(i-first)*row_h;
        c.image(*k.boxes,1,sel?937:425,710,85,list_x,ry,box_w,box_h);
        const int ic=px(58);
        const TrophyIcon* icon=secret?nullptr:(size_t(i)<v.icons.size()?v.icons[size_t(i)]:nullptr);
        if(icon&&!icon->rgba.empty())c.rgba_image(icon->rgba,icon->w,icon->h,list_x+px(20),ry+px(12),ic,!r.unlocked);
        const std::string right=r.unlocked?std::string(grade_word(r.grade)):(!secret&&r.target>0?std::to_string(std::min(r.progress,r.target))+"/"+std::to_string(r.target):std::string(grade_word(r.grade)));
        const int rw=int(k.bold->width(right,gradef));
        const Color name_col=sel?Color{255,255,255}:kGold;
        const std::string name=art_fit(*k.bold,secret?"Hidden Trophy":r.name,rowf,box_w-px(20)-ic-px(60)-rw-px(30));
        c.art_text(*k.bold,name,list_x+px(20)+ic+px(16),ry+px(18),rowf,r.unlocked||sel?name_col:Color{uint8_t(name_col[0]*6/10),uint8_t(name_col[1]*6/10),uint8_t(name_col[2]*6/10)});
        c.art_text(*k.bold,right,list_x+box_w-px(34)-rw,ry+px(26),gradef,r.unlocked?grade_ink(r.grade):Color{190,176,150});
    }
    // the highlighted trophy on a paper card
    if(!v.rows.empty()) {
        const auto& r=v.rows[size_t(std::clamp(v.selected,0,int(v.rows.size())-1))];
        const bool secret=r.hidden && !r.unlocked;
        const int cx=list_x+box_w+px(40),cw=W-cx-px(40),cy=list_y,pad=px(22),icon=px(112);
        const float cn=font_scale(*k.bold,float(px(34))),cg=font_scale(*k.bold,float(px(20))),cd=font_scale(*k.boo,float(px(24)));
        const auto lines=art_wrap(*k.boo,secret?std::string("Keep playing to reveal this trophy."):r.detail,cd,cw-2*pad);
        const std::string ptext=secret?std::string():progress_text(r);
        const int ch=pad+icon+px(14)+int(lines.size())*px(32)+(ptext.empty()?0:px(80))+pad;
        c.paper(cx,cy,cw,ch,px(10),px(6));
        const TrophyIcon* ic=secret?nullptr:(size_t(v.selected)<v.icons.size()?v.icons[size_t(v.selected)]:nullptr);
        if(ic&&!ic->rgba.empty())c.rgba_image(ic->rgba,ic->w,ic->h,cx+pad,cy+pad,icon,!r.unlocked);
        const int tx=cx+pad+icon+px(14);
        const auto head=art_wrap(*k.bold,secret?"Hidden Trophy":r.name,cn,cx+cw-pad-tx);
        int hy=cy+pad;
        for(size_t i=0;i<head.size()&&i<2;++i) { c.art_text(*k.bold,head[i],tx,hy,cn,kGold);hy+=px(38); }
        c.art_text(*k.bold,std::string(grade_word(r.grade))+(r.unlocked?", UNLOCKED":", LOCKED"),tx,hy+px(4),cg,kBrownMid,cx+cw-pad);
        int y=cy+pad+icon+px(14);
        for(const auto& line:lines) { c.art_text(*k.boo,line,cx+pad,y,cd,kBrown,cx+cw-pad);y+=px(32); }
        if(!ptext.empty()) {
            y+=px(8);
            c.art_text(*k.boo,ptext,cx+pad,y,cd,r.unlocked?kGreen:kBrown,cx+cw-pad);y+=px(34);
            paper_bar(c,cx+pad,y,cw-2*pad,px(18),r.unlocked?1.f:float(std::min(r.progress,r.target))/float(r.target));
        }
        if(r.unlocked&&!r.when_text.empty())
            c.art_text(*k.boo,r.when_text,cx+pad,cy+ch-pad-px(20),font_scale(*k.boo,float(px(18))),kBrownMid,cx+cw-pad);
    }
    c.art_text(*k.boo,v.controls,px(48),H-bar_h-px(36),hintf,{246,231,184},W-px(40));
    return std::move(c.rgba);
}

// All trophies as the open storybook from How To Play: the list on the left page, the highlighted trophy on the right.
std::vector<uint8_t> trophy_list_pixels(const TrophyListView& v,float scale,unsigned& width,unsigned& height,unsigned& radius) {
    Skin k;
    if(!load_skin(k) || !k.book)return menu_list_pixels(v,scale,width,height,radius);
    const auto px=[scale](float x){return int(std::lround(x*scale));};
    const int W=px(936),H=px(704);
    width=unsigned(W);height=unsigned(H);radius=0;
    Canvas c(width,height);
    c.copy_rgba(*k.book,8,20,936,704,0,0,W,H);
    const Color ink{160,106,18};
    const float titlef=font_scale(*k.bold,float(px(40))),rowf=font_scale(*k.boo,float(px(25))),smallf=font_scale(*k.boo,float(px(20)));
    const int lx=px(84),lw=px(350),rx=px(500),rw=px(350);
    c.art_text(*k.bold,"Trophies",lx,px(62),titlef,ink);
    const std::string count=std::to_string(v.earned)+" / "+std::to_string(v.total);
    c.art_text(*k.boo,count,lx+lw-int(k.boo->width(count,rowf)),px(70),rowf,kBrownMid);
    const int row_h=px(48),top=px(122);
    const int pages=std::max(1,(int(v.rows.size())+kTrophyPageSize-1)/kTrophyPageSize);
    const int page=std::clamp(v.selected/kTrophyPageSize,0,pages-1),first=page*kTrophyPageSize,visible=kTrophyPageSize;
    for(int i=first;i<int(v.rows.size()) && i<first+visible;++i) {
        const auto& r=v.rows[size_t(i)];
        const bool sel=i==v.selected,secret=r.hidden && !r.unlocked;
        const int ry=top+(i-first)*row_h;
        if(sel)c.box(lx-px(6),ry,lw+px(12),row_h-px(4),{255,229,106},px(6));
        else for(int x=lx;x<lx+lw;x+=px(6))c.box(x,ry+row_h-px(4),px(3),std::max(1,px(1)),{184,151,90});
        const int ic=px(36);
        const TrophyIcon* icon=secret?nullptr:(size_t(i)<v.icons.size()?v.icons[size_t(i)]:nullptr);
        if(icon&&!icon->rgba.empty())c.rgba_image(icon->rgba,icon->w,icon->h,lx,ry+px(3),ic,!r.unlocked);
        const std::string tag=r.unlocked?std::string("DONE"):(!secret&&r.target>0?std::to_string(std::min(r.progress,r.target))+"/"+std::to_string(r.target):std::string());
        const int tw=int(k.boo->width(tag,smallf));
        const Color col=r.unlocked?kBrown:Color{uint8_t(kBrown[0]+60),uint8_t(kBrown[1]+55),uint8_t(kBrown[2]+40)};
        const std::string shown=secret?"Hidden Trophy":r.name;
        const int room=lw-ic-px(16)-tw-px(8);
        const float rf=k.boo->width(shown,rowf)>room?rowf*float(room)/k.boo->width(shown,rowf):rowf;   // shrink a long name to fit
        c.art_text(*k.boo,shown,lx+ic+px(10),ry+px(8)+int((rowf-rf)*font_em(*k.boo)*.5f),rf,col,lx+lw);
        if(!tag.empty())c.art_text(*k.boo,tag,lx+lw-tw,ry+px(11),smallf,r.unlocked?kGreen:kBrownMid);
    }
    if(!v.rows.empty()) {
        const auto& r=v.rows[size_t(std::clamp(v.selected,0,int(v.rows.size())-1))];
        const bool secret=r.hidden && !r.unlocked;
        const TrophyIcon* ic=secret?nullptr:(size_t(v.selected)<v.icons.size()?v.icons[size_t(v.selected)]:nullptr);
        const int big=px(150);
        if(ic&&!ic->rgba.empty())c.rgba_image(ic->rgba,ic->w,ic->h,rx+(rw-big)/2,px(76),big,!r.unlocked);
        const float nf=font_scale(*k.bold,float(px(38))),df=font_scale(*k.boo,float(px(27))),gf=font_scale(*k.bold,float(px(22)));
        int y=px(240);
        for(const auto& line:art_wrap(*k.bold,secret?"Hidden Trophy":r.name,nf,rw)) {
            c.art_text(*k.bold,line,rx+(rw-int(k.bold->width(line,nf)))/2,y,nf,ink);y+=px(40);
        }
        const std::string grade=std::string(grade_word(r.grade))+" TROPHY";
        c.art_text(*k.bold,grade,rx+(rw-int(k.bold->width(grade,gf)))/2,y,gf,kBrownMid);y+=px(34);
        for(const auto& line:art_wrap(*k.boo,secret?std::string("Keep playing to reveal this trophy."):r.detail,df,rw)) {
            c.art_text(*k.boo,line,rx+(rw-int(k.boo->width(line,df)))/2,y,df,kBrown);y+=px(33);
        }
        const std::string ptext=secret?std::string():progress_text(r);
        if(!ptext.empty()) {
            y+=px(8);
            c.art_text(*k.boo,ptext,rx+(rw-int(k.boo->width(ptext,df)))/2,y,df,r.unlocked?kGreen:kBrown);y+=px(36);
            paper_bar(c,rx+px(20),y,rw-px(40),px(18),r.unlocked?1.f:float(std::min(r.progress,r.target))/float(r.target));y+=px(28);
        }
        const std::string st=r.unlocked?(r.when_text.empty()?std::string("Earned"):r.when_text):std::string("Not earned yet");
        c.art_text(*k.boo,st,rx+(rw-int(k.boo->width(st,smallf)))/2,std::max(y+px(6),px(560)),smallf,r.unlocked?kGreen:kBrownMid);
    }
    const std::string pg="Page "+std::to_string(page+1)+" of "+std::to_string(pages);
    c.art_text(*k.boo,pg,lx+lw-int(k.boo->width(pg,smallf)),px(580),smallf,kBrownMid);
    c.art_text(*k.boo,v.controls,lx,px(612),font_scale(*k.boo,float(px(17))),kBrownMid,rx+rw);
    // a page turning: the game's three flip frames (bookpageflip), leaf lifting off the right page and landing on the left
    if(v.flip>=0 && v.flip<1) if(const ArtImage* fl=art_image("data/textures/menus/bookpageflip")) {
        struct Frame{int x,y,w,h;};
        const Frame fr[3]={{0,100,312,730},{318,0,296,830},{620,40,404,790}};
        const int n=std::min(2,int(v.flip*3));
        const int idx=v.flip_dir>=0?2-n:n;   // forward: flat leaf, standing leaf, curling leaf; back: the reverse
        const Frame& f=fr[idx];
        const float k0=.85f;
        const int dw=int(std::lround(f.w*k0*scale)),dh=int(std::lround(f.h*k0*scale));
        const int spine=px(462),dy=px(28);
        const int dx=idx==2?spine:spine-dw;
        c.image(*fl,f.x,f.y,f.w,f.h,dx,dy,dw,dh);
    }
    return std::move(c.rgba);
}
}
