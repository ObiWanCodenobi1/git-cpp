#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <curl/curl.h>
#include <exception>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <openssl/evp.h>
#include <vector>
#include <zlib.h>
#include <arpa/inet.h>      // for ntohl, htonl
#include <openssl/sha.h>
#include <unordered_map>
#include <sstream>
#include <iomanip>

std::string to_hex(const unsigned char* data, size_t len) {
    std::ostringstream ss;
    for (size_t i = 0; i < len; i++)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    return ss.str();
}

size_t get_file_size(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open file");
    return static_cast<size_t>(in.tellg());
}

std::string git_hash_object_write(const std::string& filepath) {
    const size_t CHUNK = 16384;

    // --- Step 1: file setup ---
    size_t file_size = get_file_size(filepath);
    std::ifstream in(filepath, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file: " + filepath);

    // --- Step 2: initialize hash and zlib ---
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha1(), nullptr);

    z_stream strm{};
    if (deflateInit(&strm, Z_BEST_COMPRESSION) != Z_OK)
        throw std::runtime_error("deflateInit failed");

    // --- Step 3: compute header ---
    std::string header = "blob " + std::to_string(file_size) + '\0';

    // Hash header
    EVP_DigestUpdate(mdctx, header.data(), header.size());

    // We'll fill real hash later
    // For now, use a temporary path (we’ll rename once we have hash)
    std::filesystem::create_directories(".git/objects/tmp");
    std::ofstream out(".git/objects/tmp/blobtmp", std::ios::binary);
    if (!out) throw std::runtime_error("cannot open temp object file");

    // --- Step 4: feed header into compressor ---
    std::vector<unsigned char> outbuf(CHUNK);
    strm.next_in = (Bytef*)header.data();
    strm.avail_in = header.size();
    do {
        strm.avail_out = CHUNK;
        strm.next_out = outbuf.data();
        deflate(&strm, Z_NO_FLUSH);
        size_t have = CHUNK - strm.avail_out;
        out.write(reinterpret_cast<char*>(outbuf.data()), have);
    } while (strm.avail_out == 0);

    // --- Step 5: stream file chunks ---
    std::vector<char> inbuf(CHUNK);
    while (true) {
        in.read(inbuf.data(), CHUNK);
        std::streamsize bytes_read = in.gcount();
        if (bytes_read <= 0) break;

        // Hash
        EVP_DigestUpdate(mdctx, inbuf.data(), bytes_read);

        // Compress
        strm.next_in = reinterpret_cast<Bytef*>(inbuf.data());
        strm.avail_in = bytes_read;

        int flush = in.eof() ? Z_FINISH : Z_NO_FLUSH;
        do {
            strm.avail_out = CHUNK;
            strm.next_out = outbuf.data();
            deflate(&strm, flush);
            size_t have = CHUNK - strm.avail_out;
            out.write(reinterpret_cast<char*>(outbuf.data()), have);
        } while (strm.avail_out == 0);

        if (in.eof()) break;
    }

    deflateEnd(&strm);
    out.close();

    // --- Step 6: finalize SHA1 hash ---
    unsigned char hash_bytes[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    EVP_DigestFinal_ex(mdctx, hash_bytes, &hash_len);
    EVP_MD_CTX_free(mdctx);

    std::string hash = to_hex(hash_bytes, hash_len);

    // --- Step 7: move temp file to .git/objects/<xx>/<rest> ---
    std::string dir = hash.substr(0, 2);
    std::string filename = hash.substr(2);
    std::filesystem::path final_dir = std::filesystem::path(".git") / "objects" / dir;
    std::filesystem::create_directories(final_dir);
    std::filesystem::path final_path = final_dir / filename;
    std::filesystem::rename(".git/objects/tmp/blobtmp", final_path);

    return hash;
}

std::string git_cat_file(const std::string& hash) {
    std::string dir = hash.substr(0, 2);
    std::string filename = hash.substr(2);
    std::filesystem::path object_path = std::filesystem::path(".git") / "objects" / dir / filename;

    if (!std::filesystem::exists(object_path)) {
        throw std::runtime_error("object not found: " + hash);
    }

    // Read compressed contents
    std::ifstream file(object_path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open object file");

    std::vector<unsigned char> compressed((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    file.close();

    // Setup zlib decompression
    z_stream strm{};
    strm.next_in = compressed.data();
    strm.avail_in = compressed.size();

    if (inflateInit(&strm) != Z_OK)
        throw std::runtime_error("inflateInit failed");

    std::vector<unsigned char> outbuf(32768);
    std::string decompressed;

    int ret;
    do {
        strm.next_out = outbuf.data();
        strm.avail_out = outbuf.size();
        ret = inflate(&strm, Z_NO_FLUSH);

        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR)
            throw std::runtime_error("inflate error");

        size_t have = outbuf.size() - strm.avail_out;
        decompressed.append(reinterpret_cast<char*>(outbuf.data()), have);
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);

    // Find the header separator '\0'
    size_t header_end = decompressed.find('\0');
    if (header_end == std::string::npos)
        throw std::runtime_error("invalid blob format");

    // Extract content (after null byte)
    std::string content = decompressed.substr(header_end + 1);
    return content;
}

std::array<unsigned char, 20> hex_to_raw20(const std::string& hex) {
    std::array<unsigned char, 20> out{};
    for (int i = 0; i < 20; i++) {
        unsigned int byte;
        sscanf(hex.substr(i * 2, 2).c_str(), "%02x", &byte);
        out[i] = static_cast<unsigned char>(byte);
    }
    return out;
}

struct TreeEntry {
    std::string mode;       // e.g., "100644"
    std::string name;       // e.g., "main.cpp"
    std::array<unsigned char,20> sha; // raw 20-byte
};
std::string git_write_tree(const std::string& directory);

std::vector<TreeEntry> build_tree_entries(const std::string& path) {
    std::vector<TreeEntry> entries;

    for (auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().filename() == ".git") continue;

        if (entry.is_directory()) {
            // Recursively write nested tree
            std::string subtree_hash = git_write_tree(entry.path().string());
            TreeEntry te;
            te.mode = "40000";
            te.name = entry.path().filename().string();
            te.sha = hex_to_raw20(subtree_hash);
            entries.push_back(te);
        } 
        else if (entry.is_regular_file()) {
            std::string blob_hash = git_hash_object_write(entry.path().string());
            TreeEntry te;
            te.mode = "100644";
            te.name = entry.path().filename().string();
            te.sha = hex_to_raw20(blob_hash);
            entries.push_back(te);
        }
    }

    // Git requires lexicographic sort
    std::sort(entries.begin(), entries.end(),
              [](auto& a, auto& b) { return a.name < b.name; });

    return entries;
}

std::string git_write_tree(const std::string& directory = ".") {
    const size_t CHUNK = 16384;

    // 1. Collect tree entries
    auto entries = build_tree_entries(directory);

    // Compute total payload size
    size_t body_size = 0;
    for (auto& e : entries) {
        body_size += e.mode.size() + 1;            // mode + space
        body_size += e.name.size() + 1;            // name + null
        body_size += 20;                           // raw sha
    }

    // ----- 2. SHA1 setup -----
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha1(), nullptr);

    // ----- 3. Zlib setup -----
    z_stream strm{};
    if (deflateInit(&strm, Z_BEST_COMPRESSION) != Z_OK)
        throw std::runtime_error("deflateInit failed");

    // Create temp output
    std::filesystem::create_directories(".git/objects/tmp");
    std::ofstream out(".git/objects/tmp/tree_tmp", std::ios::binary);

    // ----- 4. Write header -----
    std::string header = "tree " + std::to_string(body_size) + '\0';

    EVP_DigestUpdate(mdctx, header.data(), header.size());

    // zlib header
    {
        std::vector<unsigned char> buf(CHUNK);
        strm.next_in = (Bytef*)header.data();
        strm.avail_in = header.size();

        do {
            strm.avail_out = CHUNK;
            strm.next_out = buf.data();
            deflate(&strm, Z_NO_FLUSH);
            size_t have = CHUNK - strm.avail_out;
            out.write((char*)buf.data(), have);
        } while (strm.avail_out == 0);
    }

    // ----- 5. Write each entry progressively -----
    std::vector<unsigned char> buf(CHUNK);

    for (auto& e : entries) {
        // mode + space
        std::string m = e.mode + " ";
        EVP_DigestUpdate(mdctx, m.data(), m.size());
        strm.next_in = (Bytef*)m.data();
        strm.avail_in = m.size();
        do {
            strm.avail_out = CHUNK;
            strm.next_out = buf.data();
            deflate(&strm, Z_NO_FLUSH);
            size_t have = CHUNK - strm.avail_out;
            out.write((char*)buf.data(), have);
        } while (strm.avail_out == 0);

        // name + null
        std::string nm = e.name;
        EVP_DigestUpdate(mdctx, nm.data(), nm.size());
        strm.next_in = (Bytef*)nm.data();
        strm.avail_in = nm.size();
        do {
            strm.avail_out = CHUNK;
            strm.next_out = buf.data();
            deflate(&strm, Z_NO_FLUSH);
            size_t have = CHUNK - strm.avail_out;
            out.write((char*)buf.data(), have);
        } while (strm.avail_out == 0);

        // null byte
        char zero = '\0';
        EVP_DigestUpdate(mdctx, &zero, 1);
        strm.next_in = (Bytef*)&zero;
        strm.avail_in = 1;
        do {
            strm.avail_out = CHUNK;
            strm.next_out = buf.data();
            deflate(&strm, Z_NO_FLUSH);
            size_t have = CHUNK - strm.avail_out;
            out.write((char*)buf.data(), have);
        } while (strm.avail_out == 0);

        // SHA raw (20 bytes)
        EVP_DigestUpdate(mdctx, e.sha.data(), 20);
        strm.next_in = (Bytef*)e.sha.data();
        strm.avail_in = 20;
        do {
            strm.avail_out = CHUNK;
            strm.next_out = buf.data();
            deflate(&strm, Z_NO_FLUSH);
            size_t have = CHUNK - strm.avail_out;
            out.write((char*)buf.data(), have);
        } while (strm.avail_out == 0);
    }

    // ---- FINISH compression ----
    int flush = Z_FINISH;
    do {
        strm.avail_out = CHUNK;
        strm.next_out = buf.data();
        deflate(&strm, flush);
        size_t have = CHUNK - strm.avail_out;
        out.write((char*)buf.data(), have);
    } while (strm.avail_out == 0);

    deflateEnd(&strm);
    out.close();

    // ----- 6. Finalize SHA1 -----
    unsigned char hash_bytes[20];
    unsigned int hash_len;
    EVP_DigestFinal_ex(mdctx, hash_bytes, &hash_len);
    EVP_MD_CTX_free(mdctx);

    // to hex
    std::string hash;
    char tmp[3];
    for (int i = 0; i < 20; i++) {
        sprintf(tmp, "%02x", hash_bytes[i]);
        hash += tmp;
    }

    // ----- 7. Move to .git/objects/<xx>/<rest> -----
    std::string dir = hash.substr(0, 2);
    std::string file = hash.substr(2);

    std::filesystem::create_directories(".git/objects/" + dir);
    std::filesystem::rename(".git/objects/tmp/tree_tmp",
                            ".git/objects/" + dir + "/" + file);

    return hash;
}

void git_ls_tree(const std::string& hash, const bool name_only) {
    std::string dir = hash.substr(0, 2);
    std::string filename = hash.substr(2);
    std::filesystem::path object_path = std::filesystem::path(".git") / "objects" / dir / filename;

    if (!std::filesystem::exists(object_path)) {
        throw std::runtime_error("object not found: " + hash);
    }

    std::ifstream file(object_path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open object file");

    std::vector<unsigned char> compressed((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    file.close();

    z_stream strm{};
    strm.next_in = compressed.data();
    strm.avail_in = compressed.size();

    if (inflateInit(&strm) != Z_OK)
        throw std::runtime_error("inflateInit failed");

    std::vector<unsigned char> outbuf(32768);
    std::string decompressed;

    int ret;
    do {
        strm.next_out = outbuf.data();
        strm.avail_out = outbuf.size();
        ret = inflate(&strm, Z_NO_FLUSH);

        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR)
            throw std::runtime_error("inflate error");

        size_t have = outbuf.size() - strm.avail_out;
        decompressed.append(reinterpret_cast<char*>(outbuf.data()), have);
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);

    // Find the header separator '\0'
    size_t header_end = decompressed.find('\0');
    if (header_end == std::string::npos)
        throw std::runtime_error("invalid blob format");

    // Extract content (after null byte)
    std::string content = decompressed.substr(header_end + 1);

    size_t i = 0;
    while (i < content.size()) {
        // ---- 1. Parse mode (ASCII until space)
        std::string mode;
        while (i < content.size() && content[i] != ' ') {
            mode.push_back(content[i]);
            i++;
        }
        if (i >= content.size()) break;
        i++; // skip ' '

        // ---- 2. Parse name (ASCII until null byte)
        std::string name;
        while (i < content.size() && content[i] != '\0') {
            name.push_back(content[i]);
            i++;
        }
        if (i >= content.size()) break;
        i++; // skip null byte

        // ---- 3. Parse 20-byte SHA
        if (i + 20 > content.size()) 
            throw std::runtime_error("invalid tree entry (sha truncated)");

        unsigned char sha_raw[20];
        std::memcpy(sha_raw, &content[i], 20);
        i += 20;

        // Convert SHA to hex for printing
        std::string sha_hex;
        sha_hex.reserve(40);
        const char* hex = "0123456789abcdef";
        for (int b = 0; b < 20; b++) {
            sha_hex.push_back(hex[(sha_raw[b] >> 4) & 0xF]);
            sha_hex.push_back(hex[(sha_raw[b] & 0xF)]);
        }

        // ---- 4. Output
        if (name_only) {
            std::cout << name << "\n";
        } else {
            std::cout << mode << " " << sha_hex << " " << name << "\n";
        }

    }
}

std::string git_commit_tree(const std::string& tree_hash, const std::string& parent_hash, const std::string& msg) {
    const size_t CHUNK = 16384;

    // -------------------------
    // 1. Build commit body in parts, measure body size
    // -------------------------

    std::string name = "User";
    std::string email = "user@example.com";

    time_t now = time(nullptr);
    long tz = 19800;  // +0530
    char tb[64];
    sprintf(tb, "%ld %+05ld", now, tz / 36);
    std::string timestamp(tb);

    std::vector<std::string> lines;

    lines.push_back("tree "   + tree_hash + "\n");

    if (!parent_hash.empty())
        lines.push_back("parent " + parent_hash + "\n");

    lines.push_back("author "    + name + " <" + email + "> " + timestamp + "\n");
    lines.push_back("committer " + name + " <" + email + "> " + timestamp + "\n");
    lines.push_back("\n");
    lines.push_back(msg + "\n");

    size_t body_size = 0;
    for (auto& s : lines)
        body_size += s.size();

    // -------------------------
    // 2. Initialize SHA1 (for header + body)
    // -------------------------
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha1(), nullptr);

    // -------------------------
    // 3. Initialize zlib (progressive)
    // -------------------------
    z_stream strm{};
    if (deflateInit(&strm, Z_BEST_COMPRESSION) != Z_OK)
        throw std::runtime_error("deflateInit failed");

    std::filesystem::create_directories(".git/objects/tmp");
    std::ofstream out(".git/objects/tmp/commit_tmp", std::ios::binary);

    std::vector<unsigned char> buf(CHUNK);

    // -------------------------
    // 4. Write header progressively
    // -------------------------

    std::string header = "commit " + std::to_string(body_size) + '\0';

    // SHA1 header
    EVP_DigestUpdate(mdctx, header.data(), header.size());

    // zlib header
    strm.next_in = (Bytef*)header.data();
    strm.avail_in = header.size();
    do {
        strm.avail_out = CHUNK;
        strm.next_out = buf.data();
        deflate(&strm, Z_NO_FLUSH);
        size_t have = CHUNK - strm.avail_out;
        out.write((char*)buf.data(), have);
    } while (strm.avail_out == 0);

    // -------------------------
    // 5. Write body lines progressively
    // -------------------------
    for (auto& s : lines) {
        EVP_DigestUpdate(mdctx, s.data(), s.size());

        strm.next_in = (Bytef*)s.data();
        strm.avail_in = s.size();

        do {
            strm.avail_out = CHUNK;
            strm.next_out = buf.data();
            deflate(&strm, Z_NO_FLUSH);
            size_t have = CHUNK - strm.avail_out;
            out.write((char*)buf.data(), have);
        } while (strm.avail_out == 0);
    }

    // -------------------------
    // 6. Finish zlib compression
    // -------------------------
    int flush = Z_FINISH;
    do {
        strm.avail_out = CHUNK;
        strm.next_out = buf.data();
        deflate(&strm, flush);
        size_t have = CHUNK - strm.avail_out;
        out.write((char*)buf.data(), have);
    } while (strm.avail_out == 0);

    deflateEnd(&strm);
    out.close();

    // -------------------------
    // 7. Finalize SHA1
    // -------------------------
    unsigned char hash_raw[20];
    unsigned int hash_len;

    EVP_DigestFinal_ex(mdctx, hash_raw, &hash_len);
    EVP_MD_CTX_free(mdctx);

    // Convert SHA to hex
    std::string hash;
    char tmp[3];
    for (int i = 0; i < 20; i++) {
        sprintf(tmp, "%02x", hash_raw[i]);
        hash += tmp;
    }

    // -------------------------
    // 8. Move compressed object into final place
    // -------------------------
    std::string dir = hash.substr(0, 2);
    std::string file = hash.substr(2);

    std::filesystem::create_directories(".git/objects/" + dir);
    std::filesystem::rename(".git/objects/tmp/commit_tmp",
                            ".git/objects/" + dir + "/" + file);

    return hash;
}

struct GitRef {
    std::string hash;
    std::string name;
};

// Full response
struct RemoteRefs {
    std::vector<GitRef> refs;
    std::vector<std::string> capabilities;
};

static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::cout<<"Total size: "<<total<<"\n";
    std::string* buf = static_cast<std::string*>(userp);
    buf->append(static_cast<char*>(contents), total);
    std::cout<<"Buff: "<<buf<<std::endl;
    return total;
}

// Parses pkt-line length (first 4 bytes as hex)
static size_t pkt_len(const std::string& s, size_t offset) {
    if (offset + 4 > s.size()) return 0;
    return std::stoul(s.substr(offset, 4), nullptr, 16);
}

RemoteRefs get_remote_refs(const std::string& base_url) {
    RemoteRefs result{};

    // --- Build URL ---
    std::string full_url = base_url;
    if (full_url.back() != '/')
        full_url += '/';
    full_url += "info/refs?service=git-upload-pack";
    std::cout<< "URL: "<<full_url<<"\n";
    // --- GET request ---
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string response;
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
    // curl_easy_setopt(curl, CURLOPT_USERAGENT, "mygit/0.1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        throw std::runtime_error("Failed GET: " + std::string(curl_easy_strerror(res)));

    // --- Parse pkt-line-encoded response ---
    size_t offset = 0;

    // First line should be "# service=git-upload-pack"
    size_t len = pkt_len(response, offset);
    if (len == 0) throw std::runtime_error("Invalid pkt-line");
    offset += 4;

    std::string first = response.substr(offset, len - 4);
    offset += (len - 4);
    if (first.find("# service=git-upload-pack") != 0)
        throw std::runtime_error("Not a git-upload-pack service");

    // Next must be 0000 delimiter
    if (response.substr(offset, 4) != "0000")
        throw std::runtime_error("Expected flush packet");
    offset += 4;

    // Now parse each ref line until EOF
    while (offset < response.size()) {
        size_t l = pkt_len(response, offset);
        if (l == 0 || l < 5) break;

        offset += 4;
        std::string line = response.substr(offset, l - 4);
        offset += (l - 4);

        // line looks like: "<hash> <refname>\0cap1 cap2 cap3"
        // first line has capabilities after NUL

        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;

        std::string hash = line.substr(0, sp);
        std::string rest = line.substr(sp + 1);

        size_t nul = rest.find('\0');

        if (nul != std::string::npos) {
            // capabilities only appear on first ref
            std::string refname = rest.substr(0, nul);
            std::string caps_str = rest.substr(nul + 1);

            std::stringstream ss(caps_str);
            std::string cap;
            while (ss >> cap)
                result.capabilities.push_back(cap);

            result.refs.push_back({hash, refname});
        } else {
            result.refs.push_back({hash, rest});
        }
    }

    return result;
}

std::string pkt(const std::string& s) {
    char len[5];
    std::snprintf(len, sizeof(len), "%04x", (unsigned)(4 + s.size()));
    return std::string(len) + s;
}

std::string flush_pkt() { return "0000"; }

std::string build_v0_fetch_request(const std::string& want_hash) {
    std::string r;

    // section: want
    r += pkt("want " + want_hash + "\x0A");
    r += flush_pkt();
    r += pkt("done\x0A");
    std::cout<<r<<std::endl;
    return r;
}

size_t write_packfile_data(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::cout<<"Total size: "<<totalSize<<"\n";
    std::vector<char>* data = static_cast<std::vector<char>*>(userp);
    char* ptr = static_cast<char*>(contents);
    
    // Safety check
    if (data == nullptr || contents == nullptr) {
        std::cerr << "Null pointer in write callback" << std::endl;
        return 0;
    }
    
    try {
        data->insert(data->end(), ptr, ptr + totalSize);
    } catch (const std::exception& e) {
        std::cerr << "Exception in write callback: " << e.what() << std::endl;
        return 0;
    }
    
    return totalSize;
}

std::string send_upload_pack_request(const std::string& url,
                                     const std::string& request_body) {
    std::cout << "Request body size: " << request_body.size() << " bytes" << std::endl;
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl init failed");

    std::string response;

    std::string full = url + "/git-upload-pack";
    std::cout << "URL: " << full << std::endl;

    curl_easy_setopt(curl, CURLOPT_URL, full.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-git-upload-pack-request");

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, request_body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        throw std::runtime_error(
            std::string("curl request failed: ") +
            curl_easy_strerror(res)
        );

    std::cout << "HTTP STATUS = " << http_code << std::endl;

    return response;
}

struct GitObject {
    std::string type;
    std::string data;
};

struct PackResult {
    std::string commit_hash;
    std::string tree_hash;
};

static std::string sha1_hex(const unsigned char sha[20]) {
    char buf[41];
    for (int i = 0; i < 20; i++) sprintf(buf + i*2, "%02x", sha[i]);
    buf[40] = 0;
    return buf;
}

static std::string sha1_hex(const std::string& raw20) {
    return sha1_hex((unsigned char*)raw20.data());
}

static void write_loose(const std::string& gitDir,
                        const std::string& hash,
                        const std::string& full)
{
    std::string dir = gitDir + "/objects/" + hash.substr(0,2);
    std::string file = dir + "/" + hash.substr(2);

    std::filesystem::create_directories(dir);

    // zlib-compress before writing (git loose object format)
    z_stream strm{};
    deflateInit(&strm, Z_BEST_COMPRESSION);
    std::vector<unsigned char> compressed(compressBound(full.size()));
    strm.next_in  = (Bytef*)full.data();
    strm.avail_in = full.size();
    strm.next_out = compressed.data();
    strm.avail_out = compressed.size();
    deflate(&strm, Z_FINISH);
    size_t compressed_size = strm.total_out;
    deflateEnd(&strm);

    std::ofstream out(file, std::ios::binary);
    out.write((char*)compressed.data(), compressed_size);
}

// Read and decompress a loose git object from gitDir, returns {type, data}
static std::pair<std::string, std::string> read_object_from_gitdir(
    const std::string& gitDir, const std::string& hash)
{
    std::string path = gitDir + "/objects/" + hash.substr(0,2) + "/" + hash.substr(2);
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("object not found: " + hash);

    std::vector<unsigned char> compressed((std::istreambuf_iterator<char>(f)),
                                           std::istreambuf_iterator<char>());

    z_stream strm{};
    inflateInit(&strm);
    strm.next_in  = compressed.data();
    strm.avail_in = compressed.size();

    std::string decompressed;
    char buf[4096];
    int ret;
    do {
        strm.next_out = (Bytef*)buf;
        strm.avail_out = sizeof(buf);
        ret = inflate(&strm, Z_NO_FLUSH);
        size_t have = sizeof(buf) - strm.avail_out;
        decompressed.append(buf, have);
    } while (ret == Z_OK);
    inflateEnd(&strm);

    size_t null_pos = decompressed.find('\0');
    if (null_pos == std::string::npos)
        throw std::runtime_error("invalid object: " + hash);

    std::string hdr  = decompressed.substr(0, null_pos);
    std::string data = decompressed.substr(null_pos + 1);
    size_t sp = hdr.find(' ');
    std::string type = hdr.substr(0, sp);
    return {type, data};
}

// Recursively write tree entries to the working directory
static void checkout_tree(const std::string& gitDir,
                          const std::string& tree_hash,
                          const std::string& dest_dir)
{
    std::filesystem::create_directories(dest_dir);

    auto [type, data] = read_object_from_gitdir(gitDir, tree_hash);
    if (type != "tree")
        throw std::runtime_error("expected tree, got " + type + " for " + tree_hash);

    size_t i = 0;
    while (i < data.size()) {
        // mode (until space)
        std::string mode;
        while (i < data.size() && data[i] != ' ') mode.push_back(data[i++]);
        if (i >= data.size()) break;
        i++; // skip space

        // name (until null)
        std::string name;
        while (i < data.size() && data[i] != '\0') name.push_back(data[i++]);
        if (i >= data.size()) break;
        i++; // skip null

        // 20-byte raw SHA1
        if (i + 20 > data.size()) break;
        char sha_hex[41];
        for (int b = 0; b < 20; b++)
            sprintf(sha_hex + b*2, "%02x", (unsigned char)data[i+b]);
        sha_hex[40] = 0;
        i += 20;

        std::string entry_hash(sha_hex);
        std::string full_path = dest_dir + "/" + name;

        if (mode == "40000" || mode == "040000") {
            checkout_tree(gitDir, entry_hash, full_path);
        } else {
            auto [btype, bdata] = read_object_from_gitdir(gitDir, entry_hash);
            std::ofstream out(full_path, std::ios::binary);
            out.write(bdata.data(), bdata.size());
        }
    }
}

// Apply a git pack delta to a base object, returning the reconstructed data
static std::string apply_delta(const std::string& base, const std::string& delta) {
    size_t i = 0;

    // Read source size (little-endian varint)
    size_t src_size = 0, shift = 0;
    do { src_size |= (size_t)((unsigned char)delta[i] & 0x7f) << shift; shift += 7; } while ((unsigned char)delta[i++] & 0x80);

    // Read target size
    size_t tgt_size = 0; shift = 0;
    do { tgt_size |= (size_t)((unsigned char)delta[i] & 0x7f) << shift; shift += 7; } while ((unsigned char)delta[i++] & 0x80);

    std::string result;
    result.reserve(tgt_size);

    while (i < delta.size()) {
        unsigned char cmd = (unsigned char)delta[i++];
        if (cmd & 0x80) {
            // Copy from base
            size_t offset = 0, sz = 0;
            if (cmd & 0x01) offset |= (unsigned char)delta[i++];
            if (cmd & 0x02) offset |= (size_t)(unsigned char)delta[i++] << 8;
            if (cmd & 0x04) offset |= (size_t)(unsigned char)delta[i++] << 16;
            if (cmd & 0x08) offset |= (size_t)(unsigned char)delta[i++] << 24;
            if (cmd & 0x10) sz |= (unsigned char)delta[i++];
            if (cmd & 0x20) sz |= (size_t)(unsigned char)delta[i++] << 8;
            if (cmd & 0x40) sz |= (size_t)(unsigned char)delta[i++] << 16;
            if (sz == 0) sz = 0x10000;
            result.append(base, offset, sz);
        } else if (cmd) {
            // Insert literal
            result.append(delta, i, cmd);
            i += cmd;
        }
    }
    return result;
}

static void zlib_decompress(const unsigned char* in, size_t inSize, std::string& out) {
    z_stream zs{};
    inflateInit(&zs);

    zs.next_in = (Bytef*)in;
    zs.avail_in = inSize;

    char buffer[8192];

    int ret;
    do {
        zs.next_out = (Bytef*)buffer;
        zs.avail_out = sizeof(buffer);

        ret = inflate(&zs, Z_NO_FLUSH);

        size_t have = sizeof(buffer) - zs.avail_out;
        if (have > 0) out.append(buffer, have);

    } while (ret == Z_OK);

    inflateEnd(&zs);
}

/*
 * SIMPLE PACK PARSER (no deltas, no refs)
 * Layout:
 *   PACK
 *   00000002  (version)
 *   0000000N  (# of objects)
 *   N entries: <type/size varint> <zlib data>
 */
PackResult process_pack_and_write_objects(const std::string& gitDir,
                                          const std::string& pack)
{
    const unsigned char* base_ptr = (const unsigned char*)pack.data();
    const unsigned char* p = base_ptr;
    const unsigned char* end = base_ptr + pack.size();

    if (memcmp(p, "PACK", 4) != 0)
        throw std::runtime_error("Not a PACK file");
    p += 4;
    uint32_t version = ntohl(*(uint32_t*)p); p += 4;
    uint32_t nobj    = ntohl(*(uint32_t*)p); p += 4;

    // Cache resolved objects for delta base lookup
    std::unordered_map<std::string, std::pair<std::string,std::string>> obj_by_hash;   // hash -> {type, data}
    std::unordered_map<size_t,      std::pair<std::string,std::string>> obj_by_offset; // pack_offset -> {type, data}

    std::string last_commit_hash, last_tree_hash;

    for (uint32_t i = 0; i < nobj; i++) {
        size_t obj_offset = p - base_ptr;

        // ---- Parse type + size varint ----
        unsigned char c = *p++;
        int type  = (c >> 4) & 7;
        size_t sz = c & 0x0F;
        int shift = 4;
        while (c & 0x80) {
            c = *p++;
            sz |= (size_t)(c & 0x7F) << shift;
            shift += 7;
        }

        // ---- Extra header for delta types ----
        std::string ref_delta_base_hash;
        size_t      ofs_delta_base_offset = 0;

        if (type == 6) { // OBJ_OFS_DELTA
            // Read negative-offset varint (MSB-first, each byte adds bits)
            unsigned char b = *p++;
            size_t ofs = b & 0x7f;
            while (b & 0x80) {
                b = *p++;
                ofs = ((ofs + 1) << 7) | (b & 0x7f);
            }
            ofs_delta_base_offset = obj_offset - ofs;
        } else if (type == 7) { // OBJ_REF_DELTA
            char hex[41];
            for (int b = 0; b < 20; b++) sprintf(hex + b*2, "%02x", *p++);
            hex[40] = 0;
            ref_delta_base_hash = hex;
        }

        // ---- Decompress object / delta data ----
        std::string raw_delta;
        {
            z_stream zs{};
            inflateInit(&zs);
            zs.next_in  = (Bytef*)p;
            zs.avail_in = (uInt)(end - p);
            char buf[4096];
            while (true) {
                zs.next_out  = (Bytef*)buf;
                zs.avail_out = sizeof(buf);
                int ret = inflate(&zs, Z_NO_FLUSH);
                size_t have = sizeof(buf) - zs.avail_out;
                if (have) raw_delta.append(buf, have);
                if (ret == Z_STREAM_END) break;
                if (ret != Z_OK)         break;
            }
            p += zs.total_in;
            inflateEnd(&zs);
        }

        // ---- Resolve to final {type_str, data} ----
        std::string type_str, raw_data;

        if (type >= 1 && type <= 4) {
            static const char* names[] = {"","commit","tree","blob","tag"};
            type_str = names[type];
            raw_data = raw_delta;
        } else if (type == 6) {
            auto it = obj_by_offset.find(ofs_delta_base_offset);
            if (it == obj_by_offset.end())
                throw std::runtime_error("OFS_DELTA base not found at offset " +
                                         std::to_string(ofs_delta_base_offset));
            type_str = it->second.first;
            raw_data = apply_delta(it->second.second, raw_delta);
        } else if (type == 7) {
            auto it = obj_by_hash.find(ref_delta_base_hash);
            if (it == obj_by_hash.end())
                throw std::runtime_error("REF_DELTA base not found: " + ref_delta_base_hash);
            type_str = it->second.first;
            raw_data = apply_delta(it->second.second, raw_delta);
        } else {
            continue; // unknown type, skip
        }

        // ---- Build full git object and hash it ----
        std::string hdr = type_str + " " + std::to_string(raw_data.size());
        hdr.push_back('\0');
        std::string full = hdr + raw_data;

        unsigned char sha[20];
        SHA1((unsigned char*)full.data(), full.size(), sha);
        std::string hex = sha1_hex(sha);

        // ---- Cache for delta resolution ----
        obj_by_hash[hex]          = {type_str, raw_data};
        obj_by_offset[obj_offset] = {type_str, raw_data};

        // ---- Write loose object ----
        write_loose(gitDir, hex, full);

        // ---- Track latest commit / tree ----
        if (type_str == "commit") {
            last_commit_hash = hex;
            size_t pos = raw_data.find("tree ");
            if (pos != std::string::npos)
                last_tree_hash = raw_data.substr(pos + 5, 40);
        }
    }

    return { last_commit_hash, last_tree_hash };
}

enum PackState {READ_ACK, READ_PACK} ;

std::string extract_packfile_from_response(const std::string& sideband) {
    size_t offset = 0;
    std::string pack;
    std::cout<<"Extracting packfile from sideband response...\n"<<sideband<<std::endl;
    std::cout << "Response size: " << sideband.size() << std::endl;
    PackState state = READ_ACK;
    while (offset + 4 <= sideband.size()) {
        // Read pkt-line length (4 hex chars)
        if (state == READ_ACK) {
            std::cout<<"Converting to len: "<<sideband.substr(offset, 4)<<std::endl;
            unsigned len = std::stoul(sideband.substr(offset, 4), 0, 16);
            offset += 4;
            std::cout<<len<<" "<<offset<<" "<<sideband.size()<<std::endl;
            if (len == 0) break; // flush packet

            if (len < 5) { // minimum pkt-line for side-band: 1 band + 0 data
                std::cerr << "Invalid pkt-line length: " << len << std::endl;
                break;
            }

            if (offset + len - 4 > sideband.size()) {
                std::cerr << "Pkt-line exceeds buffer size, stopping." << std::endl;
                break;
            }

            char band = sideband[offset]; // first byte of pkt-line
            std::cout<<"Band: "<<(int)band<<" Data: "<<sideband.substr(offset, len - 5)<<std::endl;
            std::string data = sideband.substr(offset, len - 5);
            if (data == "NAK") {
                state = READ_PACK;
            }
            if (band == 0x01) {
                // Append packfile data
                pack.append(sideband, offset + 1, len - 5);
            } else if (band == 0x02) {
                // progress messages, can log if needed
                std::cout << "Progress: " << sideband.substr(offset + 1, len - 5) << std::endl;
            } else if (band == 0x03) {
                std::cerr << "Error: " << sideband.substr(offset + 1, len - 5) << std::endl;
            }
            offset += (len - 4); // move to next pkt-line
        }
        if (state == READ_PACK) {
            // After NAK, the server sends raw packfile data without pkt-line framing
            pack.append(sideband, offset, sideband.size() - offset);
            break; // all remaining data is packfile
        }

    }
    std::cout<<"Pack: "<<pack;
    return pack;
}

int main(int argc, char *argv[])
{
    // Flush after every std::cout / std::cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    // You can use print statements as follows for debugging, they'll be visible when running tests.
    std::cerr << "Logs from your program will appear here!\n";

    // TODO: Uncomment the code below to pass the first stage
    //
    if (argc < 2) {
        std::cerr << "No command provided.\n";
        return EXIT_FAILURE;
    }
    
    std::string command = argv[1];
    
    if (command == "init") {
        try {
            std::filesystem::create_directory(".git");
            std::filesystem::create_directory(".git/objects");
            std::filesystem::create_directory(".git/refs");
    
            std::ofstream headFile(".git/HEAD");
            if (headFile.is_open()) {
                headFile << "ref: refs/heads/main\n";
                headFile.close();
            } else {
                std::cerr << "Failed to create .git/HEAD file.\n";
                return EXIT_FAILURE;
            }
    
            std::cout << "Initialized git directory\n";
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << e.what() << '\n';
            return EXIT_FAILURE;
        }
    } else if (command == "hash-object"){
        try{
            std::string flag = argv[2];
            if (flag == "-w") {
                std::string filename = argv[3];
                std::string file_hash = git_hash_object_write(filename);
                std::cout<<file_hash<<std::endl;
            } else {
                std::cerr<<"unknown params"<<std::endl;
            }
        }catch(std::exception &e) {
            std::cerr<<"Error in hash-object"<<std::endl;
        }
    } else if (command == "cat-file") {
        try{
            std::string flag = argv[2];
            if (flag == "-p") {
                std::string hash = argv[3];
                std::string content = git_cat_file(hash);
                std::cout<<content;
            } else {
                std::cerr<<"unknown params"<<std::endl;
            }
        }catch(std::exception &e) {
            std::cerr<<"Error in cat-file"<<std::endl;
        }
    } else if (command == "ls-tree") {
        bool name_only = false;
        for(int i=2; i<argc; i++) {
            if (std::string(argv[i]) == "--name-only") {
                name_only = true;
            }
        }
        std::string hash = argv[argc-1];
        git_ls_tree(hash, name_only);
    } else if (command == "write-tree") {
        std::string hash = git_write_tree();
        std::cout<<hash<<std::endl;
    } else if (command == "commit-tree") {
        std::string tree_hash = argv[2], parent_hash, msg;
        for (int i=3; i<argc; i++) {
            if (std::string(argv[i]) == "-p") {
                parent_hash = std::string(argv[++i]);
            } else if (std::string(argv[i]) == "-m") {
                msg = std::string(argv[++i]);
            }
        }

        std::string hash = git_commit_tree(tree_hash, parent_hash, msg);
        std::cout<<hash<<std::endl;
    } else if (command == "clone") {
        std::string base_url = argv[2];
        std::string folder;
        if (argc==4) {
            folder = argv[3];
        }

        // --- 1. Discover remote refs ---
        auto refs = get_remote_refs(base_url);

        // Find HEAD or first ref
        std::string want_hash, head_ref;
        for (auto& r : refs.refs) {
            if (r.name == "HEAD" || head_ref.empty()) {
                want_hash = r.hash;
                head_ref  = r.name;
            }
        }
        std::cout << "Fetching " << want_hash << " (" << head_ref << ")\n";

        // --- 2. Initialize .git in target folder ---
        std::string gitDir = folder + "/.git";
        std::filesystem::create_directories(gitDir + "/objects");
        std::filesystem::create_directories(gitDir + "/refs/heads");
        {
            std::ofstream head(gitDir + "/HEAD");
            head << "ref: refs/heads/main\n";
        }

        // --- 3. Fetch pack ---
        std::string pack_req = build_v0_fetch_request(want_hash);
        std::string response  = send_upload_pack_request(base_url, pack_req);
        std::string packData  = extract_packfile_from_response(response);

        // --- 4. Write objects ---
        PackResult r = process_pack_and_write_objects(gitDir, packData);
        std::cout << "Latest commit: " << r.commit_hash << "\n";
        std::cout << "Root tree:     " << r.tree_hash   << "\n";

        // --- 5. Checkout working tree ---
        if (!r.tree_hash.empty()) {
            checkout_tree(gitDir, r.tree_hash, folder);
            std::cout << "Checked out working tree to " << folder << "\n";
        }

        // --- 6. Write HEAD ref ---
        if (!r.commit_hash.empty()) {
            std::filesystem::create_directories(gitDir + "/refs/heads");
            std::ofstream mref(gitDir + "/refs/heads/main");
            mref << r.commit_hash << "\n";
        }
    }
    else {
        std::cerr << "Unknown command " << command << '\n';
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}
