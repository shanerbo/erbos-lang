CC = cc
CFLAGS = -Wall -Wextra -std=c11
SRC = src/main.c src/lexer.c src/parser.c src/checker.c src/codegen.c
OUT = erbos

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)

clean:
	rm -f $(OUT) *.s *.o a.out

.PHONY: all clean
