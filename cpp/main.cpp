#include "scanner.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int run(const std::filesystem::path& command, const std::filesystem::path& input) {
    using namespace dd;
    if (command == "scan" || command == "list"
#ifdef _WIN32
        || command == "layout"
#endif
    ) {
#ifdef _WIN32
        if (_setmode(_fileno(stdout), _O_BINARY) == -1)
            throw std::runtime_error("cannot set binary stdout");
#endif
        Encoder writer(std::cout);
        Sink write = [&](const Record& r) { return writer.add(r); };
#ifdef _WIN32
        if (command == "layout") scan_ntfs_layout(input, write);
        else
#endif
            scan(input, write, command == "scan");
        writer.finish();
    } else if (command == "replay" || command == "summary") {
        Index index;
        Sink collect = [&](const Record& r) { return index.add(r); };
        if (command == "replay") {
            std::ifstream in(input, std::ios::binary);
            if (!in) throw std::runtime_error("cannot open capture");
            replay(in, collect);
        } else {
            scan(input, collect);
        }
        index.summarize(std::cout);
    } else {
        throw std::invalid_argument("unknown command");
    }
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    std::ios::sync_with_stdio(false);
    try {
        if (argc != 3) throw std::invalid_argument("usage: disk-diver-cpp scan|list|summary|replay PATH");
        return run(argv[1], argv[2]);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
