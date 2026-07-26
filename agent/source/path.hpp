// Path policy for every filesystem command, deliberately free of any libnx
// dependency so it can be unit-tested on the host (agent/tests/host).
//
// This is the security boundary: a bug here is a directory traversal reachable
// by any authenticated client, so it lives in one place with its own tests
// rather than being reimplemented per handler.
#pragma once

#include <string>

namespace agent {

// Reject "..", collapse "//" and ".", require a leading '/'.
// Returns false if the path is not allowed; `out` is only valid on true.
bool NormalizePath(const std::string& in, std::string& out);

// Map a protocol path onto an sdmc: device path. Rejects anything
// NormalizePath rejects.
bool ResolveSdPath(const std::string& in, std::string& dev);

}  // namespace agent
