		
gccflags = -w
src = lthread_compute.c  lthread_io.c lthread_epoll.c lthread_poller.c lthread_sched.c lthread_socket.c lthread.c  

all: $(src)
	gcc  -c *.c $(gccflags)
	ar rvs liblthread.a *.o
	rm -f *.o *.so

install: lthread.o
	cp liblthread.a /usr/local/lib/
	cp lthread.h /usr/local/include/
	rm lthread.o

test: ../tests/test_lthread.c
	gcc ../tests/test_lthread.c -o ../tests/test_lthread  -llthread -lpthread $(gccflags)
	gcc ../tests/lthread_io.c -o ../tests/lthread_io  -llthread -lpthread $(gccflags)
	gcc  ../tests/lthread_join.c -o ../tests/lthread_join  -llthread -lpthread $(gccflags)
	gcc ../tests/lthread_pipe.c -o ../tests/lthread_pipe  -llthread -lpthread $(gccflags)
	gcc ../tests/lthread_sleep.c -o ../tests/lthread_sleep  -llthread -lpthread $(gccflags)
	gcc ../tests/lthread_socket.c -o ../tests/lthread_socket  -llthread  -lpthread $(gccflags)
	gcc ../tests/lthread_unit_test_compute.c -o ../tests/lthread_unit_test_compute -llthread -lpthread $(gccflags)


uninstall: 
	rm /usr/local/lib/liblthread.a
	rm /usr/local/include/lthread.h

clean: 
	rm -f *.o *.so *.a