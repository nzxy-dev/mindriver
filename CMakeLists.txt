cmake_minimum_required(VERSION 3.10)
project(mindriver C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 99)

if(WIN32)
    set(MINDRIVER_SOURCES
        main.c
        windows/initcore.c
        windows/resolver.c
        windows/servicefunc.c
        windows/mindriver.rc
    )

    include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/windows)

    add_executable(mindriver ${MINDRIVER_SOURCES})

    set_target_properties(mindriver PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/out/windows"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/bin/out/windows"
    )

    if(MSVC)
        target_compile_options(mindriver PRIVATE /O2 /DNDEBUG /W3)
        target_link_libraries(mindriver PRIVATE ole32 user32 mfplat mfreadwrite mfuuid avrt)
    else()
        target_compile_options(mindriver PRIVATE -O2 -DNDEBUG -Wall)
        target_link_libraries(mindriver PRIVATE ole32 user32 avrt)
    endif()

else()
    find_package(ALSA REQUIRED)

    set(MINDRIVER_SOURCES
        main.c
        linux/initcore.c
        linux/resolver.c
        linux/servicefunc.c
    )

    include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/linux)

    add_executable(mindriver ${MINDRIVER_SOURCES})

    set_target_properties(mindriver PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/out/linux"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/bin/out/linux"
    )

    target_compile_options(mindriver PRIVATE -O2 -DNDEBUG -Wall)
    target_include_directories(mindriver PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(mindriver PRIVATE pthread m ${ALSA_LIBRARIES} ${CMAKE_DL_LIBS})
endif()
