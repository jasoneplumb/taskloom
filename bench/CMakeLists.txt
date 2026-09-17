find_package(Threads REQUIRED)

function(taskloom_add_bench name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE taskloom::taskloom Threads::Threads)
  target_compile_options(${name} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
  )
endfunction()

taskloom_add_bench(wsq_bench)
taskloom_add_bench(rwlock_bench)
