# Copies mimalloc-redirect.dll (needed by the mimalloc override DLL) from SRC_DIR to DST_DIR when it exists.
file(GLOB _redirect "${SRC_DIR}/mimalloc-redirect*.dll")
foreach(_f ${_redirect})
  file(COPY "${_f}" DESTINATION "${DST_DIR}")
endforeach()
