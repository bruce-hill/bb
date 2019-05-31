PREFIX=
CC=gcc
CFLAGS=-O0 -std=gnu99 -D_XOPEN_SOURCE=500 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wpedantic -Wno-unknown-pragmas
UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
CFLAGS += -D_DARWIN_C_SOURCE -fsanitize=address -fno-omit-frame-pointer\
	-Weverything -Wno-missing-field-initializers -Wno-padded -Wno-missing-noreturn -Wno-cast-qual
endif
LIBS=
NAME=bb
G=-g

all: $(NAME)

clean:
	rm $(NAME)

config.h:
	cp config.def.h config.h

$(NAME): $(NAME).c bterm.h config.h
	$(CC) $(NAME).c $(LIBS) $(CFLAGS) $(G) -o $(NAME)

test: $(NAME)
	./$(NAME) test.xml
	
install: $(NAME)
	@prefix="$(PREFIX)"; \
	if [[ ! $$prefix ]]; then \
		read -p $$'\033[1mWhere do you want to install? (default: /usr/local) \033[0m' prefix; \
	fi; \
	if [[ ! $$prefix ]]; then \
		prefix="/usr/local"; \
	fi; \
	mkdir -pv $$prefix/bin $$prefix/share/man/man1 \
	&& cp -v $(NAME) $$prefix/bin/ \
	&& cp -v $(NAME).1 $$prefix/share/man/man1/

uninstall:
	@prefix="$(PREFIX)"; \
	if [[ ! $$prefix ]]; then \
		read -p $$'\033[1mWhere do you want to uninstall from? (default: /usr/local) \033[0m' prefix; \
	fi; \
	if [[ ! $$prefix ]]; then \
		prefix="/usr/local"; \
	fi; \
	echo "Deleting..."; \
	rm -rvf $$prefix/bin/$(NAME) $$prefix/share/man/man1/$(NAME).1

