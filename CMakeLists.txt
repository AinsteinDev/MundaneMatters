cmake_minimum_required(VERSION 3.21)

project(
	MundaneMatters
	VERSION 0.2.0
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CommonLibSSE-NG (alandtse fork, "ng" branch) as a submodule. SE + AE only.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)
# CommonLib only includes its add_commonlibsse_plugin() helper on the prebuilt path; load it for source builds.
include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

find_package(nlohmann_json CONFIG REQUIRED)

set(SOURCES
	src/main.cpp
	src/Log.cpp
	src/SkyrimNetBridge.cpp
	src/TurnPipeline.cpp
	src/Services.cpp
	src/Game.cpp
	src/Promises.cpp
	src/Ledger.cpp
	src/Bio.cpp
	src/Confirm.cpp
)

add_commonlibsse_plugin(
	${PROJECT_NAME}
	NAME "MundaneMatters"
	AUTHOR "Ainstein"
	USE_ADDRESS_LIBRARY
	SOURCES ${SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE src include)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

# Stage the built DLL next to the data files so deploy.ps1 can mirror one folder into MO2.
add_custom_command(
	TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/data/SKSE/Plugins"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${CMAKE_SOURCE_DIR}/data/SKSE/Plugins/"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${CMAKE_SOURCE_DIR}/data/SKSE/Plugins/"
)
