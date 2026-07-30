# Copy host-OS online_fix_kit into the plugin bundle (flat DEST_DIR).
#
# Inputs:
#   SOURCE_DIR — repo online_fix_kit/ (with windows/ and linux/ subdirs), or legacy flat kit
#   DEST_DIR   — bundle online_fix_kit/
#   KIT_OS     — optional: windows | linux (default: host)

if(NOT DEFINED KIT_OS OR KIT_OS STREQUAL "")
  if(WIN32)
    set(KIT_OS "windows")
  else()
    set(KIT_OS "linux")
  endif()
endif()

if(NOT EXISTS "${SOURCE_DIR}")
  message(STATUS "online_fix_kit: source missing (${SOURCE_DIR})")
  return()
endif()

set(_PLATFORM_SRC "${SOURCE_DIR}/${KIT_OS}")
set(_USED_FALLBACK FALSE)

# Prefer platform subdir; fall back to legacy flat layout.
if(NOT EXISTS "${_PLATFORM_SRC}")
  if(EXISTS "${SOURCE_DIR}/SteamFix64.dll" OR EXISTS "${SOURCE_DIR}/OnlineFix64.dll"
     OR EXISTS "${SOURCE_DIR}/OnlineFix.dll")
    set(_PLATFORM_SRC "${SOURCE_DIR}")
    message(STATUS "online_fix_kit: using legacy flat layout")
  else()
    message(STATUS "online_fix_kit: platform source missing (${SOURCE_DIR}/${KIT_OS})")
    return()
  endif()
endif()

# Linux OF.me preferred; if absent, use Windows SteamFix x64 (no unpacked x86 PE).
if(KIT_OS STREQUAL "linux")
  if(NOT EXISTS "${_PLATFORM_SRC}/OnlineFix64.dll" AND NOT EXISTS "${_PLATFORM_SRC}/OnlineFix.dll")
    if(EXISTS "${SOURCE_DIR}/windows/SteamFix64.dll"
       AND EXISTS "${SOURCE_DIR}/windows/winmm.dll")
      message(STATUS "online_fix_kit: linux OF.me missing; falling back to windows SteamFix x64")
      set(_PLATFORM_SRC "${SOURCE_DIR}/windows")
      set(_USED_FALLBACK TRUE)
    endif()
  endif()
endif()

set(_SKIP_NAMES
  "x86_optional" # unpacked 32-bit PE — never copy into AppData payload
  "README.md"
  ".gitkeep"
)

file(GLOB _KIT_ENTRIES "${_PLATFORM_SRC}/*")
foreach(_f IN LISTS _KIT_ENTRIES)
  get_filename_component(_name "${_f}" NAME)
  list(FIND _SKIP_NAMES "${_name}" _skip_idx)
  if(NOT _skip_idx EQUAL -1)
    continue()
  endif()

  # Never ship loose 32-bit FreeTP PE in the default payload (Defender Dllhijack).
  if(_name STREQUAL "SteamFix32.dll" OR _name STREQUAL "winmm32.dll")
    message(STATUS "online_fix_kit: skip ${_name} (use x86_optional.zip on demand)")
    continue()
  endif()

  if(IS_DIRECTORY "${_f}")
    if(_name STREQUAL "steamless")
      # Steamless is Windows-only; skip on pure OF.me linux kits, keep on SteamFix fallback.
      if(KIT_OS STREQUAL "windows" OR _USED_FALLBACK)
        file(COPY "${_f}" DESTINATION "${DEST_DIR}")
        message(STATUS "online_fix_kit: steamless/")
      endif()
    endif()
    continue()
  endif()

  # Prefer zip for optional x86 (Windows SteamFix32 or Linux OF.me OnlineFix.dll).
  if(_name STREQUAL "x86_optional.zip")
    file(COPY "${_f}" DESTINATION "${DEST_DIR}")
    message(STATUS "online_fix_kit: x86_optional.zip")
    continue()
  endif()

  # Never ship loose 32-bit OF.me PE next to the plugin (same Defender hygiene).
  if(_name STREQUAL "OnlineFix.dll" OR _name STREQUAL "OnlineFix32.dll"
     OR _name STREQUAL "SteamOverlay32.dll" OR _name STREQUAL "StubDRM32.dll")
    # Only skip when an x86 zip exists; otherwise keep for incomplete kits.
    if(EXISTS "${_PLATFORM_SRC}/x86_optional.zip")
      message(STATUS "online_fix_kit: skip ${_name} (use x86_optional.zip on demand)")
      continue()
    endif()
  endif()

  file(COPY "${_f}" DESTINATION "${DEST_DIR}")
  message(STATUS "online_fix_kit: ${_name}")
endforeach()

# If zip missing but x86_optional PE exist, build zip at package time.
set(_X86_DIR "${_PLATFORM_SRC}/x86_optional")
set(_ZIP_OUT "${DEST_DIR}/x86_optional.zip")
if(NOT EXISTS "${_ZIP_OUT}" AND EXISTS "${_X86_DIR}")
  find_program(_ZIP_TOOL zip)
  if(_ZIP_TOOL)
    if(EXISTS "${_X86_DIR}/SteamFix32.dll" AND EXISTS "${_X86_DIR}/winmm32.dll")
      execute_process(
        COMMAND "${_ZIP_TOOL}" -q -j "${_ZIP_OUT}"
                "${_X86_DIR}/SteamFix32.dll" "${_X86_DIR}/winmm32.dll"
        RESULT_VARIABLE _zip_rc)
      if(_zip_rc EQUAL 0)
        message(STATUS "online_fix_kit: created x86_optional.zip (SteamFix32)")
      endif()
    elseif(EXISTS "${_X86_DIR}/OnlineFix.dll" AND EXISTS "${_X86_DIR}/winmm32.dll")
      set(_OF_X86_ARGS "${_X86_DIR}/OnlineFix.dll" "${_X86_DIR}/winmm32.dll")
      if(EXISTS "${_X86_DIR}/SteamOverlay32.dll")
        list(APPEND _OF_X86_ARGS "${_X86_DIR}/SteamOverlay32.dll")
      endif()
      if(EXISTS "${_X86_DIR}/StubDRM32.dll")
        list(APPEND _OF_X86_ARGS "${_X86_DIR}/StubDRM32.dll")
      endif()
      execute_process(
        COMMAND "${_ZIP_TOOL}" -q -j "${_ZIP_OUT}" ${_OF_X86_ARGS}
        RESULT_VARIABLE _zip_rc)
      if(_zip_rc EQUAL 0)
        message(STATUS "online_fix_kit: created x86_optional.zip (OnlineFix.dll)")
      endif()
    endif()
  else()
    message(STATUS "online_fix_kit: zip tool missing; 32-bit on-demand kit not packaged")
  endif()
endif()

# Linux OF.me kits never include EpicFix (FreeTP-only). Keep a sibling
# online_fix_kit/windows/ with EpicFix64 for RedpointEOS titles under Proton.
# Do not flatten SteamFix into the linux payload — only EpicFix addon files.
if(KIT_OS STREQUAL "linux" AND NOT _USED_FALLBACK)
  set(_WIN_SRC "${SOURCE_DIR}/windows")
  set(_EPIC_DEST "${DEST_DIR}/windows")
  if(EXISTS "${_WIN_SRC}/EpicFix64.dll")
    file(MAKE_DIRECTORY "${_EPIC_DEST}")
    file(COPY "${_WIN_SRC}/EpicFix64.dll" DESTINATION "${_EPIC_DEST}")
    message(STATUS "online_fix_kit: windows/EpicFix64.dll (EOS addon for OF.me)")
    if(EXISTS "${_WIN_SRC}/EpicFix.ini")
      file(COPY "${_WIN_SRC}/EpicFix.ini" DESTINATION "${_EPIC_DEST}")
      message(STATUS "online_fix_kit: windows/EpicFix.ini")
    endif()
  endif()
endif()
