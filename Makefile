all:
	gcc -g -o ewm main.c -lxcb -lxcb-randr -lxcb-icccm
format:
	find . -name '*.c' | xargs clang-format -i -style=file
