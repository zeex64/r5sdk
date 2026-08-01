cmake_minimum_required( VERSION 3.16 )
add_module( "shared_lib" "CrouchSpamGuard" "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Core"
    "crouchspamplugin.cpp"
    "README.md"
)

end_sources( "${BUILD_OUTPUT_DIR}/bin/x64_retail/plugins/" )

target_link_libraries( ${PROJECT_NAME} PRIVATE
    "memoverride"
    "mathlib"
    "tier0"
    "tier1"
    "libdetours"
    "libprotobuf"
    "liblzham"
    "libspdlog"
    "SigCache_Pb"
    "NetCon_Pb"
    "Rpcrt4.lib"
)

target_link_options( ${PROJECT_NAME} PRIVATE
    "/STACK:8000000"
    $<$<CONFIG:Debug>:/FORCE:MULTIPLE>
)
