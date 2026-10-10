PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
BUILD := build
TARGET := $(BUILD)/PS5NetworkMounting.elf
SRC := native/main.cpp native/mount_backend.cpp
INCLUDES := -Inative -I$(PS5_PAYLOAD_SDK)/target/include -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include
CXXFLAGS_PS5 := -std=c++17 -O2 -Wall -Wextra
LDFLAGS_PS5 := -L$(PS5_PAYLOAD_SDK)/target/lib -lpthread -lkernel_sys -lSceNet -lSceNetCtl

PS5NM_WITH_FUSE ?= 1
ITEMZFLOW_ROOT ?= ../Itemzflow
ifeq ($(PS5NM_WITH_FUSE),1)
ifndef ORBISDEV
$(error Set ORBISDEV to an SDK with PS5-compatible libfuse, libnfs, and libsmb2 installed)
endif
ifeq ($(wildcard $(ITEMZFLOW_ROOT)/itemz-daemon/include/fuse/fuse.h),)
$(error Set ITEMZFLOW_ROOT to the Itemzflow source checkout containing itemz-daemon/include/fuse/fuse.h)
endif
SRC += native/nfs_fuse.cpp native/smb_fuse.cpp
CXXFLAGS_PS5 += -DPS5NM_WITH_FUSE -DFUSE_USE_VERSION=26 -D_FILE_OFFSET_BITS=64 -D__ORBIS__=1 -D_ORBIS_ -D__Freebsd__ -I$(ITEMZFLOW_ROOT)/itemz-daemon/include -I$(ORBISDEV)/include -I$(ORBISDEV)/include/orbis
LDFLAGS_PS5 += -L$(ORBISDEV)/lib -lfuse -lnfs -lsmb2 -lssl -lcrypto -lz
CHECK_FUSE := check-fuse-sdk
else
CHECK_FUSE :=
endif

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

.PHONY: all clean check-sdk check-fuse-sdk embed-ui
all: $(TARGET)

check-sdk:
	@test -f "$(PS5_PAYLOAD_SDK)/toolchain/prospero.mk" || (echo "PS5 SDK toolchain not found"; exit 1)

check-fuse-sdk:
	@test -f "$(ITEMZFLOW_ROOT)/itemz-daemon/include/fuse/fuse.h" || (echo "Itemzflow patched FUSE headers missing"; exit 1)
	@test -f "$(ORBISDEV)/include/nfsc/libnfs.h" || (echo "ORBISDEV libnfs headers missing"; exit 1)
	@test -f "$(ORBISDEV)/include/smb2/libsmb2.h" || (echo "ORBISDEV libsmb2 headers missing"; exit 1)
	@test -f "$(ORBISDEV)/lib/libfuse.a" || (echo "ORBISDEV libfuse.a missing"; exit 1)
	@test -f "$(ORBISDEV)/lib/libnfs.a" || (echo "ORBISDEV libnfs.a missing"; exit 1)
	@test -f "$(ORBISDEV)/lib/libsmb2.a" || (echo "ORBISDEV libsmb2.a missing"; exit 1)
	@test -f "$(ORBISDEV)/lib/libcrypto.a" || (echo "ORBISDEV libcrypto.a missing"; exit 1)
	@test -f "$(ORBISDEV)/lib/libssl.a" || (echo "ORBISDEV libssl.a missing"; exit 1)
	@test -f "$(ORBISDEV)/lib/libz.a" || (echo "ORBISDEV libz.a missing"; exit 1)

native/web_assets.hpp: web/index.html tools/embed_ui.py
	python3 tools/embed_ui.py $< $@

embed-ui: native/web_assets.hpp

$(TARGET): $(SRC) native/mount_backend.hpp native/web_assets.hpp native/nfs_fuse.hpp native/smb_fuse.hpp | check-sdk $(CHECK_FUSE)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_PS5) $(INCLUDES) $(SRC) -o $@ $(LDFLAGS_PS5)
	@echo "Build complete. NFS and SMB FUSE adapters were included when PS5NM_WITH_FUSE=1."

clean:
	rm -rf $(BUILD)
