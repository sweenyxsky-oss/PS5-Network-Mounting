# PS5 Network Mounting
#
# The payload serves its shares in-process: libnfs and libsmb2 are linked in, and the
# C library's file calls are redirected through native/posix_shim.cpp with --wrap.
# There is no FUSE and no Itemzflow dependency.
#
#   make                 # PS5 ELF (needs PS5_PAYLOAD_SDK and ORBISDEV)
#   make test            # host build, unit tests and the HTTP smoke test
#   make embed-ui        # regenerate native/web_assets.hpp from web/index.html

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
ORBISDEV ?= $(PS5_PAYLOAD_SDK)/target/user/homebrew
BUILD := build

TARGET := $(BUILD)/PS5NetworkMounting.elf
LAUNCHER := $(BUILD)/PS5NetworkMountingLauncher.elf
LAUNCHER_OBJECT := $(BUILD)/launcher.elf.o

CORE_SRC := native/main.cpp native/mount_backend.cpp native/vfs.cpp \
            native/shortcut.cpp native/diagnostic_log.cpp
SHARE_SRC := native/nfs_share.cpp native/smb_share.cpp native/posix_shim.cpp
SRC := $(CORE_SRC) $(SHARE_SRC)
HEADERS := native/share.hpp native/vfs.hpp native/mount_backend.hpp native/shortcut.hpp \
           native/diagnostic_log.hpp native/json.hpp native/web_assets.hpp

PS5NM_EMBED_LAUNCHER ?= 1
WRAPS := $(shell sed -e 's/#.*//' -e 's/[[:space:]]//g' -e '/^$$/d' native/wrap.list | sed 's/^/-Wl,--wrap=/')

INCLUDES := -Inative -I$(PS5_PAYLOAD_SDK)/target/include -I$(ORBISDEV)/include -I$(ORBISDEV)/include/orbis
CXXFLAGS_PS5 := -std=c++17 -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -D__ORBIS__=1 -D_ORBIS_
SHARE_LIBS := -lnfs -lsmb2 -lssl -lcrypto -lz
SYSTEM_LIBS := -lpthread -lkernel_sys -lSceNet -lSceNetCtl -lSceSystemService -lSceUserService -lSceNotification
LDFLAGS_PS5 := -L$(PS5_PAYLOAD_SDK)/target/lib -L$(ORBISDEV)/lib $(SHARE_LIBS) $(SYSTEM_LIBS)

ifeq ($(PS5NM_EMBED_LAUNCHER),1)
CXXFLAGS_PS5 += -DPS5NM_EMBED_LAUNCHER_ELF -DPS5NM_HOME_BREW_DIR=\"/data/homebrew\"
else
CXXFLAGS_PS5 += -DPS5NM_HOME_BREW_DIR=\"$(abspath $(BUILD)/test-homebrew)\"
endif

# The host build does not define _FILE_OFFSET_BITS=64 (glibc would redirect stat/open/fopen to
# their *64 aliases) and switches fortify off (it would redirect realpath to __realpath_chk).
# Either redirect would bypass the wrappers the tests exist to exercise.
HOST_FLAGS := -std=c++17 -O2 -Wall -Wextra -Inative -DPS5NM_HOST -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
HOST := $(BUILD)/ps5nm-host
HOST_SRC := native/main.cpp native/mount_backend.cpp native/vfs.cpp native/shortcut.cpp \
            native/diagnostic_log.cpp tests/net_stubs.cpp
TESTS := $(BUILD)/test-json $(BUILD)/test-vfs $(BUILD)/test-shim

ifneq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

.PHONY: all clean host test embed-ui check-deps check-arch

all: $(TARGET)

$(BUILD):
	@mkdir -p $(BUILD)

check-deps:
	@test -f "$(PS5_PAYLOAD_SDK)/toolchain/prospero.mk" || (echo "PS5 SDK toolchain not found at $(PS5_PAYLOAD_SDK)"; exit 1)
	@test -f "$(ORBISDEV)/include/nfsc/libnfs.h" || (echo "libnfs headers missing under $(ORBISDEV)"; exit 1)
	@test -f "$(ORBISDEV)/include/smb2/libsmb2.h" || (echo "libsmb2 headers missing under $(ORBISDEV)"; exit 1)
	@test -f "$(ORBISDEV)/lib/libnfs.a" || (echo "libnfs.a missing under $(ORBISDEV)/lib"; exit 1)
	@test -f "$(ORBISDEV)/lib/libsmb2.a" || (echo "libsmb2.a missing under $(ORBISDEV)/lib"; exit 1)
	@test -f "$(ORBISDEV)/lib/libssl.a" || (echo "libssl.a missing under $(ORBISDEV)/lib"; exit 1)
	@test -f "$(ORBISDEV)/lib/libcrypto.a" || (echo "libcrypto.a missing under $(ORBISDEV)/lib"; exit 1)
	@test -f "$(ORBISDEV)/lib/libz.a" || (echo "libz.a missing under $(ORBISDEV)/lib"; exit 1)
	@test -s native/wrap.list || (echo "native/wrap.list is missing or empty"; exit 1)

# The in-process mount is only as complete as this list: every wrapped call must have a
# __wrap_ definition, and every definition must be listed.
check-arch:
	@for name in $$(sed -e 's/#.*//' -e 's/[[:space:]]//g' -e '/^$$/d' native/wrap.list); do \
		grep -q "__wrap_$$name(" native/posix_shim.cpp || (echo "wrap.list names $$name but posix_shim.cpp has no __wrap_$$name"; exit 1); \
	done
	@grep -q 'Wl,--wrap' Makefile || (echo "Makefile does not pass --wrap"; exit 1)
	@! echo "$(SRC) $(SHARE_LIBS) $(INCLUDES)" | grep -qi fuse || (echo "The build still pulls in FUSE"; exit 1)
	@grep -q 'posix_shim.cpp' Makefile || (echo "Makefile does not build the shim"; exit 1)

native/web_assets.hpp: web/index.html tools/embed_ui.py
	python3 tools/embed_ui.py $< $@

embed-ui: native/web_assets.hpp

ifeq ($(PS5NM_EMBED_LAUNCHER),1)
$(LAUNCHER): native/launcher.cpp | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(INCLUDES) $< -o $@ -L$(PS5_PAYLOAD_SDK)/target/lib -lkernel_sys -lSceSystemService -lSceUserService

$(LAUNCHER_OBJECT): $(LAUNCHER)
	cd $(BUILD) && $(PS5_PAYLOAD_SDK)/bin/prospero-objcopy --input-target=binary --output-target=elf64-x86-64 --binary-architecture=i386:x86-64 --redefine-sym _binary_PS5NetworkMountingLauncher_elf_start=ps5nm_launcher_start --redefine-sym _binary_PS5NetworkMountingLauncher_elf_end=ps5nm_launcher_end $(notdir $(LAUNCHER)) $(notdir $@)
	$(PS5_PAYLOAD_SDK)/bin/prospero-objcopy --rename-section .data=.rodata,alloc,load,readonly,data,contents $@
endif

$(TARGET): $(SRC) $(HEADERS) $(if $(filter 1,$(PS5NM_EMBED_LAUNCHER)),$(LAUNCHER_OBJECT)) | $(BUILD) check-deps check-arch
	$(CXX) $(CXXFLAGS_PS5) $(INCLUDES) $(SRC) $(if $(filter 1,$(PS5NM_EMBED_LAUNCHER)),$(LAUNCHER_OBJECT)) $(WRAPS) -o $@ $(LDFLAGS_PS5)
	@echo "Built $@ (in-process NFS + SMB share layer, $(words $(WRAPS)) wrapped calls)"

host: $(HOST)

$(HOST): $(HOST_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(HOST_FLAGS) $(HOST_SRC) -o $@ -pthread

$(BUILD)/test-json: tests/test_json.cpp native/json.hpp | $(BUILD)
	$(CXX) $(HOST_FLAGS) $< -o $@

$(BUILD)/test-vfs: tests/vfs_test.cpp tests/fake_backend.hpp native/vfs.cpp native/diagnostic_log.cpp | $(BUILD)
	$(CXX) $(HOST_FLAGS) tests/vfs_test.cpp native/vfs.cpp native/diagnostic_log.cpp -o $@ -pthread

$(BUILD)/test-shim: tests/shim_host_test.cpp tests/fake_backend.hpp native/vfs.cpp native/posix_shim.cpp native/diagnostic_log.cpp | $(BUILD)
	$(CXX) $(HOST_FLAGS) $(WRAPS) tests/shim_host_test.cpp native/vfs.cpp native/posix_shim.cpp native/diagnostic_log.cpp -o $@ -pthread

test: $(TESTS) host check-arch
	$(BUILD)/test-json
	$(BUILD)/test-vfs
	$(BUILD)/test-shim
	python3 tests/http_smoke.py

clean:
	rm -rf $(BUILD)
