# Host-side unit tests for the parts of the agent that are pure logic.
#
# These deliberately do NOT need devkitPro: json.cpp and path.cpp have no libnx
# dependency, and they are the code every request passes through. Catching a
# path-traversal or parser regression here takes a second, versus a cross-build,
# a verified upload and a console reboot.
#
#   make -C agent/tests/host
# HOST_CXX, not CXX. When this is invoked from the parent devkitPro Makefile
# (`make host-tests`), CXX is already exported as the aarch64 cross-compiler,
# and `CXX ?= c++` will NOT override it — so the "host" tests would try to
# cross-compile and fail to link (-lasan/-lubsan do not exist for the target).
# That only shows up on a clean checkout, because a stale run_tests binary makes
# the rule a no-op.
HOST_CXX ?= c++
HOST_CXXFLAGS ?= -std=gnu++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined

SRC = test_json.cpp ../../source/json.cpp ../../source/path.cpp
BIN = run_tests

.PHONY: all test clean
all: test

$(BIN): $(SRC) ../../source/json.hpp ../../source/path.hpp
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $(SRC)

test: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BIN) $(BIN).dSYM
