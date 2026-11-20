cmake_minimum_required(VERSION 3.22)
project(DiskDiver VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CTest)
option(DISK_DIVER_GUI "Build the desktop application" ON)
option(DISK_DIVER_NTFS_LAYOUT "Enable the benchmark-validated Windows volume layout backend" OFF)
set(DISK_DIVER_ENGINE "rust" CACHE STRING "Desktop scanner: rust or cpp")
set_property(CACHE DISK_DIVER_ENGINE PROPERTY STRINGS cpp rust)
if(NOT DISK_DIVER_ENGINE MATCHES "^(cpp|rust)$")
  message(FATAL_ERROR "DISK_DIVER_ENGINE must be cpp or rust")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(cpp)
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:disk-diver-cpp>/disk-diver-engine.txt"
     CONTENT "${DISK_DIVER_ENGINE}\n")
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:disk-diver-cpp>/disk-diver-ntfs-layout.txt"
     CONTENT "${DISK_DIVER_NTFS_LAYOUT}\n")
install(FILES "$<TARGET_FILE_DIR:disk-diver-cpp>/disk-diver-engine.txt"
              "$<TARGET_FILE_DIR:disk-diver-cpp>/disk-diver-ntfs-layout.txt"
        DESTINATION ${CMAKE_INSTALL_BINDIR})
if(DISK_DIVER_ENGINE STREQUAL "rust")
  find_program(CARGO_EXECUTABLE cargo REQUIRED)
  set(rust_binary "${CMAKE_BINARY_DIR}/cargo/release/disk-diver-rust${CMAKE_EXECUTABLE_SUFFIX}")
  add_custom_target(disk-diver-rust ALL
    COMMAND "${CARGO_EXECUTABLE}" build --release --locked
            --manifest-path "${CMAKE_SOURCE_DIR}/rust/Cargo.toml"
            --target-dir "${CMAKE_BINARY_DIR}/cargo"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${rust_binary}"
            "$<TARGET_FILE_DIR:disk-diver-cpp>/disk-diver-rust${CMAKE_EXECUTABLE_SUFFIX}"
    VERBATIM)
  install(PROGRAMS "${rust_binary}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(DISK_DIVER_GUI)
  add_subdirectory(gui)
  install(TARGETS disk-diver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS disk-diver-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES worker.py dd_journal.py dd_privilege.py DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/disk-diver)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME cpp-cache-directory-identity COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/tests/check_excluded_dirs.py" "$<TARGET_FILE:disk-diver-cpp>")
  if(TARGET disk-diver-rust)
    add_test(NAME rust-cache-directory-identity COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tests/check_excluded_dirs.py" "${rust_binary}")
  endif()
  add_test(NAME worker-integration
    COMMAND ${CMAKE_COMMAND} -E env "DISK_DIVER_BIN_DIR=$<TARGET_FILE_DIR:disk-diver-cpp>"
    ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tests/test_worker.py")
  add_test(NAME benchmark-selection
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s "${PROJECT_SOURCE_DIR}/bench" -p "test*.py")
  add_test(NAME journal-cursors
    COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tests/test_journal.py")
endif()
