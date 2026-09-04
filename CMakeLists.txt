cmake_minimum_required(VERSION 4.2)
project(SignalLab)

set(CMAKE_CXX_STANDARD 20)

add_executable(SignalLab src/gui/main.cpp
        include/signallab/core/error.h
        include/signallab/core/result.h
        include/signallab/core/signal.h
        src/core/signal.cpp)
