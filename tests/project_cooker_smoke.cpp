#include <iostream>

#include "project_cooker.hpp"

int main() {
    aeg::tooling::ProjectCookRequest request{};
    const auto result = aeg::tooling::cook_project(request);
    if (result || result.value.diagnostics.empty() || result.value.diagnostics.front().code != "cook.request")
        return 1;
    std::cout << "Project cooker smoke passed\n";
    return 0;
}
