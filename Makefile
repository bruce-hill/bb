NAME=bb
PREFIX=
CC=cc
G=
O=-O2
CFLAGS=-std=c99 -Werror -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L
CWARN=-Wall -Wextra
#   -Wpedantic -Wsign-conversion -Wtype-limits -Wunused-result \
# 	-Wsign-conversion -Wtype-limits -Wunused-result -Wnull-dereference \
# 	-Waggregate-return -Walloc-zero -Walloca -Warith-conversion -Wcast-align -Wcast-align=strict \
# 	-Wdangling-else -Wdate-time -Wdisabled-optimization -Wdouble-promotion -Wduplicated-branches \
# 	-Wduplicated-cond -Wexpansion-to-defined -Wfloat-conversion -Wfloat-equal -Wformat-nonliteral \
# 	-Wformat-security -Wformat-signedness -Wframe-address -Winline -Winvalid-pch -Wjump-misses-init \
# 	-Wlogical-op -Wlong-long -Wmissing-format-attribute -Wmissing-include-dirs -Wmissing-noreturn \
# 	-Wnull-dereference -Woverlength-strings -Wpacked -Wpacked-not-aligned -Wpointer-arith \
# 	-Wredundant-decls -Wshadow -Wshadow=compatible-local -Wshadow=global -Wshadow=local \
# 	-Wsign-conversion -Wstack-protector -Wsuggest-attribute=const -Wswitch-default -Wswitch-enum \
# 	-Wsync-nand -Wtrampolines -Wundef -Wunused -Wunused-but-set-variable \
# 	-Wunused-const-variable -Wunused-local-typedefs -Wunused-macros -Wvariadic-macros -Wvector-operation-performance \
# 	-Wvla -Wwrite-strings
#CFLAGS += -fsanitize=address -fno-omit-frame-pointer
CFLAGS += '-DBB_NAME="$(NAME)"'
OSFLAGS != case $$(uname -s) in *BSD|Darwin) echo '-D_BSD_SOURCE';; Linux) echo '-D_GNU_SOURCE';; *) echo '-D_DEFAULT_SOURCE';; esac

CFILES=draw.c terminal.c utils.c
OBJFILES=$(CFILES:.c=.o)

all: $(NAME)

clean:
	rm -f $(NAME) $(OBJFILES)

%.o: %.c %.h types.h utils.h
	$(CC) -c $(CFLAGS) $(OSFLAGS) $(CWARN) $(G) $(O) -o $@ $<

$(NAME): $(OBJFILES) bb.c
	$(CC) $(CFLAGS) $(OSFLAGS) $(CWARN) $(G) $(O) -o $@ $(OBJFILES) bb.c

install: $(NAME)
	@prefix="$(PREFIX)"; \
	if [ ! "$$prefix" ]; then \
		printf '\033[1mWhere do you want to install? (default: /usr/local) \033[0m'; \
		read prefix; \
	fi; \
	[ ! "$$prefix" ] && prefix="/usr/local"; \
	[ ! "$$sysconfdir" ] && sysconfdir=/etc; \
	mkdir -p -m 755 "$$prefix/man/man1" "$$prefix/bin" "$$sysconfdir/$(NAME)" \
	&& cp -rv scripts/* "$$sysconfdir/$(NAME)/" \
	&& cp -v bb.1 "$$prefix/man/man1/$(NAME).1" \
	&& cp -v bbcmd.1 "$$prefix/man/man1/bbcmd.1" \
	&& rm -f "$$prefix/bin/$(NAME)" \
	&& cp -v $(NAME) "$$prefix/bin/"

uninstall:
	@prefix="$(PREFIX)"; \
	if [ ! "$$prefix" ]; then \
		printf '\033[1mWhere do you want to uninstall from? (default: /usr/local) \033[0m'; \
		read prefix; \
	fi; \
	[ ! "$$prefix" ] && prefix="/usr/local"; \
	[ ! "$$sysconfdir" ] && sysconfdir=/etc; \
	echo "Deleting..."; \
	rm -rvf "$$prefix/bin/$(NAME)" "$$prefix/man/man1/$(NAME).1" "$$prefix/man/man1/bbcmd.1" "$$sysconfdir/$(NAME)"; \
	printf "\033[1mIf you created any config files in ~/.config/$(NAME), you may want to delete them manually.\033[0m\n"

.PHONY: all, clean, install, uninstall
