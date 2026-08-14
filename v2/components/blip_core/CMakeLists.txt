idf_component_register(SRCS "src/descriptor_json.cpp" INCLUDE_DIRS "include")

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
