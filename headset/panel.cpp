// frame-keyer-panel: SteamVR dashboard panel for the chroma keyer.
// On/off, key colour (presets or any hue), tolerance, edge softness, spill.
// Changes are applied through keyer.sh while you drag (a settings change only
// rewrites a small file, no shader reload), and kept in
// ~/.config/frame-keyer/panel.conf when you let go.
// The panel's design and parts of this code are based on the dashboard panel
// in steam-frame-chroma-key by spyjacob (MIT, see LICENSE).
#include "vkshare.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <set>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vr;

static const int W = 1024, H = 850;
static std::atomic<bool> g_quit{false};
static void onSignal(int) { g_quit = true; }

static std::string home(const char *rel) {
	const char *h = getenv("HOME");
	return std::string(h ? h : "/tmp") + rel;
}

// ---------------------------------------------------------------- settings
struct Preset {
	const char *name;
	float r, g, b;
};
static const Preset kPresets[] = {
	{"Magenta", 1, 0, 1},
	{"Green", 0, 1, 0},
	{"Blue (RM)", 0, 0x47 / 255.f, 0xbb / 255.f}, // Reality Mixer's default solid colour
};
static const int kCustom = 3;

struct Settings {
	bool on = false;
	int preset = 0;
	float hue = 300;              // hue bar position
	float cr = 1, cg = 0, cb = 1; // custom colour (from the hue bar or the eyedropper)
	float tol = 0.12f, soft = 0.08f, spill = 1.0f;
};

static void hueToRgb(float h, float &r, float &g, float &b) {
	auto f = [&](float n) {
		float k = std::fmod(n + h / 60.f, 6.f);
		return 1 - std::max(0.f, std::min({k, 4 - k, 1.f}));
	};
	r = f(5), g = f(3), b = f(1);
}
static void keyColour(const Settings &s, float &r, float &g, float &b) {
	if (s.preset < kCustom) r = kPresets[s.preset].r, g = kPresets[s.preset].g, b = kPresets[s.preset].b;
	else r = s.cr, g = s.cg, b = s.cb;
}

static std::string confPath() { return home("/.config/frame-keyer/panel.conf"); }
static Settings loadSettings() {
	Settings s;
	if (FILE *f = fopen(confPath().c_str(), "r")) {
		char k[32];
		float v;
		while (fscanf(f, " %31[a-z] = %f", k, &v) == 2) {
			if (!strcmp(k, "on")) s.on = v != 0;
			else if (!strcmp(k, "preset")) s.preset = std::clamp((int)v, 0, kCustom);
			else if (!strcmp(k, "hue")) s.hue = v;
			else if (!strcmp(k, "tolerance")) s.tol = v;
			else if (!strcmp(k, "softness")) s.soft = v;
			else if (!strcmp(k, "spill")) s.spill = v;
			else if (!strcmp(k, "customr")) s.cr = v;
			else if (!strcmp(k, "customg")) s.cg = v;
			else if (!strcmp(k, "customb")) s.cb = v;
		}
		fclose(f);
	}
	return s;
}
static time_t confMtime() {
	struct stat st;
	return stat(confPath().c_str(), &st) ? 0 : st.st_mtime;
}
static time_t g_confSeen; // mtime of the last panel.conf we wrote or read
static void saveSettings(const Settings &s) {
	std::string p = confPath();
	if (FILE *f = fopen((p + ".tmp").c_str(), "w")) {
		fprintf(f, "on = %d\npreset = %d\nhue = %.1f\ncustomr = %.4f\ncustomg = %.4f\ncustomb = %.4f\n"
			"tolerance = %.3f\nsoftness = %.3f\nspill = %.2f\n", s.on, s.preset, s.hue, s.cr, s.cg, s.cb, s.tol,
			s.soft, s.spill);
		fclose(f);
		rename((p + ".tmp").c_str(), p.c_str());
		g_confSeen = confMtime();
	}
}

// ---------------------------------------------------------------- applying (keyer.sh, off the UI thread)
static std::string g_keyer; // path of keyer.sh
static std::mutex g_applyMutex;
static bool g_applyPending = false;
static Settings g_applyWanted;
static std::atomic<bool> g_applying{false};
static std::atomic<int> g_applyFailed{0};
static int g_appliedOn = -1; // last state keyer.sh was run with (worker thread only)

static void applyWorker() {
	for (;;) {
		Settings s;
		{
			std::lock_guard<std::mutex> l(g_applyMutex);
			if (!g_applyPending) {
				g_applying = false;
				return;
			}
			s = g_applyWanted;
			g_applyPending = false;
		}
		// While off, a colour or slider change has nothing to apply (and `off`
		// reloads the shaders).
		if (!s.on && g_appliedOn == 0) continue;
		char cmd[1024];
		if (s.on) {
			float r, g, b;
			keyColour(s, r, g, b);
			snprintf(cmd, sizeof cmd, "'%s' chroma %.4f %.4f %.4f %.3f %.3f %.2f >/dev/null 2>&1", g_keyer.c_str(), r, g,
				b, s.tol, s.soft, s.spill);
		} else {
			snprintf(cmd, sizeof cmd, "'%s' off >/dev/null 2>&1", g_keyer.c_str());
		}
		int rc = system(cmd);
		g_applyFailed = rc != 0;
		g_appliedOn = rc == 0 ? s.on : -1;
		printf("apply: %s -> %d\n", cmd, rc);
		fflush(stdout);
	}
}
static void apply(const Settings &s, bool save = true) {
	if (save) saveSettings(s);
	std::lock_guard<std::mutex> l(g_applyMutex);
	g_applyWanted = s;
	g_applyPending = true;
	if (!g_applying.exchange(true)) std::thread(applyWorker).detach();
}

// ---------------------------------------------------------------- eyedropper
// SteamVR mirrors the headset view (level, centred, 1920x1080 RGB) to
// /dev/video99 while something reads it. Keying is paused while picking, so the
// mirror shows only the PC stream and overlays, never the cameras.
static std::atomic<int> g_pick{0}; // 0 idle, 1 sampling, 2 got a colour, 3 failed
static float g_pickRgb[3];
static void pickWorker() {
	FILE *p = popen("ffmpeg -loglevel error -f v4l2 -i /dev/video99 -frames:v 45 -vf crop=9:9 "
			"-f rawvideo -pix_fmt rgb24 - 2>/dev/null", "r");
	std::vector<uint8_t> all;
	if (p) {
		uint8_t buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, p)) > 0) all.insert(all.end(), buf, buf + n);
		pclose(p);
	}
	const size_t frame = 9 * 9 * 3;
	if (all.size() < frame) {
		g_pick = 3;
		return;
	}
	const uint8_t *f = all.data() + (all.size() / frame - 1) * frame; // last frame
	for (int c = 0; c < 3; c++) { // median of each channel over the patch
		std::vector<uint8_t> v;
		for (size_t i = 0; i < 81; i++) v.push_back(f[i * 3 + c]);
		std::nth_element(v.begin(), v.begin() + 40, v.end());
		g_pickRgb[c] = v[40] / 255.f;
	}
	bool placeholder = f[0] == 24 && f[1] == 28 && f[2] == 53; // the mirror's "not running" frame
	printf("eyedropper: %.3f %.3f %.3f%s\n", g_pickRgb[0], g_pickRgb[1], g_pickRgb[2],
		placeholder ? " (mirror placeholder)" : "");
	fflush(stdout);
	g_pick = placeholder ? 3 : 2;
}

static bool streaming() { // is a Steam Link (vrlink) stream running?
	DIR *d = opendir("/proc");
	if (!d) return false;
	bool found = false;
	while (dirent *e = readdir(d)) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
		char p[300], comm[32] = {};
		snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
		if (FILE *f = fopen(p, "r")) {
			if (fgets(comm, sizeof comm, f) && !strncmp(comm, "vrlink", 6)) found = true;
			fclose(f);
		}
		if (found) break;
	}
	closedir(d);
	return found;
}

// ---------------------------------------------------------------- LAN discovery
// PCs on the network ask "FPPQ1" by UDP broadcast to port 47911; we answer with
// who we are and the key settings, so the PC layer keys the same colour. The
// PC asks first, so Windows' firewall lets our answer through without rules.
// "toyou=1" when the asking PC is the one our Steam Link stream comes from.
static const int kLanPort = 47911;

// Remote IPv4 addresses of vrlink's connected sockets (the PC it streams from).
static std::set<uint32_t> vrlinkPeers() {
	std::set<uint64_t> inodes;
	DIR *d = opendir("/proc");
	if (!d) return {};
	while (dirent *e = readdir(d)) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
		std::string base = std::string("/proc/") + e->d_name;
		char comm[32] = {};
		if (FILE *f = fopen((base + "/comm").c_str(), "r")) {
			if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
			fclose(f);
		}
		if (strncmp(comm, "vrlink", 6)) continue;
		if (DIR *fd = opendir((base + "/fd").c_str())) {
			while (dirent *fe = readdir(fd)) {
				char link[64] = {};
				ssize_t n = readlink((base + "/fd/" + fe->d_name).c_str(), link, sizeof link - 1);
				unsigned long long ino;
				if (n > 0 && sscanf(link, "socket:[%llu]", &ino) == 1) inodes.insert(ino);
			}
			closedir(fd);
		}
	}
	closedir(d);
	std::set<uint32_t> peers;
	for (const char *table : {"/proc/net/udp", "/proc/net/tcp"}) {
		FILE *f = fopen(table, "r");
		if (!f) continue;
		char line[512];
		if (!fgets(line, sizeof line, f)) { fclose(f); continue; } // header
		while (fgets(line, sizeof line, f)) {
			unsigned rem, remPort;
			unsigned long long ino;
			// sl local_address rem_address st tx:rx tr:when retrnsmt uid timeout inode
			if (sscanf(line, " %*d: %*x:%*x %x:%x %*x %*x:%*x %*x:%*x %*x %*u %*u %llu", &rem, &remPort, &ino) == 3 &&
				rem && inodes.count(ino))
				peers.insert(rem); // network byte order, as in sockaddr_in
		}
		fclose(f);
	}
	return peers;
}

static std::string hostName() {
	char h[128] = {};
	gethostname(h, sizeof h - 1);
	return h;
}
static std::string machineId() {
	char id[64] = {};
	if (FILE *f = fopen("/etc/machine-id", "r")) {
		if (!fgets(id, sizeof id, f)) id[0] = 0;
		fclose(f);
	}
	std::string s = id;
	return s.substr(0, std::min<size_t>(12, s.find_first_of("\r\n")));
}

struct Lan {
	int fd = -1;
	std::set<uint32_t> peers;
	double peersAt = -10;
	bool open() {
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0) return false;
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = htons(kLanPort);
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(fd, (sockaddr *)&a, sizeof a) < 0) {
			perror("LAN discovery bind");
			close(fd);
			fd = -1;
			return false;
		}
		fcntl(fd, F_SETFL, O_NONBLOCK);
		return true;
	}
	bool showAsked = false; // "FPPSHOW" from this machine: a second start (app list) wants the panel shown

	void serve(const Settings &s, bool stream, double now) {
		if (fd < 0) return;
		char buf[64];
		sockaddr_in from;
		socklen_t fl = sizeof from;
		ssize_t n;
		while ((n = recvfrom(fd, buf, sizeof buf - 1, 0, (sockaddr *)&from, &fl)) > 0) {
			buf[n] = 0;
			if (!strcmp(buf, "FPPSHOW") && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) showAsked = true;
			if (strncmp(buf, "FPPQ1", 5)) continue;
			if (now - peersAt > 2) peers = vrlinkPeers(), peersAt = now;
			float r, g, b;
			keyColour(s, r, g, b);
			char reply[512];
			int len = snprintf(reply, sizeof reply,
				"FPPA1\nid=%s\nname=%s\non=%d\nkey=%.4f %.4f %.4f\ntolerance=%.3f\nsoftness=%.3f\nspill=%.2f\n"
				"streaming=%d\ntoyou=%d\n",
				machineId().c_str(), hostName().c_str(), s.on, r, g, b, s.tol, s.soft, s.spill, stream,
				stream ? (peers.count(from.sin_addr.s_addr) ? 1 : peers.empty() ? -1 : 0) : 0);
			sendto(fd, reply, len, 0, (sockaddr *)&from, fl);
			fl = sizeof from;
		}
	}
};

// ---------------------------------------------------------------- drawing
struct Rgb {
	uint8_t r, g, b;
};
static const Rgb kBg = {24, 26, 31}, kCard = {36, 39, 46}, kText = {235, 237, 242}, kDim = {150, 156, 168},
		 kAccent = {90, 160, 255}, kTrack = {62, 66, 76}, kOff = {80, 84, 94};

struct Canvas {
	int w, h;
	std::vector<uint8_t> px;
	FT_Library ft = nullptr;
	FT_Face regular = nullptr, bold = nullptr;

	Canvas(int w_, int h_) : w(w_), h(h_), px(w_ * h_ * 4) {}
	bool initFonts() {
		if (FT_Init_FreeType(&ft)) return false;
		FT_New_Face(ft, "/usr/share/fonts/noto/NotoSans-Regular.ttf", 0, &regular);
		FT_New_Face(ft, "/usr/share/fonts/noto/NotoSans-Bold.ttf", 0, &bold);
		if (!bold) bold = regular;
		return regular != nullptr;
	}
	void blend(int x, int y, Rgb c, float a) {
		if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
		uint8_t *p = &px[(y * w + x) * 4];
		a = std::min(a, 1.f);
		p[0] = (uint8_t)(p[0] + (c.r - p[0]) * a);
		p[1] = (uint8_t)(p[1] + (c.g - p[1]) * a);
		p[2] = (uint8_t)(p[2] + (c.b - p[2]) * a);
		p[3] = 255;
	}
	void clear(Rgb c) {
		for (int i = 0; i < w * h; i++) px[i * 4] = c.r, px[i * 4 + 1] = c.g, px[i * 4 + 2] = c.b, px[i * 4 + 3] = 255;
	}
	// Rounded rectangle with antialiased corners.
	void rect(float x0, float y0, float x1, float y1, Rgb c, float radius = 0) {
		for (int y = (int)y0; y < (int)std::ceil(y1); y++)
			for (int x = (int)x0; x < (int)std::ceil(x1); x++) {
				float cx = std::clamp(x + 0.5f, x0 + radius, x1 - radius);
				float cy = std::clamp(y + 0.5f, y0 + radius, y1 - radius);
				float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy) - radius;
				blend(x, y, c, std::clamp(0.5f - d, 0.f, 1.f));
			}
	}
	void outline(float x0, float y0, float x1, float y1, Rgb c, float radius, float t) {
		for (int y = (int)(y0 - 1); y < (int)std::ceil(y1 + 1); y++)
			for (int x = (int)(x0 - 1); x < (int)std::ceil(x1 + 1); x++) {
				float cx = std::clamp(x + 0.5f, x0 + radius, x1 - radius);
				float cy = std::clamp(y + 0.5f, y0 + radius, y1 - radius);
				float d = std::fabs(std::hypot(x + 0.5f - cx, y + 0.5f - cy) - radius);
				blend(x, y, c, std::clamp(t / 2 + 0.5f - d, 0.f, 1.f));
			}
	}
	void circle(float cx, float cy, float rad, Rgb c) { rect(cx - rad, cy - rad, cx + rad, cy + rad, c, rad); }
	// Text with its baseline at y; returns the advance width.
	float text(float x, float y, int size, const std::string &s, Rgb c, bool isBold = false, bool measure = false) {
		FT_Face f = isBold ? bold : regular;
		FT_Set_Pixel_Sizes(f, 0, size);
		float pen = x;
		for (unsigned char ch : s) {
			if (FT_Load_Char(f, ch, FT_LOAD_RENDER)) continue;
			FT_GlyphSlot g = f->glyph;
			if (!measure)
				for (unsigned row = 0; row < g->bitmap.rows; row++)
					for (unsigned col = 0; col < g->bitmap.width; col++)
						blend((int)pen + g->bitmap_left + col, (int)y - g->bitmap_top + row, c,
							g->bitmap.buffer[row * g->bitmap.pitch + col] / 255.f);
			pen += g->advance.x / 64.f;
		}
		return pen - x;
	}
	float textRight(float x, float y, int size, const std::string &s, Rgb c, bool isBold = false) {
		float wdt = text(0, 0, size, s, c, isBold, true);
		return text(x - wdt, y, size, s, c, isBold);
	}
};

// ---------------------------------------------------------------- layout
struct Box {
	float x0, y0, x1, y1;
	bool hit(float x, float y, float m = 10) const { return x >= x0 - m && x < x1 + m && y >= y0 - m && y < y1 + m; }
};
static const Box kToggle = {840, 34, 984, 98};
static Box swatch(int i) { return {48 + i * 236.f, 196, 48 + i * 236.f + 220, 276}; }
static const Box kHueBar = {48, 300, 736, 336};
static const Box kPick = {756, 294, 976, 342};
struct Slider {
	const char *label, *help, *fmt;
	float lo, hi;
	float Settings::*field;
	Box box;
};
static const Slider kSliders[] = {
	{"Tolerance", "How close to the key a colour must be to turn see-through. Higher = more shades.", "%.2f", 0.0f,
		0.40f, &Settings::tol, {48, 440, 976, 490}},
	{"Edge softness", "Fade between see-through and solid. Low = sharp edge, high = smooth edge.", "%.2f", 0.01f, 0.30f,
		&Settings::soft, {48, 560, 976, 610}},
	{"Spill removal", "Removes the key colour's tint left on edges next to see-through areas.", "%.0f%%", 0.0f, 1.0f,
		&Settings::spill, {48, 680, 976, 730}},
};
static const Box kQuit = {796, 772, 976, 820};

static void drawPanel(Canvas &cv, const Settings &s, bool stream, bool applying, bool failed, bool picking = false,
	const char *note = nullptr) {
	cv.clear(kBg);
	cv.text(48, 80, 40, "Frame Key", kText, true);
	// on/off switch
	cv.rect(kToggle.x0, kToggle.y0, kToggle.x1, kToggle.y1, s.on ? kAccent : kOff, 32);
	float kx = s.on ? kToggle.x1 - 32 : kToggle.x0 + 32;
	cv.circle(kx, (kToggle.y0 + kToggle.y1) / 2, 26, kText);
	cv.textRight(kToggle.x0 - 18, 78, 28, s.on ? "On" : "Off", s.on ? kText : kDim, true);

	std::string status = stream ? "PC stream connected" : "No PC stream";
	status += s.on ? "  -  keying" : "  -  off";
	if (applying) status += "  -  applying...";
	else if (failed) status += "  -  last change failed";
	if (note) status += std::string("  -  ") + note;
	cv.text(48, 130, 24, status, kDim);

	cv.text(48, 180, 26, "Key colour", kText, true);
	float r, g, b;
	keyColour(s, r, g, b);
	char hex[16];
	snprintf(hex, sizeof hex, "#%02X%02X%02X", (int)std::lround(r * 255), (int)std::lround(g * 255),
		(int)std::lround(b * 255));
	cv.textRight(976, 180, 24, std::string("Use ") + hex + " on the PC", kDim);
	for (int i = 0; i <= kCustom; i++) {
		Box bx = swatch(i);
		float pr, pg, pb;
		Settings t = s;
		t.preset = i;
		keyColour(t, pr, pg, pb);
		cv.rect(bx.x0, bx.y0, bx.x1, bx.y1, kCard, 14);
		cv.rect(bx.x0 + 12, bx.y0 + 14, bx.x0 + 64, bx.y1 - 14, {(uint8_t)(pr * 255), (uint8_t)(pg * 255),
			(uint8_t)(pb * 255)}, 10);
		cv.text(bx.x0 + 78, bx.y0 + 50, 24, i < kCustom ? kPresets[i].name : "Custom", kText);
		if (i == s.preset) cv.outline(bx.x0, bx.y0, bx.x1, bx.y1, kAccent, 14, 4);
	}
	// hue bar for the custom colour
	for (int x = (int)kHueBar.x0; x < (int)kHueBar.x1; x++) {
		float hr, hg, hb;
		hueToRgb(360.f * (x - kHueBar.x0) / (kHueBar.x1 - kHueBar.x0), hr, hg, hb);
		Rgb c = {(uint8_t)(hr * 255), (uint8_t)(hg * 255), (uint8_t)(hb * 255)};
		bool dim = s.preset != kCustom;
		if (dim) c = {(uint8_t)(c.r / 3 + 30), (uint8_t)(c.g / 3 + 30), (uint8_t)(c.b / 3 + 30)};
		for (int y = (int)kHueBar.y0; y < (int)kHueBar.y1; y++) cv.blend(x, y, c, 1);
	}
	float hr, hg, hb;
	hueToRgb(s.hue, hr, hg, hb);
	bool fromBar = std::fabs(hr - s.cr) + std::fabs(hg - s.cg) + std::fabs(hb - s.cb) < 0.01f;
	if (s.preset == kCustom && fromBar) {
		float hx = kHueBar.x0 + (kHueBar.x1 - kHueBar.x0) * s.hue / 360.f;
		cv.outline(hx - 8, kHueBar.y0 - 6, hx + 8, kHueBar.y1 + 6, kText, 4, 3);
	} else {
		cv.text(kHueBar.x0 + 12, kHueBar.y1 - 9, 20, "Drag for any hue (switches to Custom)", kText);
	}
	cv.rect(kPick.x0, kPick.y0, kPick.x1, kPick.y1, kCard, 12);
	cv.outline(kPick.x0, kPick.y0, kPick.x1, kPick.y1, kTrack, 12, 2);
	cv.text(kPick.x0 + 22, kPick.y1 - 15, 24, picking ? "Look at it..." : "Eyedropper", kText);

	for (const Slider &sl : kSliders) {
		float v = s.*sl.field;
		char val[32];
		snprintf(val, sizeof val, sl.fmt, strstr(sl.fmt, "%%") ? v * 100 : v);
		cv.text(sl.box.x0, sl.box.y0 - 34, 26, sl.label, kText, true);
		cv.textRight(sl.box.x1, sl.box.y0 - 34, 26, val, kText);
		cv.text(sl.box.x0, sl.box.y0 - 6, 20, sl.help, kDim);
		float cy = (sl.box.y0 + sl.box.y1) / 2 + 8;
		float t = (v - sl.lo) / (sl.hi - sl.lo);
		float x = sl.box.x0 + 14 + t * (sl.box.x1 - sl.box.x0 - 28);
		cv.rect(sl.box.x0 + 14, cy - 5, sl.box.x1 - 14, cy + 5, kTrack, 5);
		cv.rect(sl.box.x0 + 14, cy - 5, x, cy + 5, s.on ? kAccent : kOff, 5);
		cv.circle(x, cy, 16, kText);
	}

	cv.rect(48, 752, 976, 754, kTrack);
	cv.text(48, 804, 20, "Quit turns keying off and closes this panel. Open it again from the app list (+).", kDim);
	cv.rect(kQuit.x0, kQuit.y0, kQuit.x1, kQuit.y1, kCard, 12);
	cv.outline(kQuit.x0, kQuit.y0, kQuit.x1, kQuit.y1, kTrack, 12, 2);
	cv.text(kQuit.x0 + 62, kQuit.y1 - 15, 24, "Quit", kText);
}

static const int kRing = 160;
static void drawRing(Canvas &cv, int count) {
	for (auto &p : cv.px) p = 0; // transparent
	const float c = kRing / 2.f;
	for (int y = 0; y < kRing; y++)
		for (int x = 0; x < kRing; x++) {
			float d = std::hypot(x + 0.5f - c, y + 0.5f - c);
			float a = std::clamp(5.f - std::fabs(d - 48.f), 0.f, 1.f); // ring
			a = std::max(a, std::clamp(4.f - d, 0.f, 1.f));             // dot on the sampled spot
			if (a > 0) cv.blend(x, y, {255, 255, 255}, a);
		}
	if (count > 0) cv.text(c - 9, 30, 30, std::to_string(count), kText, true);
}

static void drawIcon(Canvas &cv) {
	cv.clear({0, 0, 0});
	for (auto &p : cv.px) p = 0; // transparent
	cv.circle(64, 64, 56, {255, 0, 255});
	cv.circle(64, 64, 30, {24, 26, 31});
	cv.circle(64, 64, 14, {235, 237, 242});
}

// ---------------------------------------------------------------- main
int main(int argc, char **argv) {
	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);
	char exe[512] = {};
	if (readlink("/proc/self/exe", exe, sizeof exe - 1) > 0) {
		std::string d = exe;
		d.resize(d.find_last_of('/')); // .../keyer/build
		g_keyer = d.substr(0, d.find_last_of('/')) + "/keyer.sh";
	}
	if (argc > 2 && !strcmp(argv[1], "--keyer")) g_keyer = argv[2];
	if (argc > 2 && !strcmp(argv[1], "--preview")) { // draw the panel to a PPM, no VR needed
		Canvas cv(W, H);
		if (!cv.initFonts()) return 1;
		Settings s = loadSettings();
		if (argc > 3) s.on = true, s.preset = atoi(argv[3]);
		drawPanel(cv, s, true, false, false);
		FILE *f = fopen(argv[2], "wb");
		if (!f) return 1;
		fprintf(f, "P6\n%d %d\n255\n", W, H);
		for (int i = 0; i < W * H; i++) fwrite(&cv.px[i * 4], 1, 3, f);
		fclose(f);
		return 0;
	}
	bool show = false; // --show: open the dashboard on this panel (the app list entry)
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--show")) show = true;

	// One panel at a time. Started again (app list), it asks the running one to show itself.
	const char *rt = getenv("XDG_RUNTIME_DIR");
	std::string lockPath = std::string(rt ? rt : "/tmp") + "/frame-keyer-panel.lock";
	int lockFd = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lockFd >= 0 && flock(lockFd, LOCK_EX | LOCK_NB)) {
		if (show) {
			int fd = socket(AF_INET, SOCK_DGRAM, 0);
			sockaddr_in to = {};
			to.sin_family = AF_INET;
			to.sin_port = htons(kLanPort);
			to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			sendto(fd, "FPPSHOW", 7, 0, (sockaddr *)&to, sizeof to);
			close(fd);
		}
		printf("panel already running\n");
		return 0;
	}

	EVRInitError e;
	VR_Init(&e, VRApplication_Overlay);
	if (e) {
		fprintf(stderr, "VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(e));
		return 1;
	}
	VkShare vk;
	VkTexture tex, iconTex, ringTex;
	Canvas cv(W, H), icon(128, 128), ring(kRing, kRing);
	if (!vk.init() || !vk.createTexture(W, H, tex) || !vk.createTexture(128, 128, iconTex) ||
		!vk.createTexture(kRing, kRing, ringTex) || !cv.initFonts() || !ring.initFonts()) {
		fprintf(stderr, "init failed (Vulkan or fonts)\n");
		VR_Shutdown();
		return 1;
	}
	IVROverlay *ov = VROverlay();
	VROverlayHandle_t main = k_ulOverlayHandleInvalid, thumb = k_ulOverlayHandleInvalid;
	if (EVROverlayError oe = ov->CreateDashboardOverlay("frame.keyer.panel", "Frame Key", &main, &thumb)) {
		fprintf(stderr, "CreateDashboardOverlay: %s\n", ov->GetOverlayErrorNameFromEnum(oe));
		VR_Shutdown();
		return 1;
	}
	ov->SetOverlayWidthInMeters(main, 2.0f);
	ov->SetOverlayInputMethod(main, VROverlayInputMethod_Mouse);
	HmdVector2_t scale = {(float)W, (float)H};
	ov->SetOverlayMouseScale(main, &scale);
	drawIcon(icon);
	vk.upload(iconTex, icon.px.data());
	vk.setOverlay(thumb, iconTex);
	// Eyedropper ring, 1 m in front of the eyes.
	VROverlayHandle_t picker = k_ulOverlayHandleInvalid;
	ov->CreateOverlay("frame.keyer.picker", "Eyedropper", &picker);
	ov->SetOverlayWidthInMeters(picker, 0.12f);
	HmdMatrix34_t ahead = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, -1.0f}}};
	ov->SetOverlayTransformTrackedDeviceRelative(picker, k_unTrackedDeviceIndex_Hmd, &ahead);

	Settings s = loadSettings();
	g_confSeen = confMtime();
	if (s.on) apply(s, false); // make the keyer match the saved switch (it may have been turned off elsewhere)
	bool stream = streaming();
	Lan lan;
	if (!lan.open()) fprintf(stderr, "LAN discovery off (port %d busy?)\n", kLanPort);
	bool dirty = true, wasApplying = false;
	int dragging = -1; // slider index, or 10 for the hue bar
	double nextStatus = 0;
	// Eyedropper: pause keying, close the dashboard, count down, sample the view centre.
	Settings beforePick;
	double pickAt = 0;
	int shownCount = -1;
	std::string note;
	auto now = [] {
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	};
	auto setFromX = [&](int which, float x) {
		if (which == 10) {
			s.hue = std::clamp(360.f * (x - kHueBar.x0) / (kHueBar.x1 - kHueBar.x0), 0.f, 359.f);
			hueToRgb(s.hue, s.cr, s.cg, s.cb);
			s.preset = kCustom;
		} else {
			const Slider &sl = kSliders[which];
			float t = std::clamp((x - sl.box.x0 - 14) / (sl.box.x1 - sl.box.x0 - 28), 0.f, 1.f);
			s.*sl.field = sl.lo + t * (sl.hi - sl.lo);
		}
		dirty = true;
	};

	auto startPick = [&]() {
		beforePick = s;
		if (s.on) {
			Settings off = s;
			off.on = false;
			apply(off, false);
		}
		std::thread([] {
			system("LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 timeout 5 /opt/steamvr/bin/linuxarm64/vrcmd "
			       "--hidedashboard >/dev/null 2>&1");
		}).detach();
		pickAt = now() + 3.5;
		shownCount = -1;
		g_pick = 0;
		note.clear();
		dirty = true;
	};
	auto endPick = [&]() {
		pickAt = 0;
		ov->HideOverlay(picker);
		ov->ShowDashboard("frame.keyer.panel");
		dirty = true;
	};

	bool quitting = false;
	if (show) ov->ShowDashboard("frame.keyer.panel");
	printf("panel up; keyer %s; %s\n", g_keyer.c_str(), s.on ? "on" : "off");
	fflush(stdout);
	while (!g_quit) {
		if (quitting && !g_applying) {
			printf("quit from the panel\n");
			fflush(stdout);
			break;
		}
		if (lan.showAsked) lan.showAsked = false, ov->ShowDashboard("frame.keyer.panel");
		VREvent_t ev;
		while (ov->PollNextOverlayEvent(main, &ev, sizeof ev)) {
			float x = ev.data.mouse.x, y = H - ev.data.mouse.y; // overlay mouse y runs bottom-up
			switch (ev.eventType) {
			case VREvent_MouseButtonDown:
				printf("click %.0f,%.0f\n", x, y);
				fflush(stdout);
				if (kToggle.hit(x, y)) {
					s.on = !s.on;
					apply(s);
					dirty = true;
				}
				for (int i = 0; i <= kCustom; i++)
					if (swatch(i).hit(x, y, 8)) {
						s.preset = i;
						apply(s);
						dirty = true;
					}
				if (kHueBar.hit(x, y, 4)) dragging = 10, setFromX(10, x);
				if (kPick.hit(x, y, 4) && !g_pick) startPick();
				for (int i = 0; i < 3; i++)
					if (kSliders[i].box.hit(x, y)) dragging = i, setFromX(i, x);
				if (kQuit.hit(x, y, 4) && !quitting) { // keying off first, then leave (the saved switch stays)
					Settings off = s;
					off.on = false;
					apply(off, false);
					quitting = true;
					note = "quitting...";
					dirty = true;
				}
				break;
			case VREvent_MouseMove:
				if (dragging >= 0) {
					setFromX(dragging, x);
					if (s.on) apply(s, false); // live; only the newest setting runs when keyer.sh is busy
				}
				break;
			case VREvent_MouseButtonUp:
				if (dragging >= 0) {
					dragging = -1;
					apply(s); // also saves
				}
				break;
			default:
				break;
			}
		}
		while (VRSystem()->PollNextEvent(&ev, sizeof ev))
			if (ev.eventType == VREvent_Quit) {
				VRSystem()->AcknowledgeQuit_Exiting();
				g_quit = true;
			}
		double t = now();
		if (t > nextStatus) {
			nextStatus = t + 1.0;
			// The PC app writes panel.conf and applies the change itself; just show it.
			time_t m = confMtime();
			if (m != g_confSeen && dragging < 0 && pickAt == 0) {
				g_confSeen = m;
				s = loadSettings();
				dirty = true;
				printf("settings changed on disk (PC app): %s\n", s.on ? "on" : "off");
				fflush(stdout);
			}
			bool st = streaming();
			if (st != stream) stream = st, dirty = true;
		}
		if (pickAt > 0) {
			int count = (int)std::ceil(pickAt - t);
			if (g_pick == 0 && count > 0 && count != shownCount) {
				shownCount = count;
				drawRing(ring, count);
				vk.upload(ringTex, ring.px.data());
				vk.setOverlay(picker, ringTex);
				ov->ShowOverlay(picker);
			} else if (g_pick == 0 && t >= pickAt) {
				ov->HideOverlay(picker); // ring and dot out of the picture; the last mirror frame is used
				g_pick = 1;
				std::thread(pickWorker).detach();
			} else if (g_pick == 2) {
				s = beforePick;
				s.preset = kCustom;
				s.cr = g_pickRgb[0], s.cg = g_pickRgb[1], s.cb = g_pickRgb[2];
				s.on = true;
				apply(s);
				char hex[32];
				snprintf(hex, sizeof hex, "picked #%02X%02X%02X", (int)std::lround(s.cr * 255),
					(int)std::lround(s.cg * 255), (int)std::lround(s.cb * 255));
				note = hex;
				g_pick = 0;
				endPick();
			} else if (g_pick == 3) {
				s = beforePick;
				if (s.on) apply(s, false);
				note = "eyedropper failed";
				g_pick = 0;
				endPick();
			}
		}
		bool applying = g_applying;
		if (applying != wasApplying) wasApplying = applying, dirty = true;
		if (dirty && ov->IsDashboardVisible()) {
			drawPanel(cv, s, stream, applying, g_applyFailed, pickAt > 0, note.empty() ? nullptr : note.c_str());
			vk.upload(tex, cv.px.data());
			vk.setOverlay(main, tex);
			dirty = false;
		}
		lan.serve(s, stream, t);
		std::this_thread::sleep_for(std::chrono::milliseconds(ov->IsDashboardVisible() ? 11 : 100));
	}
	// Hide and clear before destroying (see vkshare.h).
	ov->HideOverlay(main);
	ov->ClearOverlayTexture(main);
	ov->ClearOverlayTexture(thumb);
	ov->HideOverlay(picker);
	ov->ClearOverlayTexture(picker);
	ov->DestroyOverlay(picker);
	ov->DestroyOverlay(main);
	VR_Shutdown(); // before our Vulkan device goes: SteamVR's client frees its own objects on it
	vk.destroyTexture(tex);
	vk.destroyTexture(ringTex);
	vk.destroyTexture(iconTex);
	vk.shutdown();
	return 0;
}
