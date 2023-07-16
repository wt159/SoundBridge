#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "g3log" for configuration "Debug"
set_property(TARGET g3log APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(g3log PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/lib/libg3log.dll.a"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/bin/libg3log.dll"
  )

list(APPEND _cmake_import_check_targets g3log )
list(APPEND _cmake_import_check_files_for_g3log "${_IMPORT_PREFIX}/lib/libg3log.dll.a" "${_IMPORT_PREFIX}/bin/libg3log.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
