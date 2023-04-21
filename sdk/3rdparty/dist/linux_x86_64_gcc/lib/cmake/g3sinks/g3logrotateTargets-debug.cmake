#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "G3::g3logrotate" for configuration "Debug"
set_property(TARGET G3::g3logrotate APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(G3::g3logrotate PROPERTIES
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libg3logrotate.so.2.2.-"
  IMPORTED_SONAME_DEBUG "libg3logrotate.so.2.2.-"
  )

list(APPEND _cmake_import_check_targets G3::g3logrotate )
list(APPEND _cmake_import_check_files_for_G3::g3logrotate "${_IMPORT_PREFIX}/lib/libg3logrotate.so.2.2.-" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
