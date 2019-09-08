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

PICKER_FLAG=
ifeq (, $(PICKER))
	PICKER=$(shell sh -c "(which fzy >/dev/null 2>/dev/null && echo 'fzy') || (which fzf >/dev/null 2>/dev/null && echo 'fzf') || (which pick >/dev/null 2>/dev/null && echo 'pick') || (which ask >/dev/null 2>/dev/null && echo 'ask')")
endif
ifneq (, $(PICKER))
	PICKER_FLAG=-D"PICK(prompt, initial)=\"$(PICKER)\""
	ifeq ($(shell which $(PICKER)),$(shell which fzy 2>/dev/null || echo '<none>'))
		PICKER_FLAG=-D'PICK(prompt, initial)="{ printf \"\\033[3A\" >/dev/tty; fzy --lines=3 --prompt=\"" prompt "\" --query=\"" initial "\"; }"'
	endif
	ifeq ($(shell which $(PICKER)),$(shell which fzf 2>/dev/null || echo '<none>'))
		PICKER_FLAG=-D'PICK(prompt, initial)="{ printf \"\\033[3A\" >/dev/tty; fzf --height=4 --prompt=\"" prompt "\" --query=\"" initial "\"; }"'
	endif
	ifeq ($(shell which $(PICKER)),$(shell which ask 2>/dev/null || echo '<none>'))
		PICKER_FLAG=-D'PICK(prompt, initial)="ask --prompt=\"" prompt "\" --query=\"" initial "\""'
	endif
	ifeq ($(shell which $(PICKER)),$(shell which pick 2>/dev/null || echo '<none>'))
		PICKER_FLAG=-D'PICK(prompt, initial)="pick -q \"" initial "\""'
	endif
	ifeq ($(shell which $(PICKER)),$(shell which dmenu 2>/dev/null || echo '<none>'))
		PICKER_FLAG=-D'PICK(prompt, initial)="dmenu -p \"" prompt "\""'
	endif
endif
CFLAGS += $(PICKER_FLAG)

ifneq (, $(USE_ASK))
	CFLAGS += -D'USE_ASK=1'
endif

all: $(NAME)

clean:
	rm -f $(NAME)

config.h:
	cp config.def.h config.h

$(NAME): $(NAME).c bterm.h config.h
	$(CC) $(NAME).c $(CFLAGS) $(CWARN) $(G) $(O) -o $(NAME)
	
install: $(NAME)
	@prefix="$(PREFIX)"; \
	if test -z $$prefix; then \
		read -p $$'\033[1mWhere do you want to install? (default: /usr/local) \033[0m' prefix; \
	fi; \
	if test -z $$prefix; then \
		prefix="/usr/local"; \
	fi; \
	mkdir -pv $$prefix/bin $$prefix/share/man/man1 \
	&& cp -v $(NAME) $$prefix/bin/ \
	&& cp -v $(NAME).1 $$prefix/share/man/man1/

uninstall:
	@prefix="$(PREFIX)"; \
	if test -z $$prefix; then \
		read -p $$'\033[1mWhere do you want to uninstall from? (default: /usr/local) \033[0m' prefix; \
	fi; \
	if test -z $$prefix; then \
		prefix="/usr/local"; \
	fi; \
	echo "Deleting..."; \
	rm -rvf $$prefix/bin/$(NAME) $$prefix/share/man/man1/$(NAME).1

