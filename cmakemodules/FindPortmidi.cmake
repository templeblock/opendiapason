# - Try to find Portmidi
# Once done this will define
#  PORTMIDI_FOUND - System has portmidi
#  PORTMIDI_INCLUDE_DIRS - The portmidi include directories
#  PORTMIDI_LIBRARIES - The libraries needed to use portmidi
#  PORTMIDI_DEFINITIONS - Compiler switches required for using portmidi

if (PORTMIDI_LIBRARIES AND PORTMIDI_INCLUDE_DIRS)

	# in cache already
	set(PORTMIDI_FOUND TRUE)

else (PORTMIDI_LIBRARIES AND PORTMIDI_INCLUDE_DIRS)

	set(PORTMIDI_DEFINITIONS "")

	# Look for pkg-config and use it (if available) to find package
	find_package(PkgConfig QUIET)
	if (PKG_CONFIG_FOUND)
		pkg_search_module(PORTAUDIO QUIET portmidi-2.0)
	endif (PKG_CONFIG_FOUND)

	if (NOT PORTMIDI_FOUND)

		find_path(PORTMIDI_INCLUDE_DIR portmidi.h HINTS ${PORTMIDI_INCLUDEDIR} ${PORTMIDI_INCLUDE_DIRS} PATH_SUFFIXES portmidi)
		find_library(PORTMIDI_LIBRARY NAMES portmidi HINTS ${PORTMIDI_LIBDIR} ${PORTMIDI_LIBRARY_DIRS})

		set(PORTMIDI_LIBRARIES    ${PORTMIDI_LIBRARY})
		set(PORTMIDI_INCLUDE_DIRS ${PORTMIDI_INCLUDE_DIR})

		include(FindPackageHandleStandardArgs)

		# Set PORTMIDI_FOUND if the library and include paths were found
		find_package_handle_standard_args(portmidi DEFAULT_MSG PORTMIDI_LIBRARY PORTMIDI_INCLUDE_DIR)

		# Don't show include/library paths in cmake GUI
		mark_as_advanced(PORTMIDI_INCLUDE_DIR PORTMIDI_LIBRARY)

	endif (NOT PORTMIDI_FOUND)

endif (PORTMIDI_LIBRARIES AND PORTMIDI_INCLUDE_DIRS)