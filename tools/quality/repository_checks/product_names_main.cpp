#include "checker_main.hpp"
#include "product_names.hpp"
int main(int argc, char** argv) {
    return bloom::quality::runRepositoryChecker({argv, static_cast<std::size_t>(argc)},
                                                "Product names passed", "Product names failed",
                                                bloom::quality::scanProductNames);
}
