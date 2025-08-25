#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>  

using namespace std;
namespace fs = std::filesystem;

// ----------- Byte order helpers -----------
static inline uint32_t to_be32(uint32_t x) {
    // host -> big endian
    uint8_t b[4] = {
        (uint8_t)((x >> 24) & 0xff),
        (uint8_t)((x >> 16) & 0xff),
        (uint8_t)((x >> 8) & 0xff),
        (uint8_t)(x & 0xff)
    };
    uint32_t y;
    memcpy(&y, b, 4);
    return y;
}
static inline uint32_t from_be32(uint32_t x) {
    uint8_t b[4];
    memcpy(b, &x, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

// ----------- CRC32 (IEEE) -----------
static uint32_t crc32_table[256];
static void crc32_init() {
    uint32_t poly = 0xEDB88320u;
    for (uint32_t i=0; i<256; ++i) {
        uint32_t c = i;
        for (int j=0; j<8; ++j) {
            if (c & 1) c = poly ^ (c >> 1);
            else       c >>= 1;
        }
        crc32_table[i] = c;
    }
}
static inline uint32_t crc32(const uint8_t* data, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i=0; i<n; ++i) {
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// ----------- I/O helpers -----------
static bool write_all(ofstream& f, const void* buf, size_t n) {
    f.write(reinterpret_cast<const char*>(buf), n);
    return bool(f);
}
static bool read_exact(ifstream& f, void* buf, size_t n) {
    f.read(reinterpret_cast<char*>(buf), n);
    return size_t(f.gcount()) == n;
}

// ----------- Writer -----------
struct WalWriter {
    string path;
    WalWriter(string p): path(std::move(p)) {}
    bool append_record(const vector<uint8_t>& payload) {
        ofstream f(path, ios::binary | ios::app);
        if (!f) return false;
        uint32_t len_be = to_be32((uint32_t)payload.size());
        uint32_t c = crc32(payload.data(), payload.size());
        uint32_t crc_be = to_be32(c);
        if (!write_all(f, &len_be, 4)) return false;
        if (!write_all(f, payload.data(), payload.size())) return false;
        if (!write_all(f, &crc_be, 4)) return false;
        f.flush(); // userspace flush; OS flush optional for demo
        return bool(f);
    }
};

// ----------- Recovery Scanner -----------
struct ScanResult {
    size_t good_records = 0;
    uint64_t last_good_offset = 0;
    bool clean = true; // true if no truncation needed
};

static bool truncate_file(const string& path, uint64_t new_size) {
    try {
        fs::resize_file(path, new_size);
        return true;
    } catch (const fs::filesystem_error& e) {
        cerr << "[recover] truncate error: " << e.what() << "\n";
        return false;
    }
}

static ScanResult scan_and_maybe_truncate(const string& path, bool perform_truncate=true) {
    ScanResult R;
    uint64_t sz = 0;
    try {
        sz = fs::file_size(path);
    } catch (...) {
        cerr << "[recover] cannot stat file\n";
        R.clean = true;
        return R;
    }

    ifstream f(path, ios::binary);
    if (!f) {
        cerr << "[recover] cannot open file\n";
        R.clean = true;
        return R;
    }

    const uint32_t MAX_REC = 32 * 1024 * 1024; // 32MB sanity
    uint64_t off = 0;
    while (true) {
        if (off + 4 > sz) { // no room for len
            break;
        }
        f.seekg(off);
        uint32_t len_be = 0;
        if (!read_exact(f, &len_be, 4)) {
            break;
        }
        uint32_t len = from_be32(len_be);
        if (len == 0 || len > MAX_REC) {
            // implausible length -> cut here
            R.clean = false;
            break;
        }
        uint64_t need = (uint64_t)4 + len + 4;
        if (off + need > sz) {
            // partial tail
            R.clean = false;
            break;
        }
        // read payload+crc
        vector<uint8_t> payload(len);
        if (!read_exact(f, payload.data(), len)) {
            R.clean = false;
            break;
        }
        uint32_t crc_be=0;
        if (!read_exact(f, &crc_be, 4)) {
            R.clean = false;
            break;
        }
        uint32_t crc_stored = from_be32(crc_be);
        uint32_t crc_now = crc32(payload.data(), payload.size());
        if (crc_stored != crc_now) {
            // corruption -> cut at off
            R.clean = false;
            break;
        }
        // good record
        off += need;
        R.good_records++;
        R.last_good_offset = off;
        if (off == sz) break; // exact end
    }

    if (!R.clean && perform_truncate) {
        if (truncate_file(path, R.last_good_offset)) {
            cout << "[recover] truncated tail from offset=" << R.last_good_offset << " to size=" << R.last_good_offset << "\n";
        } else {
            cerr << "[recover] truncate failed; file may still have a torn tail\n";
        }
    }
    return R;
}

// ----------- Corrupt (truncate bytes from end) -----------
static bool corrupt_tail(const string& path, uint64_t cut_bytes) {
    uint64_t sz = fs::file_size(path);
    if (cut_bytes >= sz) cut_bytes = sz/2;
    uint64_t new_size = sz - cut_bytes;
    cout << "[corrupt] truncating " << cut_bytes << " bytes: " << sz << " -> " << new_size << "\n";
    return truncate_file(path, new_size);
}

// ----------- Demo -----------
static int run_demo(const string& path, int N, int payload_bytes) {
    crc32_init();
    // write
    WalWriter w(path);
    vector<uint8_t> buf(payload_bytes);
    // deterministic content
    for (int i=0;i<N;i++) {
        for (int j=0;j<payload_bytes;j++) buf[j] = uint8_t((i+j) & 0xFF);
        if (!w.append_record(buf)) {
            cerr << "[write] failed at i=" << i << "\n";
            return 1;
        }
    }
    auto sz_before = fs::file_size(path);
    cout << "[write] wrote " << N << " entries, bytes=" << sz_before << "\n";

    // cut approx half of last record (len+payload+crc)
    uint64_t cut = (uint64_t)(payload_bytes/2 + 6);
    if (!corrupt_tail(path, cut)) return 2;

    // recover
    auto R = scan_and_maybe_truncate(path, /*perform_truncate=*/true);
    cout << "[recover] scanned " << R.good_records << " good entries\n";
    if (R.clean) {
        cout << "[recover] CLEAN (no action needed)\n";
    } else {
        cout << "[recover] OK: Recovered " << R.good_records << " entries, no parse error.\n";
    }
    return 0;
}

// ----------- Main CLI -----------
int main(int argc, char** argv) {
    ios::sync_with_stdio(false);

    if (argc < 3) {
        cerr << "Usage:\n"
             << "  " << argv[0] << " write   <file> <N> <payload_bytes>\n"
             << "  " << argv[0] << " corrupt <file> <bytes_to_cut>\n"
             << "  " << argv[0] << " recover <file>\n"
             << "  " << argv[0] << " demo    <file> <N> <payload_bytes>\n";
        return 2;
    }

    string mode = argv[1];
    string path = argv[2];
    crc32_init();

    try {
        if (mode == "write") {
            if (argc < 5) { cerr << "need N and payload_bytes\n"; return 2; }
            int N = stoi(argv[3]);
            int payload = stoi(argv[4]);
            WalWriter w(path);
            vector<uint8_t> buf(payload);
            for (int i=0;i<N;i++) {
                for (int j=0;j<payload;j++) buf[j] = uint8_t((i+j) & 0xFF);
                if (!w.append_record(buf)) {
                    cerr << "[write] failed at i=" << i << "\n";
                    return 1;
                }
            }
            auto sz = fs::file_size(path);
            cout << "[write] wrote " << N << " entries, bytes=" << sz << "\n";
            return 0;
        }
        else if (mode == "corrupt") {
            if (argc < 4) { cerr << "need bytes_to_cut\n"; return 2; }
            uint64_t cut = stoull(argv[3]);
            if (!fs::exists(path)) { cerr << "file not found\n"; return 2; }
            return corrupt_tail(path, cut) ? 0 : 1;
        }
        else if (mode == "recover") {
            if (!fs::exists(path)) { cerr << "file not found\n"; return 2; }
            auto R = scan_and_maybe_truncate(path, /*perform_truncate=*/true);
            cout << "[recover] scanned " << R.good_records << " good entries\n";
            if (R.clean) {
                cout << "[recover] CLEAN (no action needed)\n";
            } else {
                cout << "[recover] OK: Recovered " << R.good_records << " entries, no parse error.\n";
            }
            return 0;
        }
        else if (mode == "demo") {
            if (argc < 5) { cerr << "need N and payload_bytes\n"; return 2; }
            int N = stoi(argv[3]);
            int payload = stoi(argv[4]);
            return run_demo(path, N, payload);
        }
        else {
            cerr << "unknown mode\n"; return 2;
        }
    } catch (const exception& e) {
        cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
