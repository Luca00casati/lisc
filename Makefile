LLVM_CONFIG ?= llvm-config-18
LLVM_LIBDIR := $(shell $(LLVM_CONFIG) --libdir)
CLANG_RESOURCE_DIR := $(shell $(LLVM_LIBDIR)/../bin/clang -print-resource-dir)

CXXFLAGS ?= -O2 -g -Wall -Wextra
CXXFLAGS += -Wno-missing-field-initializers $(patsubst -I%,-isystem %,$(shell $(LLVM_CONFIG) --cxxflags)) -MMD -MP -DCLANG_RESOURCE_DIR='"$(CLANG_RESOURCE_DIR)"'
LDLIBS = -L$(LLVM_LIBDIR) -Wl,-rpath,$(LLVM_LIBDIR) -lLLVM -lclang -lpthread

SRCS = src/main.cpp src/reader.cpp src/type.cpp src/compiler.cpp src/cimport.cpp src/fmt.cpp src/build.cpp
OBJS = $(SRCS:.cpp=.o)

lisc: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDLIBS)

test: lisc
	./tests/run.sh

clean:
	rm -f lisc $(OBJS) $(OBJS:.o=.d)

.PHONY: test clean
-include $(OBJS:.o=.d)
