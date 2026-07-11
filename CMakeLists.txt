cmake_minimum_required(VERSION 3.10)
project(DirectTransfer VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Download nlohmann/json single header
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/nlohmann/json.hpp")
    message(STATUS "Downloading nlohmann/json.hpp...")
    file(DOWNLOAD
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
        "${CMAKE_SOURCE_DIR}/external/nlohmann/json.hpp"
        STATUS download_status
    )
    list(GET download_status 0 status_code)
    if(NOT status_code EQUAL 0)
        message(WARNING "Failed to download nlohmann/json.hpp. "
            "Manually place it at external/nlohmann/json.hpp from "
            "https://github.com/nlohmann/json/releases")
    endif()
endif()

set(SOURCES
    src/main.cpp
    src/AppConfig.cpp
    src/Utils.cpp
    src/AuditLogger.cpp
    src/MainWindow.cpp
    src/RoleSelectPage.cpp
    src/SenderPage.cpp
    src/ReceiverPage.cpp
    src/DirPicker.cpp
    src/NetworkDiscovery.cpp
    src/PairingHandler.cpp
    src/TransferProtocol.cpp
    src/TransferSession.cpp
    src/FileScanner.cpp
    src/Manifest.cpp
    src/TransferPlanner.cpp
    src/ResumeManager.cpp
    src/ExcludeRules.cpp
    src/ProgressTracker.cpp
    src/ReportGenerator.cpp
    src/IpConfigurator.cpp
)

add_executable(DirectTransfer WIN32 ${SOURCES} src/resources.rc)
set_source_files_properties(src/resources.rc PROPERTIES LANGUAGE RC)

target_include_directories(DirectTransfer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
)

target_link_libraries(DirectTransfer PRIVATE
    ws2_32
    iphlpapi
    advapi32
    ole32
    oleaut32
    shell32
    comctl32
    uuid
    bcrypt
)

target_link_options(DirectTransfer PRIVATE -static)
target_compile_options(DirectTransfer PRIVATE
    -finput-charset=utf-8 -fexec-charset=utf-8
    -D_WIN32_WINNT=0x0601 -DNTDDI_VERSION=0x06010000
)
