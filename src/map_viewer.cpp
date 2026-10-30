#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

struct Vec3 { float x, y, z; };
struct Triangle { unsigned a, b, c, na, nb, nc; };

static float length(Vec3 a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); }

static bool load_obj(const char* path, std::vector<Vec3>& vertices,
                     std::vector<Vec3>& normals, std::vector<Triangle>& triangles,
                     Vec3& center, float& radius) {
    std::ifstream file(path);
    if (!file) { std::cerr << "cannot open OBJ: " << path << '\n'; return false; }
    std::string line;
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 hi{-lo.x, -lo.y, -lo.z};
    while (std::getline(file, line)) {
        std::istringstream row(line);
        char kind[8]{};
        row >> kind;
        if (kind[0] == 'v' && kind[1] == '\0') {
            Vec3 v{}; row >> v.x >> v.y >> v.z;
            if (!row) continue;
            vertices.push_back(v);
            lo.x=std::min(lo.x,v.x); lo.y=std::min(lo.y,v.y); lo.z=std::min(lo.z,v.z);
            hi.x=std::max(hi.x,v.x); hi.y=std::max(hi.y,v.y); hi.z=std::max(hi.z,v.z);
        } else if (kind[0] == 'f') {
            unsigned a=0,b=0,c=0,na=0,nb=0,nc=0;
            std::string sa,sb,sc;
            row >> sa >> sb >> sc;
            auto indices = [](const std::string& s) {
                const auto slash=s.find("//");
                const auto vi=static_cast<unsigned>(std::stoul(s.substr(0,slash)));
                const auto ni=slash==std::string::npos ? vi : static_cast<unsigned>(std::stoul(s.substr(slash+2)));
                return std::pair<unsigned,unsigned>{vi,ni};
            };
            try {
                std::tie(a,na)=indices(sa); std::tie(b,nb)=indices(sb); std::tie(c,nc)=indices(sc);
                if (a && b && c && a<=vertices.size() && b<=vertices.size() && c<=vertices.size()
                    && na && nb && nc && na<=normals.size() && nb<=normals.size() && nc<=normals.size())
                    triangles.push_back({a-1,b-1,c-1,na-1,nb-1,nc-1});
            } catch (...) { }
        } else if (kind[0] == 'v' && kind[1] == 'n' && kind[2] == '\0') {
            Vec3 n{}; row >> n.x >> n.y >> n.z;
            if (row) normals.push_back(n);
        }
    }
    if (vertices.empty() || triangles.empty()) {
        std::cerr << "OBJ has no usable triangle geometry (" << vertices.size()
                  << " vertices, " << triangles.size() << " triangles)\n";
        return false;
    }
    center={(lo.x+hi.x)*0.5f,(lo.y+hi.y)*0.5f,(lo.z+hi.z)*0.5f};
    radius=std::max({hi.x-lo.x,hi.y-lo.y,hi.z-lo.z})*0.5f;
    if (!(radius>0.0f)) radius=1.0f;
    std::cout << "loaded " << vertices.size() << " vertices and " << triangles.size()
              << " triangles; bounds " << lo.x << ',' << lo.y << ',' << lo.z << " .. "
              << hi.x << ',' << hi.y << ',' << hi.z << '\n';
    return true;
}

int main(int argc, char** argv) {
    const char* screenshot=nullptr;
    int max_frames=0;
    if (argc<2) {
        std::cerr << "Usage: fp_map_viewer <mesh.obj> [--screenshot image.ppm] [--frames N]\n"
                  << "WASD / arrow keys orbit, mouse drag orbits, wheel zooms, Esc exits.\n";
        return 2;
    }
    for (int i=2;i<argc;i++) {
        const std::string arg=argv[i];
        if (arg=="--screenshot" && i+1<argc) screenshot=argv[++i];
        else if (arg=="--frames" && i+1<argc) max_frames=std::max(1,std::atoi(argv[++i]));
        else { std::cerr << "unknown or incomplete option: " << arg << '\n'; return 2; }
    }
    std::vector<Vec3> vertices;
    std::vector<Vec3> normals;
    std::vector<Triangle> triangles;
    Vec3 center{}; float radius=1.0f;
    if (!load_obj(argv[1],vertices,normals,triangles,center,radius)) return 1;
    if (SDL_Init(SDL_INIT_VIDEO)!=0) { std::cerr << SDL_GetError() << '\n'; return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    SDL_Window* window=SDL_CreateWindow("Fat Princess — Static Map Viewer",
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,1280,800,
        SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    if (!window) { std::cerr << SDL_GetError() << '\n'; SDL_Quit(); return 1; }
    SDL_GLContext context=SDL_GL_CreateContext(window);
    if (!context) { std::cerr << SDL_GetError() << '\n'; SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_GL_SetSwapInterval(1);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    float yaw=30.0f,pitch=55.0f,distance=radius*2.4f;
    bool running=true,dragging=false;
    int prev_x=0,prev_y=0;
    int rendered_frames=0;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type==SDL_QUIT) running=false;
            if (event.type==SDL_KEYDOWN) {
                if (event.key.keysym.sym==SDLK_ESCAPE) running=false;
                if (event.key.keysym.sym==SDLK_LEFT || event.key.keysym.sym==SDLK_a) yaw-=5;
                if (event.key.keysym.sym==SDLK_RIGHT || event.key.keysym.sym==SDLK_d) yaw+=5;
                if (event.key.keysym.sym==SDLK_UP || event.key.keysym.sym==SDLK_w) pitch=std::min(88.0f,pitch+4);
                if (event.key.keysym.sym==SDLK_DOWN || event.key.keysym.sym==SDLK_s) pitch=std::max(5.0f,pitch-4);
            }
            if (event.type==SDL_MOUSEBUTTONDOWN && event.button.button==SDL_BUTTON_LEFT) {
                dragging=true; prev_x=event.button.x; prev_y=event.button.y;
            }
            if (event.type==SDL_MOUSEBUTTONUP && event.button.button==SDL_BUTTON_LEFT) dragging=false;
            if (event.type==SDL_MOUSEMOTION && dragging) {
                yaw+=static_cast<float>(event.motion.x-prev_x)*0.45f;
                pitch=std::clamp(pitch-static_cast<float>(event.motion.y-prev_y)*0.35f,5.0f,88.0f);
                prev_x=event.motion.x; prev_y=event.motion.y;
            }
            if (event.type==SDL_MOUSEWHEEL) distance=std::clamp(distance*std::pow(0.88f,static_cast<float>(event.wheel.y)),radius*0.25f,radius*12.0f);
        }
        int width=1,height=1; SDL_GL_GetDrawableSize(window,&width,&height);
        if (height<1) height=1;
        glViewport(0,0,width,height);
        glClearColor(0.09f,0.13f,0.17f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        const double near_plane=std::max(0.01,static_cast<double>(radius)*0.001);
        const double far_plane=static_cast<double>(radius)*40.0;
        const double top=near_plane*0.42;
        const double aspect=static_cast<double>(width)/static_cast<double>(height);
        glFrustum(-top*aspect,top*aspect,-top,top,near_plane,far_plane);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glTranslatef(0,0,-distance);
        glRotatef(pitch,1,0,0); glRotatef(yaw,0,1,0);
        glTranslatef(-center.x,-center.y,-center.z);
        glBegin(GL_TRIANGLES);
        for (const Triangle& t:triangles) {
            const Vec3 a=vertices[t.a],b=vertices[t.b],c=vertices[t.c];
            auto shade = [](Vec3 n) {
                const float nl=std::max(length(n),1.0e-8f); n.x/=nl; n.y/=nl; n.z/=nl;
                const float light=0.35f+0.65f*std::max(0.0f,n.x*0.28f+n.y*0.88f+n.z*0.37f);
                glColor3f(0.40f*light,0.57f*light,0.27f*light);
            };
            shade(normals[t.na]);
            glVertex3f(a.x,a.y,a.z);
            shade(normals[t.nb]); glVertex3f(b.x,b.y,b.z);
            shade(normals[t.nc]); glVertex3f(c.x,c.y,c.z);
        }
        glEnd();
        if (screenshot && rendered_frames==0) {
            std::vector<unsigned char> pixels(static_cast<size_t>(width)*static_cast<size_t>(height)*3);
            glPixelStorei(GL_PACK_ALIGNMENT,1);
            glReadBuffer(GL_BACK);
            glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
            std::ofstream out(screenshot,std::ios::binary);
            out << "P6\n" << width << ' ' << height << "\n255\n";
            for (int y=height-1;y>=0;y--)
                out.write(reinterpret_cast<const char*>(pixels.data()+static_cast<size_t>(y)*static_cast<size_t>(width)*3),
                          static_cast<std::streamsize>(width)*3);
            if (!out) { std::cerr << "failed to write screenshot: " << screenshot << '\n'; running=false; }
            else std::cout << "wrote rendered frame " << screenshot << '\n';
        }
        SDL_GL_SwapWindow(window);
        rendered_frames++;
        if (max_frames && rendered_frames>=max_frames) running=false;
    }
    SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
