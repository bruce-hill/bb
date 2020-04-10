NAME=bb
PREFIX=
CC ?= gcc
O ?= -O2
CFLAGS=-std=c99 -D_XOPEN_SOURCE=500 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
CWARN=-Wall -Wpedantic -Wextra -Wno-unknown-pragmas -Wno-missing-field-initializers\
	  -Wno-padded -Wsign-conversion -Wno-missing-noreturn -Wno-cast-qual -Wtype-limits
#CFLAGS += -fsanitize=address -fno-omit-frame-pointer
G=

all: $(NAME)

clean:
	rm -f $(NAME)

$(NAME): $(NAME).c bterm.h bb.h columns.h
	$(CC) $(NAME).c $(CFLAGS) $(CWARN) $(G) $(O) -o $@

install: $(NAME)
	@prefix="$(PREFIX)"; \
	if [ ! "$$prefix" ]; then \
		printf '\033[1mWhere do you want to install? (default: /usr/local) \033[0m'; \
		read prefix; \
	fi; \
	[ ! "$$prefix" ] && prefix="/usr/local"; \
	[ ! "$$sysconfdir" ] && sysconfdir=/etc; \
	mkdir -pv -m 755 "$$prefix/share/man/man1" "$$prefix/bin" "$$sysconfdir/xdg/bb" \
	&& cp -rv scripts/* "$$sysconfdir/xdg/bb/" \
	&& cp -v bb.1 bbcmd.1 "$$prefix/share/man/man1/" \
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
	rm -rvf "$$prefix/bin/$(NAME)" "$$prefix/share/man/man1/bb.1" "$$prefix/share/man/man1/bbcmd.1" "$$sysconfdir/xdg/bb"; \
	printf "\033[1mIf you created any config files in ~/.config/bb, you may want to delete them manually.\033[0m\n"

.PHONY: all, clean, install, uninstall
