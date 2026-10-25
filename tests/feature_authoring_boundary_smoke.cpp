/** @brief Checks the real authoring target cannot import retired execution or author APIs. */
#include <iostream>
#include <type_traits>

#include "feature_graph_io.hpp"
#include "parameter_value.hpp"

int main() {
    bool exposed = false;
#if __has_include("pipeline.hpp")
    std::cerr << "Feature authoring consumers inherit the Pipeline execution include directory\n";
    exposed = true;
#endif
#if __has_include("pipeline_configuration.hpp")
    std::cerr << "Feature authoring consumers inherit Pipeline configuration\n";
    exposed = true;
#endif
#if __has_include("pipeline_authoring.hpp") || __has_include("pipeline_document_io.hpp")
    std::cerr << "Feature authoring consumers can import the retired catalog or serializer\n";
    exposed = true;
#endif
    static_assert(std::is_same_v<decltype(aeg::feature::FeatureGraphDocument{}.nodes)::value_type,
                                 aeg::feature::FeatureNode>);
    aeg::foundation::ParameterBlock parameters;
    parameters.emplace(1, true);
    if (parameters.size() != 1 || !std::get<bool>(parameters.at(1))) return 2;
    if (exposed) return 1;
    std::cout << "Feature authoring target and neutral parameter boundary passed\n";
    return 0;
}
