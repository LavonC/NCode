# NCode
A C++ Text Editor built using SDL3, with Inline AI Suggestions and Live Collaboration

## Features:
- Create, Read, Write and Save files
- Text Selection
- Ctrl+C, Ctrl+V and Ctrl+A functionality

## Work In Progress
- Folder Structure View
- Inline AI Suggestions
- Live Collaboration

## Steps to Run the Project
- Install VCPKG
- Install dependencies using VCPKG "vcpkg install sdl3 sdl3-image sdl3-mixer sdl3-net sdl3-ttf llama-cpp"
- Configure CMAKE in Project Folder using "cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE={PATH TO PARENT FOLDER OF VCPKG}/VCPKG/vcpkg/scripts/buildsystems/vcpkg.cmake"
- Run "cmake --build build" in Project Folder
- Execute NCode.exe (in build folder)