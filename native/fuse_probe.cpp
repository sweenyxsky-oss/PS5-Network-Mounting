#include "fuse_probe.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__FreeBSD__) || defined(__ORBIS__) || defined(_ORBIS_)
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#else
#include <sys/vfs.h>
#endif

#include "diagnostic_log.hpp"

namespace ps5nm {
namespace {

std::string WithErrno(int error) {
    if (error == 0) return "no error";
    return std::string(std::strerror(error)) + " (errno " + std::to_string(error) + ")";
}

void CheckDevFuse(FuseProbe* probe) {
    struct stat info {};
    errno = 0;
    if (stat("/dev/fuse", &info) != 0) {
        probe->dev_present = false;
        probe->dev_errno = errno;
        return;
    }
    probe->dev_present = true;
    errno = 0;
    const int fd = open("/dev/fuse", O_RDWR);
    if (fd >= 0) {
        probe->dev_opens = true;
        close(fd);
    } else {
        probe->dev_errno = errno;
    }
}

#if defined(__FreeBSD__) || defined(__ORBIS__) || defined(_ORBIS_)
// The kernel has no "list filesystem types" call, so ask it for a fusefs mount that can never
// succeed: no "fd" option is supplied. ENODEV means the type is not registered at all; any other
// refusal means the type exists but the request was incomplete, which is all this probe needs.
void CheckKernelType(FuseProbe* probe) {
    std::string scratch = DiagnosticLogPath();
    const size_t slash = scratch.find_last_of('/');
    scratch = (slash == std::string::npos ? std::string(".") : scratch.substr(0, slash)) + "/.fuseprobe";
    if (mkdir(scratch.c_str(), 0775) != 0 && errno != EEXIST) {
        probe->check_method = "scratch directory unavailable: " + WithErrno(errno);
        return;
    }
    struct iovec parts[] = {
        {const_cast<char*>("fstype"), 7}, {const_cast<char*>("fusefs"), 7},
        {const_cast<char*>("fspath"), 7}, {const_cast<char*>(scratch.c_str()), scratch.size() + 1},
    };
    errno = 0;
    const int rc = nmount(parts, 4, 0);
    probe->check_method = "nmount(fusefs) probe";
    if (rc == 0) {
        // Should be impossible without an fd; never leave anything mounted.
        (void)unmount(scratch.c_str(), MNT_FORCE);
        probe->kernel_checked = true;
        probe->type_registered = true;
    } else if (errno == ENODEV) {
        probe->kernel_checked = true;
        probe->type_registered = false;
        probe->type_errno = errno;
    } else {
        // EINVAL is ambiguous: a registered fusefs refuses an fd-less mount with it, and so does an
        // unknown type on some kernels. Report it instead of guessing.
        probe->type_errno = errno;
    }
    (void)rmdir(scratch.c_str());
}

void ReadJail(FuseProbe* probe) {
    int jailed = 0;
    size_t size = sizeof(jailed);
    if (sysctlbyname("security.jail.jailed", &jailed, &size, nullptr, 0) == 0) probe->jailed = jailed;
}

void ReadHostAppType(FuseProbe* probe) {
    if (!probe->hostapp_present) return;
    struct statfs info {};
    if (statfs("/hostapp", &info) == 0) probe->hostapp_fstype = info.f_fstypename;
}
#else
// Linux host build: the kernel publishes its filesystem types, so read that instead of mounting.
void CheckKernelType(FuseProbe* probe) {
    probe->check_method = "/proc/filesystems";
    FILE* list = std::fopen("/proc/filesystems", "r");
    if (list == nullptr) {
        probe->check_method = std::string("/proc/filesystems unreadable: ") + WithErrno(errno);
        return;
    }
    char line[256];
    bool found = false;
    while (std::fgets(line, sizeof(line), list) != nullptr) {
        char first[64] = {};
        if (std::sscanf(line, "%63s", first) != 1) continue;
        // Each line is either "fuse" or "nodev\tfuse".
        if (std::strcmp(first, "nodev") == 0 && std::sscanf(line, "%*s %63s", first) != 1) continue;
        if (std::strcmp(first, "fuse") == 0 || std::strcmp(first, "fuseblk") == 0) {
            found = true;
            break;
        }
    }
    std::fclose(list);
    probe->kernel_checked = true;
    probe->type_registered = found;
}

void ReadHostAppType(FuseProbe* probe) {
    if (!probe->hostapp_present) return;
    struct statfs info {};
    if (statfs("/hostapp", &info) == 0) {
        // FUSE_SUPER_MAGIC from linux/magic.h, spelled out to avoid another header dependency.
        probe->hostapp_fstype = info.f_type == 0x65735546 ? "fuse" : "other";
    }
}
#endif

}  // namespace

std::string FuseProbe::KernelType() const {
    if (!kernel_checked) return "unknown";
    return type_registered ? "registered" : "missing";
}

std::string FuseProbe::Report() const {
    std::string out;
    out += "uid=" + std::to_string(uid) + " euid=" + std::to_string(euid) +
           " jailed=" + (jailed < 0 ? std::string("unknown") : std::to_string(jailed)) + "\n";

    if (!kernel_checked) {
        out += "fusefs kernel type: unknown (" + check_method +
               (type_errno ? ", probe refused with " + WithErrno(type_errno) : std::string()) + ")\n";
    } else if (type_registered) {
        out += "fusefs kernel type: registered (" + check_method + ")\n";
    } else {
        out += "fusefs kernel type: NOT registered (" + check_method + ")\n";
    }

    if (!dev_present) out += "/dev/fuse: missing (" + WithErrno(dev_errno) + ")\n";
    else if (dev_opens) out += "/dev/fuse: present, opens read/write\n";
    else out += "/dev/fuse: present, but cannot be opened (" + WithErrno(dev_errno) + ")\n";

    out += hostapp_present
        ? "/hostapp: present" + (hostapp_fstype.empty() ? std::string() : " (fstype=" + hostapp_fstype + ")") + "\n"
        : "/hostapp: missing\n";
    out += sandbox_present ? "/mnt/sandbox: visible\n" : "/mnt/sandbox: not visible\n";
    out += "not required by this payload: shares are served in-process, no FUSE mount is made.";
    return out;
}

std::string FuseProbe::Hint() const {
    std::string hint;
    const auto add = [&hint](const std::string& sentence) {
        if (!hint.empty()) hint += " ";
        hint += sentence;
    };
    if (!kernel_checked) {
        add("The kernel could not be asked whether it has fusefs (" + check_method + ").");
    } else if (!type_registered) {
        add("The kernel does not have the fusefs filesystem type, so no FUSE mount can work here.");
    }
    if (!dev_present) {
        add("There is no /dev/fuse (" + WithErrno(dev_errno) + ").");
    } else if (!dev_opens) {
        add("/dev/fuse exists but cannot be opened (" + WithErrno(dev_errno) + ").");
    }
    if (jailed == 1) add("The process is jailed.");
    if (euid != 0 && (!kernel_checked || type_errno == EPERM || type_errno == EACCES))
        add("The probe did not run as root, so a refusal may be a permission problem.");
    if (hint.empty()) {
        add("fusefs is available to this process; it is still unused here because the payload serves shares in-process.");
    }
    return hint;
}

FuseProbe ProbeFuseEnvironment(bool kernel_probe) {
    FuseProbe probe;
    probe.uid = static_cast<unsigned>(getuid());
    probe.euid = static_cast<unsigned>(geteuid());

    struct stat info {};
    probe.hostapp_present = stat("/hostapp", &info) == 0;
    probe.sandbox_present = stat("/mnt/sandbox", &info) == 0;

#if defined(__FreeBSD__) || defined(__ORBIS__) || defined(_ORBIS_)
    ReadJail(&probe);
#endif
    if (kernel_probe) {
        CheckKernelType(&probe);
    } else {
        probe.check_method = "startup: type not checked";
    }
    CheckDevFuse(&probe);
    ReadHostAppType(&probe);

    LogEvent("INFO", "probe", "fusefs " + probe.KernelType() + "; " + probe.Hint());
    return probe;
}

}  // namespace ps5nm
