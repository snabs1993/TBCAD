# Empties a scenario's settings and data folders, then copies SEED (a folder of settings files)
# into the settings folder when it exists.
#   cmake -DCONFIG=dir -DDATA=dir -DSEED=dir -P reset_profile.cmake
file(REMOVE_RECURSE "${CONFIG}" "${DATA}")
file(MAKE_DIRECTORY "${CONFIG}" "${DATA}")
if(IS_DIRECTORY "${SEED}")
  file(COPY "${SEED}/" DESTINATION "${CONFIG}")
endif()
