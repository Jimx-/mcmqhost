# This module can find FUSE3 Library
#
# Requirements:
# - CMake >= 3.5
#
# The following variables will be defined for your use:
# - FUSE3_FOUND : was FUSE3 found?
# - FUSE3_INCLUDE_DIRS : FUSE3 include directory
# - FUSE3_LIBRARIES : FUSE3 library
# - FUSE3_DEFINITIONS : FUSE3 cflags
# - FUSE3_VERSION : complete version of FUSE3 (major.minor)
# - FUSE3_MAJOR_VERSION : major version of FUSE3
# - FUSE3_MINOR_VERSION : minor version of FUSE3
#
# Example Usage:
#
# 1. Copy this file in the root of your project source directory
# 2. Then, tell CMake to search this non-standard module in your project directory by adding to your CMakeLists.txt:
# set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR})
# 3. Finally call find_package() once, here are some examples to pick from
#
# Require FUSE3 3.12 or later
# find_package(FUSE3 3.12 REQUIRED)
#
# if(FUSE3_FOUND)
# add_definitions(${FUSE3_DEFINITIONS})
# include_directories(${FUSE3_INCLUDE_DIRS})
# add_executable(myapp myapp.c)
# target_link_libraries(myapp ${FUSE3_LIBRARIES})
# endif()

#=============================================================================
# Copyright (c) 2012, julp
#
# Distributed under the OSI-approved BSD License
#
# This software is distributed WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#=============================================================================

cmake_minimum_required(VERSION 3.5)

########## Private ##########
function(fusedebug _varname)
    if(FUSE3_DEBUG)
        message("${_varname} = ${${_varname}}")
    endif(FUSE3_DEBUG)
endfunction(fusedebug)

########## Public ##########
set(FUSE3_FOUND TRUE)
set(FUSE3_LIBRARIES )
set(FUSE3_DEFINITIONS )
set(FUSE3_INCLUDE_DIRS )

find_package(PkgConfig)

set(PC_FUSE3_INCLUDE_DIRS )
set(PC_FUSE3_LIBRARY_DIRS )
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_FUSE3 "fuse3" QUIET)
    if(PC_FUSE3_FOUND)
# fusedebug(PC_FUSE3_LIBRARIES)
# fusedebug(PC_FUSE3_LIBRARY_DIRS)
# fusedebug(PC_FUSE3_LDFLAGS)
# fusedebug(PC_FUSE3_LDFLAGS_OTHER)
# fusedebug(PC_FUSE3_INCLUDE_DIRS)
# fusedebug(PC_FUSE3_CFLAGS)
# fusedebug(PC_FUSE3_CFLAGS_OTHER)
        set(FUSE3_DEFINITIONS "${PC_FUSE3_CFLAGS_OTHER}")
    endif(PC_FUSE3_FOUND)
endif(PKG_CONFIG_FOUND)

find_path(
    FUSE3_INCLUDE_DIRS
    NAMES fuse_common.h
    PATHS "${PC_FUSE3_INCLUDE_DIRS}"
    DOC "Include directories for FUSE3"
)

if(NOT FUSE3_INCLUDE_DIRS)
    set(FUSE3_FOUND FALSE)
endif(NOT FUSE3_INCLUDE_DIRS)

find_library(
    FUSE3_LIBRARIES
    NAMES "fuse3"
    PATHS "${PC_FUSE3_LIBRARY_DIRS}"
    DOC "Libraries for FUSE3"
)

if(NOT FUSE3_LIBRARIES)
    set(FUSE3_FOUND FALSE)
endif(NOT FUSE3_LIBRARIES)

if(FUSE3_FOUND)
    if(EXISTS "${FUSE3_INCLUDE_DIRS}/fuse3/fuse_common.h")
        file(READ "${FUSE3_INCLUDE_DIRS}/fuse3/fuse_common.h" _contents)
        string(REGEX REPLACE ".*# *define *FUSE_MAJOR_VERSION *([0-9]+).*" "\\1" FUSE3_MAJOR_VERSION "${_contents}")
        string(REGEX REPLACE ".*# *define *FUSE_MINOR_VERSION *([0-9]+).*" "\\1" FUSE3_MINOR_VERSION "${_contents}")
        set(FUSE3_VERSION "${FUSE3_MAJOR_VERSION}.${FUSE3_MINOR_VERSION}")
    endif()

    include(CheckCSourceCompiles)
    # Backup CMAKE_REQUIRED_*
    set(OLD_CMAKE_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES}")
    set(OLD_CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
    set(OLD_CMAKE_REQUIRED_DEFINITIONS "${CMAKE_REQUIRED_DEFINITIONS}")
    # Add FUSE3 compilation flags
    set(CMAKE_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES}" "${FUSE3_INCLUDE_DIRS}")
    set(CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}" "${FUSE3_LIBRARIES}")
    set(CMAKE_REQUIRED_DEFINITIONS "${CMAKE_REQUIRED_DEFINITIONS}" "${FUSE3_DEFINITIONS}")
    check_c_source_compiles("#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 12)
#include <stdlib.h>
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
int main(void) {
return 0;
}" FUSE3_CFLAGS_CHECK)
    if(NOT FUSE3_CFLAGS_CHECK)
        set(FUSE3_DEFINITIONS "-D_FILE_OFFSET_BITS=64")
        # Should we run again previous test to assume the failure was due to missing definition -D_FILE_OFFSET_BITS=64?
    endif(NOT FUSE3_CFLAGS_CHECK)
    # Restore CMAKE_REQUIRED_*
    set(CMAKE_REQUIRED_INCLUDES "${OLD_CMAKE_REQUIRED_INCLUDES}")
    set(CMAKE_REQUIRED_LIBRARIES "${OLD_CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_DEFINITIONS "${OLD_CMAKE_REQUIRED_DEFINITIONS}")
endif(FUSE3_FOUND)

if(FUSE3_INCLUDE_DIRS)
    include(FindPackageHandleStandardArgs)
    if(FUSE3_FIND_REQUIRED AND NOT FUSE3_FIND_QUIETLY)
        find_package_handle_standard_args(FUSE3 REQUIRED_VARS FUSE3_LIBRARIES FUSE3_INCLUDE_DIRS VERSION_VAR FUSE3_VERSION)
    else()
        find_package_handle_standard_args(FUSE3 "FUSE3 not found" FUSE3_LIBRARIES FUSE3_INCLUDE_DIRS)
    endif()
else(FUSE3_INCLUDE_DIRS)
    if(FUSE3_FIND_REQUIRED AND NOT FUSE3_FIND_QUIETLY)
        message(FATAL_ERROR "Could not find FUSE3 include directory")
    endif()
endif(FUSE3_INCLUDE_DIRS)

mark_as_advanced(
    FUSE3_INCLUDE_DIRS
    FUSE3_LIBRARIES
)

# IN (args)
fusedebug("FUSE3_FIND_COMPONENTS")
fusedebug("FUSE3_FIND_REQUIRED")
fusedebug("FUSE3_FIND_QUIETLY")
fusedebug("FUSE3_FIND_VERSION")
# OUT
# Found
fusedebug("FUSE3_FOUND")
# Definitions
fusedebug("FUSE3_DEFINITIONS")
# Linking
fusedebug("FUSE3_INCLUDE_DIRS")
fusedebug("FUSE3_LIBRARIES")
# Version
fusedebug("FUSE3_MAJOR_VERSION")
fusedebug("FUSE3_MINOR_VERSION")
fusedebug("FUSE3_VERSION")
