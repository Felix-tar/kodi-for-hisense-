#.rst:
# FindDirectFB
# ------------
# Finds the DirectFB library (DirectFB 1.4, as used by MStar based TVs)
#
# This will define the following targets:
#
#   DirectFB::DirectFB - The DirectFB library
#   DirectFB::Fusion   - The fusion library (IPC between the clients of a DirectFB session)
#   DirectFB::Direct   - The direct library (DirectFB base library)

if(NOT TARGET DirectFB::DirectFB)
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_DIRECTFB directfb QUIET)
  endif()

  # DirectFB installs its headers to <prefix>/include/directfb and includes them from there
  # without that prefix (<dfb_types.h>, <direct/interface.h>, ...)
  find_path(DIRECTFB_INCLUDE_DIR NAMES directfb.h
                                 HINTS ${PC_DIRECTFB_INCLUDEDIR} ${PC_DIRECTFB_INCLUDE_DIRS}
                                 PATH_SUFFIXES directfb
                                 NO_CACHE)

  # Headers of libdirect and the configure generated direct/build.h. An installed DirectFB has
  # both next to directfb.h; they are searched separately so that a source tree plus a directory
  # with the generated headers works as well.
  find_path(DIRECTFB_DIRECT_INCLUDE_DIR NAMES direct/interface.h
                                        HINTS ${DIRECTFB_INCLUDE_DIR}
                                              ${PC_DIRECTFB_INCLUDEDIR} ${PC_DIRECTFB_INCLUDE_DIRS}
                                        PATH_SUFFIXES directfb
                                        NO_CACHE)
  find_path(DIRECTFB_BUILD_INCLUDE_DIR NAMES direct/build.h
                                       HINTS ${DIRECTFB_INCLUDE_DIR}
                                             ${PC_DIRECTFB_INCLUDEDIR} ${PC_DIRECTFB_INCLUDE_DIRS}
                                       PATH_SUFFIXES directfb
                                       NO_CACHE)

  find_library(DIRECTFB_LIBRARY NAMES directfb directfb-1.4
                                HINTS ${PC_DIRECTFB_LIBDIR} ${PC_DIRECTFB_LIBRARY_DIRS}
                                NO_CACHE)
  find_library(DIRECTFB_FUSION_LIBRARY NAMES fusion fusion-1.4
                                       HINTS ${PC_DIRECTFB_LIBDIR} ${PC_DIRECTFB_LIBRARY_DIRS}
                                       NO_CACHE)
  find_library(DIRECTFB_DIRECT_LIBRARY NAMES direct direct-1.4
                                       HINTS ${PC_DIRECTFB_LIBDIR} ${PC_DIRECTFB_LIBRARY_DIRS}
                                       NO_CACHE)

  if(PC_DIRECTFB_VERSION)
    set(DIRECTFB_VERSION ${PC_DIRECTFB_VERSION})
  elseif(DIRECTFB_INCLUDE_DIR AND EXISTS "${DIRECTFB_INCLUDE_DIR}/directfb_version.h")
    file(STRINGS "${DIRECTFB_INCLUDE_DIR}/directfb_version.h" _directfb_version_str
                 REGEX "^#define[\t ]+DIRECTFB_(MAJOR|MINOR|MICRO)_VERSION[\t ]+")
    foreach(_part MAJOR MINOR MICRO)
      if(_directfb_version_str MATCHES "DIRECTFB_${_part}_VERSION[\t ]+\\(?([0-9]+)")
        list(APPEND _directfb_version_parts ${CMAKE_MATCH_1})
      endif()
    endforeach()
    list(LENGTH _directfb_version_parts _directfb_version_count)
    if(_directfb_version_count EQUAL 3)
      string(REPLACE ";" "." DIRECTFB_VERSION "${_directfb_version_parts}")
    endif()
    unset(_directfb_version_str)
    unset(_directfb_version_parts)
    unset(_directfb_version_count)
  endif()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(DirectFB
                                    REQUIRED_VARS DIRECTFB_LIBRARY
                                                  DIRECTFB_FUSION_LIBRARY
                                                  DIRECTFB_DIRECT_LIBRARY
                                                  DIRECTFB_INCLUDE_DIR
                                                  DIRECTFB_DIRECT_INCLUDE_DIR
                                                  DIRECTFB_BUILD_INCLUDE_DIR
                                    VERSION_VAR DIRECTFB_VERSION)

  if(DIRECTFB_FOUND)
    # Deliberately not named DIRECTFB_INCLUDE_DIRS: core_require_dep would add that variable to
    # the global include path. The include directories only come with the target.
    set(_directfb_include_dirs ${DIRECTFB_INCLUDE_DIR}
                               ${DIRECTFB_DIRECT_INCLUDE_DIR}
                               ${DIRECTFB_BUILD_INCLUDE_DIR})
    list(REMOVE_DUPLICATES _directfb_include_dirs)

    add_library(DirectFB::Direct UNKNOWN IMPORTED)
    set_target_properties(DirectFB::Direct PROPERTIES
                                           IMPORTED_LOCATION "${DIRECTFB_DIRECT_LIBRARY}")

    add_library(DirectFB::Fusion UNKNOWN IMPORTED)
    set_target_properties(DirectFB::Fusion PROPERTIES
                                           IMPORTED_LOCATION "${DIRECTFB_FUSION_LIBRARY}"
                                           INTERFACE_LINK_LIBRARIES DirectFB::Direct)

    add_library(DirectFB::DirectFB UNKNOWN IMPORTED)
    set_target_properties(DirectFB::DirectFB PROPERTIES
                                             IMPORTED_LOCATION "${DIRECTFB_LIBRARY}"
                                             INTERFACE_INCLUDE_DIRECTORIES "${_directfb_include_dirs}"
                                             INTERFACE_COMPILE_DEFINITIONS HAVE_DIRECTFB=1
                                             INTERFACE_LINK_LIBRARIES "DirectFB::Fusion;DirectFB::Direct")
    unset(_directfb_include_dirs)

    set_property(GLOBAL APPEND PROPERTY INTERNAL_DEPS_PROP DirectFB::DirectFB)
  endif()
endif()
