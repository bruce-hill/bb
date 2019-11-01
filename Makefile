NAME=bb
PREFIX=
CC=gcc
O=-O2
CFLAGS=-std=c99 -D_XOPEN_SOURCE=500 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
CWARN=-Wall -Wpedantic -Wno-unknown-pragmas
#CWARN += -fsanitize=address -fno-omit-frame-pointer
G=

ifeq ($(shell uname),Darwin)
	CFLAGS += -D_DARWIN_C_SOURCE
	CWARN += -Weverything -Wno-missing-field-initializers -Wno-padded\
			  -Wno-missing-noreturn -Wno-cast-qual
endif

ifneq (, $(SH))
	CFLAGS += -D'SH="$(SH)"'
endif

PICKER_FLAG=
ifeq (, $(PICKER))
	PICKER=$(shell sh -c "(which fzf >/dev/null 2>/dev/null && echo 'fzf') || (which fzy >/dev/null 2>/dev/null && echo 'fzy') || (which pick >/dev/null 2>/dev/null && echo 'pick') || (which ask >/dev/null 2>/dev/null && echo 'ask')")
endif
ifneq (, $(PICKER))
	PICKER_FLAG=-D"PICK=\"$(PICKER) --prompt=\\\"$$1\\\"\""
	ifeq ($(shell which $(PICKER)),$(shell which fzf 2>/dev/null || echo '<none>'))
		PICKER_FLAG=-D'PICK="printf \"\\033[3A\\033[?25h\" >/dev/tty; fzf --read0 --height=4 --prompt=\"$$1\""'
	endif
	ifeq ($(shell which $(PICKER)),$(shell which fzy 2>/dev/null || echo '<none>'))
		PICKER_FLAG=-D'PICK="printf \"\\033[3A\\033[?25h\" >/dev/tty; tr "\\0" "\\n" | fzy --lines=3 --prompt=\"\033[1m$$1\033[0m\""'
	endif
	ifeq ($(shell which $(PICKER)),$(shell which ask 2>/dev/null || echo '<none>'))
		PICKER_FLAG=-D'PICK="/usr/bin/env ask --read0 --prompt=\"$$1\033[?25h\""'
	endif
	ifeq ($(shell which $(PICKER)),$(shell which pick 2>/dev/null || echo '<none>'))
		PICKER_FLAG=-D'PICK="printf \"\\033[?25h\" >/dev/tty; tr "\\0" "\\n" | pick"'
	endif
	ifeq ($(shell which $(PICKER)),$(shell which dmenu 2>/dev/null || echo '<none>'))
		PICKER_FLAG=-D'PICK="tr "\\0" "\\n" | dmenu -i -l 10 -p \"$$1\""'
	endif
endif
CFLAGS += $(PICKER_FLAG)

ifneq (, $(ASKER))
	PERCENT := %
	ifeq ($(shell which $(ASKER)),$(shell which ask 2>/dev/null || echo '<none>'))
		CFLAGS += -D'ASK="eval \"$$1=\\$$(/usr/bin/env ask --history=bb.hist --prompt=\\\"$$2\033[?25h\\\" --query=\\\"$$3\\\")\""'
		CFLAGS += -D'CONFIRM="/usr/bin/env ask -n \"Is that okay?\033[?25h\""'
	endif
	ifeq ($(shell which $(ASKER)),$(shell which dmenu 2>/dev/null || echo '<none>'))
		CFLAGS += -D'ASK="eval \"$$1=\\$$(echo \"$$3\" | dmenu -p \"$$2\")\""'
	endif
endif

all: $(NAME)

clean:
	rm -f $(NAME)

$(NAME): $(NAME).c bterm.h bb.h
	$(CC) $(NAME).c $(CFLAGS) $(CWARN) $(G) $(O) -o $(NAME)
	
install: $(NAME)
	@prefix="$(PREFIX)"; \
	if [ ! "$$prefix" ]; then \
		printf '\033[1mWhere do you want to install? (default: /usr/local) \033[0m'; \
		read prefix; \
	fi; \
	[ ! "$$prefix" ] && prefix="/usr/local"; \
	[ ! "$$sysconfdir" ] && sysconfdir=/etc; \
	mkdir -m 644 -pv "$$prefix/share/man/man1" \
	mkdir -m 755 -pv "$$prefix/bin" "$$sysconfdir/xdg/bb" \
	&& cp -v $(NAME) "$$prefix/bin/" \
	&& cp -v $(NAME).1 "$$prefix/share/man/man1/" \
	&& cp -v bbstartup.sh bindings.bb "$$sysconfdir/xdg/bb/"

uninstall:
	@prefix="$(PREFIX)"; \
	if [ ! "$$prefix" ]; then \
		printf '\033[1mWhere do you want to uninstall from? (default: /usr/local) \033[0m'; \
		read prefix; \
	fi; \
	[ ! "$$prefix" ] && prefix="/usr/local"; \
	[ ! "$$sysconfdir" ] && sysconfdir=/etc; \
	[ ! "$$XDG_CONFIG_HOME" ] && XDG_CONFIG_HOME=~/.config; \
	echo "Deleting..."; \
	rm -rvf "$$prefix/bin/$(NAME)" "$$prefix/share/man/man1/$(NAME).1" "$$sysconfdir/xdg/bb"; \
	printf "\033[1mIf you created any config files in ~/.config/bb, you may want to delete them manually.\033[0m"

