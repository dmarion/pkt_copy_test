CFLAGS ?= -I. -fPIC -march=haswell -O3 -g
LDFLAGS ?= -lpthread -lm

.DEFAULT_GOAL := all

srcs = $(wildcard *.c)

$(srcs:.c=.d):%.d:%.c
	@echo "DEP  $@"
	@$(CC) $(CFLAGS) -MM -MT $*.o $< >$@

$(srcs:.c=.E):%.E:%.c
	@echo "PRE  $@"
	@$(CC) $(CFLAGS) -E -P -o $@ $<

$(srcs:.c=.S):%.S:%.c
	@echo "ASM $@"
	@$(CC) $(CFLAGS) -fpie -ffunction-sections -fdata-sections -S -o $@ $<

ifneq ($(findstring $(MAKECMDGOALS), fixstyle clean),)
-include $(srcs:.c=.d)
endif

$(srcs:.c=.o):%.o:%.c %.d
	@echo "CC   $@"
	@$(CC) $(CFLAGS) -c -o $@ $<

%.bin: %.o
	@echo "BIN  $@"
	@$(CC) -o $@ $^ $(LDFLAGS)

all: $(srcs:.c=.bin)

clean:
	@rm -rf $(srcs:.c=.o) $(srcs:.c=.d)  $(srcs:.c=.bin)

fixstyle:
	@clang-format -i -style=Gnu $(srcs)
