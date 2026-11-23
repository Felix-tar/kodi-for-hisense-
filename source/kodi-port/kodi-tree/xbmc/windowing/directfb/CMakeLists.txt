set(SOURCES EvdevInputHandler.cpp
            FlexDenonStatus.cpp
            FlexVolumeBar.cpp
            WinSystemDirectFB.cpp
            WinSystemDirectFBGLESContext.cpp)

set(HEADERS EvdevInputHandler.h
            FlexVolumeBar.h
            WinSystemDirectFB.h
            WinSystemDirectFBGLESContext.h)

core_add_library(windowing_directfb)
