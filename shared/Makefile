#shared

include ../common.mk

CPPFLAGS := -I. -I../kernel
CFLAGS   := $(CFLAGS_BASE) $(CPPFLAGS)
CXXFLAGS := $(CXXFLAGS_BASE) $(CPPFLAGS)

CLEAN_OBJS := $(shell find . -name '*.o')
CLEAN_DEPS := $(shell find . -name '*.d')
C_SRC   := $(shell find . -name '*.c')
CPP_SRC := $(shell find . -name '*.cpp')
ASM_SRC := $(shell find . -name '*.S')
OBJ     := $(C_SRC:.c=.o) $(ASM_SRC:.S=.o) $(CPP_SRC:.cpp=.o)
DEP     := $(C_SRC:.c=.d) $(ASM_SRC:.S=.d) $(CPP_SRC:.cpp=.d)

TARGET  := libshared.a

.PHONY: all clean prepare

all: prepare $(TARGET)

prepare:
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJ)
	echo "Finishing build"
	echo $(addprefix $(BUILD_DIR)/,$(notdir $(OBJ)))
	$(VAR) rcs $@ $(addprefix $(BUILD_DIR)/,$(notdir $(OBJ)))

%.o: %.S
	$(VAS) $(CFLAGS) -c $< -o $(BUILD_DIR)/$(notdir $@)

%.o: %.c
	$(VCC) $(CFLAGS) -c -MMD -MP $< -o $(BUILD_DIR)/$(notdir $@)

%.o: %.cpp
	$(VCXX) $(CXXFLAGS) -c -MMD -MP $< -o $(BUILD_DIR)/$(notdir $@)

clean:
	$(RM) $(CLEAN_OBJS) $(CLEAN_DEPS) $(TARGET)

-include $(DEP)
