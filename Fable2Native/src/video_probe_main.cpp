#include "f2/native_video_decoder.h"

#include <objbase.h>

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: f2native_video_probe <video.mp4>\n";
        return 2;
    }
    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        std::cerr << "COM could not initialize\n";
        return 1;
    }
    std::string error;
    if (!f2::start_native_video_runtime(error)) {
        std::cerr << error << "\n";
        return 1;
    }
    f2::NativeVideoDecoder decoder;
    f2::NativeVideoFrame frame;
    const bool opened = decoder.open(argv[1], error);
    const bool decoded = opened && decoder.read_next_frame(frame, error);
    if (!decoded) std::cerr << error << "\n";
    if (decoded) {
        std::cout << "decoded video frame\n"
                  << "  dimensions: " << frame.width << "x" << frame.height << "\n"
                  << "  rgba_bytes: " << frame.rgba8.size() << "\n"
                  << "  serial: " << frame.serial << "\n";
    }
    decoder.close();
    f2::stop_native_video_runtime();
    if (SUCCEEDED(com_result)) CoUninitialize();
    return decoded ? 0 : 1;
}
