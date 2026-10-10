PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
BUILD := build
TARGET := $(BUILD)/PS5NetworkMounting.elf
SRC := native/main.cpp native/mount_backend.cpp
INCLUDES := -Inative -I$(PS5_PAYLOAD_SDK)/target/include -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include
CXXFLAGS_PS5 := -std=c++17 -O2 -Wall -Wextra
LDFLAGS_PS5 := $(PS5_PAYLOAD_SDK)/target/lib/libmicrohttpd.a -L$(PS5_PAYLOAD_SDK)/target/lib -lpthread -lkernel_sys -lSceNet -lSceNetCtl

PS5NM_WITH_FUSE ?= 1
ifeq ($(PS5NM_WITH_FUSE),1)
ifndef ORBISDEV
$(error Set ORBISDEV to an SDK with the PS5-compatible FUSE and NFS libraries installed)
endif
SRC += native/nfs_fuse.cpp
CXXFLAGS_PS5 += -DPS5NM_WITH_FUSE -D_FILE_OFFSET_BITS=64 -D__ORBIS__=1 -I$(ORBISDEV)/usr/include -I$(ORBISDEV)/usr/include/orbis
LDFLAGS_PS5 += -L$(ORBISDEV)/usr/lib -lfuse -lorbisNfs -lorbis -lkernel_stub -lSceNet_stub -lSceNetCtl_stub -lSceSysUtil_stub -lSceSysmodule_stub -lkernel_sys_stub -lSceLibcInternal_stub
endif

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

.PHONY: all clean check-sdk
all: check-sdk $(TARGET)
check-sdk:
	@test -f "$(PS5_PAYLOAD_SDK)/toolchain/prospero.mk" || (echo "PS5 SDK toolchain not found"; exit 1)
	@test -f "$(PS5_PAYLOAD_SDK)/target/lib/libmicrohttpd.a" || (echo "libmicrohttpd.a missing"; exit 1)
$(TARGET): $(SRC) native/mount_backend.hpp native/web_assets.hpp native/nfs_fuse.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_PS5) $(INCLUDES) $(SRC) -o $@ $(LDFLAGS_PS5)
	@echo "Build complete. Enable PS5NM_WITH_FUSE=1 to compile the optional NFS FUSE adapter."
clean:
	rm -rf $(BUILD)
