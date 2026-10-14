#define WIN32_LEAN_AND_MEAN
#include "Gui.h"
#include <iostream>
namespace {
    struct QuietConsole : std::streambuf {
        int_type overflow(int_type c) override { return traits_type::not_eof(c); }
        std::streamsize xsputn(const char*,std::streamsize count) override { return count; }
    };
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Typed events are the GUI interface; retain console formatting in the CLI.
    QuietConsole quiet;
    auto* out=std::cout.rdbuf(&quiet);auto* error=std::cerr.rdbuf(&quiet);
    int result=RunGui(instance, show);
    std::cout.rdbuf(out);std::cerr.rdbuf(error);return result;
}
