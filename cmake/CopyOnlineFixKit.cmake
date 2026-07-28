# Copy online_fix_kit files when present (Linux CI may lack harvested DLLs;
# Windows CI / committed kit supplies SteamFix64.dll + winmm.dll).
if(NOT EXISTS "${SOURCE_DIR}")
  message(STATUS "online_fix_kit: source missing (${SOURCE_DIR})")
  return()
endif()

file(GLOB _KIT_FILES "${SOURCE_DIR}/*")
foreach(_f IN LISTS _KIT_FILES)
  get_filename_component(_name "${_f}" NAME)
  if(IS_DIRECTORY "${_f}")
    # Steamless DRM unpacker (Windows) - required so SteamStub titles don't
    # open the Steam install/license dialog before winmm/SteamFix can load.
    if(_name STREQUAL "steamless")
      file(COPY "${_f}" DESTINATION "${DEST_DIR}")
      message(STATUS "online_fix_kit: steamless/")
    endif()
    continue()
  endif()
  file(COPY "${_f}" DESTINATION "${DEST_DIR}")
  message(STATUS "online_fix_kit: ${_name}")
endforeach()
