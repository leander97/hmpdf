# make these work for both my remotes and my local
user=$(shell whoami)
ifeq ($(user),leander)
  PATHTOCLASS = /Users/leander/class_public
else
  PATHTOCLASS = /home/lthiele/class_public
  PATHTOFFTW = /usr/local/fftw/gcc/3.3.4/api
endif

CC = gcc
ARCHIVE = libhmpdf.a
SHARED = libhmpdf.so

CFLAGS = --std=gnu99 -fPIC -Wall -Wextra -Wpedantic -Wno-variadic-macros -Winline -DHAVE_INLINE -DDEBUG -DARICO20
OPTFLAGS = -O4 -ggdb3 -ffast-math
OMPFLAGS = -fopenmp

INCLUDE = -I./include
INCLUDE += -I$(PATHTOCLASS)/include \
           -I$(PATHTOCLASS)/external/HyRec2020 \
           -I$(PATHTOCLASS)/external/RecfastCLASS \
           -I$(PATHTOCLASS)/external/heating \
           -I$(PATHTOFFTW)

LINKER = -L$(PATHTOCLASS)
LINKER += -lclass -lgsl -lgslcblas -lm -lfftw3

SRCDIR = ./src
OBJDIR = ./obj
OUTDIR = ./lib
SODIR  = .

.PHONY: directories

all: directories $(SHARED) $(ARCHIVE)

$(SHARED): $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(wildcard $(SRCDIR)/*.c))
	$(CC) -shared -o $(SODIR)/$@ $^ $(LINKER) $(OMPFLAGS)

$(ARCHIVE): $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(wildcard $(SRCDIR)/*.c))
	ar -r $(OUTDIR)/$@ $^

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) -c $(CFLAGS) $(INCLUDE) $(OPTFLAGS) $(OMPFLAGS) -o $@ $<

directories: $(OBJDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)
	mkdir -p $(OUTDIR)

.PHONY: python
python:
	echo $(shell pwd) > hmpdf/PATHTOHMPDF.txt
	pip install . --user

.PHONY: clean
clean:
	rm $(OBJDIR)/*.o
	rmdir $(OBJDIR)
	rm $(SODIR)/$(SHARED)
	rm $(OUTDIR)/$(ARCHIVE)
	rmdir $(OUTDIR)
