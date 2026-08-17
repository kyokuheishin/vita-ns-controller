cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
endif()

project(scebt_test C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITASDK_COMMON "$ENV{VITASDK}/share/gcc-arm-vita-eabi/samples/common")
add_executable(${PROJECT_NAME}
  main.c
  ${VITASDK_COMMON}/debugScreen.c
)
target_include_directories(${PROJECT_NAME} PRIVATE ${VITASDK_COMMON})
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Werror -O2 -std=gnu99)
target_link_libraries(${PROJECT_NAME}
  SceCtrl_stub
  SceDisplay_stub
  SceIofilemgr_stub
  SceProcessmgr_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk SCEBTTEST ${PROJECT_NAME}.self
  VERSION 01.00
  NAME "SceBt Test"
)
