PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
BUILD := build
TARGET := $(BUILD)/PS5NetworkMounting.elf
SRC := native/main.cpp native/mount_backend.cpp
INCLUDES := -Inative -I$(PS5_PAYLOAD_SDK)/target/include -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include
CXXFLAGS_PS5 := -std=c++17 -O2 -Wall -Wextra
LDFLAGS_PS5 := $(PS5_PAYLOAD_SDK)/target/lib/libmicrohttpd.a -L$(PS5_PAYLOAD_SDK)/target/lib -lpthread -lkernel_sys -lSceNet -lSceNetCtl
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif
.PHONY: all clean check-sdk
all: check-sdk $(TARGET)
check-sdk:
	@test -f "$(PS5_PAYLOAD_SDK)/toolchain/prospero.mk" || (echo "PS5 SDK toolchain not found"; exit 1)
	@test -f "$(PS5_PAYLOAD_SDK)/target/lib/libmicrohttpd.a" || (echo "libmicrohttpd.a missing"; exit 1)
$(TARGET): $(SRC) native/mount_backend.hpp native/web_assets.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_PS5) $(INCLUDES) $(SRC) -o $@ $(LDFLAGS_PS5)
clean:
	rm -rf $(BUILD)
