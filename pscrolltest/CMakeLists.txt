
cmake_minimum_required(VERSION 2.6)

project(pscrolltest)

find_package(X11)

add_executable(pscrolltest pscrolltest.c)
target_link_libraries(pscrolltest ${X11_LIBRARIES} ${X11_Xinput_LIB})

install(TARGETS pscrolltest
	RUNTIME DESTINATION bin
)
