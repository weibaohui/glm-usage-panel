cmake_minimum_required(VERSION 3.14)
project(glm-usage-panel C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(CURL QUIET)

include(FetchContent)
FetchContent_Declare(cui
    GIT_REPOSITORY https://github.com/yappologistic/CUI.git
    GIT_TAG v1.4.0)
FetchContent_MakeAvailable(cui)

add_executable(glm-usage-panel
    src/main.c
    src/ui.c
    src/glm.c
    src/crypto.c
    src/minijson.c)
target_link_libraries(glm-usage-panel PRIVATE cui::cui)
cui_app_target(glm-usage-panel)

if(CURL_FOUND)
    target_compile_definitions(glm-usage-panel PRIVATE GLM_HAVE_CURL)
    if(TARGET CURL::libcurl)
        target_link_libraries(glm-usage-panel PRIVATE CURL::libcurl)
    else()
        target_include_directories(glm-usage-panel PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(glm-usage-panel PRIVATE ${CURL_LIBRARIES})
    endif()
else()
    # no libcurl on this machine: build anyway, with canned demo data only
    target_compile_definitions(glm-usage-panel PRIVATE GLM_NO_NET)
    message(WARNING "libcurl not found: building demo-only (GLM_NO_NET)")
endif()
