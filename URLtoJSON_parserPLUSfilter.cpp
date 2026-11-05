#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cctype>
#include <curl/curl.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace std;
namespace fs = std::filesystem;

// Paths are built from UTF-8 strings (category names may be Cyrillic)
static fs::path u8path(const string &s)
{
    return fs::path(u8string(s.begin(), s.end()));
}

static inline string trim(const string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static string to_lower(string s)
{
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Very small CSV line parser: returns columns (handles simple quoted values)
vector<string> parse_csv_line(const string &line, char delim)
{
    vector<string> cols;
    string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                cur.push_back(c);
            }
        } else {
            if (c == '"') {
                in_quotes = true;
            } else if (c == delim) {
                cols.push_back(trim(cur));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
    }
    cols.push_back(trim(cur));
    return cols;
}

// Excel in a Russian locale saves CSV with ';', other tools use ','.
// Pick whichever occurs more often (outside quotes) in the first line.
static char detect_delimiter(const string &line)
{
    size_t commas = 0, semis = 0;
    bool in_quotes = false;
    for (char c : line) {
        if (c == '"') in_quotes = !in_quotes;
        else if (!in_quotes && c == ',') ++commas;
        else if (!in_quotes && c == ';') ++semis;
    }
    return semis > commas ? ';' : ',';
}

// libcurl write callback
static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    string *s = reinterpret_cast<string*>(userp);
    s->append(reinterpret_cast<char*>(contents), realsize);
    return realsize;
}

struct FetchResult
{
    bool ok = false;        // transport-level success (DNS, connect, TLS, timeout)
    long http_code = 0;     // final HTTP status after redirects
    string final_url;       // URL after redirects (differs from the requested one on redirect)
    string body;
};

FetchResult fetch_url(const string &url)
{
    FetchResult r;
    CURL *curl = curl_easy_init();
    if (!curl) return r;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // enable all supported encodings
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; URLtoJSONParser/1.0)");
    // small timeout
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        cerr << "curl error: " << curl_easy_strerror(res) << " for " << url << endl;
        r.body.clear();
    } else {
        r.ok = true;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.http_code);
        char *eff = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff)
            r.final_url = eff;
    }
    curl_easy_cleanup(curl);
    return r;
}

// Extract all <script type="application/ld+json"> ... </script> blocks.
// Plain string search instead of std::regex: std::regex with [\s\S]*? is slow on
// big pages and can overflow the stack on MSVC.
vector<string> extract_ld_json(const string &html)
{
    vector<string> results;
    const string low = to_lower(html);
    size_t pos = 0;
    while ((pos = low.find("<script", pos)) != string::npos) {
        size_t tag_end = low.find('>', pos);
        if (tag_end == string::npos) break;
        const string tag = low.substr(pos, tag_end - pos);
        size_t close = low.find("</script", tag_end + 1);
        if (close == string::npos) break;
        if (tag.find("ld+json") != string::npos)
            results.push_back(html.substr(tag_end + 1, close - tag_end - 1));
        pos = close + 8;
    }
    return results;
}

static string sanitize_component(const string &s)
{
    string out;
    for (char ch : s) {
        // keep alnum, UTF-8 bytes (non-Latin letters) and a few safe chars, replace others with '_'
        unsigned char u = (unsigned char)ch;
        if (u >= 0x80 || isalnum(u) || ch == '-' || ch == '_' || ch == '.') out.push_back(ch);
        else out.push_back('_');
    }
    return out;
}

static string csv_quote(const string &s)
{
    string r = "\"";
    for (char ch : s) {
        if (ch == '"') r += "\"\"";
        else r.push_back(ch);
    }
    r += "\"";
    return r;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); // categories/URLs are UTF-8
#endif
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " input.csv [url_col] [category_col] [output_dir]" << endl;
        cerr << "Columns: 1-based indices. Defaults: category=1 url=2. CSV may have header." << endl;
        return 1;
    }

    string input_csv = argv[1];
    // parse optional numeric column args (1-based)
    int url_col_1 = 2; // default URL column (1-based)
    int cat_col_1 = 1; // default category column (1-based)
    string out_dir = "output";
    if (argc >= 3) {
        // try parse argv[2] as integer; if not integer, assume it's output dir
        try {
            url_col_1 = stoi(argv[2]);
        } catch (...) {
            url_col_1 = 2;
            out_dir = argv[2];
        }
    }
    if (argc >= 4) {
        try {
            cat_col_1 = stoi(argv[3]);
        } catch (...) {
            cat_col_1 = 1;
            out_dir = argv[3];
        }
    }
    if (argc >= 5) {
        out_dir = argv[4];
    }
    // convert to 0-based
    int url_col = max(0, url_col_1 - 1);
    int cat_col = max(0, cat_col_1 - 1);

    ifstream in(input_csv);
    if (!in) {
        cerr << "Failed to open CSV file: " << input_csv << endl;
        return 2;
    }
    fs::create_directories(out_dir);

    // initialize curl global once
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Manifest maps every input row to its result. It is rewritten on each run,
    // so repeated runs into the same folder don't accumulate duplicate rows.
    // It uses the same delimiter as the input CSV (detected from the first line).
    string first_line;
    while (std::getline(in, first_line) && trim(first_line).empty()) {}
    if (first_line.rfind("\xEF\xBB\xBF", 0) == 0) first_line.erase(0, 3); // UTF-8 BOM
    const char delim = detect_delimiter(first_line);
    in.clear();
    in.seekg(0);

    string manifest_path = out_dir + "/manifest.csv";
    ofstream manifest(u8path(manifest_path), ios::trunc);
    if (!manifest) {
        cerr << "Warning: failed to open manifest file: " << manifest_path << endl;
    } else {
        manifest << "\xEF\xBB\xBF"; // BOM: lets Excel open the UTF-8 manifest correctly
        manifest << "category" << delim << "url" << delim << "http_code" << delim
                 << "final_url" << delim << "filename\n";
    }
    auto log_row = [&](const string &tag, const string &url, long code,
                       const string &final_url, const string &filename) {
        if (manifest)
            manifest << csv_quote(tag) << delim << csv_quote(url) << delim << code << delim
                     << csv_quote(final_url) << delim << csv_quote(filename) << '\n';
    };

    string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line_no == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        if (trim(line).empty()) continue;
        auto cols = parse_csv_line(line, delim);
        if ((int)cols.size() <= max(url_col, cat_col)) {
            cerr << "Skipping malformed CSV line " << line_no << endl;
            continue;
        }
        string tag = cols[cat_col];
        string url = cols[url_col];
        // Skip a header row: the first line whose URL column is not a URL
        if (line_no == 1 && to_lower(url).rfind("http", 0) != 0) continue;
        if (tag.empty() || url.empty()) {
            cerr << "Skipping empty tag/url at line " << line_no << endl;
            continue;
        }

        cout << "Fetching: " << url << " (tag=" << tag << ")" << endl;
        FetchResult res = fetch_url(url);
        if (!res.ok || res.body.empty()) {
            cerr << "Failed to fetch or empty response for: " << url << endl;
            log_row(tag, url, res.http_code, res.final_url, "");
            continue;
        }

        auto blocks = extract_ld_json(res.body);
        if (blocks.empty()) {
            cerr << "No ld+json blocks found for: " << url << endl;
            log_row(tag, url, res.http_code, res.final_url, "");
            continue;
        }

        // Save each block separately. The CSV line number keeps names unique
        // when several rows share the same category.
        string safe_tag = sanitize_component(tag);
        for (size_t i = 0; i < blocks.size(); ++i) {
            string filename = out_dir + "/" + safe_tag + "_line" + to_string(line_no) +
                              "_" + to_string(i + 1) + ".json";
            ofstream out(u8path(filename));
            if (!out) {
                cerr << "Failed to write file: " << filename << endl;
                continue;
            }
            out << blocks[i] << "\n";
            cout << "Saved: " << filename << " (length=" << blocks[i].size() << ")" << endl;
            log_row(tag, url, res.http_code, res.final_url, filename);
        }
    }

    if (manifest) manifest.close();
    curl_global_cleanup();
    return 0;
}
