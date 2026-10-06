CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror
CPPFLAGS += $(shell pkg-config --cflags ncursesw)
LDLIBS += $(shell pkg-config --libs ncursesw)

all: operator
operator: operator.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)
check: operator
	python3 test_operator.py
clean:
	rm -f operator
.PHONY: all check clean
