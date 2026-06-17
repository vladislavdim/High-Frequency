cmake_minimum_required(VERSION 3.14)
project(Sentinel_Core VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Оптимизация для максимальной скорости (-O3)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -pthread")

# Поиск необходимых библиотек индустриального стандарта
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Включаем заголовочные файлы
include_directories(include)

# Собираем исполняемый файл
add_executable(sentinel_node 
    src/Sentinel_Main.cpp 
    # Сюда же добавляется скомпилированный ранее Sentinel_Analysis_Core.cpp
)

# Линковка библиотек
target_link_libraries(sentinel_node 
    Boost::system 
    Boost::thread 
    OpenSSL::SSL 
    OpenSSL::Crypto 
    Threads::Threads
)
