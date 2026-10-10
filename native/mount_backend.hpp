#pragma once
#include <string>
namespace ps5nm {
struct MountStatus { bool mounted=false; std::string protocol, endpoint; std::string mountpoint="/hostapp"; std::string last_result="No operation yet"; std::string last_error; };
class MountBackend { public: MountStatus Status() const; bool Mount(const std::string&,const std::string&,int,const std::string&,const std::string&,std::string*); bool Unmount(const std::string&,std::string*); private: MountStatus status_; };
}
