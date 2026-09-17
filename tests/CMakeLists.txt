include(FetchContent)

find_package(Threads REQUIRED)

FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG 1da23a3e8119ec5cce4f9388e91b065e20bf06f5 # v2.4.12
)
FetchContent_MakeAvailable(doctest)

function(taskloom_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE taskloom::taskloom doctest::doctest
    Threads::Threads)
  target_compile_options(${name} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
  )
  if(TASKLOOM_SANITIZE)
    if(MSVC)
      message(WARNING "TASKLOOM_SANITIZE is not supported with MSVC; ignoring")
    else()
      target_compile_options(${name} PRIVATE
        -fsanitize=${TASKLOOM_SANITIZE} -g -fno-omit-frame-pointer)
      target_link_options(${name} PRIVATE -fsanitize=${TASKLOOM_SANITIZE})
    endif()
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

taskloom_add_test(hw_test)
taskloom_add_test(atomic_test)
taskloom_add_test(rwlock_test)
taskloom_add_test(stats_test)
taskloom_add_test(event_test)
