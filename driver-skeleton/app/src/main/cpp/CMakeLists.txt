cmake_minimum_required(VERSION 3.22.1)
project(siano CXX)

add_library(siano SHARED
    siano_proto.cpp
    t230_proto.cpp
    jni_bridge.cpp
)

target_compile_features(siano PRIVATE cxx_std_17)
target_compile_options(siano PRIVATE -Wall -Wextra)

find_library(log-lib log)
target_link_libraries(siano ${log-lib})
