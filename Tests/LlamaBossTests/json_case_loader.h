#pragma once

#include <filesystem>

namespace lbtest {

std::filesystem::path ResolveJsonCaseDirectory(int argc, char** argv);
void RegisterJsonCases(const std::filesystem::path& directory);

} // namespace lbtest
