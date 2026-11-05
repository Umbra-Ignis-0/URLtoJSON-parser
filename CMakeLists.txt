cmake_minimum_required(VERSION 3.20)

project(URLtoJSON_parserPLUSfilter LANGUAGES CXX)

add_executable(URLtoJSON_parserPLUSfilter URLtoJSON_parserPLUSfilter.cpp)
target_compile_features(URLtoJSON_parserPLUSfilter PRIVATE cxx_std_20)

# libcurl comes from vcpkg (see vcpkg.json)
find_package(CURL CONFIG REQUIRED)
target_link_libraries(URLtoJSON_parserPLUSfilter PRIVATE CURL::libcurl)
