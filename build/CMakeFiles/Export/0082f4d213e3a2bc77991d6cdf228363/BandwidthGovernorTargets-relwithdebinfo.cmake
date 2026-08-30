#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Bandwidth::bgcore" for configuration "RelWithDebInfo"
set_property(TARGET Bandwidth::bgcore APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(Bandwidth::bgcore PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "CXX"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/bgcore.lib"
  )

list(APPEND _cmake_import_check_targets Bandwidth::bgcore )
list(APPEND _cmake_import_check_files_for_Bandwidth::bgcore "${_IMPORT_PREFIX}/lib/bgcore.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
