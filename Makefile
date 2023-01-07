prefix = /usr
CPP=g++
LIBEVENTDIR=/usr
CPPFLAGS := -std=c++11
CPPFLAGS += -O2
#CPPFLAGS += -g
CPPFLAGS += -s

all: iccom_recv iccom_send iccsh iccshd

iccom_recv: ./lib/iccom.c iccom_recv.c
	$(CPP) $(CPPFLAGS) ./lib/iccom.c iccom_recv.cpp -I./ -o iccom_recv

iccom_send: ./lib/iccom.c iccom_send.c
	$(CPP) $(CPPFLAGS) ./lib/iccom.c iccom_send.cpp -I./ -o iccom_send

iccsh: ./lib/iccom.c iccsh.cpp
	$(CPP) $(CPPFLAGS) -DBUILD_TARGET=0 ./lib/iccom.c iccsh.cpp -I./ -lpthread -lutil -o iccsh

iccshd: ./lib/iccom.c iccsh.cpp
	$(CPP) $(CPPFLAGS) -DBUILD_TARGET=1 ./lib/iccom.c iccsh.cpp -I./ -lpthread -lutil -o iccshd

.PHONY: clean install
clean:
	rm -vf iccom_recv iccom_send iccsh iccshd
install:
	cp iccom_send $(prefix)/bin/iccom_send
	cp iccom_recv $(prefix)/bin/iccom_recv
	cp iccsh $(prefix)/bin/iccsh
	cp iccshd $(prefix)/bin/iccshd
	cp install_symspi.sh $(prefix)/bin/install_symspi.sh
	cp test_loop_send.sh $(prefix)/bin/test_loop_send.sh
	cp test_loop_recv.sh $(prefix)/bin/test_loop_recv.sh
