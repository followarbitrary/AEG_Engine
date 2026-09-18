#include <iostream>

#include "project_scaffold.hpp"

int main(int argc, char** argv) {
    std::filesystem::path output;
    std::string name;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--output" && index + 1 < argc)
            output = argv[++index];
        else if (argument == "--name" && index + 1 < argc)
            name = argv[++index];
        else {
            std::cerr << "usage: aeg_project_scaffold_tool --output <root> --name <name>\n";
            return 2;
        }
    }
    const auto result = aeg::application::project::create_project({output, name, "GameModule"});
    if (!result) {
        std::cerr << result.result.message << '\n';
        return 1;
    }
    std::cout << result.value.string() << '\n';
    return 0;
}
