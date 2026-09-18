#include <iostream>

#include "project_cooker.hpp"

int main(int argc, char** argv) {
    aeg::tooling::ProjectCookRequest request{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument == "--project" && index + 1 < argc)
            request.project_file = argv[++index];
        else if (argument == "--module-manifest" && index + 1 < argc)
            request.module_manifest = argv[++index];
        else if (argument == "--output" && index + 1 < argc)
            request.output_directory = argv[++index];
        else if (argument == "--config" && index + 1 < argc)
            request.configuration = argv[++index];
        else {
            std::cerr << "usage: aeg_project_cook_tool --project <file> --module-manifest <file> --output <dir> "
                         "--config Debug|Release\n";
            return 2;
        }
    }
    const auto result = aeg::tooling::cook_project(request);
    if (!result) {
        std::cerr << result.result.message << '\n';
        return 1;
    }
    std::cout << result.value.package.string() << '\n';
    return 0;
}
