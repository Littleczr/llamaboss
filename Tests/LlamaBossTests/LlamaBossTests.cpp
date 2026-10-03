#include <string>
// LlamaBossTests.cpp
// Entry point for the independent native LlamaBoss regression runner.

#include "test_framework.h"
#include "json_case_loader.h"

int main(int argc, char** argv) {

    bool helpOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") helpOnly = true;
    }

    if (!helpOnly) {
        try {
            const std::filesystem::path cases =
                lbtest::ResolveJsonCaseDirectory(argc, argv);
            lbtest::RegisterJsonCases(cases);
        } catch (const std::exception& ex) {
            std::cerr << "Could not load JSON test cases: " << ex.what() << '\n';
            return 2;
        }
    }

    return lbtest::RunAll(argc, argv);
}
