#include "self_test.h"
#include "game_controls.h"
#include "settings.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Sample { double time; float x, y, z; bool ai, available; uint32_t character; };
struct Run {
    fp::SelfTestStatus status;
    Clock::time_point start;
    uint32_t player = 0, last_flip = 0;
    bool original_ai = false, expected_ai = true, camera = false, awaiting = true;
    double last_report = -5, last_sample = -1, last_motion = 0, last_shot = -15, last_camera = -1;
    float x = 0, y = 0, z = 0;
    unsigned frames = 0, moves = 0, camera_moves = 0, acknowledgements = 0, requests = 0, phase = 0, suspects = 0, shots = 0;
    std::string ai_profile;
    unsigned respawns = 0, class_actions = 0, class_stage = 0, periodic_shots = 0;
    double class_resume = 0;
    uint32_t character = 0;
    std::string failure, review;
    std::vector<Sample> samples;
    std::array<std::vector<float>, 3> grids;
    std::array<std::vector<unsigned char>, 3> images;
    std::vector<std::string> captures;
};
Run& run() { static Run r; return r; }
std::string escape(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c >= 32 && c < 127) out += c;
        else out += '?';
    }
    return out;
}
void screenshot(Run& r, const unsigned char* rgba, unsigned w, unsigned h, const char* kind) {
    if (!rgba || !w || !h || r.shots >= 24) return;
    const std::string name = std::string(kind) + '-' + std::to_string(r.shots++) + ".bmp";
    std::ofstream file(std::filesystem::path(r.status.report).parent_path() / name, std::ios::binary);
    const uint32_t pitch=(w*3+3)&~3u;
    std::array<unsigned char,54> header{};
    const auto put=[&](unsigned at,uint32_t value) { for(unsigned c=0;c<4;++c) header[at+c]=value>>(c*8); };
    header[0]='B';header[1]='M';put(2,54+pitch*h);put(10,54);put(14,40);put(18,w);put(22,uint32_t(-int32_t(h)));
    header[26]=1;header[28]=24;put(34,pitch*h);
    file.write(reinterpret_cast<const char*>(header.data()),header.size());
    std::vector<unsigned char> rgb(size_t(pitch)*h);
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) for(unsigned c=0;c<3;++c)
        rgb[size_t(y)*pitch+x*3+c]=rgba[(size_t(y)*w+x)*4+2-c];
    file.write(reinterpret_cast<const char*>(rgb.data()),rgb.size());
    r.captures.push_back(name);
}
void report(Run& r) {
    const auto path = std::filesystem::path(r.status.report);
    std::ofstream json(path.string() + ".tmp");
    json << "{\n  \"result\":\"" << r.status.result << "\",\n  \"scenario\":\"" << r.status.scenario
         << "\",\n  \"ai_profile\":\"" << escape(r.ai_profile)
         << "\",\n  \"elapsed_s\":" << r.status.elapsed << ",\n  \"duration_s\":" << r.status.duration
         << ",\n  \"frames\":" << r.frames << ",\n  \"movement_samples\":" << r.moves
         << ",\n  \"free_camera_movement_samples\":" << r.camera_moves
         << ",\n  \"control_requests\":" << r.requests << ",\n  \"control_acknowledgements\":" << r.acknowledgements
         << ",\n  \"character_replacements\":" << r.respawns << ",\n  \"class_actions_sent\":" << r.class_actions << ",\n  \"visual_review_candidates\":" << r.suspects
         << ",\n  \"review_reason\":\"" << escape(r.review) << "\""
         << ",\n  \"failure\":\"" << escape(r.failure) << "\",\n  \"samples\":[";
    for (size_t i=0; i<r.samples.size(); ++i) {
        const auto& s=r.samples[i];
        if(i) json << ',';
        json << "{\"t\":" << s.time << ",\"ai\":" << s.ai << ",\"available\":" << s.available
             << ",\"x\":" << s.x << ",\"y\":" << s.y << ",\"z\":" << s.z << '}';
    }
    json << "]\n}\n"; json.close();
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::rename(path.string() + ".tmp", path, error);
    if(error) { r.failure="COULD NOT WRITE REPORT"; r.status.result="FAIL"; }
    std::ofstream html(path.parent_path() / "report.html");
    html << "<!doctype html><meta charset=utf-8>" << (r.status.running?"<meta http-equiv=refresh content=5>":"") << "<title>Self-test report</title><style>body{background:#181b24;color:#f2ebdc;font:18px system-ui;max-width:1000px;margin:40px auto;padding:20px}h1,b{color:#ee91ae}svg{background:#242836;width:100%;height:240px}td,th{padding:10px;text-align:left;border-bottom:1px solid #555}a{color:#ee91ae}</style>"
         << "<h1>" << r.status.scenario << ": " << r.status.result << "</h1><p>" << escape(r.failure.empty()?r.review:r.failure)
         << "</p><p>This run checks AI control, movement and continued presentation. Visual candidates require inspection. Character replacements can include respawns or class changes.</p><table>"
         << "<tr><th>AI profile</th><td>" << escape(r.ai_profile) << "</td></tr>"
         << "<tr><th>Elapsed</th><td>" << r.status.elapsed << " / " << r.status.duration << " seconds</td></tr>"
         << "<tr><th>Frames</th><td>" << r.frames << "</td></tr><tr><th>Movement samples</th><td>" << r.moves
         << "</td></tr><tr><th>Control changes acknowledged</th><td>" << r.acknowledgements << " / " << r.requests
         << "</td></tr><tr><th>Movement samples with free camera active</th><td>" << r.camera_moves
         << "</td></tr><tr><th>Class actions sent</th><td>" << r.class_actions << "</td></tr><tr><th>Character replacements</th><td>" << r.respawns
         << "</td></tr><tr><th>Visual review candidates</th><td>" << r.suspects << "</td></tr></table>"
         << "<h2>Player movement</h2><p>Each point is a sampled X/Y position in the match. The line connects them in time order.</p><svg viewBox='0 0 1000 240'><polyline fill='none' stroke='#ee91ae' stroke-width='2' points='";
    for(const auto& s:r.samples) if(s.available) html << 500+s.x*4 << ',' << 120-s.y*2 << ' ';
    html << "'/></svg><p><a href='result.json'>Machine-readable results</a></p><ul>";
    for(const auto& name:r.captures) html << "<li><a href='" << name << "'>" << name << "<br><img style='max-width:460px;width:100%' src='" << name << "'></a></li>";
    html << "</ul>";
}
void finish(Run& r, const char* result) {
    r.status.running=false;
    r.status.result=result;
    const auto state=fp::player_control_state();
    if(r.camera) {
        if(state.player==r.player) fp::player_command_request("freecamera toggle");
        r.camera=false;
    }
    if(state.available && state.player==r.player) fp::player_control_request(r.original_ai);
    report(r);
    std::fprintf(stderr,"[self-test] %s %s report=%s\n",r.status.scenario.c_str(),r.status.result.c_str(),r.status.report.c_str());
}
}
namespace fp {
SelfTestStatus self_test_status() { return run().status; }
bool self_test_start(const std::string& scenario, unsigned seconds) {
    auto& r=run(); const auto state=player_control_state();
    if(r.status.running || !state.available || (scenario=="WATER SWEEP" && state.free_camera) || seconds<20 || seconds>7200 ||
       (scenario!="AI SOAK" && scenario!="CONTROL HANDOFF" && scenario!="WATER SWEEP" && scenario!="CLASS CHANGES")) return false;
    r=Run{}; r.player=state.player; r.original_ai=state.ai; r.ai_profile=state.ai_profile; r.start=Clock::now();
    r.x=state.x; r.y=state.y; r.z=state.z; r.character=state.character;
    r.status.scenario=scenario; r.status.duration=seconds; r.status.running=true; r.status.result="RUNNING";
    const auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const char* root=std::getenv("FP_SELFTEST_DIR");
    static unsigned sequence=0;
    const auto directory=std::filesystem::path(root&&*root?root:"reports")/("self-test-"+std::to_string(stamp)+"-"+std::to_string(++sequence));
    std::error_code error;
    const bool existed=std::filesystem::exists(directory,error);
    std::filesystem::create_directories(directory,error);
    if(error || existed) { r.status.running=false; r.status.result="CANNOT CREATE REPORT DIRECTORY"; return false; }
    r.status.report=(directory/"result.json").string();
    r.requests=1; player_control_request(true); report(r);
    return true;
}
void self_test_stop() { auto& r=run(); if(r.status.running) finish(r,"CANCELLED"); }
void self_test_frame(uint32_t flip, const unsigned char* rgba, unsigned w, unsigned h) {
    auto& r=run(); if(!r.status.running) return;
    const double elapsed=std::chrono::duration<double>(Clock::now()-r.start).count();
    r.status.elapsed=unsigned(elapsed);
    if(flip && flip!=r.last_flip) { ++r.frames; r.last_flip=flip; }
    const auto state=player_control_state();
    if(state.player && state.player!=r.player) { r.failure="LOCAL PLAYER CHANGED"; finish(r,"FAIL"); return; }
    if(elapsed-r.last_sample>=.5) {
        r.last_sample=elapsed;
        if(state.available && std::isfinite(state.x) && std::isfinite(state.y) && std::isfinite(state.z)) {
            const double distance=std::hypot(std::hypot(state.x-r.x,state.y-r.y),state.z-r.z);
            if(distance>.15) { if(state.ai) { ++r.moves; if(state.free_camera) ++r.camera_moves; } r.last_motion=elapsed; r.x=state.x;r.y=state.y;r.z=state.z; }
            if(state.character && r.character && state.character!=r.character) ++r.respawns;
            r.character=state.character;
        } else if(state.available) { r.failure="NON-FINITE PLAYER POSITION"; finish(r,"FAIL"); return; }
        r.samples.push_back({elapsed,std::isfinite(state.x)?state.x:0.f,std::isfinite(state.y)?state.y:0.f,
                             std::isfinite(state.z)?state.z:0.f,state.ai,state.available,state.character});
        if(!state.pending && state.available && state.ai==r.expected_ai && r.awaiting) { ++r.acknowledgements; r.awaiting=false; }
        if(!state.error.empty()) { r.failure=state.error; finish(r,"FAIL"); return; }
        if(state.ai_profile!=r.ai_profile) r.review="AI PROFILE CHANGED DURING TEST";
        if(elapsed>12 && state.ai && r.ai_profile!="ORIGINAL" && !state.profile_applied) { r.failure="SELECTED AI PROFILE DID NOT APPLY";finish(r,"FAIL");return; }
        if(elapsed>12 && !state.available && r.samples.size()>20 &&
           std::none_of(r.samples.end()-20,r.samples.end(),[](const Sample& s){return s.available;})) {
            r.failure="PLAYER UNAVAILABLE FOR TEN SECONDS"; finish(r,"FAIL"); return;
        }
        if(state.ai && elapsed-r.last_motion>60) { r.failure="NO PLAYER MOVEMENT FOR SIXTY SECONDS"; finish(r,"FAIL"); return; }
        if(elapsed-r.last_report>=5) { report(r); r.last_report=elapsed; }
    }
    if(r.status.scenario=="CONTROL HANDOFF") {
        const unsigned phase=std::min(3u,unsigned(elapsed/(r.status.duration/4.0)));
        if(phase!=r.phase) {
            r.phase=phase; r.expected_ai=phase%2==0;
            if(player_control_request(r.expected_ai)) { ++r.requests; r.awaiting=true; }
        }
    }
    if(r.status.scenario=="CLASS CHANGES" && !fp::settings_menu_open()) {
        if(r.class_stage==0 && r.class_actions<4 && elapsed>=10+r.class_actions*12) {
            r.expected_ai=false;r.awaiting=true;
            if(player_control_request(false)) { ++r.requests;r.class_stage=1; }
        } else if(r.class_stage==1 && state.available && !state.ai && !state.pending) {
            static const char* actions[]={"act_warrior","act_ranger","act_mage","act_worker"};
            if(fp::settings_action(actions[r.class_actions])) { ++r.class_actions;r.class_stage=2;r.class_resume=elapsed+1; }
            else { r.failure="CLASS ACTION UNAVAILABLE. ENABLE DEBUG CHEAT COMBOS";finish(r,"FAIL");return; }
        } else if(r.class_stage==2 && elapsed>=r.class_resume) {
            r.expected_ai=true;r.awaiting=true;
            if(player_control_request(true)) { ++r.requests;r.class_stage=0; }
        }
    }
    if(r.status.scenario=="WATER SWEEP" && state.ai && elapsed>=10) {
        if(!r.camera) {
            player_command_request("freecamera toggle"); player_command_request("freecamera setDir 0 1 -2"); r.camera=true;
        }
        if(elapsed-r.last_camera>=1) {
            const double phase=std::fmod(elapsed-10,48.0)/24;
            const double z=18+36*(phase<=1?phase:2-phase);
            char command[96]; std::snprintf(command,sizeof command,"freecamera setPos 25 -20 %.3f",z);
            player_command_request(command); r.last_camera=elapsed;
        }
    }
    if(rgba && w && h) {
        if(r.periodic_shots<12 && (r.periodic_shots==0 || elapsed-r.last_shot>=std::max(15.0,r.status.duration/12.0))) {
            screenshot(r,rgba,w,h,"periodic");r.last_shot=elapsed;++r.periodic_shots;
        }
        r.images[r.frames%3].assign(rgba,rgba+size_t(w)*h*4);
        auto& grid=r.grids[r.frames%3]; grid.assign(48*3,0);
        for(unsigned gy=0;gy<6;++gy) for(unsigned gx=0;gx<8;++gx) {
            unsigned count=0;
            for(unsigned y=gy*h/6;y<(gy+1)*h/6;y+=4) for(unsigned x=gx*w/8;x<(gx+1)*w/8;x+=4) {
                for(unsigned c=0;c<3;++c) grid[(gy*8+gx)*3+c]+=rgba[(size_t(y)*w+x)*4+c]; ++count;
            }
            if(count) for(unsigned c=0;c<3;++c) grid[(gy*8+gx)*3+c]/=count;
        }
        if(r.frames>=3 && r.grids[0].size() && r.grids[1].size() && r.grids[2].size()) {
            const auto& a=r.grids[(r.frames+1)%3]; const auto& b=r.grids[(r.frames+2)%3];
            unsigned cells=0;
            for(unsigned cell=0;cell<48;++cell) {
                float ab=0,bc=0,ac=0;
                for(unsigned c=0;c<3;++c) { const auto i=cell*3+c;ab+=std::abs(a[i]-b[i]);bc+=std::abs(b[i]-grid[i]);ac+=std::abs(a[i]-grid[i]); }
                cells+=std::min(ab,bc)-ac>25;
            }
            if(cells>=6 && r.suspects<4 && (!r.camera || elapsed-r.last_camera>.2)) {
                ++r.suspects;
                const auto& before=r.images[(r.frames+1)%3];const auto& middle=r.images[(r.frames+2)%3];
                if(before.size()==size_t(w)*h*4 && middle.size()==before.size()) {
                    screenshot(r,before.data(),w,h,"review-before");screenshot(r,middle.data(),w,h,"review-middle");
                    screenshot(r,rgba,w,h,"review-after");
                }
            }
        }
    }
    if(elapsed>=r.status.duration) {
        if(!r.moves) r.failure="NO AI MOVEMENT OBSERVED";
        else if(r.status.scenario=="WATER SWEEP" && !r.camera_moves) r.failure="NO AI MOVEMENT WHILE FREE CAMERA ACTIVE";
        else if(r.acknowledgements<r.requests) r.failure="CONTROL TRANSFER WAS NOT ACKNOWLEDGED";
        else if(r.frames<r.status.duration*5) r.failure="INSUFFICIENT PRESENTED FRAMES";
        if(r.suspects) r.review="VISUAL CANDIDATES NEED INSPECTION";
        if(r.status.scenario=="CLASS CHANGES") r.review="CLASS IDENTITY NEEDS VISUAL REVIEW; SEE THE CLASS SCREENSHOTS";
        finish(r,!r.failure.empty()?"FAIL":!r.review.empty()?"REVIEW":"PASS");
    }
}
}
