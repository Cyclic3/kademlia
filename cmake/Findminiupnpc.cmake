#find_path(MINIUPNPC_INCLUDE_DIR miniupnpc.h)
find_library(MINIUPNPC_LIBRARIES miniupnpc)

find_package_handle_standard_args(Miniupnpc DEFAULT_MSG
  #MINIUPNPC_INCLUDE_DIR MINIUPNPC_LIBRARIES
)