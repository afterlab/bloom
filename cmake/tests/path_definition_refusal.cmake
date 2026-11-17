# Helper of path_definition.cmake: asks for a definition of the path in BAD, which must fail.
cmake_minimum_required(VERSION 3.25)
include("${MODULE_DIR}/BloomPathDefinition.cmake")
bloom_path_definition(unused "BLOOM_X_DIRECTORY" "${BAD}")
