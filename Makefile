PREFIX=
CC=cc
CFLAGS=-O0 -std=gnu99 -Wall -Wpedantic -Weverything -Wno-missing-field-initializers -Wno-padded -Wno-missing-noreturn -Wno-cast-qual \
-fsanitize=address -fno-omit-frame-pointer
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

