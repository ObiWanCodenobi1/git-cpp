#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <zlib.h>
#include<vector>>

#define CHUNK 16384

std::string decompress(std::string& compressed){
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("inflateInit failed");
    }

    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
    zs.avail_in = static_cast<uInt>(compressed.size());

    std::string out;
    std::vector<char> buffer(32768);

    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buffer.data());
        zs.avail_out = static_cast<uInt>(buffer.size());

        ret = inflate(&zs, 0);
        if (ret != Z_OK && ret != Z_STREAM_END) {
        inflateEnd(&zs);
        throw std::runtime_error("inflate failed");
        }

        out.append(buffer.data(), buffer.size() - zs.avail_out);
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    return out;

        
}

int main(int argc, char *argv[])
{
    // Flush after every std::cout / std::cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    // You can use print statements as follows for debugging, they'll be visible when running tests.
    std::cerr << "Logs from your program will appear here!\n";

    // TODO: Uncomment the code below to pass the first stage
    
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
    }
    else if(command=="cat-file"){
        std::string mode = argv[2];
        if (mode != "-p") {
            std::cerr << mode << '\n';
            return EXIT_FAILURE;
        }
        std::string blob_sha = argv[3];

        std::filesystem::path file_path = ".git/objects" + blob_sha.substr(0,2) + "/" + blob_sha(2);

        if (!exists(file_path)) {
            std::cerr << "Object not found.\n";
            return EXIT_FAILURE;
        }

        std::ifstream catfile(file_path);
        std::string compressed="",line;

        while(getline(catfile,line)){
            compressed += line;
        }

        catfile.close();

        std::string decompressed = decompress(compressed);

        int idx = decompressed.find('\0');
        std::cout << decompressed.substr(idx+1);

    }
    else {
        std::cerr << "Unknown command " << command << '\n';
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}
