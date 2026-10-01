idf_component_register(SRCS "src/fleet.cpp" INCLUDE_DIRS "include" REQUIRES blip_core)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
