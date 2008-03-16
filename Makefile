dss_objects := dss.o cmdline.o string.o fd.o exec.o signal.o daemon.o df.o
all: dss
man: dss.1

DEBUG_CPPFLAGS += -Wno-sign-compare -g -Wunused -Wundef -W
DEBUG_CPPFLAGS += -Wredundant-decls
CPPFLAGS += -Os
CPPFLAGS += -Wall
CPPFLAGS += -Wuninitialized
CPPFLAGS += -Wchar-subscripts
CPPFLAGS += -Wformat-security
CPPFLAGS += -Werror-implicit-function-declaration
CPPFLAGS += -Wmissing-format-attribute
CPPFLAGS += -Wunused-macros
CPPFLAGS += -Wbad-function-cast

Makefile.deps: $(wildcard *.c *.h)
	gcc -MM -MG *.c > $@

include Makefile.deps

dss: $(dss_objects)
	$(CC) $(CPPFLAGS) $(DEBUG_CPPFLAGS) -o $@ $(dss_objects)

%.o: %.c
	$(CC) -c $(CPPFLAGS) $(DEBUG_CPPFLAGS) $<

%.ppm: %.sk
	sk2ppm $< > $@
%.png: %.ppm
	convert $< $@

cmdline.c cmdline.h: dss.ggo
	gengetopt --unamed-opts=command --conf-parser < $<

dss.1: dss
	help2man -N ./$< > $@

%.1.html: %.1
	man2html $< > $@

clean:
	rm -f *.o dss dss.1 dss.1.html Makefile.deps *.ppm *.png *~ cmdline.c cmdline.h


