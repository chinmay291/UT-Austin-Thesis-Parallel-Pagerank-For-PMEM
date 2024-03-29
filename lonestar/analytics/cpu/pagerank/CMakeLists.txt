add_executable(pagerank-pull-cpu PageRank-pull.cpp)
add_dependencies(apps pagerank-pull-cpu)
target_link_libraries(pagerank-pull-cpu PRIVATE Galois::shmem lonestar pmemobj pmem)
install(TARGETS pagerank-pull-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_test_scale(small pagerank-pull-cpu -transposedGraph -tolerance=0.01 "${BASEINPUT}/scalefree/transpose/rmat10.tgr")
add_test_scale(small-topo pagerank-pull-cpu -transposedGraph -tolerance=0.01 -algo=Topo "${BASEINPUT}/scalefree/transpose/rmat10.tgr")

add_executable(pagerank-push-cpu PageRank-push.cpp)
add_dependencies(apps pagerank-push-cpu)
target_link_libraries(pagerank-push-cpu PRIVATE Galois::shmem lonestar pmemobj pmem)
install(TARGETS pagerank-push-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_test_scale(small pagerank-push-cpu -tolerance=0.01 "${BASEINPUT}/scalefree/transpose/rmat10.tgr")
add_test_scale(small-sync pagerank-push-cpu -tolerance=0.01 -algo=Sync "${BASEINPUT}/scalefree/transpose/rmat10.tgr")

add_executable(pagerank-chinmay-cpu PageRank-chinmay.cpp)
add_dependencies(apps pagerank-chinmay-cpu)
target_link_libraries(pagerank-chinmay-cpu PRIVATE Galois::shmem lonestar /usr/lib/x86_64-linux-gnu/pmdk_dbg/libpmemobj.so.1 /usr/lib/x86_64-linux-gnu/pmdk_dbg/libpmem.so.1)
install(TARGETS pagerank-chinmay-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

find_package(Boost 1.65.1 COMPONENTS system filesystem serialization REQUIRED)
INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIR})
add_executable(pagerank-csrseg-cpu PageRank-csr_segment.cpp)
add_dependencies(apps pagerank-csrseg-cpu)
target_link_libraries(pagerank-csrseg-cpu PRIVATE Galois::shmem lonestar pmemobj pmem ${Boost_LIBRARIES})
install(TARGETS pagerank-chinmay-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)
