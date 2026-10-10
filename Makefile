PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
CXX ?= $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
BUILD := build
TARGET := $(BUILD)/PS5NetworkMounting.elf
INCLUDES := -Inative -I$(PS5_PAYLOAD_SDK)/target/include -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDFLAGS := -L$(PS5_PAYLOAD_SDK)/target/lib -lmicrohttpd -lpthread -lkernel_sys -lSceNet -lSceNetCtl
.PHONY: all clean check-sdk embed
all: check-sdk embed $(TARGET)
check-sdk:
	@test -d "$(PS5_PAYLOAD_SDK)/target/include" || (echo "PS5 SDK not found"; exit 1)
	@test -f "$(PS5_PAYLOAD_SDK)/target/lib/libmicrohttpd.a" || (echo "libmicrohttpd.a missing"; exit 1)
embed:
	python3 tools/embed_web.py
$(TARGET): native/main.cpp native/mount_backend.cpp native/mount_backend.hpp native/web_assets.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) native/main.cpp native/mount_backend.cpp -o $@ $(LDFLAGS)
	@echo "HTTP payload built; network filesystem mounting remains disabled until the PS5 FUSE/NFS adapter is integrated."
clean:
	rm -rf $(BUILD) native/web_assets.hpp
