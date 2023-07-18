#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "zlib::zlib" for configuration "Debug"
set_property(TARGET zlib::zlib APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(zlib::zlib PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/lib/libzlib.dll.a"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/bin/libzlib.dll"
  )

list(APPEND _cmake_import_check_targets zlib::zlib )
list(APPEND _cmake_import_check_files_for_zlib::zlib "${_IMPORT_PREFIX}/lib/libzlib.dll.a" "${_IMPORT_PREFIX}/bin/libzlib.dll" )

# Import target "zlib::zlibstatic" for configuration "Debug"
set_property(TARGET zlib::zlibstatic APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(zlib::zlibstatic PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libzlib.a"
  )

list(APPEND _cmake_import_check_targets zlib::zlibstatic )
list(APPEND _cmake_import_check_files_for_zlib::zlibstatic "${_IMPORT_PREFIX}/lib/libzlib.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
