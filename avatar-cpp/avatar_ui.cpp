// Lite avatar dashboard: GStreamer (NVDEC hardware) plays the avatar video; a cairo
// overlay draws the cyan ring + live telemetry. One process, no Qt. Loops the clip.
//   build: g++ -O2 avatar_ui.cpp -o avatar_ui $(pkg-config --cflags --libs gstreamer-1.0 cairo)
//   run  : WAYLAND_DISPLAY=wayland-av QT_QPA... not needed; needs XDG_RUNTIME_DIR + weston
#include <gst/gst.h>
#include <cairo.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct Tele {
    std::atomic<double> cpu{0}, gpu{0}, mem{0}, swap{0}, temp{0};
    char memText[32] = "", swapText[32] = "";
    std::string name = "speaker";
};
static Tele g_t;
static GstElement *g_pipe = nullptr;
static GMainLoop *g_loop = nullptr;
static bool g_shot = false;

// Live captions fed by the voice orchestrator over its Unix socket.
struct Subs {
    std::mutex m;
    std::string state = "idle", transcript, reply;
};
static Subs g_subs;

// Extract a string field from a flat JSON line ({"type":"x","value":"y"}), unescaping.
static std::string jget(const std::string &js, const char *key) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = js.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    std::string out;
    for (size_t i = p; i < js.size(); ++i) {
        char c = js[i];
        if (c == '\\' && i + 1 < js.size()) {
            char n = js[++i]; out += (n == 'n' ? '\n' : n == 't' ? ' ' : n); continue;
        }
        if (c == '"') break;
        out += c;
    }
    return out;
}

// Connect to $XDG_RUNTIME_DIR/avatar-voice.sock and track state/transcript/reply.
static void sock_thread() {
    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(xdg && *xdg ? xdg : "/run/user/1000") + "/avatar-voice.sock";
    while (true) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{}; addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (fd >= 0 && connect(fd, (sockaddr *)&addr, sizeof(addr)) == 0) {
            std::string buf; char tmp[2048]; ssize_t n;
            while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
                buf.append(tmp, n);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl); buf.erase(0, nl + 1);
                    std::string type = jget(line, "type"), val = jget(line, "value");
                    std::lock_guard<std::mutex> lk(g_subs.m);
                    if (type == "state") g_subs.state = val;
                    else if (type == "transcript") g_subs.transcript = val;
                    else if (type == "reply") g_subs.reply = val;
                }
            }
        }
        if (fd >= 0) close(fd);
        std::this_thread::sleep_for(std::chrono::seconds(2));   // wait + reconnect
    }
}

static std::string slurp(const char *p) {
    std::ifstream f(p);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static void tele_thread() {
    unsigned long long lastIdle = 0, lastTotal = 0;
    while (true) {
        {   std::ifstream f("/proc/stat");
            std::string c; unsigned long long u = 0, n = 0, s = 0, i = 0, io = 0, ir = 0, so = 0, st = 0;
            f >> c >> u >> n >> s >> i >> io >> ir >> so >> st;
            unsigned long long idle = i + io, total = u + n + s + idle + ir + so + st;
            if (lastTotal && total > lastTotal) {
                double dt = total - lastTotal, di = idle - lastIdle;
                g_t.cpu = 100.0 * (dt - di) / dt;
            }
            lastIdle = idle; lastTotal = total; }
        try { g_t.gpu = std::stod(slurp("/sys/devices/platform/17000000.gpu/load")) / 10.0; } catch (...) {}
        {   std::ifstream f("/proc/meminfo"); std::string k, unit; long v;
            double mt = 0, ma = 0, swt = 0, swf = 0;
            while (f >> k >> v >> unit) {
                if (k == "MemTotal:") mt = v; else if (k == "MemAvailable:") ma = v;
                else if (k == "SwapTotal:") swt = v; else if (k == "SwapFree:") swf = v;
            }
            if (mt) { g_t.mem = 100.0 * (mt - ma) / mt;
                snprintf(g_t.memText, 32, "%.1f/%.1fG", (mt - ma) / 1048576.0, mt / 1048576.0); }
            if (swt) { g_t.swap = 100.0 * (swt - swf) / swt;
                snprintf(g_t.swapText, 32, "%.1f/%.1fG", (swt - swf) / 1048576.0, swt / 1048576.0); } }
        try { g_t.temp = std::stod(slurp("/sys/class/thermal/thermal_zone8/temp")) / 1000.0; } catch (...) {}
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

static void txt(cairo_t *cr, double x, double y, const char *s, double sz,
                double r, double g, double b, bool bold = false) {
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, s);
}

static void metric(cairo_t *cr, double x, double y, const char *label,
                   double pct, const char *detail) {
    txt(cr, x, y, label, 18, 0, 0.9, 1, true);
    txt(cr, x + 165 - 70, y, detail, 15, 1, 1, 1);
    cairo_set_source_rgb(cr, 0.05, 0.15, 0.19);
    cairo_rectangle(cr, x, y + 9, 165, 7); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0, 0.9, 0.78);
    cairo_rectangle(cr, x, y + 9, 165 * std::min(100.0, pct) / 100.0, 7); cairo_fill(cr);
}

// Word-wrap + centered draw; returns the y after the last line.
static double drawWrap(cairo_t *cr, const std::string &s, double cx, double y,
                       double maxW, double sz, double r, double g, double b, int maxLines) {
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    std::stringstream ss(s); std::string w; std::vector<std::string> lines, words;
    while (ss >> w) words.push_back(w);
    std::string line; cairo_text_extents_t ext;
    for (auto &wd : words) {
        std::string trial = line.empty() ? wd : line + " " + wd;
        cairo_text_extents(cr, trial.c_str(), &ext);
        if (ext.width > maxW && !line.empty()) { lines.push_back(line); line = wd; }
        else line = trial;
    }
    if (!line.empty()) lines.push_back(line);
    cairo_set_source_rgb(cr, r, g, b);
    for (int i = 0; i < (int)lines.size() && i < maxLines; ++i) {
        std::string ln = lines[i];
        if (i == maxLines - 1 && (int)lines.size() > maxLines) ln += " ...";
        cairo_text_extents(cr, ln.c_str(), &ext);
        cairo_move_to(cr, cx - ext.width / 2, y);
        cairo_show_text(cr, ln.c_str());
        y += sz * 1.28;
    }
    return y;
}

static void draw_captions(cairo_t *cr) {
    std::string st, tr, rp;
    { std::lock_guard<std::mutex> lk(g_subs.m); st = g_subs.state; tr = g_subs.transcript; rp = g_subs.reply; }

    // state badge: colour by phase
    double sr = 0.6, sg = 0.6, sb = 0.6; std::string label = "IDLE";
    if (st == "listening") { sr = 0; sg = 0.85; sb = 1;   label = "LISTENING"; }
    else if (st == "thinking") { sr = 1; sg = 0.8; sb = 0.1; label = "THINKING"; }
    else if (st == "speaking") { sr = 0.2; sg = 1; sb = 0.4; label = "SPEAKING"; }

    // translucent caption panel near the bottom (kept inside the round bezel)
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_rectangle(cr, 230, 778, 620, 250);
    cairo_fill(cr);

    cairo_text_extents_t ext;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 20);
    cairo_set_source_rgb(cr, sr, sg, sb);
    cairo_text_extents(cr, label.c_str(), &ext);
    cairo_move_to(cr, 540 - ext.width / 2, 812);
    cairo_show_text(cr, label.c_str());

    double y = 852;
    if (!tr.empty())
        y = drawWrap(cr, "You: " + tr, 540, y, 580, 22, 0.85, 0.92, 1, 2) + 8;
    if (!rp.empty())
        drawWrap(cr, g_t.name + ": " + rp, 540, y, 580, 24, 0.2, 1, 0.6, 3);
}

static void on_draw(GstElement *, cairo_t *cr, guint64, guint64, gpointer) {
    // cyan ring around the round bezel
    cairo_set_line_width(cr, 10);
    cairo_set_source_rgb(cr, 0, 0.9, 1);
    cairo_arc(cr, 540, 540, 525, 0, 2 * M_PI);
    cairo_stroke(cr);

    // telemetry, left side
    double x = 70, y = 420; char b[48];
    txt(cr, x, y - 26, "SYSTEM", 17, 0, 0.9, 1, true);
    snprintf(b, 48, "%.0f%% %.0f\xC2\xB0", g_t.cpu.load(), 0.0);
    snprintf(b, 48, "%.0f%%", g_t.cpu.load());   metric(cr, x, y,      "CPU",  g_t.cpu,  b);
    snprintf(b, 48, "%.0f%%", g_t.gpu.load());   metric(cr, x, y + 44, "GPU",  g_t.gpu,  b);
    metric(cr, x, y + 88,  "MEM",  g_t.mem,  g_t.memText);
    metric(cr, x, y + 132, "SWAP", g_t.swap, g_t.swapText);
    snprintf(b, 48, "%.0f\xC2\xB0""C", g_t.temp.load()); metric(cr, x, y + 176, "TEMP", g_t.temp, b);

    // live captions: state + STT transcript + LLM/TTS reply
    draw_captions(cr);
}

static gboolean on_bus(GstBus *, GstMessage *msg, gpointer) {
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
        if (g_shot) g_main_loop_quit(g_loop);
        else gst_element_seek_simple(g_pipe, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0);
    }
    return TRUE;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    std::thread(tele_thread).detach();
    std::thread(sock_thread).detach();   // live captions from the voice orchestrator
    const char *video = argc > 1 ? argv[1] : "/home/jetson/avatar-studio/media/idle.mp4";
    g_shot = (argc > 2 && std::string(argv[2]) == "shot");
    if (g_shot) {   // let telemetry + captions populate (AVATAR_SHOT_DELAY secs)
        const char *sd = std::getenv("AVATAR_SHOT_DELAY");
        std::this_thread::sleep_for(std::chrono::milliseconds(sd ? (long)(atof(sd) * 1000) : 1500));
    }
    std::string sink = g_shot
        ? "videoconvert ! pngenc snapshot=true ! filesink location=/tmp/shot.png"
        : "waylandsink sync=true";
    // NVDEC -> RGBA (native 1920x1080) -> center-crop to 1080x1080 (no squish) -> BGRA -> overlay
    std::string desc =
        std::string("filesrc location=\"") + video + "\" ! qtdemux ! h264parse ! "
        "nvv4l2decoder ! nvvidconv ! video/x-raw,format=RGBA ! "
        "videocrop left=420 right=420 ! "
        "videoconvert ! video/x-raw,format=BGRA ! cairooverlay name=ov ! " + sink;
    GError *err = nullptr;
    g_pipe = gst_parse_launch(desc.c_str(), &err);
    if (!g_pipe) { fprintf(stderr, "pipeline: %s\n", err ? err->message : "?"); return 1; }
    GstElement *ov = gst_bin_get_by_name(GST_BIN(g_pipe), "ov");
    g_signal_connect(ov, "draw", G_CALLBACK(on_draw), nullptr);
    GstBus *bus = gst_element_get_bus(g_pipe);
    gst_bus_add_watch(bus, on_bus, nullptr);
    gst_element_set_state(g_pipe, GST_STATE_PLAYING);
    g_loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(g_loop);
    return 0;
}
