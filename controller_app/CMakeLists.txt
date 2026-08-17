cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
endif()

project(vita_ns_pad C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# The internal target remains stable for the kernel-stub ABI; the distributable
# filename follows the user-facing product name.
set(RELEASE_BASENAME vita_ns_controller)

set(SCEBT_PLUGIN_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build" CACHE PATH
  "Directory containing the VitaNsPad kernel import stub")
set(SCEBT_PLUGIN_STUB "${SCEBT_PLUGIN_BUILD_DIR}/libscebt_probe_stub.a")

add_executable(${PROJECT_NAME} main.c)
target_include_directories(${PROJECT_NAME} PRIVATE ../protocol)
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Werror -O2 -std=gnu99)
target_link_libraries(${PROJECT_NAME}
  vita2d
  SceAppMgr_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceIofilemgr_stub
  ScePgf_stub
  SceProcessmgr_stub
  ScePvf_stub
  SceSysmodule_stub
  SceTouch_stub
  ${SCEBT_PLUGIN_STUB}
  m
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${RELEASE_BASENAME}.vpk VITANSPAD ${PROJECT_NAME}.self
  VERSION 01.00
  NAME "Vita NS Controller"
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
