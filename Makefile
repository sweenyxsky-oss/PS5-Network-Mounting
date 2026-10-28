PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
BUILD := build
TARGET := $(BUILD)/PS5NetworkMounting.elf
LAUNCHER := $(BUILD)/PS5NetworkMountingLauncher.elf
LAUNCHER_OBJECT := $(BUILD)/launcher.elf.o
SRC := native/main.cpp native/mount_backend.cpp native/shortcut.cpp native/diagnostic_log.cpp
PS5NM_EMBED_LAUNCHER ?= 1
INCLUDES := -Inative -I$(PS5_PAYLOAD_SDK)/target/include -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include
CXXFLAGS_PS5 := -std=c++17 -O2 -Wall -Wextra
LDFLAGS_PS5 := -L$(PS5_PAYLOAD_SDK)/target/lib -lpthread -lkernel_sys -lSceNet -lSceNetCtl -lSceSystemService -lSceUserService -lSceNotification

ifeq ($(PS5NM_EMBED_LAUNCHER),1)
CXXFLAGS_PS5 += -DPS5NM_EMBED_LAUNCHER_ELF -DPS5NM_HOME_BREW_DIR=\"/data/homebrew\"
else
CXXFLAGS_PS5 += -DPS5NM_HOME_BREW_DIR=\"$(abspath $(BUILD)/test-homebrew)\"
endif

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

ifeq ($(PS5NM_EMBED_LAUNCHER),1)
$(LAUNCHER): native/launcher.cpp | check-sdk
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(INCLUDES) $< -o $@ -L$(PS5_PAYLOAD_SDK)/target/lib -lkernel_sys -lSceSystemService -lSceUserService

$(LAUNCHER_OBJECT): $(LAUNCHER) | check-sdk
	cd $(BUILD) && $(PS5_PAYLOAD_SDK)/bin/prospero-objcopy --input-target=binary --output-target=elf64-x86-64 --binary-architecture=i386:x86-64 --redefine-sym _binary_PS5NetworkMountingLauncher_elf_start=ps5nm_launcher_start --redefine-sym _binary_PS5NetworkMountingLauncher_elf_end=ps5nm_launcher_end $(notdir $(LAUNCHER)) $(notdir $@)
	$(PS5_PAYLOAD_SDK)/bin/prospero-objcopy --rename-section .data=.rodata,alloc,load,readonly,data,contents $@
endif

$(TARGET): $(SRC) native/mount_backend.hpp native/shortcut.hpp native/diagnostic_log.hpp native/json.hpp native/web_assets.hpp native/nfs_fuse.hpp native/smb_fuse.hpp $(if $(filter 1,$(PS5NM_EMBED_LAUNCHER)),$(LAUNCHER_OBJECT)) | check-sdk $(CHECK_FUSE)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_PS5) $(INCLUDES) $(SRC) $(if $(filter 1,$(PS5NM_EMBED_LAUNCHER)),$(LAUNCHER_OBJECT)) -o $@ $(LDFLAGS_PS5)
	@echo "Build complete. NFS and SMB FUSE adapters were included when PS5NM_WITH_FUSE=1."

clean:
	rm -rf $(BUILD)
