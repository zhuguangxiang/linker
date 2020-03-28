
ifeq ($(CFLAGS),)
	CFLAGS := -g
endif

ifeq ($(BUILDDIR),)
	MD := .
else
	MD := $(BUILDDIR)/linker
endif

INCLUDE := -I../include -Iinclude/linker -Iinclude -I../include/nc -I../kernel/include

.PHONY: mod.o klib

all: demo mod.o link

link: link.c elf.c
	$(CC) $(CFLAGS) $(INCLUDE) link.c elf.c -o link

demo: demo.c elf.c
	$(CC) $(CFLAGS) $(INCLUDE) demo.c elf.c -static -o demo

mod.o:
	$(CC) -c mod.c -o mod.o

klib: elf.c Makefile
	@mkdir -p $(MD)
	@echo "XCC elf.c"
	$(Q)$(CC) $(CFLAGS) $(INCLUDE) -c elf.c -o $(MD)/elf.o
	$(Q)ar rcs $(BUILDDIR)/liblinker.a $(MD)/elf.o

clean:
	rm -f demo mod.o link elf.o liblinker.a
