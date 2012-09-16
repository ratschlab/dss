dss_objects := cmdline.o dss.o str.o file.o exec.o sig.o daemon.o df.o tv.o snap.o ipc.o
all: dss
man: dss.1

DEBUG_CFLAGS ?=
DEBUG_CFLAGS += -Wno-sign-compare -g -Wunused -Wundef -W
DEBUG_CFLAGS += -Wredundant-decls
CFLAGS ?=
CFLAGS += -Os
CFLAGS += -Wall
CFLAGS += -Wuninitialized
CFLAGS += -Wchar-subscripts
CFLAGS += -Wformat-security
CFLAGS += -Werror-implicit-function-declaration
CFLAGS += -Wmissing-format-attribute
CFLAGS += -Wunused-macros
CFLAGS += -Wbad-function-cast

Makefile.deps: $(wildcard *.c *.h)
	gcc -MM -MG *.c > $@

-include Makefile.deps

dss: $(dss_objects)
	$(CC) -o $@ $(dss_objects)

cmdline.o: cmdline.c cmdline.h
	$(CC) -c $(CFLAGS) $<

%.o: %.c Makefile
	$(CC) -c $(CFLAGS) $(DEBUG_CFLAGS) $<

%.png: %.dia
	dia -e $@ -t png $<

cmdline.c cmdline.h: dss.ggo
	gengetopt --conf-parser < $<

dss.1: dss dss.1.inc
	help2man -h --detailed-help --include dss.1.inc -N ./$< > $@

%.1.html: %.1
	man2html $< > $@

clean:
	rm -f *.o dss dss.1 dss.1.html Makefile.deps *.png *~ cmdline.c cmdline.h index.html

index.html: dss.1.html index.html.in INSTALL README NEWS
	sed -e '/@README@/,$$d' index.html.in > $@
	grutatxt -nb < README >> $@
	sed -e '1,/@README@/d' -e '/@NEWS@/,$$d' index.html.in >> $@
	grutatxt -nb < NEWS >> $@
	sed -e '1,/@NEWS@/d' -e '/@INSTALL@/,$$d' index.html.in >> $@
	grutatxt -nb < INSTALL >> $@
	sed -e '1,/@INSTALL@/d' -e '/@MAN_PAGE@/,$$d' index.html.in >> $@
	sed -e '1,/Return to Main Contents/d' -e '/Index/,$$d' dss.1.html >> $@
	sed -e '1,/@MAN_PAGE@/d' index.html.in >> $@
