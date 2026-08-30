# Bandwidth Governor tests.
add_executable(bg_tests
  test_main.cpp
  test_core.cpp
)
target_link_libraries(bg_tests PRIVATE bgcore)
target_include_directories(bg_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bg_core_tests COMMAND bg_tests)

add_executable(bg_mp test_mp.cpp)
target_link_libraries(bg_mp PRIVATE bgcore)
target_include_directories(bg_mp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bg_authority_proof COMMAND bg_mp)
