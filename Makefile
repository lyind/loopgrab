
loopgrab: main.cpp
	g++ -g -O3 -std=c++17 -Wall -Wextra -Werror -Wpedantic -pedantic-errors main.cpp -o loopgrab -lX11 -lXtst -lXext -lpng

