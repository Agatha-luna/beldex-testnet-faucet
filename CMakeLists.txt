cmake_minimum_required(VERSION 3.15)
project(Beldex)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CPR_FORCE_USE_SYSTEM_CURL ON)

# Crow is header-only. Expose it through a CMake target so its include path and
# Boost.Asio configuration are inherited by every target that uses Crow.
find_package(Boost 1.64 REQUIRED)
find_package(Threads REQUIRED)
add_library(crow INTERFACE)
add_library(Crow::Crow ALIAS crow)
target_include_directories(crow INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/external/Crow/include)
target_compile_definitions(crow INTERFACE CROW_USE_BOOST BOOST_ERROR_CODE_HEADER_ONLY)
target_link_libraries(crow INTERFACE Boost::boost Threads::Threads)

# Subdirectories
add_subdirectory(external/fmt)
add_subdirectory(external/cpr)
add_subdirectory(faucet-helpers)

# Executable
add_executable(Beldex-faucet main.cpp)

target_include_directories(Beldex-faucet PRIVATE external/nlohmann_json/include)

# Link libraries
target_link_libraries(Beldex-faucet 
    PRIVATE 
        fmt::fmt 
        faucet-helpers
        cpr
        Crow::Crow
)
