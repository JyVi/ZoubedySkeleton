# Check if version is set (must be set BEFORE including this file)
if(NOT DEFINED rapids-cmake-version)
    message(FATAL_ERROR "The CMake variable `rapids-cmake-version` must be defined before including fetch_rapids.cmake. Example: set(rapids-cmake-version \"26.08\")")
endif()

message(STATUS "Using rapids-cmake version: ${rapids-cmake-version}")

# Optional overrides (uncomment if needed)
# set(rapids-cmake-repo "rapidsai")
# set(rapids-cmake-branch "branch-${rapids-cmake-version}")
# set(rapids-cmake-tag "v${rapids-cmake-version}.00")
# set(rapids-cmake-sha "")

# Download rapids-cmake if not already present
set(RAPIDS_CMAKE_FILE ${CMAKE_CURRENT_BINARY_DIR}/RAPIDS_${rapids-cmake-version}.cmake)

if(NOT EXISTS ${RAPIDS_CMAKE_FILE})
    message(STATUS "Downloading rapids-cmake v${rapids-cmake-version}...")
    
    # Construct download URL based on available variables
    if(DEFINED rapids-cmake-url)
        set(DOWNLOAD_URL ${rapids-cmake-url})
        message(STATUS "Using custom URL: ${DOWNLOAD_URL}")
    elseif(DEFINED rapids-cmake-sha)
        set(DOWNLOAD_URL "https://raw.githubusercontent.com/rapidsai/rapids-cmake/${rapids-cmake-sha}/RAPIDS.cmake")
        message(STATUS "Using SHA: ${rapids-cmake-sha}")
    elseif(DEFINED rapids-cmake-tag)
        set(DOWNLOAD_URL "https://raw.githubusercontent.com/rapidsai/rapids-cmake/${rapids-cmake-tag}/RAPIDS.cmake")
        message(STATUS "Using tag: ${rapids-cmake-tag}")
    elseif(DEFINED rapids-cmake-branch)
        set(DOWNLOAD_URL "https://raw.githubusercontent.com/rapidsai/rapids-cmake/${rapids-cmake-branch}/RAPIDS.cmake")
        message(STATUS "Using branch: ${rapids-cmake-branch}")
    else()
        # Default: use branch based on version
        set(DOWNLOAD_URL "https://raw.githubusercontent.com/rapidsai/rapids-cmake/branch-${rapids-cmake-version}/RAPIDS.cmake")
        message(STATUS "Using branch: branch-${rapids-cmake-version}")
    endif()
    
    file(DOWNLOAD ${DOWNLOAD_URL}
         ${RAPIDS_CMAKE_FILE}
         STATUS download_status
         LOG download_log
    )
    
    list(GET download_status 0 status_code)
    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "Failed to download rapids-cmake from ${DOWNLOAD_URL}\nError: ${download_log}")
    endif()
    
    message(STATUS "Successfully downloaded rapids-cmake to ${RAPIDS_CMAKE_FILE}")
else()
    message(STATUS "Using existing rapids-cmake file: ${RAPIDS_CMAKE_FILE}")
endif()

# Include rapids-cmake
include(${RAPIDS_CMAKE_FILE})
