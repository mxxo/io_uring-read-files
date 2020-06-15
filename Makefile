# make && ./read_files && echo "OK" 

default: 
	gcc -Wall -O2 -o read_files read_files.c -Iliburing/src/include liburing/src/liburing.a 
