#include <cstdio>

extern "C" {
int sceUserServiceInitialize(void*);
int sceUserServiceTerminate(void);
int sceSystemServiceLaunchWebBrowser(const char* uri, void* reserved);
}

int main() {
    const int init_result = sceUserServiceInitialize(nullptr);
    if (init_result != 0 && init_result != static_cast<int>(0x80550001)) {
        std::fprintf(stderr, "sceUserServiceInitialize failed: 0x%08x\n",
                     static_cast<unsigned>(init_result));
        return 1;
    }

    const int launch_result = sceSystemServiceLaunchWebBrowser("http://127.0.0.1:18080", nullptr);
    if (launch_result != 0) {
        std::fprintf(stderr, "sceSystemServiceLaunchWebBrowser failed: 0x%08x\n",
                     static_cast<unsigned>(launch_result));
    }
    (void)sceUserServiceTerminate();
    return launch_result < 0 ? 2 : 0;
}
