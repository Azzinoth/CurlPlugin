cmake_minimum_required(VERSION 3.15)

set(BUILD_TYPE "Debug and Release" CACHE STRING "Choose Build type")
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CONFIGURATION_TYPES Debug Release)

add_definitions(-DUNICODE)
add_definitions(-D_UNICODE)

set(PROJECT_NAME CurlPlugin)

project(${PROJECT_NAME})
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set compiler flags based on USE_STATIC_RUNTIME
if(MSVC)
    # Disable C4251 warning
    add_compile_options(/wd4251)

    # Runtime is set as default for targets, so static libraries of modules could use static runtime.
    if(USE_STATIC_RUNTIME)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    # Always add /MP for multi-processor compilation
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /D FE_GRAPHICS_API_OPENGL")
endif()

# Turn on the ability to create folders to organize projects (.vcproj)
# It creates "CMakePredefinedTargets" folder by default and adds CMake
# defined projects like INSTALL.vcproj and ZERO_CHECK.vcproj
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# *************** FOCAL ENGINE ***************
# Engine comes as git submodule, it is not part of plugin VS project, plugin only needs engine headers and import libraries.
# Engine is built only when its libraries are missing in EngineLibs, delete that folder to rebuild engine after submodule update.
set(FOCAL_ENGINE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/SubSystems/FocalEngine")
set(FOCAL_ENGINE_LIBS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/EngineLibs")
set(FOCAL_ENGINE_BUILD_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/EngineBuild")
set(FOCAL_ENGINE_CONFIGURATIONS Debug Release)
set(FOCAL_ENGINE_LIBRARIES FocalEngine FEBasicApplication)

if(NOT EXISTS "${FOCAL_ENGINE_DIRECTORY}/CMakeLists.txt" OR NOT EXISTS "${FOCAL_ENGINE_DIRECTORY}/SubSystems/FEBasicApplication/CMakeLists.txt")
    message(FATAL_ERROR "Focal Engine submodule is missing, run: git submodule update --init --recursive")
endif()

set(bFocalEngineLibrariesMissing OFF)
foreach(Configuration ${FOCAL_ENGINE_CONFIGURATIONS})
    foreach(Library ${FOCAL_ENGINE_LIBRARIES})
        if(NOT EXISTS "${FOCAL_ENGINE_LIBS_DIRECTORY}/${Configuration}/${Library}.lib")
            set(bFocalEngineLibrariesMissing ON)
        endif()
    endforeach()
endforeach()

if(bFocalEngineLibrariesMissing)
    message(STATUS "Focal Engine libraries are missing, building engine from submodule, it could take a while.")

    # Engine should be built with the same options that editor uses: shared libraries with dynamic runtime.
    set(EngineConfigureCommand ${CMAKE_COMMAND} -S "${FOCAL_ENGINE_DIRECTORY}" -B "${FOCAL_ENGINE_BUILD_DIRECTORY}" -G "${CMAKE_GENERATOR}" -DBUILD_SHARED_LIBS=ON -DUSE_STATIC_RUNTIME=OFF)
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND EngineConfigureCommand -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()

    execute_process(COMMAND ${EngineConfigureCommand} RESULT_VARIABLE EngineConfigureResult)
    if(NOT EngineConfigureResult EQUAL 0)
        message(FATAL_ERROR "Focal Engine configure failed with error ${EngineConfigureResult}")
    endif()

    foreach(Configuration ${FOCAL_ENGINE_CONFIGURATIONS})
        # FocalEngine target also builds FEBasicApplication, because engine depends on it.
        execute_process(COMMAND ${CMAKE_COMMAND} --build "${FOCAL_ENGINE_BUILD_DIRECTORY}" --target FocalEngine --config ${Configuration} RESULT_VARIABLE EngineBuildResult)
        if(NOT EngineBuildResult EQUAL 0)
            message(FATAL_ERROR "Focal Engine ${Configuration} build failed with error ${EngineBuildResult}")
        endif()

        foreach(Library ${FOCAL_ENGINE_LIBRARIES})
            # Library location depends on engine build layout, so it is searched for instead of being hardcoded.
            file(GLOB_RECURSE FoundLibraries "${FOCAL_ENGINE_BUILD_DIRECTORY}/${Library}.lib")
            list(FILTER FoundLibraries INCLUDE REGEX "/${Configuration}/${Library}\\.lib$")
            list(LENGTH FoundLibraries FoundLibrariesCount)
            if(NOT FoundLibrariesCount EQUAL 1)
                message(FATAL_ERROR "Expected one ${Configuration} ${Library}.lib in ${FOCAL_ENGINE_BUILD_DIRECTORY}, found: ${FoundLibraries}")
            endif()

            file(COPY ${FoundLibraries} DESTINATION "${FOCAL_ENGINE_LIBS_DIRECTORY}/${Configuration}")
        endforeach()
    endforeach()
endif()

# Applications compare Build/EngineCommit.txt with commit of their engine, because plugin binaries depend on engine headers they were built with.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FOCAL_ENGINE_DIRECTORY}" rev-parse HEAD OUTPUT_VARIABLE FocalEngineCommit OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE FocalEngineCommitResult ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FOCAL_ENGINE_DIRECTORY}" rev-parse --absolute-git-dir OUTPUT_VARIABLE FocalEngineGitDirectory OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(FocalEngineCommitResult EQUAL 0 AND EXISTS "${FocalEngineGitDirectory}/HEAD")
        file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/Build/EngineCommit.txt" "${FocalEngineCommit}")
        # Submodule update changes HEAD, so configure runs again and file never has commit of older engine headers.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FocalEngineGitDirectory}/HEAD")
    endif()
endif()

# *************** CURL ***************
# curl is built from ThirdParty/curl submodule as static library and linked into plugin DLL, so plugin has no DLLs to ship.
# Only what Windows has is used: Schannel for TLS, so certificates come from Windows certificate store and no CA bundle is needed.
# Optional dependencies are turned off, otherwise curl would use whatever it finds on the machine.
# It is added before include_directories of engine, so curl sources do not see engine headers.
set(CURL_OPTIONS
    BUILD_SHARED_LIBS=OFF
    BUILD_STATIC_LIBS=ON
    BUILD_CURL_EXE=OFF
    BUILD_TESTING=OFF
    BUILD_EXAMPLES=OFF
    BUILD_LIBCURL_DOCS=OFF
    BUILD_MISC_DOCS=OFF
    ENABLE_CURL_MANUAL=OFF
    CURL_DISABLE_INSTALL=ON
    CURL_ENABLE_EXPORT_TARGET=OFF
    # Plugin defines UNICODE for everything, curl should know about it.
    ENABLE_UNICODE=ON
    CURL_USE_SCHANNEL=ON
    CURL_USE_OPENSSL=OFF
    CURL_USE_LIBPSL=OFF
    CURL_USE_LIBSSH2=OFF
    USE_LIBIDN2=OFF
    USE_NGHTTP2=OFF
    CURL_ZLIB=OFF
    CURL_BROTLI=OFF
    CURL_ZSTD=OFF
    CURL_DISABLE_LDAP=ON
)

foreach(Option ${CURL_OPTIONS})
    string(REPLACE "=" ";" OptionParts "${Option}")
    list(GET OptionParts 0 OptionName)
    list(GET OptionParts 1 OptionValue)
    set(${OptionName} ${OptionValue} CACHE STRING "" FORCE)
endforeach()

if(MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP")
endif()

# Plugin is configured in its source folder, without separate build folder curl build files would be written into the submodule.
add_subdirectory(ThirdParty/curl "${CMAKE_CURRENT_SOURCE_DIR}/CurlBuild" EXCLUDE_FROM_ALL)
set_target_properties(libcurl_static PROPERTIES FOLDER "ThirdParty")

# Games and applications that link engine statically use static runtime, so static library of plugin needs curl built with static runtime too.
# curl can not be added to this project twice, so second build is a separate CMake project with the same options.
include(ExternalProject)
set(CURL_STATIC_RUNTIME_ARGUMENTS ${CURL_OPTIONS})
list(TRANSFORM CURL_STATIC_RUNTIME_ARGUMENTS PREPEND "-D")
set(CURL_STATIC_RUNTIME_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/CurlBuildStaticRuntime")
ExternalProject_Add(CurlStaticRuntime
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/curl"
    PREFIX "${CURL_STATIC_RUNTIME_DIRECTORY}"
    BINARY_DIR "${CURL_STATIC_RUNTIME_DIRECTORY}/Build"
    CMAKE_ARGS ${CURL_STATIC_RUNTIME_ARGUMENTS} -DCURL_STATIC_CRT=ON -DCMAKE_C_FLAGS_INIT=/MP
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG> --target libcurl_static
    INSTALL_COMMAND ""
    # Build is checked every time, so curl is rebuilt after submodule update.
    BUILD_ALWAYS ON
    EXCLUDE_FROM_ALL ON
)
set_target_properties(CurlStaticRuntime PROPERTIES FOLDER "ThirdParty")
set(CURL_STATIC_RUNTIME_LIBRARY "${CURL_STATIC_RUNTIME_DIRECTORY}/Build/lib/$<IF:$<CONFIG:Debug>,Debug/libcurl-d.lib,Release/libcurl.lib>")

file(GLOB Connection_To_Engine_SRC
	"SubSystems/FocalEngine/SubSystems/PluginSystem/FEPluginConnector.cpp"
    "SubSystems/FocalEngine/SubSystems/PluginSystem/FEPluginConnector.h"
)

file(GLOB Main_SRC
	"CurlPlugin.h"
	"CurlPlugin.cpp"
)

# Public headers of plugin API, applications include them and get plugin through PLUGIN_MANAGER.GetPlugin<T>.
file(GLOB Public_SRC "Public/*.h" "Public/Editor/*.h")

link_directories(${GLEW_LIB_DIR})
link_directories(${GLFW_LIB_DIR})

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/SubSystems/FocalEngine/SubSystems/FEBasicApplication
    ${CMAKE_CURRENT_SOURCE_DIR}/SubSystems/FocalEngine/SubSystems/FEBasicApplication/SubSystems
    ${CMAKE_CURRENT_SOURCE_DIR}/SubSystems/FocalEngine/SubSystems/FEBasicApplication/ThirdParty
    ${CMAKE_CURRENT_SOURCE_DIR}/SubSystems/FocalEngine/SubSystems/FEBasicApplication/ThirdParty/stduuid
    ${CMAKE_CURRENT_SOURCE_DIR}/SubSystems/FocalEngine/SubSystems/FEBasicApplication/ThirdParty/glew2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/SubSystems/FocalEngine
    ${CMAKE_CURRENT_SOURCE_DIR}/SubSystems/FocalEngine/ThirdParty
    ${CMAKE_CURRENT_SOURCE_DIR}/SubSystems/FocalEngine/ThirdParty/entt
)

# Creates one plugin module DLL, every module of the plugin is set up the same way.
# With STATIC_LIBRARY module also gets static library, that games link instead of DLL.
# Usage: fe_add_plugin_module(TargetName [STATIC_LIBRARY] SOURCES File1.h File1.cpp ...)
function(fe_add_plugin_module TARGET_NAME)
    cmake_parse_arguments(MODULE "STATIC_LIBRARY" "" "SOURCES" ${ARGN})

    add_library(${TARGET_NAME}
        SHARED
            ${MODULE_SOURCES}
            ${Public_SRC}
            ${Connection_To_Engine_SRC}
    )

    target_link_libraries(${TARGET_NAME}
        PRIVATE
            debug ${FOCAL_ENGINE_LIBS_DIRECTORY}/Debug/FEBasicApplication.lib
            optimized ${FOCAL_ENGINE_LIBS_DIRECTORY}/Release/FEBasicApplication.lib
            debug ${FOCAL_ENGINE_LIBS_DIRECTORY}/Debug/FocalEngine.lib
            optimized ${FOCAL_ENGINE_LIBS_DIRECTORY}/Release/FocalEngine.lib
    )

    # FocalEngine is headers-only, we need to add its include directories
    target_include_directories(${TARGET_NAME} PRIVATE ${FOCAL_ENGINE_INCLUDES})
    target_compile_definitions(${TARGET_NAME} PRIVATE FOCAL_ENGINE_HEADERS_ONLY FEBASICAPPLICATION_SHARED FOCAL_ENGINE_SHARED)

    target_compile_definitions(${TARGET_NAME} PRIVATE $<$<CONFIG:Debug>:FE_DEBUG_ENABLED>)

    source_group("Source Files/" FILES ${MODULE_SOURCES})
    source_group("Source Files/Public/" FILES ${Public_SRC})
    source_group("Source Files/Engine/" FILES ${Connection_To_Engine_SRC})

    # Gather files that plugin manager needs in one folder, DLL name gets configuration postfix.
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/Build"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${TARGET_NAME}>" "${CMAKE_CURRENT_SOURCE_DIR}/Build/${TARGET_NAME}_$<CONFIG>.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.json" "${CMAKE_CURRENT_SOURCE_DIR}/Build/"
    )

    # Copy public headers, applications include them to call plugin API.
    if(Public_SRC)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Public" "${CMAKE_CURRENT_SOURCE_DIR}/Build/Public"
        )
    endif()

    # Build other configuration too, so both DLLs in Build folder are always up to date.
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -D BUILT_CONFIGURATION=$<CONFIG> -D "BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}" -D TARGET_NAME=${TARGET_NAME} -P "${CMAKE_CURRENT_SOURCE_DIR}/BuildManagement/BuildOtherConfiguration.cmake"
    )

    if(MODULE_STATIC_LIBRARY)
        add_library(${TARGET_NAME}_Static
            STATIC
                ${MODULE_SOURCES}
                ${Public_SRC}
                ${Connection_To_Engine_SRC}
        )

        # Games link engine statically, with static runtime and static GLEW.
        set_target_properties(${TARGET_NAME}_Static PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_include_directories(${TARGET_NAME}_Static PRIVATE ${FOCAL_ENGINE_INCLUDES})
        target_compile_definitions(${TARGET_NAME}_Static PRIVATE FOCAL_ENGINE_HEADERS_ONLY GLEW_STATIC $<$<CONFIG:Debug>:FE_DEBUG_ENABLED>)

        add_custom_command(TARGET ${TARGET_NAME}_Static POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/Build/Static"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${TARGET_NAME}_Static>" "${CMAKE_CURRENT_SOURCE_DIR}/Build/Static/${TARGET_NAME}_$<CONFIG>.lib"
        )

        # Static library is built together with DLL, in both configurations.
        add_dependencies(${TARGET_NAME} ${TARGET_NAME}_Static)
        set_property(GLOBAL APPEND PROPERTY FE_PLUGIN_STATIC_MODULES ${TARGET_NAME})
    endif()
endfunction()

# Writes Build/Static/<PluginName>.cmake, games include it to link static libraries of modules together with what they depend on.
# LIBRARIES are library files, that are copied next to it, or names of system libraries. LINK_OPTIONS are added to the game link.
# RUNTIME_NEXT_TO_EXECUTABLE is for plugins whose DLLs executable imports, so they should be found when it starts.
# Applications then copy files of Build/Debug or Build/Release next to executable with fe_copy_plugin_runtime.
# Usage: fe_write_static_link_file([RUNTIME_NEXT_TO_EXECUTABLE] [LIBRARIES Library1.lib ...] [LINK_OPTIONS Option1 ...])
function(fe_write_static_link_file)
    cmake_parse_arguments(STATIC_LINK "RUNTIME_NEXT_TO_EXECUTABLE" "" "LIBRARIES;LINK_OPTIONS" ${ARGN})

    get_property(StaticModules GLOBAL PROPERTY FE_PLUGIN_STATIC_MODULES)
    if(NOT StaticModules)
        return()
    endif()

    set(StaticDirectory "${CMAKE_CURRENT_SOURCE_DIR}/Build/Static")
    file(MAKE_DIRECTORY "${StaticDirectory}")

    set(LinkLibraries "")
    set(LinkOptions "")
    foreach(Module ${StaticModules})
        # Runtime of static library should match application, so Debug configuration links Debug library and any other configuration links Release library.
        set(ModuleLibrary "\${CMAKE_CURRENT_LIST_DIR}/${Module}_$<IF:$<CONFIG:Debug>,Debug,Release>.lib")
        # Nothing references static registration of REGISTER_PLUGIN, so without /WHOLEARCHIVE linker would drop it.
        list(APPEND LinkLibraries "${ModuleLibrary}")
        list(APPEND LinkOptions "/WHOLEARCHIVE:${ModuleLibrary}")
    endforeach()

    foreach(Library ${STATIC_LINK_LIBRARIES})
        if(EXISTS "${Library}")
            file(COPY "${Library}" DESTINATION "${StaticDirectory}")
            get_filename_component(LibraryFileName "${Library}" NAME)
            list(APPEND LinkLibraries "\${CMAKE_CURRENT_LIST_DIR}/${LibraryFileName}")
        else()
            list(APPEND LinkLibraries "${Library}")
        endif()
    endforeach()

    list(APPEND LinkOptions ${STATIC_LINK_LINK_OPTIONS})

    set(RuntimeProperty "")
    if(STATIC_LINK_RUNTIME_NEXT_TO_EXECUTABLE)
        set(RuntimeProperty "\n        FE_RUNTIME_NEXT_TO_EXECUTABLE ON")
    endif()

    file(WRITE "${StaticDirectory}/${PROJECT_NAME}.cmake"
        "# Generated by CMakeLists.txt of ${PROJECT_NAME}, games include it to link plugin statically.\n"
        "if(NOT TARGET ${PROJECT_NAME}::Static)\n"
        "    add_library(${PROJECT_NAME}::Static INTERFACE IMPORTED)\n"
        "    set_target_properties(${PROJECT_NAME}::Static PROPERTIES\n"
        "        INTERFACE_LINK_LIBRARIES \"${LinkLibraries}\"\n"
        "        INTERFACE_LINK_OPTIONS \"${LinkOptions}\"${RuntimeProperty})\n"
        "endif()"
    )
endfunction()

# Plugin without main module gets all its modules from AdditionalModules.cmake.
if(Main_SRC)
    fe_add_plugin_module(${PROJECT_NAME} STATIC_LIBRARY SOURCES ${Main_SRC})
    # CURL::libcurl_static also brings CURL_STATICLIB definition, curl include directory and Windows libraries that curl needs.
    target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl_static)

    # Static library does not link anything, so it gets definition and include directory explicitly.
    target_compile_definitions(${PROJECT_NAME}_Static PRIVATE CURL_STATICLIB)
    target_include_directories(${PROJECT_NAME}_Static PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/curl/include")
    # Librarian merges curl into static library of plugin, so Debug and Release libraries of plugin have curl with matching runtime
    # and applications link only one library.
    add_dependencies(${PROJECT_NAME}_Static CurlStaticRuntime)
    set_target_properties(${PROJECT_NAME}_Static PROPERTIES STATIC_LIBRARY_OPTIONS "${CURL_STATIC_RUNTIME_LIBRARY}")

    # DLLs and static libraries of plugin contain curl, its license requires copyright notice to be shipped with them.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/curl/COPYING" "${CMAKE_CURRENT_SOURCE_DIR}/Build/curl_License.txt"
    )

    # set the startup project
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})
endif()

# Applications that generate plugin project could add more modules here, for example editor adds editor module.
include(${CMAKE_CURRENT_SOURCE_DIR}/AdditionalModules.cmake OPTIONAL)

# After all modules are added.
# Windows libraries that curl uses, plugin DLL gets them from CURL::libcurl_static.
fe_write_static_link_file(LIBRARIES ws2_32 iphlpapi bcrypt advapi32 crypt32 secur32)