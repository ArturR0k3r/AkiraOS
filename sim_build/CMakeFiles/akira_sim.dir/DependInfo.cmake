
# Consider dependencies only in project.
set(CMAKE_DEPENDS_IN_PROJECT_ONLY OFF)

# The set of languages for which implicit dependencies are needed:
set(CMAKE_DEPENDS_LANGUAGES
  "ASM"
  )
# The set of files for implicit dependencies of each language:
set(CMAKE_DEPENDS_CHECK_ASM
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/arch/invokeNative_em64.s" "/home/artur_ubuntu/Akira/AkiraOS/sim_build/CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/arch/invokeNative_em64.s.o"
  )
set(CMAKE_ASM_COMPILER_ID "GNU")

# Preprocessor definitions for this target.
set(CMAKE_TARGET_DEFINITIONS_ASM
  "BH_DEBUG=1"
  "BH_FREE=wasm_runtime_free"
  "BH_MALLOC=wasm_runtime_malloc"
  "BH_PLATFORM_LINUX"
  "BUILD_TARGET_X86_64"
  "WASM_DISABLE_HW_BOUND_CHECK=0"
  "WASM_DISABLE_STACK_HW_BOUND_CHECK=0"
  "WASM_DISABLE_WAKEUP_BLOCKING_OP=0"
  "WASM_DISABLE_WRITE_GS_BASE=0"
  "WASM_ENABLE_AOT_INTRINSICS=0"
  "WASM_ENABLE_BULK_MEMORY=1"
  "WASM_ENABLE_BULK_MEMORY_OPT=1"
  "WASM_ENABLE_CALL_INDIRECT_OVERLONG=1"
  "WASM_ENABLE_EXTENDED_CONST_EXPR=0"
  "WASM_ENABLE_FAST_INTERP=0"
  "WASM_ENABLE_GLOBAL_HEAP_POOL=1"
  "WASM_ENABLE_INTERP=1"
  "WASM_ENABLE_LIBC_BUILTIN=1"
  "WASM_ENABLE_MINI_LOADER=0"
  "WASM_ENABLE_MULTI_MODULE=0"
  "WASM_ENABLE_QUICK_AOT_ENTRY=0"
  "WASM_ENABLE_REF_TYPES=1"
  "WASM_ENABLE_SHARED_MEMORY=0"
  "WASM_ENABLE_SHRUNK_MEMORY=1"
  "WASM_GLOBAL_HEAP_SIZE=131072"
  "WASM_HAVE_MREMAP=1"
  "_GNU_SOURCE"
  )

# The include file search paths:
set(CMAKE_ASM_TARGET_INCLUDE_PATH
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/interpreter"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/libraries/libc-builtin"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/../modules/wasm-micro-runtime/core/iwasm/include"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/linux"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/linux/../include"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils"
  "/home/artur_ubuntu/Akira/AkiraOS/sim"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/host"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/wamr"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/../modules/wasm-micro-runtime/core/shared"
  "/usr/include"
  "/usr/include/SDL2"
  )

# The set of dependency files which are needed:
set(CMAKE_DEPENDS_DEPENDENCY_FILES
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_application.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_application.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_application.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_blocking_op.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_blocking_op.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_blocking_op.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_c_api.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_c_api.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_c_api.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_exec_env.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_exec_env.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_exec_env.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_loader_common.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_loader_common.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_loader_common.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_memory.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_memory.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_memory.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_native.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_native.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_native.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_runtime_common.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_runtime_common.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_runtime_common.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_shared_memory.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_shared_memory.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/common/wasm_shared_memory.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/interpreter/wasm_interp_classic.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/interpreter/wasm_interp_classic.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/interpreter/wasm_interp_classic.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/interpreter/wasm_loader.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/interpreter/wasm_loader.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/interpreter/wasm_loader.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/interpreter/wasm_runtime.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/interpreter/wasm_runtime.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/interpreter/wasm_runtime.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/libraries/libc-builtin/libc_builtin_wrapper.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/libraries/libc-builtin/libc_builtin_wrapper.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/iwasm/libraries/libc-builtin/libc_builtin_wrapper.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_alloc.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_alloc.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_alloc.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_gc.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_gc.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_gc.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_hmu.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_hmu.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_hmu.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_kfc.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_kfc.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/ems/ems_kfc.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/mem_alloc.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/mem_alloc.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/mem-alloc/mem_alloc.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_blocking_op.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_blocking_op.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_blocking_op.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_malloc.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_malloc.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_malloc.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_memmap.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_memmap.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_memmap.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_sleep.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_sleep.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_sleep.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_thread.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_thread.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_thread.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_time.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_time.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/common/posix/posix_time.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/linux/platform_init.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/linux/platform_init.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/platform/linux/platform_init.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_assert.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_assert.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_assert.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_bitmap.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_bitmap.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_bitmap.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_common.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_common.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_common.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_hashmap.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_hashmap.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_hashmap.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_leb128.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_leb128.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_leb128.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_list.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_list.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_list.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_log.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_log.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_log.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_queue.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_queue.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_queue.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_vector.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_vector.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/bh_vector.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/runtime_timer.c" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/runtime_timer.c.o" "gcc" "CMakeFiles/akira_sim.dir/home/artur_ubuntu/Akira/AkiraOS/modules/wasm-micro-runtime/core/shared/utils/runtime_timer.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/host/sim_display.c" "CMakeFiles/akira_sim.dir/host/sim_display.c.o" "gcc" "CMakeFiles/akira_sim.dir/host/sim_display.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/host/sim_input.c" "CMakeFiles/akira_sim.dir/host/sim_input.c.o" "gcc" "CMakeFiles/akira_sim.dir/host/sim_input.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/host/sim_log.c" "CMakeFiles/akira_sim.dir/host/sim_log.c.o" "gcc" "CMakeFiles/akira_sim.dir/host/sim_log.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/host/sim_storage.c" "CMakeFiles/akira_sim.dir/host/sim_storage.c.o" "gcc" "CMakeFiles/akira_sim.dir/host/sim_storage.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/host/sim_time.c" "CMakeFiles/akira_sim.dir/host/sim_time.c.o" "gcc" "CMakeFiles/akira_sim.dir/host/sim_time.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/main.c" "CMakeFiles/akira_sim.dir/main.c.o" "gcc" "CMakeFiles/akira_sim.dir/main.c.o.d"
  "/home/artur_ubuntu/Akira/AkiraOS/sim/wamr/wamr_host.c" "CMakeFiles/akira_sim.dir/wamr/wamr_host.c.o" "gcc" "CMakeFiles/akira_sim.dir/wamr/wamr_host.c.o.d"
  )

# Targets to which this target links which contain Fortran sources.
set(CMAKE_Fortran_TARGET_LINKED_INFO_FILES
  )

# Targets to which this target links which contain Fortran sources.
set(CMAKE_Fortran_TARGET_FORWARD_LINKED_INFO_FILES
  )

# Fortran module output directory.
set(CMAKE_Fortran_TARGET_MODULE_DIR "")
