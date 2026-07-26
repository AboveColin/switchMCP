#include "path.hpp"

#include <vector>

namespace agent {

bool NormalizePath(const std::string& in, std::string& out) {
    if (in.empty() || in[0] != '/') return false;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < in.size()) {
        size_t j = in.find('/', i);
        if (j == std::string::npos) j = in.size();
        std::string seg = in.substr(i, j - i);
        if (seg == "..") return false;  // no traversal, ever
        if (!seg.empty() && seg != ".") parts.push_back(seg);
        i = j + 1;
    }
    out = "/";
    for (size_t k = 0; k < parts.size(); k++) {
        out += parts[k];
        if (k + 1 < parts.size()) out += "/";
    }
    return true;
}

bool ResolveSdPath(const std::string& in, std::string& dev) {
    std::string norm;
    if (!NormalizePath(in, norm)) return false;
    dev = "sdmc:" + norm;
    return true;
}

}  // namespace agent
