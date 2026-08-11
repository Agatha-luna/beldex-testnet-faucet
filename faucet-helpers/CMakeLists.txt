add_library(faucet-helpers faucet-helpers.cpp)

target_include_directories(faucet-helpers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Find dependencies
find_package(OpenSSL REQUIRED)  
find_package(SQLite3 REQUIRED)

target_include_directories(faucet-helpers PRIVATE ${CMAKE_SOURCE_DIR}/external/Crow/include)
target_include_directories(faucet-helpers PRIVATE ${CMAKE_SOURCE_DIR}/external/nlohmann_json/include)

# Link dependencies
target_link_libraries(
    faucet-helpers
    PRIVATE
        SQLite::SQLite3
        fmt::fmt
        OpenSSL::SSL
        OpenSSL::Crypto
        cpr
)
