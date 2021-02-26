add_executable(testruntime testruntime.cpp)
add_dependencies(apps testruntime)
target_link_libraries(testruntime PRIVATE Galois::shmem lonestar pmemobj pmem)
install(TARGETS testruntime DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)
