all:
	gcc -g -o ewm main.c -lxcb -lxcb-randr
format:
	find . -name '*.c' | xargs clang-format -i -style=file
