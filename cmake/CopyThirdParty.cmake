# Copy OS-specific third_party tree into the plugin bundle (flat: DDMod, SLSsteam/LumaCore, dotnet).
if(NOT EXISTS "${SOURCE_DIR}")
  message(WARNING "third_party OS dir missing: ${SOURCE_DIR} — run tools/fetch_third_party.sh")
  return()
endif()

file(GLOB _CHILDREN RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")
foreach(_child ${_CHILDREN})
  if(IS_DIRECTORY "${SOURCE_DIR}/${_child}")
    file(COPY "${SOURCE_DIR}/${_child}" DESTINATION "${DEST_DIR}")
  else()
    file(COPY "${SOURCE_DIR}/${_child}" DESTINATION "${DEST_DIR}")
  endif()
endforeach()
