NAME=bb
PREFIX=
CC=cc
G=
O=-O2
CFLAGS=-std=c99 -Werror -D_XOPEN_SOURCE=700 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
CWARN=-Wall -Wpedantic -Wextra -Wsign-conversion -Wtype-limits -Wunused-result
#CFLAGS += -fsanitize=address -fno-omit-frame-pointer
CFLAGS += '-DBB_NAME="$(NAME)"'

CFILES=draw.c terminal.c
OBJFILES=$(CFILES:.c=.o)

all: $(NAME)

clean:
	rm -f $(NAME) $(OBJFILES)

.c.o:
	$(CC) -c $(CFLAGS) $(CWARN) $(G) $(O) -o $@ $<

$(NAME): $(OBJFILES) bb.c
	$(CC) $(CFLAGS) $(CWARN) $(G) $(O) -o $@ $(OBJFILES) bb.c

install: $(NAME)
	@prefix="$(PREFIX)"; \
	if [ ! "$$prefix" ]; then \
		printf '\033[1mWhere do you want to install? (default: /usr/local) \033[0m'; \
		read prefix; \
	fi; \
	[ ! "$$prefix" ] && prefix="/usr/local"; \
	[ ! "$$sysconfdir" ] && sysconfdir=/etc; \
	mkdir -pv -m 755 "$$prefix/man/man1" "$$prefix/bin" "$$sysconfdir/xdg/$(NAME)" \
	&& cp -rv scripts/* "$$sysconfdir/xdg/$(NAME)/" \
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
	rm -rvf "$$prefix/bin/$(NAME)" "$$prefix/man/man1/$(NAME).1" "$$prefix/man/man1/bbcmd.1" "$$sysconfdir/xdg/$(NAME)"; \
	printf "\033[1mIf you created any config files in ~/.config/$(NAME), you may want to delete them manually.\033[0m\n"

.PHONY: all, clean, install, uninstall
