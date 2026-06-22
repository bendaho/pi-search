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
#include <unordered_map>
#include "sqlite3.h"

static std::string pi_digits;
static std::atomic<bool> running{true};
static sqlite3* db = nullptr;
static std::mutex db_mutex;

struct CacheEntry { int tc, fp; std::string json; };
static std::unordered_map<std::string, CacheEntry> mem_cache;
static std::mutex cache_mutex;
static std::string cert_template;

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
    std::string html = cert_template;

    auto now = std::time(nullptr);
    char datebuf[64];
    std::strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&now));

    char yearbuf[8];
    std::strftime(yearbuf, sizeof(yearbuf), "%Y", std::gmtime(&now));

    char posbuf[64];
    snprintf(posbuf, sizeof(posbuf), "%d", first_position + 1);

    char cntbuf[64];
    snprintf(cntbuf, sizeof(cntbuf), "%d", total_count);

    char msbuf[64];
    snprintf(msbuf, sizeof(msbuf), "%.0f", search_time_ms);

    auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    replace_all(html, "__QUERY__", query);
    replace_all(html, "__YEAR__", yearbuf);
    replace_all(html, "__TOTAL_COUNT__", cntbuf);
    replace_all(html, "__FIRST_POSITION__", posbuf);
    replace_all(html, "__GENERATED__", datebuf);
    replace_all(html, "__SEARCH_TIME__", msbuf);

    return html;
}

// ---- Database ----
static void db_init() {
    char* err = nullptr;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS searches ("
        "query TEXT PRIMARY KEY, total_count INTEGER NOT NULL,"
        "first_position INTEGER NOT NULL, results_json TEXT NOT NULL,"
        "created_at TEXT NOT NULL);",
        nullptr, nullptr, &err);
    if (err) { std::cerr << "DB: " << err << "\n"; sqlite3_free(err); }
}

static bool db_lookup(const std::string& q, int& tc, int& fp, std::string& rj) {
    std::lock_guard<std::mutex> lk(db_mutex);
    sqlite3_stmt* st;
    if (sqlite3_prepare_v2(db, "SELECT total_count, first_position, results_json FROM searches WHERE query=?;", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, q.c_str(), -1, SQLITE_STATIC);
    bool f = sqlite3_step(st) == SQLITE_ROW;
    if (f) { tc = sqlite3_column_int(st, 0); fp = sqlite3_column_int(st, 1);
        const char* r = (const char*)sqlite3_column_text(st, 2); if(r) rj = r; }
    sqlite3_finalize(st);
    return f;
}

static void db_store(const std::string& q, int tc, int fp, const std::string& rj) {
    std::lock_guard<std::mutex> lk(db_mutex);
    sqlite3_stmt* st;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO searches VALUES(?,?,?,?,datetime('now'));", -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, q.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, tc); sqlite3_bind_int(st, 3, fp);
    sqlite3_bind_text(st, 4, rj.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(st); sqlite3_finalize(st);
}

static void db_warm_cache() {
    std::lock_guard<std::mutex> lk(db_mutex);
    sqlite3_stmt* st;
    if (sqlite3_prepare_v2(db, "SELECT query, total_count, first_position, results_json FROM searches;", -1, &st, nullptr) != SQLITE_OK) return;
    int cnt = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* q = (const char*)sqlite3_column_text(st, 0);
        int tc = sqlite3_column_int(st, 1);
        int fp = sqlite3_column_int(st, 2);
        const char* rj = (const char*)sqlite3_column_text(st, 3);
        if (q && rj) { mem_cache[q] = {tc, fp, rj}; cnt++; }
    }
    sqlite3_finalize(st);
    std::cerr << "In-memory cache warmed: " << cnt << " entries\n";
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
    {
        std::lock_guard<std::mutex> lk(cache_mutex);
        auto it=mem_cache.find(q);
        if(it!=mem_cache.end()){
            auto t1=std::chrono::high_resolution_clock::now();
            double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
            std::ostringstream j;
            j << "{\"query\":\"" << je(q) << "\",\"total_count\":" << it->second.tc << ",\"first_position\":" << it->second.fp
              << ",\"search_time_ms\":" << std::fixed << j.precision(3) << ms
              << ",\"from_cache\":true"
              << ",\"pi_length\":" << pi_digits.size() << ",\"results\":" << it->second.json << "}";
            return j.str();
        }
    }
    int tc=0,fp=-1; std::string cached_rj; bool cached=db_lookup(q,tc,fp,cached_rj);
    auto t1=std::chrono::high_resolution_clock::now();
    double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
    if(cached && !cached_rj.empty()){
        {std::lock_guard<std::mutex> lk(cache_mutex); mem_cache[q]={tc,fp,cached_rj};}
        std::ostringstream j;
        j << "{\"query\":\"" << je(q) << "\",\"total_count\":" << tc << ",\"first_position\":" << fp
          << ",\"search_time_ms\":" << std::fixed << j.precision(3) << ms
          << ",\"from_cache\":true"
          << ",\"pi_length\":" << pi_digits.size() << ",\"results\":" << cached_rj << "}";
        return j.str();
    }
    auto si=search_pi(q); tc=si.tc; fp=si.fp;
    std::ostringstream rj; rj << "[";
    for(size_t i=0;i<si.res.size();i++){if(i)rj<<",";rj<<"{\"position\":"<<si.res[i].pos<<",\"context_before\":\""<<je(si.res[i].bef)<<"\",\"match\":\""<<je(si.res[i].match)<<"\",\"context_after\":\""<<je(si.res[i].aft)<<"\"}";}
    rj << "]"; std::string rjs=rj.str();
    db_store(q,tc,fp,rjs);
    {std::lock_guard<std::mutex> lk(cache_mutex); mem_cache[q]={tc,fp,rjs};}
    std::ostringstream j;
    j << "{\"query\":\"" << je(q) << "\",\"total_count\":" << tc << ",\"first_position\":" << fp
      << ",\"total_found\":" << si.res.size() << ",\"search_time_ms\":" << std::fixed;
    j.precision(3); j << ms
      << ",\"from_cache\":false"
      << ",\"pi_length\":" << pi_digits.size() << ",\"results\":" << rjs << "}";
    return j.str();
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
        else{auto t0=std::chrono::high_resolution_clock::now();int tc=0,fp=-1; std::string rj;
        if(!db_lookup(q,tc,fp,rj)){auto si=search_pi(q,0);tc=si.tc;fp=si.fp;std::ostringstream rjs;rjs<<"[";for(size_t i=0;i<si.res.size();i++){if(i)rjs<<",";rjs<<"{\"position\":"<<si.res[i].pos<<",\"context_before\":\""<<je(si.res[i].bef)<<"\",\"match\":\""<<je(si.res[i].match)<<"\",\"context_after\":\""<<je(si.res[i].aft)<<"\"}";}rjs<<"]";rj=rjs.str();db_store(q,tc,fp,rj);}
        auto t1=std::chrono::high_resolution_clock::now();double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
        std::string html=generate_certificate(q,tc,fp,ms);
        sendr(fd,200,"text/html; charset=utf-8",html);}}
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
    db_warm_cache();
    {std::ifstream tf("sert2.html");
    if(tf){std::ostringstream ss;ss<<tf.rdbuf();cert_template=ss.str();std::cerr<<"Cert template loaded: "<<cert_template.size()<<" bytes\n";}
    else{std::cerr<<"sert2.html not found, certificate endpoint disabled\n";}}
    int sfd=socket(AF_INET,SOCK_STREAM,0); int opt=1;
    setsockopt(sfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={AF_INET,htons(port),{INADDR_ANY}};
    if(bind(sfd,(struct sockaddr*)&addr,sizeof(addr))<0){perror("bind");return 1;}
    if(listen(sfd,128)<0){perror("listen");return 1;}
    std::cerr<<"Server on http://localhost:"<<port<<"\n";
    while(running){int c=accept(sfd,0,0);if(c>=0)handle_client(c);}
    close(sfd); sqlite3_close(db); return 0;
}
