#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <zlib.h>
#include<vector>
#include <openssl/sha.h>
#include <stdio.h>
#include <cassert>

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

int def(FILE *source, FILE *dest, int level){
    int ret, flush;
    unsigned have;
    z_stream strm;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];

    /* allocate deflate state */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    ret = deflateInit(&strm, level);
    if (ret != Z_OK) return ret;

    do{
        strm.avail_in = fread(in, 1, CHUNK, source);
        if (ferror(source)) {
            (void)deflateEnd(&strm);
            return Z_ERRNO;
        }
        flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;
        strm.next_in = in;
        do{
            strm.avail_out = CHUNK;
            strm.next_out = out;
            ret = deflate(&strm, flush);    /* no bad return value */
            assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
            have = CHUNK - strm.avail_out;
            if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
                (void)deflateEnd(&strm);
                return Z_ERRNO;
            }
        }while (strm.avail_out == 0);
        assert(strm.avail_in == 0);

    }while (flush != Z_FINISH);
    assert(ret == Z_STREAM_END);
    (void)deflateEnd(&strm);
    return Z_OK;
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

        std::filesystem::path file_path = "./.git/objects/" + blob_sha.substr(0,2) + "/" + blob_sha.substr(2);

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
    else if(command=="hash-object"){
        std::string mode = argv[2];
        std::string file;

        if(mode=="-w"){
            file = argv[3];
        }
        else file = mode;
        
        std::string file_path = "./" + file;

        std::FILE *input_file;
        input_file = fopen(file_path.c_str(), "r");

        if(input_file==NULL){
            std::cerr<<"Could not open file";
            return EXIT_FAILURE;
        }
        
        char ch;
        std::string contents;
        while ((ch = fgetc(input_file)) != EOF){
            contents += ch;
        }

        unsigned char in[4+(std::to_string(contents.size())).size()+contents.size()] = "blob ";
        std::cout << 6+(std::to_string(contents.size())).size()+contents.size()<<"\n";
        int idx = 5;
        for(int i = 0; i < (std::to_string(contents.size())).size();i++){
            in[idx] = std::to_string(contents.size())[i];
            idx++;
        }

        in[idx] = "\0";
        idx++;

        for(int i = 0; i < contents.size(); i++){
            in[idx] = contents[i];
            idx++;
        }

        std::cout << contents << "\n";
        std::cout << in << "\n";
        unsigned char out[40];

        SHA1(in, sizeof(in)-1, out);
        std::cout << out << "\n";
        std::string hash(reinterpret_cast<char*>(out));
        std::cout<<hash;

        if(mode=="-w"){
           std::filesystem::create_directory("./.git/objects" + hash.substr(0,2));

           std::FILE* output_file;
           std::string output_path = "./.git/objects/" + hash.substr(0,2) + "/" + hash.substr(2);

           output_file = fopen(output_path.c_str(),"w");

           if(output_file == NULL){
            std::cerr<<"Could not create file";
            return EXIT_FAILURE;
           }

           def(input_file,output_file,-1);
           fclose(output_file);
        }

        fclose(input_file);
    }
    else {
        std::cerr << "Unknown command " << command << '\n';
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}
