CC ?= cc
AR ?= ar
RANLIB ?= ranlib

CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS ?= -ldl -lpthread -lm

TARGET ?= nesd
BUILD_DIR ?= .build
FCEUMM_DIR ?=
FCEUMM_GIT_VERSION ?= unknown
FCEUMM_ENDIAN_CPPFLAGS ?=

ifeq ($(strip $(FCEUMM_DIR)),)
$(error FCEUMM_DIR must point to the pinned libretro-fceumm source tree)
endif

HOST_SOURCES := main.c host.c http.c rpc_client.c jpeg_soft.c sha256.c \
	fceumm_bind.c
HOST_OBJECTS := $(HOST_SOURCES:%.c=$(BUILD_DIR)/host/%.o)

FCEUMM_CORE_DIR := $(FCEUMM_DIR)/src
FCEUMM_COMMON_DIR := $(FCEUMM_CORE_DIR)/drivers/libretro/libretro-common
FCEUMM_SOURCES := \
	$(sort $(wildcard $(FCEUMM_CORE_DIR)/boards/*.c)) \
	$(sort $(wildcard $(FCEUMM_CORE_DIR)/input/*.c)) \
	$(FCEUMM_CORE_DIR)/ntsc/nes_ntsc.c \
	$(FCEUMM_CORE_DIR)/drivers/libretro/libretro.c \
	$(FCEUMM_CORE_DIR)/drivers/libretro/libretro_dipswitch.c \
	$(FCEUMM_CORE_DIR)/cart.c \
	$(FCEUMM_CORE_DIR)/cheat.c \
	$(FCEUMM_CORE_DIR)/crc32.c \
	$(FCEUMM_CORE_DIR)/fceu-endian.c \
	$(FCEUMM_CORE_DIR)/fceu-memory.c \
	$(FCEUMM_CORE_DIR)/fceu.c \
	$(FCEUMM_CORE_DIR)/fds.c \
	$(FCEUMM_CORE_DIR)/fds_apu.c \
	$(FCEUMM_CORE_DIR)/file.c \
	$(FCEUMM_CORE_DIR)/filter.c \
	$(FCEUMM_CORE_DIR)/general.c \
	$(FCEUMM_CORE_DIR)/input.c \
	$(FCEUMM_CORE_DIR)/md5.c \
	$(FCEUMM_CORE_DIR)/nsf.c \
	$(FCEUMM_CORE_DIR)/palette.c \
	$(FCEUMM_CORE_DIR)/ppu.c \
	$(FCEUMM_CORE_DIR)/sound.c \
	$(FCEUMM_CORE_DIR)/state.c \
	$(FCEUMM_CORE_DIR)/video.c \
	$(FCEUMM_CORE_DIR)/vsuni.c \
	$(FCEUMM_CORE_DIR)/ines.c \
	$(FCEUMM_CORE_DIR)/unif.c \
	$(FCEUMM_CORE_DIR)/x6502.c \
	$(FCEUMM_COMMON_DIR)/streams/memory_stream.c \
	$(FCEUMM_COMMON_DIR)/compat/compat_posix_string.c \
	$(FCEUMM_COMMON_DIR)/compat/compat_snprintf.c \
	$(FCEUMM_COMMON_DIR)/compat/compat_strcasestr.c \
	$(FCEUMM_COMMON_DIR)/compat/compat_strl.c \
	$(FCEUMM_COMMON_DIR)/compat/fopen_utf8.c \
	$(FCEUMM_COMMON_DIR)/encodings/encoding_utf.c \
	$(FCEUMM_COMMON_DIR)/file/file_path.c \
	$(FCEUMM_COMMON_DIR)/file/file_path_io.c \
	$(FCEUMM_COMMON_DIR)/streams/file_stream.c \
	$(FCEUMM_COMMON_DIR)/streams/file_stream_transforms.c \
	$(FCEUMM_COMMON_DIR)/string/stdstring.c \
	$(FCEUMM_COMMON_DIR)/time/rtime.c \
	$(FCEUMM_COMMON_DIR)/vfs/vfs_implementation.c

FCEUMM_OBJECTS := \
	$(patsubst $(FCEUMM_DIR)/%.c,$(BUILD_DIR)/fceumm/%.o,$(FCEUMM_SOURCES))
FCEUMM_LIBRARY := $(BUILD_DIR)/libfceumm.a
DEPENDENCY_FILES := \
	$(HOST_OBJECTS:.o=.d) \
	$(FCEUMM_OBJECTS:.o=.d)

HOST_CPPFLAGS := $(CPPFLAGS) -DFCEUMM_BUILTIN
FCEUMM_CPPFLAGS := $(CPPFLAGS) \
	-D__LIBRETRO__ \
	-DPATH_MAX=1024 \
	-DFCEU_VERSION_NUMERIC=9813 \
	-DFRONTEND_SUPPORTS_RGB565 \
	-DHAVE_NTSC_FILTER \
	-DNDEBUG \
	-DGIT_VERSION=\"$(FCEUMM_GIT_VERSION)\" \
	$(FCEUMM_ENDIAN_CPPFLAGS) \
	-I$(FCEUMM_CORE_DIR) \
	-I$(FCEUMM_CORE_DIR)/drivers/libretro \
	-I$(FCEUMM_COMMON_DIR)/include \
	-I$(FCEUMM_CORE_DIR)/input \
	-I$(FCEUMM_CORE_DIR)/boards \
	-I$(FCEUMM_CORE_DIR)/ntsc
FCEUMM_CFLAGS := $(filter-out -std=%,$(CFLAGS)) -std=gnu11 \
	-Wno-missing-prototypes \
	-Wno-sign-compare \
	-Wno-undef \
	-fno-sanitize=undefined

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(HOST_OBJECTS) $(FCEUMM_LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(HOST_OBJECTS) $(FCEUMM_LIBRARY) $(LDLIBS)

$(FCEUMM_LIBRARY): $(FCEUMM_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(BUILD_DIR)/host/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/fceumm/%.o: $(FCEUMM_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(FCEUMM_CPPFLAGS) $(FCEUMM_CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET)

-include $(DEPENDENCY_FILES)
