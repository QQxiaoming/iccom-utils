prefix = /usr
CPP=g++
LIBEVENTDIR=/usr
CPPFLAGS := -std=c++11
CPPFLAGS += -O2
#CPPFLAGS += -g
CPPFLAGS += -s

all: iccom_recv iccom_send iccshd iccsh icccp

iccom_recv: ./lib/iccom.c iccom_recv.cpp
	$(CPP) $(CPPFLAGS) ./lib/iccom.c iccom_recv.cpp -I./ -I./lib/ -o iccom_recv

iccom_send: ./lib/iccom.c iccom_send.cpp
	$(CPP) $(CPPFLAGS) ./lib/iccom.c iccom_send.cpp -I./ -I./lib/  -o iccom_send

iccshd: ./lib/iccom.c iccsh.cpp
	$(CPP) $(CPPFLAGS) -DBUILD_TARGET=0 ./lib/iccom.c iccsh.cpp -I./ -I./lib/  -lpthread -lutil -o iccshd

iccsh: ./lib/iccom.c iccsh.cpp
	$(CPP) $(CPPFLAGS) -DBUILD_TARGET=1 ./lib/iccom.c iccsh.cpp -I./ -I./lib/  -lpthread -lutil -o iccsh

icccp: ./lib/iccom.c iccsh.cpp
	$(CPP) $(CPPFLAGS) -DBUILD_TARGET=2 ./lib/iccom.c iccsh.cpp -I./ -I./lib/  -lpthread -lutil -o icccp

.PHONY: clean install
clean:
	rm -vf iccom_recv iccom_send iccshd iccsh icccp
install:
	cp iccom_send $(prefix)/bin/iccom_send
	cp iccom_recv $(prefix)/bin/iccom_recv
	cp iccshd $(prefix)/bin/iccshd
	cp iccsh $(prefix)/bin/iccsh
	cp icccp $(prefix)/bin/icccp
	cp install_symspi.sh $(prefix)/bin/install_symspi.sh
	cp test_loop_send.sh $(prefix)/bin/test_loop_send.sh
	cp test_loop_recv.sh $(prefix)/bin/test_loop_recv.sh
