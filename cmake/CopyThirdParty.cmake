# Copy OS-specific third_party tree into the plugin bundle (flat: SLSsteam/LumaCore).
# Skip legacy DepotDownloaderMod / portable .NET - downloads use linked depotkit.
if(NOT EXISTS "${SOURCE_DIR}")
  message(WARNING "third_party OS dir missing: ${SOURCE_DIR} — run tools/fetch_third_party.sh")
  return()
endif()

file(GLOB _CHILDREN RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")
foreach(_child ${_CHILDREN})
  if(_child STREQUAL "DDMod" OR _child STREQUAL "dotnet")
    continue()
  endif()
  if(IS_DIRECTORY "${SOURCE_DIR}/${_child}")
    file(COPY "${SOURCE_DIR}/${_child}" DESTINATION "${DEST_DIR}")
  else()
    file(COPY "${SOURCE_DIR}/${_child}" DESTINATION "${DEST_DIR}")
  endif()
endforeach()
