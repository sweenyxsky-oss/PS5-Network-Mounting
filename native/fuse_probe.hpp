#pragma once

#include <string>

namespace ps5nm {

// What this build can see of the kernel's FUSE support. The payload itself never uses FUSE (it
// serves shares in-process), so this exists only to answer "does this console have fusefs?".
struct FuseProbe {
    unsigned uid = 0;
    unsigned euid = 0;
    int jailed = -1;              // -1 = not reported by this kernel, otherwise security.jail.jailed
    bool kernel_checked = false;  // true once the filesystem type itself was tested
    bool type_registered = false; // the kernel knows a fusefs filesystem type
    int type_errno = 0;           // errno of the check when it could not decide, 0 when it succeeded
    bool dev_present = false;
    bool dev_opens = false;
    int dev_errno = 0;
    bool hostapp_present = false;
    std::string hostapp_fstype;
    bool sandbox_present = false;  // /mnt/sandbox visible: where app sandboxes live
    std::string check_method;      // how the type was checked, for the log

    // "registered", "missing", or "unknown" when the platform cannot answer.
    std::string KernelType() const;
    std::string Report() const;  // multi-line, human readable
    std::string Hint() const;    // one sentence naming the most likely blocker
};

// kernel_probe asks the kernel directly on platforms where that is possible (PS5/FreeBSD); it is
// harmless but does attempt one mount that is never allowed to succeed. Pass false for startup.
FuseProbe ProbeFuseEnvironment(bool kernel_probe = true);

}  // namespace ps5nm
