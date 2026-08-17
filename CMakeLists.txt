cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
endif()

project(scebt_probe C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# Keep the established internal target and exported stub ABI, but publish the
# kernel plugin under the controller product name.
set(RELEASE_BASENAME vita_ns_controller)

add_executable(${PROJECT_NAME}
  trace.c
  kernel/diagnostic_trace.c
  protocol/procon.c
  storage/bond_record.c
  storage/bond_state.c
)
target_include_directories(${PROJECT_NAME} PRIVATE
  kernel
  protocol
  storage
)
target_compile_definitions(${PROJECT_NAME} PRIVATE __VITA_KERNEL__)
target_compile_options(${PROJECT_NAME} PRIVATE
  -Wall -Wextra -Werror -O2 -std=gnu99 -fno-pic -fno-PIC
)
target_link_options(${PROJECT_NAME} PRIVATE -nostdlib -Wl,-q)
target_link_libraries(${PROJECT_NAME}
  taihenForKernel_stub
  SceIofilemgrForDriver_stub
  SceModulemgrForDriver_stub
  SceModulemgrForKernel_stub
  SceProcEventForDriver_stub
  ScePowerForDriver_stub
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceSysrootForKernel_stub
  SceThreadmgrForDriver_stub
  gcc
)

vita_create_self(${RELEASE_BASENAME}.skprx ${PROJECT_NAME}
  CONFIG ${CMAKE_SOURCE_DIR}/${PROJECT_NAME}.yml
  UNSAFE
)

vita_create_stubs(${PROJECT_NAME}_stubs ${PROJECT_NAME}
  ${CMAKE_SOURCE_DIR}/${PROJECT_NAME}.yml
  KERNEL
)
