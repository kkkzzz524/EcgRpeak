cmake_minimum_required(VERSION 3.21)

project(EcgRpeakDetector VERSION 1.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

# ---------------- 算法核心库（不依赖 Widgets，可单独做精度自测） ----------------
qt_add_library(ecgcore STATIC
    src/pantompkins.cpp
    src/pantompkins.h
    src/ecgsignal.cpp
    src/ecgsignal.h
    src/pwave.cpp
    src/pwave.h
    src/dsp.h
)
target_link_libraries(ecgcore PUBLIC Qt6::Core)
target_include_directories(ecgcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------- GUI 程序 ----------------
qt_add_executable(EcgRpeakDetector
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/binarydialog.cpp
    src/binarydialog.h
    src/plotwidget.cpp
    src/plotwidget.h
    src/resources.qrc
)

if (WIN32)
    enable_language(RC)
    target_sources(EcgRpeakDetector PRIVATE src/app.rc)
endif()

target_link_libraries(EcgRpeakDetector PRIVATE ecgcore Qt6::Widgets)

set_target_properties(EcgRpeakDetector PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

# ---------------- 命令行精度自测 ----------------
qt_add_executable(rpeak_selftest tools/selftest.cpp)
target_link_libraries(rpeak_selftest PRIVATE ecgcore)

# ---------------- 命令行 P 波自测 ----------------
qt_add_executable(pwave_test tools/pwave_test.cpp)
target_link_libraries(pwave_test PRIVATE ecgcore)

if (MINGW)
    target_compile_options(ecgcore PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(EcgRpeakDetector PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(rpeak_selftest PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(pwave_test PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

install(TARGETS EcgRpeakDetector
    BUNDLE  DESTINATION "."
    RUNTIME DESTINATION "."
)
