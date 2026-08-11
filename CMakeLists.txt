cmake_minimum_required(VERSION 3.14)
project(Beldex)

set(CMAKE_CXX_STANDARD 17)

set(CPR_FORCE_USE_SYSTEM_CURL ON)

# Subdirectories
add_subdirectory(external/fmt)
add_subdirectory(external/cpr)
add_subdirectory(faucet-helpers)

# Executable
add_executable(Beldex-faucet main.cpp)

target_include_directories(Beldex-faucet PRIVATE external/Crow/include)
target_include_directories(Beldex-faucet PRIVATE external/nlohmann_json/include)

# Link libraries
target_link_libraries(Beldex-faucet 
    PRIVATE 
        fmt::fmt 
        faucet-helpers
        cpr
)
