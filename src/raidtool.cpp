// raidtool: GUI front-end for raid_engine.exe.
//
// The CLI stays the source of truth for all format work; this app shells
// out to raid_engine.exe (expected next to raidtool.exe), streams its
// stdout into a log pane, and plays the bundled UI sounds on actions.
//
//   raidtool.exe            (no arguments; double-click friendly)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "raylib.h"

#include "raidtool_pick.hpp"

namespace {


// ---------------------------------------------------------------- sounds --

struct Sfx {
    Sound click, confirm, critical, deselect, help, openApp, procErr,
        procDone, select, startProc, traverse, warning;
    bool ok = false;

    bool init(const std::string& base) {
        // LoadSound on a .wav path works; keep it simple.
        auto L = [&](Sound& s, const char* n) {
            const std::string p = base + "/assets/tool_sfx/" + n + ".wav";
            if (FileExists(p.c_str())) s = LoadSound(p.c_str());
        };
        L(click, "Click"); L(confirm, "ConfirmAction"); L(critical, "CriticalError");
        L(deselect, "Deselect"); L(help, "HelpIcon"); L(openApp, "OpenApplication");
        L(procErr, "ProcessError"); L(procDone, "ProcessFinished");
        L(select, "Select"); L(startProc, "StartProcess"); L(traverse, "Traverse");
        L(warning, "Warning");
        ok = click.frameCount > 0;
        return ok;
    }
    void play(Sound s) const { if (ok && s.frameCount) PlaySound(s); }
};

// ------------------------------------------------------------------ log ---

struct Log {
    std::mutex m;
    std::deque<std::string> lines;
    std::string partial;
    void add(const std::string& s) {
        std::lock_guard<std::mutex> lk(m);
        if (lines.size() > 4000) lines.pop_front();
        lines.push_back(s);
    }
    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lk(m);
        return {lines.begin(), lines.end()};
    }
};

// ------------------------------------------------------------- worker -----

struct Job {
    std::atomic<bool> running{false};
    std::atomic<bool> failed{false};
    std::thread th;
    double startedAt = 0;
    std::string cmd;

    void run(const std::string& command, Log& log) {
        if (running.exchange(true)) return;
        failed = false;
        cmd = command;
        th = std::thread([&, command]() {
            // _popen runs `cmd /c <string>`; wrapping the whole command in a
            // quote pair is required or cmd mangles the quoted exe/args.
            FILE* p = _popen(('"' + command + "\" 2>&1").c_str(), "r");
            if (!p) { log.add("[tool] failed to launch raid_engine.exe"); failed = true; running = false; return; }
            char buf[4096];
            while (std::fgets(buf, sizeof buf, p)) {
                std::string s = buf;
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (!s.empty()) log.add(s);
            }
            const int rc = _pclose(p);
            if (rc != 0) {
                failed = true;
                log.add("[tool] raid_engine exited with code " + std::to_string(rc));
            } else {
                log.add("[tool] done.");
            }
            running = false;
        });
        if (th.joinable()) th.detach();
    }
    ~Job() { if (th.joinable()) th.join(); }
};

// ------------------------------------------------------------- pickers ----

// ------------------------------------------------------------ text input --

struct Input {
    std::string text;
    int caret = 0;
    bool focused = false;

    void handle() {
        const int k = GetCharPressed();
        if (k >= 32 && k < 0x110000 && text.size() < 1024) {
            if (IsKeyDown(KEY_LEFT_CONTROL)) {
                if (k == 'v' || k == 'V') {
                    const char* cb = GetClipboardText();
                    if (cb) {
                        for (const char* c = cb; *c; ++c)
                            if (*c >= 32 && text.size() < 1024) text.insert(size_t(caret++), 1, *c);
                        RL_FREE((void*)cb);
                    }
                }
            } else {
                text.insert(size_t(caret++), 1, char(k));
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) && caret > 0) { text.erase(size_t(--caret), 1); }
        if (IsKeyPressed(KEY_DELETE) && caret < int(text.size())) text.erase(size_t(caret), 1);
        if (IsKeyPressed(KEY_LEFT)) caret = std::max(0, caret - 1);
        if (IsKeyPressed(KEY_RIGHT)) caret = std::min(int(text.size()), caret + 1);
        if (IsKeyPressed(KEY_HOME)) caret = 0;
        if (IsKeyPressed(KEY_END)) caret = int(text.size());
    }
};

// ----------------------------------------------------------------- draw ---

Color fade(Color c, float a) { c.a = unsigned char(c.a * a); return c; }

// modal flag: while a modal (tour) is up, buttons don't respond
bool g_modal = false;

// word-wrap helper for tour text
std::vector<std::string> wrapText(Font font, const std::string& text, float size, float maxW) {
    std::vector<std::string> out;
    std::istringstream ss(text);
    std::string paragraph, word;
    auto flush = [&]() {
        if (paragraph.empty()) return;
        std::string cur;
        std::istringstream ws(paragraph);
        while (ws >> word) {
            const std::string test = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && MeasureTextEx(font, test.c_str(), size, 1).x > maxW) {
                out.push_back(cur); cur = word;
            } else cur = test;
        }
        if (!cur.empty()) out.push_back(cur);
        paragraph.clear();
    };
    std::string line;
    while (std::getline(ss, line)) { paragraph += line; flush(); }
    return out;
}

// first-run tour content
struct TourPage { const char* title; const char* body; };
const TourPage kTour[] = {
    {"Welcome to R-Slice Tool",
     "This is a friendly front-end for raid_engine.exe - the R-Slice\n"
     "engine toolkit for Friday Night Funkin' style rhythm charts.\n\n"
     "RAID (Robust Arrangement of Index Data) files are a compact binary\n"
     "chart format built for enormous FL Studio arrangements - billions to\n"
     "even TRILLIONS of note events stay small on disk and stream during\n"
     "playback instead of loading into memory all at once."},
    {"How RAID files work",
     "A .RAID file stores your chart as indexed chunks of encoded note\n"
     "operations plus a pattern table. Version 1 covers basic ops;\n"
     "version 2 adds overlapping pattern groups and burst repeats\n"
     "(produced when converting FL Studio playlists).\n\n"
     "The reader validates every chunk's checksum, so corrupt files\n"
     "are caught instead of silently misplaying."},
    {"The workflow",
     "1. Pick an input with FILE (single chart) or FOLDER (batch).\n"
     "2. Type extra arguments in EXTRA ARGS if needed.\n"
     "3. Press an action button - jobs run in the background so the\n"
     "   window stays responsive.\n"
     "4. Watch the log pane; SAVE LOG exports everything to a .txt."},
    {"The actions",
     "CONVERT   chart.json / .flp / .fsc -> .raid\n"
     "VERIFY    decode the whole file and check integrity\n"
     "INFO      header stats: version, events, chunks, patterns\n"
     "UPGRADE   fix files stamped v1 that contain v2 ops\n"
     "META      dump chart metadata as JSON\n"
     "STREAMTEST  decode the event stream end-to-end\n"
     "WINDOW    extract a time range (args: startMs endMs)\n"
     "PATTERNS  log the op stream (args: optional range)"},
    {"Good to know",
     "The raid_engine.exe command line still works exactly the same -\n"
     "this window is just a front-end. If verify warns about a\n"
     "mislabeled version, run UPGRADE on the file or folder.\n\n"
     "Press TOUR in the header any time to see this again."},
};
const int kTourCount = int(sizeof(kTour) / sizeof(kTour[0]));

void drawButton(Rectangle r, const char* label, const char* sub, bool hot,
                bool down, Color accent, Font font) {
    Color bg = down ? fade(accent, 0.55f) : (hot ? fade(accent, 0.32f) : Color{28, 36, 54, 255});
    DrawRectangleRounded(r, 0.12f, 8, fade(bg, hot || down ? 1.0f : 0.9f));
    DrawRectangleRoundedLinesEx(r, 0.12f, 8, 1.0f,
                                hot ? accent : Color{70, 90, 130, 255});
    const float fs = sub ? 17.0f : 20.0f;
    const char* txt = label;
    Vector2 ts = MeasureTextEx(font, txt, fs, 1);
    DrawTextEx(font, txt, {r.x + (r.width - ts.x) * 0.5f, r.y + (sub ? 10.0f : r.height * 0.5f - ts.y * 0.5f)},
               fs, 1, WHITE);
    if (sub) {
        Vector2 ss = MeasureTextEx(font, sub, 12, 1);
        DrawTextEx(font, sub, {r.x + (r.width - ss.x) * 0.5f, r.y + r.height - 24.0f}, 12, 1,
                   fade(Color{160, 180, 210, 255}, 1));
    }
}

bool button(Rectangle r, const char* label, const char* sub, Font font,
            Color accent, Sfx& sfx) {
    if (g_modal) { drawButton(r, label, sub, false, false, accent, font); return false; }
    const bool hot = CheckCollisionPointRec(GetMousePosition(), r);
    // traverse tick only on hover-enter: track hovered labels across frames
    static std::vector<const char*> s_hovered;
    auto it = std::find(s_hovered.begin(), s_hovered.end(), label);
    if (hot && it == s_hovered.end()) { sfx.play(sfx.traverse); s_hovered.push_back(label); }
    else if (!hot && it != s_hovered.end()) s_hovered.erase(it);
    if (hot && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) sfx.play(sfx.click);
    drawButton(r, label, sub, hot, hot && IsMouseButtonDown(MOUSE_LEFT_BUTTON), accent, font);
    return hot && IsMouseButtonReleased(MOUSE_LEFT_BUTTON);
}

}  // namespace

static int run() {
    raidtool::installCrashLog();
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(960, 640, "R-Slice Tool");
    InitAudioDevice();
    SetTargetFPS(60);

    // base dir = exe dir
    const std::string base = raidtool::moduleDir();

    Font font = LoadFont((base + "/assets/fonts/vcr.ttf").c_str());
    if (font.texture.id == 0) font = GetFontDefault();
    Sfx sfx;
    sfx.init(base);
    sfx.play(sfx.openApp);

    Log log;
    Job job;
    Input pathIn, argsIn;
    Rectangle logRect{};
    float logScroll = 0;
    std::string lastStatus = "Ready.";
    double jobStart = 0;

    // first-run tour (reopenable via the TOUR button)
    const std::string tourFlag = base + "/raidtool_tour_done.txt";
    bool tourActive = !FileExists(tourFlag.c_str());
    int tourPage = 0;
    auto closeTour = [&]() {
        FILE* f = nullptr;
        if (fopen_s(&f, tourFlag.c_str(), "w") == 0 && f) fclose(f);
        tourActive = false;
        sfx.play(sfx.confirm);
    };

    auto quote = [](const std::string& s) { return "\"" + s + "\""; };
    auto exeCmd = [&]() { return quote(base + "/raid_engine.exe"); };
    auto isDir = [](const std::string& p) { return raidtool::isDir(p); };

    auto launch = [&](const std::string& args, const char* what) {
        if (job.running) { log.add("[tool] busy - wait for the current job"); return; }
        if (pathIn.text.empty()) { log.add("[tool] set an input path first"); return; }
        const std::string cmd = exeCmd() + " " + args;
        log.add("[tool] " + std::string(what) + ": " + cmd);
        jobStart = GetTime();
        lastStatus = std::string(what) + " running...";
        sfx.play(sfx.startProc);
        job.run(cmd, log);
    };

    auto convertOut = [&]() {
        std::string p = pathIn.text;
        const size_t dot = p.find_last_of('.');
        const size_t slash = p.find_last_of('/');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return p + ".raid";
        return p.substr(0, dot) + ".raid";
    };

    // NOTE: this raylib fork is built with SUPPORT_CUSTOM_FRAME_CONTROL, so
    // EndDrawing() does NOT swap buffers or poll input events. Both must be
    // driven manually (same contract raide follows in app.cpp).
    while (true) {
        PollInputEvents();
        if (WindowShouldClose()) break;
        const double frameT0 = GetTime();
        g_modal = tourActive;
        // ---- input focus ----
        static Rectangle s_pathRect{}, s_argsRect{};
        if (!tourActive) {
            if (CheckCollisionPointRec(GetMousePosition(), s_pathRect)) pathIn.focused = true;
            else if (CheckCollisionPointRec(GetMousePosition(), s_argsRect)) { argsIn.focused = true; pathIn.focused = false; }
            else if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) { pathIn.focused = argsIn.focused = false; }
            if (pathIn.focused) pathIn.handle();
            if (argsIn.focused) argsIn.handle();
        }

        // ---- completion sfx ----
        static bool wasRunning = false;
        if (wasRunning && !job.running) {
            sfx.play(job.failed ? sfx.procErr : sfx.procDone);
            lastStatus = job.failed ? "Failed - see log." : "Finished.";
        }
        wasRunning = job.running;

        // ---- draw ----
        BeginDrawing();
        ClearBackground(Color{12, 16, 26, 255});

        // header
        DrawRectangleGradientV(0, 0, GetScreenWidth(), 58, Color{16, 24, 40, 255}, Color{10, 14, 24, 255});
        DrawTextEx(font, "R-SLICE TOOL", {20.0f, 14.0f}, 30, 1, Color{120, 220, 255, 255});
        DrawTextEx(font, "R-Slice engine front-end", {20, 42}, 13, 1, Color{120, 150, 190, 255});
        const char* st = job.running ? "RUNNING" : (job.failed ? "FAILED" : "IDLE");
        Color sc = job.running ? Color{255, 200, 80, 255} : (job.failed ? Color{255, 90, 90, 255} : Color{90, 220, 130, 255});
        DrawTextEx(font, st, {float(GetScreenWidth() - 90), 24}, 16, 1, sc);

        const float W = GetScreenWidth();
        // TOUR button (top-right of header, left of the status text)
        if (button({W - 200, 14, 92, 30}, "TOUR", nullptr, font,
                   Color{120, 220, 255, 255}, sfx)) {
            tourActive = true; tourPage = 0;
            sfx.play(sfx.select);
        }
        // path row
        DrawTextEx(font, "INPUT", {20, 74}, 14, 1, Color{140, 160, 200, 255});
        s_pathRect = {20, 92, W - 220, 36};
        DrawRectangleRounded(s_pathRect, 0.15f, 6, Color{20, 28, 44, 255});
        DrawRectangleRoundedLinesEx(s_pathRect, 0.15f, 6, 1.5f,
                                    pathIn.focused ? Color{120, 220, 255, 255} : Color{60, 80, 120, 255});
        std::string shown = pathIn.text;
        if (pathIn.focused) {
            const int cp = int(MeasureTextEx(font, shown.substr(0, size_t(pathIn.caret)).c_str(), 17, 1).x);
            shown += "|";
            DrawTextEx(font, shown.c_str(), {s_pathRect.x + 10, s_pathRect.y + 9}, 17, 1, WHITE);
            (void)cp;
        } else {
            DrawTextEx(font, shown.empty() ? "chart.json / .flp / .fsc / .raid / folder..." : shown.c_str(),
                       {s_pathRect.x + 10, s_pathRect.y + 9}, 17, 1,
                       shown.empty() ? fade(Color{110, 130, 170, 255}, 1) : WHITE);
        }
        if (button({W - 190, 92, 82, 36}, "FILE", nullptr, font, Color{90, 160, 255, 255}, sfx)) {
            std::string f = raidtool::pickFile("Choose input", "All supported\0*.json;*.flp;*.fsc;*.raid\0JSON\0*.json\0FL Studio\0*.flp;*.fsc\0RAID\0*.raid\0\0");
            if (!f.empty()) { pathIn.text = f; pathIn.caret = int(f.size()); sfx.play(sfx.select); }
        }
        if (button({W - 100, 92, 82, 36}, "FOLDER", nullptr, font, Color{90, 160, 255, 255}, sfx)) {
            std::string f = raidtool::pickFolder("Choose folder");
            if (!f.empty()) { pathIn.text = f; pathIn.caret = int(f.size()); sfx.play(sfx.select); }
        }

        // extra args row
        DrawTextEx(font, "EXTRA ARGS", {20, 140}, 14, 1, Color{140, 160, 200, 255});
        s_argsRect = {150, 134, W - 170, 30};
        DrawRectangleRounded(s_argsRect, 0.15f, 6, Color{20, 28, 44, 255});
        DrawRectangleRoundedLinesEx(s_argsRect, 0.15f, 6, 1.5f,
                                    argsIn.focused ? Color{120, 220, 255, 255} : Color{60, 80, 120, 255});
        BeginScissorMode(int(s_argsRect.x) + 2, int(s_argsRect.y) + 2,
                         int(s_argsRect.width) - 4, int(s_argsRect.height) - 4);
        if (argsIn.focused) {
            std::string shown = argsIn.text + "|";
            DrawTextEx(font, shown.c_str(), {s_argsRect.x + 10, s_argsRect.y + 7}, 15, 1, WHITE);
        } else {
            DrawTextEx(font, argsIn.text.c_str(), {s_argsRect.x + 10, s_argsRect.y + 7}, 15, 1,
                       argsIn.text.empty() ? fade(Color{110, 130, 170, 255}, 1) : WHITE);
        }
        EndScissorMode();

        // action grid
        struct Act { const char* label; const char* sub; const char* cmd; Color col; };
        const Act acts[] = {
            {"CONVERT", "chart/flp -> .raid", "convert", {90, 220, 140, 255}},
            {"VERIFY", "decode check", "verify", {120, 200, 255, 255}},
            {"INFO", "header stats", "info", {140, 170, 255, 255}},
            {"UPGRADE", "fix version stamp", "upgrade", {255, 200, 90, 255}},
            {"META", "dump metadata", "meta", {220, 140, 255, 255}},
            {"STREAMTEST", "decode stream", "streamtest", {255, 140, 200, 255}},
            {"WINDOW", "extract range", "window", {140, 255, 200, 255}},
            {"PATTERNS", "op-stream log", "patterns", {255, 170, 120, 255}},
        };
        const float bw = (W - 40 - 2 * 16) / 3, bh = 62;
        int ai = 0;
        for (const Act& a : acts) {
            Rectangle r{20 + float(ai % 3) * (bw + 16), 178 + float(ai / 3) * (bh + 12), bw, bh};
            if (button(r, a.label, a.sub, font, a.col, sfx)) {
                std::string args;
                const std::string p = quote(pathIn.text);
                if (!std::strcmp(a.cmd, "convert")) {
                    if (isDir(pathIn.text)) { log.add("[tool] convert takes a single file, not a folder"); sfx.play(sfx.warning); }
                    else args = "convert " + p + " " + quote(convertOut());
                } else if (!std::strcmp(a.cmd, "window")) {
                    if (argsIn.text.empty()) { log.add("[tool] window needs EXTRA ARGS like: 0 30000"); sfx.play(sfx.warning); }
                    else args = "window " + p + " " + argsIn.text;
                } else if (!std::strcmp(a.cmd, "patterns")) {
                    args = "patterns " + p + (argsIn.text.empty() ? "" : " " + argsIn.text);
                } else {
                    args = std::string(a.cmd) + " " + p;
                }
                if (!args.empty()) launch(args, a.label);
            }
            ++ai;
        }

        // log pane (below the 3-row action grid which ends at y=388)
        logRect = {20.0f, 400.0f, W - 40.0f, float(GetScreenHeight()) - 434.0f};
        DrawRectangleRounded(logRect, 0.04f, 6, Color{8, 11, 18, 255});
        DrawRectangleRoundedLinesEx(logRect, 0.04f, 6, 1.5f, Color{45, 60, 95, 255});
        // header strip: SAVE LOG button (this 30px zone is excluded from wheel scroll)
        Rectangle saveRect{logRect.x + logRect.width - 96, logRect.y + 4, 88, 22};
        DrawRectangleRounded(saveRect, 0.2f, 4, Color{24, 34, 54, 255});
        DrawRectangleRoundedLinesEx(saveRect, 0.2f, 4, 1.0f, Color{70, 90, 130, 255});
        {
            Vector2 ts = MeasureTextEx(font, "SAVE LOG", 12, 1);
            DrawTextEx(font, "SAVE LOG",
                       {saveRect.x + (saveRect.width - ts.x) * 0.5f, saveRect.y + 5},
                       12, 1, Color{170, 195, 235, 255});
        }
        const bool saveHot = CheckCollisionPointRec(GetMousePosition(), saveRect);
        if (saveHot && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            sfx.play(sfx.click);
            const auto now = std::chrono::system_clock::now();
            const std::time_t tt = std::chrono::system_clock::to_time_t(now);
            std::tm tmv{};
            localtime_s(&tmv, &tt);
            char name[64];
            std::snprintf(name, sizeof name, "/raidtool_log_%04d%02d%02d_%02d%02d%02d.txt",
                          tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                          tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            const std::string outPath = base + name;
            auto lines = log.snapshot();
            if (FILE* f = nullptr; fopen_s(&f, outPath.c_str(), "w") == 0 && f) {
                for (const auto& ln : lines) { fputs(ln.c_str(), f); fputc('\n', f); }
                fclose(f);
                log.add("[tool] log saved to " + outPath);
                sfx.play(sfx.confirm);
            } else {
                log.add("[tool] failed to write " + outPath);
                sfx.play(sfx.procErr);
            }
        }
        const float lineH = 17;
        auto snap = log.snapshot();
        const float maxScroll = std::max(0.0f, float(snap.size()) * lineH - (logRect.height - 12));
        if (IsKeyDown(KEY_LEFT_SHIFT) && GetMouseWheelMove() != 0.0f) logScroll += float(GetMouseWheelMove()) * lineH * 3;
        else if (!CheckCollisionPointRec(GetMousePosition(), {logRect.x, logRect.y, logRect.width, 30.0f})) logScroll += float(GetMouseWheelMove()) * lineH * 3;
        logScroll = std::clamp(logScroll, 0.0f, maxScroll);
        // autoscroll when new lines arrive and user is at bottom
        static size_t lastCount = 0;
        if (snap.size() != lastCount) {
            if (logScroll >= maxScroll - lineH * 2 || snap.size() < lastCount) logScroll = maxScroll;
            lastCount = snap.size();
        }
        BeginScissorMode(int(logRect.x) + 2, int(logRect.y) + 2,
                         int(logRect.width) - 4, int(logRect.height) - 4);
        for (size_t i = 0; i < snap.size(); ++i) {
            const float y = logRect.y + 8 + float(i) * lineH - logScroll;
            if (y < logRect.y - lineH || y > logRect.y + logRect.height) continue;
            Color c = Color{205, 220, 245, 255};
            if (snap[i].rfind("[tool]", 0) == 0) c = Color{120, 200, 255, 255};
            else if (snap[i].find("WARNING") != std::string::npos ||
                     snap[i].find("failed") != std::string::npos ||
                     snap[i].find("error") != std::string::npos) c = Color{255, 150, 120, 255};
            else if (snap[i].find("version") != std::string::npos) c = Color{255, 220, 140, 255};
            DrawTextEx(font, snap[i].c_str(), {logRect.x + 10, y}, 14, 1, c);
        }
        EndScissorMode();

        // status bar
        DrawRectangle(0, GetScreenHeight() - 30, GetScreenWidth(), 30, Color{10, 14, 22, 255});
        std::string sb = lastStatus;
        if (job.running) sb += "  (" + std::to_string(int(GetTime() - jobStart)) + "s)";
        DrawTextEx(font, sb.c_str(), {20.0f, float(GetScreenHeight()) - 24.0f}, 14, 1, Color{150, 170, 210, 255});
        DrawTextEx(font, "raid_engine CLI remains fully functional - R-Slice is a front-end.",
                   {float(GetScreenWidth()) - 380.0f, float(GetScreenHeight()) - 24.0f}, 12, 1,
                   fade(Color{110, 130, 170, 255}, 1));

        // ---------------- tour overlay ----------------
        if (tourActive) {
            // keyboard nav
            if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) {
                if (tourPage < kTourCount - 1) { ++tourPage; sfx.play(sfx.traverse); }
                else closeTour();
            }
            if (IsKeyPressed(KEY_LEFT) && tourPage > 0) { --tourPage; sfx.play(sfx.traverse); }
            if (IsKeyPressed(KEY_ESCAPE)) { closeTour(); sfx.play(sfx.deselect); }

            const float sw = GetScreenWidth(), sh = GetScreenHeight();
            DrawRectangle(0, 0, int(sw), int(sh), Color{4, 6, 12, 210});
            const Rectangle panel{sw * 0.5f - 330, sh * 0.5f - 210, 660, 420};
            DrawRectangleRounded(panel, 0.03f, 8, Color{16, 22, 36, 255});
            DrawRectangleRoundedLinesEx(panel, 0.03f, 8, 2.0f, Color{90, 140, 220, 255});

            const TourPage& pg = kTour[tourPage];
            DrawTextEx(font, pg.title, {panel.x + 28, panel.y + 22}, 24, 1,
                       Color{120, 220, 255, 255});
            DrawRectangle(panel.x + 28, panel.y + 62, panel.width - 56, 2,
                          Color{50, 70, 110, 255});

            auto lines = wrapText(font, pg.body, 16, panel.width - 56);
            float ty = panel.y + 84;
            for (const auto& ln : lines) {
                DrawTextEx(font, ln.c_str(), {panel.x + 28, ty}, 16, 1,
                           Color{205, 220, 245, 255});
                ty += 22;
            }

            // page dots
            const float dotY = panel.y + panel.height - 58;
            const float dotX0 = panel.x + panel.width * 0.5f - float(kTourCount) * 9;
            for (int i = 0; i < kTourCount; ++i)
                DrawCircle(dotX0 + i * 18 + 4, dotY, i == tourPage ? 5.0f : 3.0f,
                           i == tourPage ? Color{120, 220, 255, 255} : Color{70, 90, 130, 255});

            // nav buttons (lift the modal flag so these respond)
            g_modal = false;
            const char* nextLabel = tourPage < kTourCount - 1 ? "NEXT" : "DONE";
            if (button({panel.x + panel.width - 220, panel.y + panel.height - 52, 100, 34},
                       "BACK", nullptr, font, Color{120, 160, 220, 255}, sfx) && tourPage > 0) {
                --tourPage; sfx.play(sfx.traverse);
            }
            if (button({panel.x + panel.width - 110, panel.y + panel.height - 52, 90, 34},
                       nextLabel, nullptr, font, Color{90, 220, 140, 255}, sfx)) {
                if (tourPage < kTourCount - 1) { ++tourPage; sfx.play(sfx.traverse); }
                else closeTour();
            }
            g_modal = true;
            DrawTextEx(font, "ENTER/ESC", {panel.x + 28, panel.y + panel.height - 40}, 12, 1,
                       fade(Color{120, 140, 180, 255}, 1));
        }

        EndDrawing();
        SwapScreenBuffer();

        // frame pacing (~60fps); WaitTime also yields the CPU between frames
        const double remain = 1.0 / 60.0 - (GetTime() - frameT0);
        if (remain > 0.0) WaitTime(remain);
    }

    if (job.th.joinable()) job.th.join();
    if (font.texture.id) UnloadFont(font);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

int main() { return run(); }
