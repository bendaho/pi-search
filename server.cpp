#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <atomic>
#include <mutex>
#include "sqlite3.h"

static std::string pi_digits;
static std::atomic<bool> running{true};
static sqlite3* db = nullptr;
static std::mutex db_mutex;

void handle_sigint(int) { running = false; }

// ---- Helvetica width table (approximate, per char at font size 1.0) ----
static const float HELV_WIDTHS[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0.278,0.278,0.355,0.556,0.556,0.889,0.667,0.222,0.333,0.333,0.389,0.584,
    0.278,0.333,0.278,0.278,0.556,0.556,0.556,0.556,0.556,0.556,0.556,0.556,
    0.556,0.556,0.278,0.278,0.584,0.584,0.584,0.556,1.015,0.667,0.667,0.722,
    0.722,0.667,0.611,0.778,0.722,0.278,0.5,0.667,0.556,0.833,0.722,0.778,
    0.667,0.778,0.722,0.667,0.611,0.722,0.667,0.944,0.667,0.667,0.611,0.278,
    0.278,0.278,0.469,0.556,0.222,0.556,0.5,0.444,0.5,0.444,0.278,0.5,
    0.556,0.278,0.278,0.5,0.278,0.833,0.556,0.5,0.556,0.5,0.444,0.389,
    0.333,0.556,0.5,0.722,0.5,0.5,0.444,0.394,0.22,0.394,0.584,0.355,
    0,0.222,0,0.333,0.556,0,0.556,0,0.222,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static float text_width(const char* s, float size) {
    float w = 0;
    for (const char* p = s; *p; p++) w += HELV_WIDTHS[(unsigned char)*p];
    return w * size;
}

static std::string esc_pdf(const char* s) {
    std::string r;
    for (const char* p = s; *p; p++) {
        if (*p == '(' || *p == ')' || *p == '\\') r += '\\';
        r += *p;
    }
    return r;
}

// ---- PDF Generator ----
struct PDF {
    std::string stream;

    void fill_bg(int w, int h, float r, float g, float b) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%f %f %f rg 0 0 %d %d re f\n", r, g, b, w, h);
        stream += buf;
    }

    void rect(float x, float y, float w, float h, float r, float g, float b) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%f %f %f rg %f %f %f %f re f\n", r, g, b, x, y, w, h);
        stream += buf;
    }

    void stroke(float x, float y, float w, float h, float r, float g, float b, float lw) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%f %f %f RG %f w %f %f %f %f re S\n", r, g, b, lw, x, y, w, h);
        stream += buf;
    }

    void line(float x1, float y1, float x2, float y2, float r, float g, float b, float lw) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%f %f %f RG %f w %f %f m %f %f l S\n", r, g, b, lw, x1, y1, x2, y2);
        stream += buf;
    }

    void text_left(float x, float y, const char* font, float size, float r, float g, float b, const char* str) {
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "BT %f %f %f rg /%s %f Tf %f %f Td (%s) Tj ET\n",
            r, g, b, font, size, x, y, esc_pdf(str).c_str());
        stream += buf;
    }

    void text_center(float cx, float y, const char* font, float size, float r, float g, float b, const char* str) {
        float w = text_width(str, size);
        text_left(cx - w/2, y, font, size, r, g, b, str);
    }

    void text_right(float right_x, float y, const char* font, float size, float r, float g, float b, const char* str) {
        float w = text_width(str, size);
        text_left(right_x - w, y, font, size, r, g, b, str);
    }

    std::string render() {
        std::vector<std::string> objs;
        // 1: Catalog
        objs.push_back("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
        // 2: Pages
        objs.push_back("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
        // 3: Page
        char pb[512];
        snprintf(pb, sizeof(pb),
            "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 842 595] "
            "/Contents 4 0 R /Resources << /Font << /FB 5 0 R /FN 6 0 R /FI 7 0 R >> >> >>\nendobj\n");
        objs.push_back(pb);
        // 4: Content
        char cb[256];
        snprintf(cb, sizeof(cb), "4 0 obj\n<< /Length %zu >>\nstream\n", stream.size());
        objs.push_back(std::string(cb) + stream + "\nendstream\nendobj\n");
        // 5: Helvetica-Bold
        objs.push_back("5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>\nendobj\n");
        // 6: Helvetica
        objs.push_back("6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");
        // 7: Helvetica-Oblique
        objs.push_back("7 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Oblique >>\nendobj\n");

        std::string out = "%PDF-1.4\n";
        std::vector<int> offsets;
        for (auto& s : objs) { offsets.push_back(out.size()); out += s; }
        int xref = out.size();
        out += "xref\n0 " + std::to_string(objs.size()+1) + "\n";
        out += "0000000000 65535 f \n";
        for (int off : offsets) { char buf[32]; snprintf(buf, sizeof(buf), "%010d 00000 n \n", off); out += buf; }
        out += "trailer\n<< /Size " + std::to_string(objs.size()+1) + " /Root 1 0 R >>\n";
        char sx[64]; snprintf(sx, sizeof(sx), "startxref\n%d\n%%%%EOF\n", xref);
        out += sx;
        return out;
    }
};

static std::string generate_certificate(const std::string& query, int total_count, int first_position, double search_time_ms) {
    PDF p;
    const float W = 842, H = 595;
    p.fill_bg(W, H, 0.031, 0.047, 0.078);

    // ===== DECORATIVE BORDER =====
    // Triple-line border
    p.stroke(12, 12, W-24, H-24, 0.0, 0.85, 1.0, 1.2);
    p.stroke(18, 18, W-36, H-36, 0.0, 0.5, 0.65, 0.5);
    p.stroke(22, 22, W-44, H-44, 0.0, 0.3, 0.4, 0.3);

    // Corner ornaments (small L-shapes)
    float cl = 22;
    // TL
    p.rect(28, H-28-cl, cl, 2, 0.0, 1.0, 0.62);
    p.rect(28, H-28, 2, cl, 0.0, 1.0, 0.62);
    // TR
    p.rect(W-28-cl, H-28-cl, cl, 2, 0.0, 1.0, 0.62);
    p.rect(W-30, H-28, 2, cl, 0.0, 1.0, 0.62);
    // BL
    p.rect(28, 26, cl, 2, 0.0, 1.0, 0.62);
    p.rect(28, 28, 2, cl, 0.0, 1.0, 0.62);
    // BR
    p.rect(W-28-cl, 26, cl, 2, 0.0, 1.0, 0.62);
    p.rect(W-30, 28, 2, cl, 0.0, 1.0, 0.62);

    // ===== HEADER AREA (top 40% of page) =====
    // Separator line under header
    p.line(60, H-95, W-60, H-95, 0.0, 0.5, 0.65, 0.8);

    // Title: "CERTIFICATE OF PI DISCOVERY"
    p.text_center(W/2, H-62, "FB", 32, 1.0, 1.0, 1.0, "CERTIFICATE");
    p.text_center(W/2, H-82, "FB", 18, 0.0, 0.9, 1.0, "OF  PI  DISCOVERY");

    // Subtitle
    p.text_center(W/2, H-115, "FI", 11, 0.35, 0.45, 0.55, "1,000,000,000 Digits of Pi Analyzed");

    // ===== PI SYMBOL BOX =====
    float piBoxW = 120, piBoxH = 55;
    float piBoxX = W/2 - piBoxW/2;
    float piBoxY = H - 195;
    p.rect(piBoxX, piBoxY, piBoxW, piBoxH, 0.05, 0.08, 0.13);
    p.stroke(piBoxX, piBoxY, piBoxW, piBoxH, 0.0, 0.85, 1.0, 1.0);
    // Subtle inner glow
    p.stroke(piBoxX+3, piBoxY+3, piBoxW-6, piBoxH-6, 0.0, 0.3, 0.45, 0.4);
    p.text_center(W/2, piBoxY + 16, "FB", 26, 0.0, 1.0, 0.62, "PI");

    // ===== "SEARCHING FOR" =====
    p.text_center(W/2, H-222, "FN", 10, 0.3, 0.4, 0.5, "SEARCHING FOR");

    // ===== NUMBER BOX =====
    float numBoxW = 440, numBoxH = 52;
    float numBoxX = W/2 - numBoxW/2;
    float numBoxY = H - 290;

    // Outer glow
    p.rect(numBoxX-2, numBoxY-2, numBoxW+4, numBoxH+4, 0.0, 0.08, 0.04);
    // Box
    p.rect(numBoxX, numBoxY, numBoxW, numBoxH, 0.06, 0.09, 0.15);
    p.stroke(numBoxX, numBoxY, numBoxW, numBoxH, 0.0, 1.0, 0.62, 1.8);
    // Inner line top
    p.line(numBoxX+8, numBoxY+numBoxH-1, numBoxX+numBoxW-8, numBoxY+numBoxH-1, 0.0, 0.3, 0.2, 0.3);

    // Number text - centered in box
    char display[256];
    if (query.size() > 18) {
        snprintf(display, sizeof(display), "%.8s  ...  %s", query.c_str(), query.c_str() + query.size() - 6);
    } else {
        snprintf(display, sizeof(display), "%s", query.c_str());
    }
    p.text_center(W/2, numBoxY + 16, "FB", 26, 0.0, 1.0, 0.62, display);

    // ===== SEPARATOR =====
    p.line(80, H-315, W-80, H-315, 0.12, 0.18, 0.28, 0.8);

    // ===== STATS AREA =====
    if (total_count == 0) {
        // NOT FOUND
        p.text_center(W/2, H-360, "FB", 18, 1.0, 0.15, 0.15, "NOT FOUND IN PI");
        p.text_center(W/2, H-390, "FN", 11, 0.45, 0.5, 0.55,
            "This number does not appear in the first billion digits.");
        p.text_center(W/2, H-410, "FI", 10, 0.3, 0.35, 0.4,
            "Perhaps it exists deeper in infinity...");
    } else {
        // Two stat boxes side by side
        float boxW = 220, boxH = 90;
        float gap = 40;
        float leftX = W/2 - boxW - gap/2;
        float rightX = W/2 + gap/2;
        float boxY = H - 420;

        // --- Left box: Total Occurrences ---
        p.rect(leftX, boxY, boxW, boxH, 0.04, 0.07, 0.12);
        p.stroke(leftX, boxY, boxW, boxH, 0.0, 0.6, 0.8, 0.7);
        // Top accent line
        p.rect(leftX+1, boxY+boxH-3, boxW-2, 3, 0.0, 0.85, 1.0);

        p.text_center(leftX + boxW/2, boxY + boxH - 25, "FN", 8, 0.35, 0.45, 0.55, "OCCURRENCES");

        char cnt[64];
        snprintf(cnt, sizeof(cnt), "%d", total_count);
        p.text_center(leftX + boxW/2, boxY + 15, "FB", 34, 0.0, 1.0, 0.62, cnt);

        // --- Right box: First Position ---
        p.rect(rightX, boxY, boxW, boxH, 0.04, 0.07, 0.12);
        p.stroke(rightX, boxY, boxW, boxH, 0.0, 0.6, 0.8, 0.7);
        // Top accent line
        p.rect(rightX+1, boxY+boxH-3, boxW-2, 3, 0.0, 0.85, 1.0);

        p.text_center(rightX + boxW/2, boxY + boxH - 25, "FN", 8, 0.35, 0.45, 0.55, "FIRST POSITION");

        char pos[64];
        snprintf(pos, sizeof(pos), "#%d", first_position + 1);
        p.text_center(rightX + boxW/2, boxY + 15, "FB", 34, 0.0, 1.0, 0.62, pos);

        // Status badge
        float badgeW = 260, badgeH = 22;
        float badgeX = W/2 - badgeW/2;
        float badgeY = boxY - 35;
        p.rect(badgeX, badgeY, badgeW, badgeH, 0.0, 0.12, 0.06);
        p.stroke(badgeX, badgeY, badgeW, badgeH, 0.0, 0.8, 0.4, 0.8);
        p.text_center(W/2, badgeY + 5, "FB", 10, 0.0, 1.0, 0.62, "FOUND IN PI");
    }

    // ===== FOOTER =====
    p.line(60, 68, W-60, 68, 0.15, 0.2, 0.3, 0.6);

    auto now = std::time(nullptr);
    char datebuf[64];
    std::strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&now));

    char f1[256];
    snprintf(f1, sizeof(f1), "Generated: %s", datebuf);
    p.text_center(W/2, 50, "FN", 8, 0.3, 0.35, 0.4, f1);

    char f2[256];
    snprintf(f2, sizeof(f2), "Search: %d ms  |  C++ engine  |  MIT SIPB pi-billion.txt", (int)search_time_ms);
    p.text_center(W/2, 36, "FN", 7, 0.2, 0.25, 0.35, f2);

    // Small decorative dots at bottom center
    p.rect(W/2-8, 24, 4, 4, 0.0, 0.5, 0.65);
    p.rect(W/2-1, 24, 4, 4, 0.0, 0.85, 1.0);
    p.rect(W/2+6, 24, 4, 4, 0.0, 0.5, 0.65);

    return p.render();
}

// ---- Database ----
static void db_init() {
    char* err = nullptr;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS searches ("
        "query TEXT PRIMARY KEY, total_count INTEGER NOT NULL,"
        "first_position INTEGER NOT NULL, search_time_ms REAL NOT NULL,"
        "created_at TEXT NOT NULL);",
        nullptr, nullptr, &err);
    if (err) { std::cerr << "DB: " << err << "\n"; sqlite3_free(err); }
}

static bool db_lookup(const std::string& q, int& tc, int& fp) {
    std::lock_guard<std::mutex> lk(db_mutex);
    sqlite3_stmt* st;
    if (sqlite3_prepare_v2(db, "SELECT total_count, first_position FROM searches WHERE query=?;", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, q.c_str(), -1, SQLITE_STATIC);
    bool f = sqlite3_step(st) == SQLITE_ROW;
    if (f) { tc = sqlite3_column_int(st, 0); fp = sqlite3_column_int(st, 1); }
    sqlite3_finalize(st);
    return f;
}

static void db_store(const std::string& q, int tc, int fp, double ms) {
    std::lock_guard<std::mutex> lk(db_mutex);
    sqlite3_stmt* st;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO searches VALUES(?,?,?,?,datetime('now'));", -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, q.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, tc); sqlite3_bind_int(st, 3, fp); sqlite3_bind_double(st, 4, ms);
    sqlite3_step(st); sqlite3_finalize(st);
}

// ---- Search ----
struct SR { int pos; std::string bef, match, aft; };
struct SI { int tc, fp; std::vector<SR> res; };

static SI search_pi(const std::string& q, int mx = 10) {
    SI si{0, -1, {}};
    if (q.empty() || pi_digits.empty()) return si;
    size_t s = 0;
    while (true) {
        size_t f = pi_digits.find(q, s);
        if (f == std::string::npos) break;
        si.tc++;
        if (si.fp < 0) si.fp = (int)f;
        if ((int)si.res.size() < mx) {
            size_t ctx = 20;
            size_t a = f > ctx ? f - ctx : 0;
            size_t e = std::min(f + q.size() + ctx, pi_digits.size());
            SR r; r.pos = (int)f;
            r.bef = pi_digits.substr(a, f-a);
            r.match = pi_digits.substr(f, q.size());
            r.aft = pi_digits.substr(f+q.size(), e-f-q.size());
            si.res.push_back(r);
        }
        s = f + 1;
    }
    return si;
}

// ---- HTTP ----
static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary); if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
static std::string get_mime(const std::string& p) {
    if (p.find(".html")!=std::string::npos) return "text/html";
    if (p.find(".css")!=std::string::npos) return "text/css";
    if (p.find(".js")!=std::string::npos) return "application/javascript";
    return "application/octet-stream";
}
static std::string urldec(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i=0;i<s.size();++i) {
        if (s[i]=='%'&&i+2<s.size()){int v;std::istringstream is(s.substr(i+1,2));
        if(is>>std::hex>>v){r+=(char)v;i+=2;}else r+=s[i];}
        else if(s[i]=='+')r+=' ';else r+=s[i];
    } return r;
}
static std::string qs(const std::string& r, const std::string& k) {
    size_t q=r.find('?'); if(q==std::string::npos)return""; std::string qs=r.substr(q+1);
    size_t kp=qs.find(k+"="); if(kp==std::string::npos)return"";
    size_t s=kp+k.size()+1, e=qs.find('&',s); if(e==std::string::npos)e=qs.size();
    return urldec(qs.substr(s,e-s));
}
static std::string jstr(const std::string& b, const std::string& k) {
    std::string s="\""+k+"\""; size_t p=b.find(s); if(p==std::string::npos)return"";
    p=b.find(':',p); if(p==std::string::npos)return""; p++;
    while(p<b.size()&&b[p]==' ')p++; if(p>=b.size()||b[p]!='"')return""; p++;
    size_t e=p; while(e<b.size()){if(b[e]=='\\'){e+=2;continue;}if(b[e]=='"')break;e++;}
    return urldec(b.substr(p,e-p));
}
static std::string je(const std::string& s) {
    std::string r; for(char c:s){switch(c){case'"':r+="\\\"";break;case'\\':r+="\\\\";break;case'\n':r+="\\n";break;default:r+=c;}} return r;
}

static std::string search_json(const std::string& q) {
    auto t0=std::chrono::high_resolution_clock::now();
    int tc=0,fp=-1; bool cached=db_lookup(q,tc,fp);
    std::vector<SR> res;
    if(!cached){auto si=search_pi(q);tc=si.tc;fp=si.fp;res=si.res;db_store(q,tc,fp,0);}
    else if(tc>0&&fp>=0){size_t ctx=20,f=fp,a=f>ctx?f-ctx:0,e=std::min(f+q.size()+ctx,pi_digits.size());
        SR r;r.pos=fp;r.bef=pi_digits.substr(a,f-a);r.match=pi_digits.substr(f,q.size());r.aft=pi_digits.substr(f+q.size(),e-f-q.size());res.push_back(r);}
    auto t1=std::chrono::high_resolution_clock::now();
    double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
    std::ostringstream j;
    j << "{\"query\":\"" << je(q) << "\",\"total_count\":" << tc << ",\"first_position\":" << fp
      << ",\"total_found\":" << res.size() << ",\"search_time_ms\":" << std::fixed;
    j.precision(3); j << ms
      << ",\"from_cache\":" << (cached?"true":"false")
      << ",\"pi_length\":" << pi_digits.size() << ",\"results\":[";
    for (size_t i = 0; i < res.size(); i++) {
        if (i) j << ",";
        j << "{\"position\":" << res[i].pos << ",\"context_before\":\"" << je(res[i].bef)
          << "\",\"match\":\"" << je(res[i].match) << "\",\"context_after\":\"" << je(res[i].aft) << "\"}";
    }
    j << "]}"; return j.str();
}

static void sendr(int fd, int st, const std::string& ct, const std::string& b, const std::string& ex="") {
    std::ostringstream r; const char* sn="OK"; if(st==404)sn="Not Found";else if(st==400)sn="Bad Request";else if(st==503)sn="Service Unavailable";
    r<<"HTTP/1.1 "<<st<<" "<<sn<<"\r\nContent-Type: "<<ct<<"\r\nContent-Length: "<<b.size()
     <<"\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n";
    if(!ex.empty())r<<ex; r<<"\r\n"<<b;
    std::string rs=r.str(); send(fd,rs.c_str(),rs.size(),MSG_NOSIGNAL);
}

static std::string rmethod(const std::string& r){return r.substr(0,r.find(' '));}
static std::string rpath(const std::string& r){size_t s=r.find(' ')+1,e=r.find(' ',s);if(e==std::string::npos)e=r.find('\r',s);if(e==std::string::npos)e=r.size();return r.substr(s,e-s);}

static void handle_client(int fd) {
    char buf[8192]; int n=recv(fd,buf,sizeof(buf)-1,0); if(n<=0){close(fd);return;}
    buf[n]=0; std::string req(buf,n); std::string m=rmethod(req),p=rpath(req);
    if(pi_digits.empty()){sendr(fd,503,"application/json","{\"error\":\"Loading...\"}");close(fd);return;}
    if(m=="OPTIONS"){sendr(fd,200,"text/plain","");}
    else if(p=="/"||p=="/index.html"){auto h=read_file("static/index.html");sendr(fd,200,"text/html; charset=utf-8",h.empty()?"404":h);}
    else if(p.find("/api/search")==0){sendr(fd,200,"application/json",search_json(qs(p,"q")));}
    else if(p.find("/api/certificate")==0){std::string q=qs(p,"q");
        if(q.empty()){sendr(fd,400,"application/json","{\"error\":\"Missing q\"}");}
        else{auto t0=std::chrono::high_resolution_clock::now();int tc=0,fp=-1;
        if(!db_lookup(q,tc,fp)){auto si=search_pi(q,0);tc=si.tc;fp=si.fp;db_store(q,tc,fp,0);}
        auto t1=std::chrono::high_resolution_clock::now();double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
        std::string pdf=generate_certificate(q,tc,fp,ms);
        std::string hdr="Content-Disposition: attachment; filename=\"pi-cert-"+je(q)+".pdf\"\r\n";
        sendr(fd,200,"application/pdf",pdf,hdr);}}
    else if(p=="/api/stats"){std::ostringstream j;j << "{\"pi_digits_loaded\":" << pi_digits.size() << ",\"status\":\"ok\"}";sendr(fd,200,"application/json",j.str());}
    else{auto c=read_file("static"+p);sendr(fd,c.empty()?404:200,get_mime(p),c.empty()?"404":c);}
    close(fd);
}

int main(int argc, char* argv[]) {
    signal(SIGINT,handle_sigint); signal(SIGPIPE,SIG_IGN);
    int port=argc>1?std::atoi(argv[1]):8080;
    if(sqlite3_open("pi_cache.db",&db)!=SQLITE_OK){std::cerr<<"DB fail\n";return 1;}
    sqlite3_exec(db,"PRAGMA journal_mode=WAL;",nullptr,nullptr,nullptr); db_init();
    std::cerr<<"DB ready\nLoading pi-billion.txt...\n";
    auto t0=std::chrono::high_resolution_clock::now();
    std::ifstream file("pi-billion.txt",std::ios::binary); if(!file){std::cerr<<"pi-billion.txt not found!\n";return 1;}
    file.seekg(0,std::ios::end); pi_digits.reserve(file.tellg()); file.seekg(0,std::ios::beg);
    std::string ln; while(std::getline(file,ln)) for(char c:ln) if(c>='0'&&c<='9') pi_digits+=c;
    file.close();
    auto t1=std::chrono::high_resolution_clock::now();
    std::cerr<<"Loaded "<<pi_digits.size()<<" digits in "<<std::chrono::duration<double,std::milli>(t1-t0).count()<<" ms\n";
    int sfd=socket(AF_INET,SOCK_STREAM,0); int opt=1;
    setsockopt(sfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={AF_INET,htons(port),{INADDR_ANY}};
    if(bind(sfd,(struct sockaddr*)&addr,sizeof(addr))<0){perror("bind");return 1;}
    if(listen(sfd,128)<0){perror("listen");return 1;}
    std::cerr<<"Server on http://localhost:"<<port<<"\n";
    while(running){int c=accept(sfd,0,0);if(c>=0)handle_client(c);}
    close(sfd); sqlite3_close(db); return 0;
}
